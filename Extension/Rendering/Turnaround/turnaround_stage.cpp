#include "turnaround_stage.h"

namespace dingosdk::turnaround {
namespace {
// Until the Board screen's own turning is found (`turnaround boards`, `turnaround stick`), the
// mod cannot turn the board, and says so.
class NotYet final : public Stage {
public:
    bool begin(std::string& why) override {
        why = "This build cannot turn the board yet: it is for finding out how (see the turnaround console commands).";
        return false;
    }
    void pose(double, bool) override {}
    bool changes_background() const override { return false; }
    void end() override {}
};
} // namespace

std::shared_ptr<Stage> board_stage() { return std::make_shared<NotYet>(); }
} // namespace dingosdk::turnaround
