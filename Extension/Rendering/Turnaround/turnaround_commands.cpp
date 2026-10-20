#include "Extension/Console/commands.h"
#include "turnaround.h"
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
    arguments.complete = [](const Model&, auto) { return std::vector<std::string>{"export", "set", "open", "cancel", "grab", "boards", "parts", "stick"}; };
    auto command = action("turnaround",
                          "Board turnarounds (V on the Board screen): export [gif|apng|webm] [clear|matte] [frames] [size] [parts|noparts] [key=value], "
                          "set <key> <value> (format, background, matte, parts, speed, direction, frames, size, fps; kept), open (the folder), cancel. Research aids: grab (save this frame to Turnarounds\\), boards [seconds] "
                          "(find the board on show; spin it while this watches), parts (the items worn now), stick <x> <y> [seconds] (push the right "
                          "stick, -1..1). Answers go to the log.",
                          Group::console, {std::move(arguments)});
    command.run = [](const Model&, const Values& values, const Output& out) {
        std::istringstream words(values.empty() ? std::string{} : std::get<std::string>(values[0]));
        std::string verb;
        words >> verb;
        if (verb == "export") {
            // One-word settings for this and later turnarounds: gif, webm, matte, noparts, 72, 768...
            for (std::string word; words >> word;) {
                const auto equals = word.find('=');
                if (equals != std::string::npos) { turnaround::set_option(word.substr(0, equals), word.substr(equals + 1)); continue; }
                if (word == "gif" || word == "apng" || word == "png" || word == "webm") turnaround::set_option("format", word);
                else if (word == "clear" || word == "transparent" || word == "matte") turnaround::set_option("background", word);
                else if (word == "parts" || word == "noparts") turnaround::set_option("parts", word == "parts" ? "on" : "off");
                else if (std::uint32_t n{}; std::from_chars(word.data(), word.data() + word.size(), n).ec == std::errc{} && n)
                    turnaround::set_option(n <= 240 ? "frames" : "size", word);
            }
            out(turnaround::start_export());
        } else if (verb == "set") {
            std::string key, value;
            words >> key >> value;
            out(turnaround::set_option(key, value));
        } else if (verb == "open") {
            turnaround::open_folder();
            out("Opened the Turnarounds folder.");
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
            out("turnaround export [...] | set <key> <value> | open | cancel | grab | boards [seconds] | parts | stick <x> <y> [seconds]");
        }
    };
    registry.add(std::move(command));
}
} // namespace dingosdk::console
