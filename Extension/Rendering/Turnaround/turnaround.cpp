#include "turnaround.h"
#include "turnaround_research.h"
#include "turnaround_session.h"
#include "turnaround_stage.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Extension/Customization/local_customization.h"
#include "Extension/Customization/local_customization_runtime.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>

namespace dingosdk::turnaround {
namespace {
Session& session() { static auto* value = new Session; return *value; } // process lifetime: its thread may outlive statics

std::span<const std::uint8_t> built_in(const wchar_t* name) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&built_in), &module))
        return {};
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto* data = handle ? static_cast<const std::uint8_t*>(LockResource(handle)) : nullptr;
    return data ? std::span<const std::uint8_t>(data, SizeofResource(module, resource)) : std::span<const std::uint8_t>{};
}

std::filesystem::path game_folder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) { path.resize(length); break; }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
}

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// What a board item is, from where its asset lives ("Characters/Customization/Board/Decks/...").
std::string slot_label(const std::string& asset) {
    const auto path = lower(asset);
    struct Known { const char* part; const char* label; };
    static constexpr Known known[] = {{"grip", "Grip tape"}, {"deck", "Deck"},       {"truck", "Trucks"},
                                      {"wheel", "Wheels"},   {"bearing", "Bearings"}, {"hardware", "Hardware"},
                                      {"riser", "Risers"},   {"bolt", "Hardware"}};
    for (const auto& k : known)
        if (path.find(k.part) != std::string::npos) return k.label;
    // Otherwise the folder it is in.
    const auto file = asset.find_last_of('/');
    if (file == std::string::npos || !file) return "Part";
    const auto folder = asset.find_last_of('/', file - 1);
    return asset.substr(folder == std::string::npos ? 0 : folder + 1, file - (folder == std::string::npos ? 0 : folder + 1));
}

// The board the skater wears now: its items, deck first, read from the game's loadouts (the ones
// local_customization_runtime.cpp saves). The board items are the ones under Customization/Board.
std::vector<Part> worn_board() {
    std::vector<Part> parts;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::uintptr_t manager{}, begin{}, end{};
    if (!memory::read(base + addr::engine::loadout_manager, manager) || !manager || !memory::read(manager + 0x18, begin) ||
        !memory::read(manager + 0x20, end) || begin > end || (end - begin) % 0x48 || (end - begin) / 0x48 > 10)
        return parts;
    for (std::uintptr_t record = begin; record < end && parts.empty(); record += 0x48) {
        profile::CosmeticLoadout loadout;
        std::vector<profile_runtime::CosmeticNativeRecipe> native;
        if (!profile_runtime::read_cosmetic_recipes(reinterpret_cast<const void*>(record), loadout, native)) continue;
        for (const auto& recipe : loadout.recipes)
            for (const auto& item : recipe.items) {
                if (lower(item.asset).find("/board/") == std::string::npos) continue;
                auto title = profile::item_display_metadata(item.asset, Json::object()).value("title", "");
                if (title.empty()) title = item.asset.substr(item.asset.find_last_of('/') + 1);
                parts.push_back({slot_label(item.asset), title});
            }
    }
    std::stable_partition(parts.begin(), parts.end(), [](const Part& p) { return p.slot == "Deck"; });
    return parts;
}

// Notices for the turnaround's progress, raised from the render thread's poll: once for each phase
// of each turnaround (`runs` counts them).
std::atomic<unsigned> runs{};
void report(const Progress& now) {
    static unsigned last_run = 0;
    static Phase last = Phase::idle;
    const auto run = runs.load();
    if (now.phase == last && run == last_run) return;
    last = now.phase, last_run = run;
    switch (now.phase) {
    case Phase::capturing: overlay::notify(overlay::NoticeLevel::info, "Turnaround", "Turning the board. Hands off for a few seconds."); break;
    case Phase::encoding: overlay::notify(overlay::NoticeLevel::info, "Turnaround", "Saving..."); break;
    case Phase::done: overlay::notify(overlay::NoticeLevel::info, "Turnaround saved", now.message); break;
    case Phase::failed: overlay::notify(overlay::NoticeLevel::warning, "Turnaround", now.message); break;
    default: break;
    }
    logging::log(now.phase == Phase::failed ? logging::Level::warning : logging::Level::info, logging::Channel::graphics,
                 "Turnaround: {}", now.message);
}

bool frame_grab_active() noexcept {
    try {
        report(session().progress());
    } catch (...) {}
    return research::grab_requested() || session().busy();
}
bool frame_grab_wants() noexcept { return research::grab_requested() || session().wants_frame(); }
void frame_grab_take(const overlay::GrabbedFrame& frame) noexcept {
    if (research::grab_requested()) {
        research::take_grab(frame);
        return;
    }
    session().take_frame({frame.data, frame.width, frame.height, frame.pitch, frame.bgra});
}
constexpr overlay::FrameGrabHost frame_grab_host{&frame_grab_active, &frame_grab_wants, &frame_grab_take};
} // namespace

const overlay::FrameGrabHost* frame_grab() noexcept { return &frame_grab_host; }

Options& options() noexcept {
    static Options value;
    return value;
}

std::string start_export() {
    if (session().busy()) return "A turnaround is already being made.";
    try {
        Session::Inputs in;
        in.options = options();
        in.parts = worn_board();
        const auto deck = std::find_if(in.parts.begin(), in.parts.end(), [](const Part& p) { return p.slot == "Deck"; });
        in.title = deck != in.parts.end() ? deck->name : "board";
        in.fonts = {built_in(L"FONT_HEADING"), built_in(L"FONT_BODY"), built_in(L"FONT_BRUSH")};
        if (in.fonts.heading.empty() || in.fonts.body.empty() || in.fonts.brush.empty()) in.options.parts = false;
        in.folder = game_folder() / L"Turnarounds";
        in.stage = board_stage();
        ++runs;
        if (!session().start(std::move(in))) return "A turnaround is already being made.";
        const auto progress = session().progress();
        if (progress.phase == Phase::failed) return progress.message;
        return "Turning the board; the file goes to the Turnarounds folder beside Skate.exe.";
    } catch (const std::exception& error) {
        return std::string("The turnaround could not start: ") + error.what();
    }
}

void cancel_export() noexcept { session().cancel(); }
bool exporting() noexcept { return session().busy(); }
} // namespace dingosdk::turnaround
