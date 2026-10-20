#include "skate_menu_internal.h"
#include "skate_style.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

// ADDONS > TURNAROUND: how the board turnaround (V on the Board screen) is made. Every change is a
// `turnaround set` console command, so the settings are kept the same way however they are set.
namespace dingosdk::overlay::menu {
namespace {
struct Matte {
    const char* name;
    std::array<std::uint8_t, 3> colour;
};
constexpr std::array<Matte, 5> mattes{{{"Black", {0, 0, 0}},
                                       {"Charcoal", {24, 25, 28}},
                                       {"Discord", {49, 51, 56}},
                                       {"White", {255, 255, 255}},
                                       {"Green screen", {0, 177, 64}}}};

// A choice the player just made, shown until the setting comes back from the turnaround: the page
// is drawn from the menu's model, which catches up a moment later, and stepping a full screen
// "< GIF >" twice before then would step from the old value twice.
struct Pending { int value = -1; double at = 0; };
Pending& pending(const char* id) {
    static std::array<std::pair<std::string, Pending>, 12> all{};
    for (auto& [name, p] : all)
        if (name == id) return p;
    for (auto& [name, p] : all)
        if (name.empty()) { name = id; return p; }
    return all.back().second;
}
int shown(const char* id, int current) {
    auto& p = pending(id);
    if (p.value < 0) return current;
    if (p.value == current || ImGui::GetTime() - p.at > 2.0) { p.value = -1; return current; }
    return p.value;
}
void picked(const char* id, int value) { pending(id) = {value, ImGui::GetTime()}; }

std::string hex(const std::array<std::uint8_t, 3>& c) {
    char text[8];
    std::snprintf(text, sizeof text, "#%02x%02x%02x", c[0], c[1], c[2]);
    return text;
}

// A slider whose value is sent once let go, not every frame of a drag; the dragged value shows until
// the setting comes back.
void setting_slider(SkateMenu& menu, const CallbacksV3& callbacks, bool enabled, const char* id, int current, int low, int high,
                    int step, const char* format, const char* key) {
    struct Edit { int value = -1; double at = 0; };
    static std::array<std::pair<std::string, Edit>, 4> edits{};
    Edit* edit = nullptr;
    for (auto& [name, e] : edits)
        if (name == id || name.empty()) { name = id; edit = &e; break; }
    if (!edit) return;
    int value = edit->value >= 0 ? edit->value : current;
    ImGui::BeginDisabled(!enabled);
    if (slider_int(id, &value, low, high, format, ImGuiSliderFlags_AlwaysClamp)) {
        edit->value = std::clamp((value + step / 2) / step * step, low, high);
        edit->at = ImGui::GetTime();
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && edit->value >= 0) {
        send_console(menu, callbacks, std::string("turnaround set ") + key + " " + std::to_string(edit->value));
        edit->at = ImGui::GetTime();
    }
    if (edit->value >= 0 && !ImGui::IsItemActive() && (current == edit->value || ImGui::GetTime() - edit->at > 2.0)) edit->value = -1;
    ImGui::EndDisabled();
}
} // namespace

void turnaround_addon(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& t = model.turnaround;
    const bool can = t.available && callbacks.queue_console_command && !t.busy;
    const auto set = [&](const std::string& key, const std::string& value) {
        send_console(menu, callbacks, "turnaround set " + key + " " + value);
    };

    begin_card(menu, "turnaround", "TURNAROUND", "A spinning picture of your board, to share");
    note("On the Board screen press V: the board turns once by itself and is saved to the Turnarounds folder beside Skate.exe. "
         "Keep your hands off the controls while it turns.");
    if (!t.status.empty()) info(menu, t.busy ? "Now" : "Last", t.status);
    const int pressed = button_row(menu, "turnaround-actions",
                                   {{"OPEN TURNAROUNDS FOLDER", callbacks.queue_console_command != nullptr, false, "See the saved turnarounds."},
                                    {"CANCEL", t.busy, false, "Stop the turnaround being made."}});
    if (pressed == 0) send_console(menu, callbacks, "turnaround open");
    else if (pressed == 1) send_console(menu, callbacks, "turnaround cancel");
    end_card();

    begin_card(menu, "turnaround-file", "FILE");
    field(menu, "Format");
    int format = shown("format", t.format);
    if (choice(menu, "turnaround-format", format, {"GIF", "APNG", "WebM"}, can, 0.0f,
               {"Plays everywhere. Hard edges: soft ones are blended toward the edge colour.",
                "Animated PNG: full colour and soft see-through edges. Discord and browsers play it.",
                "Video: small files, soft see-through edges in Chrome and Firefox."}))
        picked("format", format), set("format", format == 1 ? "apng" : format == 2 ? "webm" : "gif");
    field(menu, "Background");
    int background = shown("background", t.transparent ? 0 : 1);
    if (choice(menu, "turnaround-background", background, {"Transparent", "Matte"}, can, 0.0f,
               {"No background: just the board.", "The board over a solid colour."}))
        picked("background", background), set("background", background == 0 ? "clear" : "matte");
    field(menu, t.transparent ? (t.format == 0 ? "Edge colour" : "Matte colour (when matte)") : "Matte colour",
          t.transparent && t.format == 0 ? "A GIF has no soft edges: they are blended toward this colour. Pick the page's." : nullptr);
    int matte = -1;
    for (std::size_t i = 0; i < mattes.size(); ++i)
        if (mattes[i].colour == t.matte) matte = static_cast<int>(i);
    matte = shown("matte", matte);
    if (choice(menu, "turnaround-matte", matte, {"Black", "Charcoal", "Discord", "White", "Green screen"}, can))
        picked("matte", matte), set("matte", hex(mattes[static_cast<std::size_t>(std::clamp(matte, 0, static_cast<int>(mattes.size()) - 1))].colour));
    if (matte < 0) info(menu, "Colour", hex(t.matte) + " (set with turnaround set matte #rrggbb)");
    bool parts = shown("parts", t.parts ? 1 : 0) == 1;
    if (toggle_row(menu, "Parts card", "Show the deck, grip, trucks and wheels by name beside the board.", parts, can))
        picked("parts", parts ? 1 : 0), set("parts", parts ? "on" : "off");
    end_card();

    begin_card(menu, "turnaround-spin", "SPIN");
    field(menu, "Speed", "How hard the stick is pushed to turn the board. Slower turns blur less; faster ones finish sooner.");
    setting_slider(menu, callbacks, can, "##turnaround-speed", t.speed, 10, 100, 5, "%d%%", "speed");
    field(menu, "Direction");
    int direction = shown("direction", t.spin_left ? 0 : 1);
    if (choice(menu, "turnaround-direction", direction, {"Left", "Right"}, can))
        picked("direction", direction), set("direction", direction == 0 ? "left" : "right");
    field(menu, "Turn time", "How long one whole turn takes to play. Making it takes about ten times as long: the board is stepped round and each step waits to settle.");
    int seconds = shown("seconds", t.seconds <= 2 ? 0 : t.seconds <= 3 ? 1 : t.seconds <= 4 ? 2 : t.seconds <= 6 ? 3 : 4);
    if (choice(menu, "turnaround-seconds", seconds, {"2 s", "3 s", "4 s", "6 s", "8 s"}, can))
        picked("seconds", seconds), set("seconds", std::array{"2", "3", "4", "6", "8"}[static_cast<std::size_t>(seconds)]);
    field(menu, "Frame rate", "Frames a second when it plays: smoother, and a longer turnaround to make.");
    int fps = shown("fps", t.fps <= 15 ? 0 : t.fps <= 24 ? 1 : 2);
    if (choice(menu, "turnaround-fps", fps, {"15", "24", "30"}, can))
        picked("fps", fps), set("fps", std::array{"15", "24", "30"}[static_cast<std::size_t>(fps)]);
    field(menu, "Size", "The board's square, in pixels. The parts card adds to the width.");
    int size = shown("size", t.size <= 256 ? 0 : t.size <= 384 ? 1 : t.size <= 512 ? 2 : t.size <= 768 ? 3 : 4);
    if (choice(menu, "turnaround-size", size, {"256", "384", "512", "768", "1024"}, can))
        picked("size", size), set("size", std::array{"256", "384", "512", "768", "1024"}[static_cast<std::size_t>(size)]);
    end_card();
}
} // namespace dingosdk::overlay::menu
