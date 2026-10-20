#pragma once
#include "turnaround_encode.h"
#include "turnaround_matte.h"
#include "turnaround_panel.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// One board turnaround, from the press of the button to the saved file: turns the board on show a
// step at a time through a whole turn, grabs each step's frame (twice, over a dark and a light
// background, when it is to be see-through), cuts the board out, and saves the animation.
// Driven by the presented frames (on_present / wants_frame / take_frame, render thread); the
// encoding runs on a thread of its own. The game side is a Stage, so this file knows nothing of
// the game.
namespace dingosdk::turnaround {
enum class Format { gif, apng, webm };
const char* extension(Format format) noexcept;

struct Options {
    Format format{Format::gif};
    bool transparent{true};    // no background; otherwise the board over `matte`
    Rgb matte{24, 25, 28};
    std::uint32_t frames{48};  // pictures in one whole turn
    std::uint32_t fps{24};
    std::uint32_t size{512};   // the board's square, in pixels; the parts card adds to the width
    bool parts{true};          // the "parts used" card beside the board
    std::uint32_t settle{6};   // frames to wait after each change, for the game's temporal smoothing
    float spin{0.5f};          // how hard the stick is pushed to turn the board (-1..1)
    std::uint8_t spin_axis{2}; // which stick axis: 0 left x, 1 left y, 2 right x, 3 right y
};

// What the game does for a turnaround. Called on the render thread; an implementation that must act
// on the game's own thread queues the work and is given time (Options::settle frames) to apply it.
class Stage {
public:
    virtual ~Stage() = default;
    // Takes the board on show over: true, or false with the reason.
    virtual bool begin(std::string& why) = 0;
    // Turns it to `turn` (0..1 of a whole turn from where it started), over the dark or the light
    // background. A stage that cannot change the background ignores `light`.
    virtual void pose(double turn, bool light) = 0;
    virtual bool changes_background() const = 0;
    // Gives it back as it was.
    virtual void end() = 0;
    // A stage that turns the board on its own (as the player's stick would), at a speed it does not
    // know, rather than to a pose: begun, it turns the board at full strength; hold() changes how
    // hard (0 stops it). The session measures a whole turn, then steps the board round, taking each
    // step once it has come to rest, so no frame carries the game's motion trails.
    virtual bool continuous() const { return false; }
    virtual void hold(float /*strength, 0..1*/) {}
};

enum class Phase { idle, capturing, encoding, done, failed };
struct Progress {
    Phase phase{Phase::idle};
    std::uint32_t step{}, steps{};
    std::string message; // for the notice
    std::filesystem::path file;
};

class Session {
public:
    struct Inputs {
        Options options;
        std::string title;             // the deck's name, for the file name
        std::vector<Part> parts;
        PanelFonts fonts;              // must outlive the session
        std::filesystem::path folder;  // where the file goes
        std::shared_ptr<Stage> stage;
    };
    // Starts one; false (and Progress says why) while another is still running.
    bool start(Inputs inputs);
    void cancel() noexcept;
    Progress progress() const;
    bool busy() const noexcept;
    // Render thread, every presented frame: whether this frame's pixels are wanted.
    bool wants_frame() noexcept;
    void take_frame(const PixelView& frame) noexcept;

private:
    void finish(std::string message, bool failed) noexcept;
    void encode() noexcept;

    mutable std::mutex mutex_;
    Progress progress_;
    std::atomic<bool> busy_{false}, cancelled_{false};
    // Render thread state.
    Inputs in_;
    std::uint32_t angle_{}, wait_{};
    bool light_{};
    bool posed_{};
    Rect area_;              // where on screen the board can be
    std::uint32_t work_scale_{1};
    std::vector<std::uint8_t> dark_; // the dark capture of this angle, waiting for its light one
    std::uint32_t dark_pitch_{};
    std::vector<Image> captured_;
    // Continuous stages (see Stage::continuous): measuring the turn, then stepping it.
    enum class Turn { measure, resting, push, settling };
    Turn turn_{};
    std::vector<double> kept_at_;          // each kept picture's step
    std::vector<float> difference_;        // how unlike the first each frame (measuring) or step looks
    std::vector<std::vector<float>> thumbnails_; // every frame (measuring) or step, for coming round
    std::vector<float> last_thumbnail_;
    std::uint32_t taken_{}, stride_{1}, warmup_{};
    std::uint32_t still_{}, waited_{}, push_left_{}, steps_{}, steps_target_{};
    double period_{};                      // frames (measuring), then steps, in one whole turn
    double push_per_step_{}, push_owed_{};
    float strength_{1};
    void take_continuous(const PixelView& frame);
    bool next_continuous();
    double come_round(const std::vector<float>& small);
};
} // namespace dingosdk::turnaround
