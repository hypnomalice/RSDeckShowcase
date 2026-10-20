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
#include <ShlObj.h>
#include <shellapi.h>
#include <charconv>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>

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
                const auto path = lower(item.asset);
                const bool board = path.find("/board/") != std::string::npos || path.find("skateboard") != std::string::npos ||
                                   path.find("/deck") != std::string::npos || path.find("griptape") != std::string::npos ||
                                   path.find("/trucks") != std::string::npos || path.find("/wheels") != std::string::npos;
                if (!board) continue;
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

namespace {
// The player's choices, kept between sessions: <Local AppData>\ReSkate\turnaround\settings.txt,
// one key=value a line, the keys set_option takes.
std::filesystem::path settings_path() {
    PWSTR raw{};
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw)
        result = std::filesystem::path(raw) / L"ReSkate" / L"turnaround" / L"settings.txt";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::mutex& options_mutex() { static auto* value = new std::mutex; return *value; }
std::string hex(Rgb colour) { return std::format("#{:02x}{:02x}{:02x}", colour[0], colour[1], colour[2]); }

// Applies one setting to `o`; what to tell the player, or empty when the key or value is not one.
std::string apply(Options& o, std::string_view key, std::string_view value) {
    const auto number = [&](int low, int high) -> std::optional<int> {
        int n{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (error != std::errc{} || end != value.data() + value.size() || n < low || n > high) return std::nullopt;
        return n;
    };
    const bool yes = value == "on" || value == "1" || value == "true", no = value == "off" || value == "0" || value == "false";
    if (key == "format") {
        if (value == "gif") o.format = Format::gif;
        else if (value == "apng" || value == "png") o.format = Format::apng;
        else if (value == "webm") o.format = Format::webm;
        else return {};
        return std::string("Format: ") + (o.format == Format::gif ? "GIF" : o.format == Format::apng ? "APNG" : "WebM") + ".";
    }
    if (key == "background") {
        if (value == "clear" || value == "transparent") o.transparent = true;
        else if (value == "matte") o.transparent = false;
        else return {};
        return o.transparent ? "Background: transparent." : "Background: matte " + hex(o.matte) + ".";
    }
    if (key == "matte") {
        auto text = value;
        if (text.starts_with('#')) text.remove_prefix(1);
        unsigned v{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
        if (text.size() != 6 || error != std::errc{} || end != text.data() + text.size()) return {};
        o.matte = {static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
        return "Matte colour: " + hex(o.matte) + ".";
    }
    if (key == "parts") {
        if (!yes && !no) return {};
        o.parts = yes;
        return o.parts ? "The parts card is on." : "The parts card is off.";
    }
    if (key == "speed") {
        const auto n = number(5, 100);
        if (!n) return {};
        o.spin = static_cast<float>(*n) / 100.0f * (o.spin < 0 ? -1.0f : 1.0f);
        return std::format("Spin speed: {}%.", *n);
    }
    if (key == "direction") {
        if (value != "left" && value != "right") return {};
        o.spin = std::abs(o.spin) * (value == "left" ? -1.0f : 1.0f);
        return std::string("Spins to the ") + std::string(value) + ".";
    }
    if (key == "axis") {
        if (value == "lx") o.spin_axis = 0;
        else if (value == "ly") o.spin_axis = 1;
        else if (value == "rx") o.spin_axis = 2;
        else if (value == "ry") o.spin_axis = 3;
        else return {};
        return "Spin stick: " + std::string(value) + ".";
    }
    if (key == "frames") {
        const auto n = number(8, 240);
        if (!n) return {};
        o.frames = static_cast<std::uint32_t>(*n);
        return std::format("{} frames a turn.", *n);
    }
    if (key == "size") {
        const auto n = number(64, 2048);
        if (!n) return {};
        o.size = static_cast<std::uint32_t>(*n);
        return std::format("Size: {} px.", *n);
    }
    if (key == "fps") {
        const auto n = number(1, 60);
        if (!n) return {};
        o.fps = static_cast<std::uint32_t>(*n);
        return std::format("{} frames a second.", *n);
    }
    return {};
}

void save(const Options& o) {
    const auto path = settings_path();
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::trunc);
    out << "format=" << extension(o.format) << "\n"
        << "background=" << (o.transparent ? "clear" : "matte") << "\n"
        << "matte=" << hex(o.matte) << "\n"
        << "parts=" << (o.parts ? "on" : "off") << "\n"
        << "speed=" << std::lround(std::abs(o.spin) * 100) << "\n"
        << "direction=" << (o.spin < 0 ? "left" : "right") << "\n"
        << "axis=" << (o.spin_axis == 0 ? "lx" : o.spin_axis == 1 ? "ly" : o.spin_axis == 3 ? "ry" : "rx") << "\n"
        << "frames=" << o.frames << "\n"
        << "size=" << o.size << "\n"
        << "fps=" << o.fps << "\n";
}
} // namespace

Options& options() noexcept {
    static Options value = [] {
        Options o;
        try {
            std::ifstream in(settings_path());
            for (std::string line; std::getline(in, line);) {
                const auto equals = line.find('=');
                if (equals != std::string::npos) {
                    auto key = std::string_view(line).substr(0, equals), value = std::string_view(line).substr(equals + 1);
                    if (key == "format" && value == "png") value = "apng";
                    apply(o, key, value);
                }
            }
        } catch (...) {}
        return o;
    }();
    return value;
}

std::string set_option(std::string_view key, std::string_view value) {
    std::lock_guard lock(options_mutex());
    auto& o = options();
    auto said = apply(o, key, value);
    if (said.empty()) return "turnaround set format gif|apng|webm | background clear|matte | matte #rrggbb | parts on|off | "
                             "speed 5-100 | direction left|right | frames 8-240 | size 64-2048 | fps 1-60";
    try { save(o); } catch (...) {}
    return said;
}

Options current_options() {
    std::lock_guard lock(options_mutex());
    return options();
}

std::string last_message() {
    const auto progress = session().progress();
    if (progress.phase == Phase::capturing) return std::format("Turning the board... {} / {}", progress.step, progress.steps);
    return progress.message;
}

void open_folder() {
    const auto folder = game_folder() / L"Turnarounds";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string start_export() {
    if (session().busy()) return "A turnaround is already being made.";
    try {
        Session::Inputs in;
        in.options = current_options();
        in.parts = worn_board();
        const auto deck = std::find_if(in.parts.begin(), in.parts.end(), [](const Part& p) { return p.slot == "Deck"; });
        in.title = deck != in.parts.end() ? deck->name : "board";
        in.fonts = {built_in(L"FONT_HEADING"), built_in(L"FONT_BODY"), built_in(L"FONT_BRUSH")};
        if (in.fonts.heading.empty() || in.fonts.body.empty() || in.fonts.brush.empty()) in.options.parts = false;
        in.folder = game_folder() / L"Turnarounds";
        in.stage = board_stage(in.options);
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
