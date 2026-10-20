#include "Extension/Console/commands.h"
#include "turnaround.h"
#include "turnaround_research.h"
#include <charconv>
#include <sstream>

namespace dingosdk::turnaround {
namespace {
bool frame_grab_active() noexcept { return research::grab_requested(); }
bool frame_grab_wants() noexcept { return research::grab_requested(); }
void frame_grab_take(const overlay::GrabbedFrame& frame) noexcept { research::take_grab(frame); }
constexpr overlay::FrameGrabHost frame_grab_host{&frame_grab_active, &frame_grab_wants, &frame_grab_take};
} // namespace
const overlay::FrameGrabHost* frame_grab() noexcept { return &frame_grab_host; }
} // namespace dingosdk::turnaround

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
    arguments.complete = [](const Model&, auto) { return std::vector<std::string>{"grab", "boards", "parts", "stick"}; };
    auto command = action("turnaround",
                          "Board turnaround research aids: grab (save this frame to Turnarounds\\), boards [seconds] "
                          "(find the board on show; spin it while this watches), parts (the items worn now), stick <x> <y> [seconds] (push the right "
                          "stick, -1..1). Answers go to the log.",
                          Group::console, {std::move(arguments)});
    command.run = [](const Model&, const Values& values, const Output& out) {
        std::istringstream words(values.empty() ? std::string{} : std::get<std::string>(values[0]));
        std::string verb;
        words >> verb;
        if (verb == "grab") {
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
            out("turnaround grab | boards [seconds] | parts | stick <x> <y> [seconds]");
        }
    };
    registry.add(std::move(command));
}
} // namespace dingosdk::console
