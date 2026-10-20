#include "Extension/Console/commands.h"
#include "turnaround.h"
#include "turnaround_session.h"
#include "turnaround_research.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace dingosdk::console {
namespace {
double number(std::istringstream& words, double fallback) {
    std::string word;
    if (!(words >> word)) return fallback;
    double value{};
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    return error == std::errc{} && end == word.data() + word.size() ? value : fallback;
}
} // namespace

void register_turnaround_commands(Commands& registry) {
    auto arguments = argument("verb_and_arguments", Type::text, true);
    arguments.rest = true;
    arguments.complete = [](const Model&, auto) { return std::vector<std::string>{"export", "cancel", "grab", "boards", "parts", "stick"}; };
    auto command = action("turnaround",
                          "Board turnarounds: export [gif|apng|webm] [clear|matte] [frames] [size] [parts|noparts] [spin=0.5] [axis=rx|ry|lx|ly] "
                          "(on the Board screen), cancel. Research aids: grab (save this frame to Turnarounds\\), boards [seconds] "
                          "(find the board on show; spin it while this watches), parts (the items worn now), stick <x> <y> [seconds] (push the right "
                          "stick, -1..1). Answers go to the log.",
                          Group::console, {std::move(arguments)});
    command.run = [](const Model&, const Values& values, const Output& out) {
        std::istringstream words(values.empty() ? std::string{} : std::get<std::string>(values[0]));
        std::string verb;
        words >> verb;
        if (verb == "export") {
            auto& o = turnaround::options();
            for (std::string word; words >> word;) {
                if (word == "gif") o.format = turnaround::Format::gif;
                else if (word == "apng" || word == "png") o.format = turnaround::Format::apng;
                else if (word == "webm") o.format = turnaround::Format::webm;
                else if (word == "clear" || word == "transparent") o.transparent = true;
                else if (word == "matte") o.transparent = false;
                else if (word == "parts") o.parts = true;
                else if (word == "noparts") o.parts = false;
                else if (word.starts_with("spin=")) o.spin = static_cast<float>(std::clamp(std::strtod(word.c_str() + 5, nullptr), -1.0, 1.0));
                else if (word.starts_with("axis=")) {
                    const auto axis = word.substr(5);
                    o.spin_axis = axis == "lx" ? 0 : axis == "ly" ? 1 : axis == "ry" ? 3 : 2;
                }
                else if (std::uint32_t n{}; std::from_chars(word.data(), word.data() + word.size(), n).ec == std::errc{} && n) {
                    if (n <= 240) o.frames = n;
                    else o.size = n;
                }
            }
            out(turnaround::start_export());
        } else if (verb == "cancel") {
            turnaround::cancel_export();
            out(turnaround::exporting() ? "Cancelling the turnaround." : "No turnaround is being made.");
        } else if (verb == "grab") {
            turnaround::research::request_grab();
            out("The next frame will be saved in the Turnarounds folder beside Skate.exe.");
        } else if (verb == "boards") {
            out(turnaround::research::request_boards(number(words, 6)));
        } else if (verb == "parts") {
            out(turnaround::research::request_parts());
        } else if (verb == "stick") {
            const auto x = number(words, 0.5), y = number(words, 0), seconds = number(words, 2);
            out(turnaround::research::request_stick(x, y, seconds));
        } else {
            out("turnaround export [gif|apng|webm] [clear|matte] [frames] [size] [parts|noparts] | cancel | grab | boards [seconds] | parts | stick <x> <y> [seconds]");
        }
    };
    registry.add(std::move(command));
}
} // namespace dingosdk::console
