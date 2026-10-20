#include "turnaround_stage.h"
#include "turnaround.h"
#include "Extension/UI/Overlay/overlay.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace dingosdk::turnaround {
namespace {
// The Board screen turns its board on the stick, as the player does: the turnaround holds the
// stick over for the whole turn, through a stand-in controller when none is connected
// (DingoSDKOverlaySetVirtualPad), and gives the gamepad filter back afterwards.
std::atomic<std::int16_t> spin_value{};
std::atomic<std::uint8_t> spin_axis{2};
void spin_filter(unsigned, overlay::GamepadValues& pad) noexcept {
    const auto v = spin_value.load();
    switch (spin_axis.load()) {
    case 0: pad.left_x = v; break;
    case 1: pad.left_y = v; break;
    case 3: pad.right_y = v; break;
    default: pad.right_x = v; break;
    }
}

class StickStage final : public Stage {
public:
    StickStage(float spin, std::uint8_t axis) : spin_(spin), axis_(axis) {}
    bool begin(std::string& why) override {
        if (std::abs(spin_) < 0.05f) {
            why = "The spin is set too gently to turn the board.";
            return false;
        }
        hold(1);
        spin_axis.store(axis_);
        previous_ = DingoSDKOverlayGamepadFilter();
        DingoSDKOverlaySetGamepadFilter(&spin_filter);
        DingoSDKOverlaySetVirtualPad(true);
        // The board's shadow on the backdrop would be cut out with it: shadows off while it turns.
        queue_console("turnaround shadows off");
        held_ = true;
        return true;
    }
    void pose(double, bool) override {}
    void hold(float strength) {
        const float value = std::clamp(spin_ * std::clamp(strength, 0.0f, 1.0f), -1.0f, 1.0f);
        spin_value.store(static_cast<std::int16_t>(std::lround(value * 32767)));
    }
    bool changes_background() const override { return false; }
    bool continuous() const override { return true; }
    void end() override {
        if (!held_) return;
        held_ = false;
        DingoSDKOverlaySetVirtualPad(false);
        DingoSDKOverlaySetGamepadFilter(previous_ == &spin_filter ? nullptr : previous_);
        queue_console("turnaround shadows restore");
    }
    ~StickStage() override { end(); }

private:
    float spin_;
    std::uint8_t axis_;
    overlay::GamepadFilter previous_{};
    bool held_{};
};
} // namespace

std::shared_ptr<Stage> board_stage(const Options& options) { return std::make_shared<StickStage>(options.spin, options.spin_axis); }
} // namespace dingosdk::turnaround
