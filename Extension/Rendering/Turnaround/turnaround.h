#pragma once
#include <string>

namespace dingosdk::overlay { struct FrameGrabHost; }
// Board turnarounds: a spinning picture of the board on the Board screen, saved as a GIF, APNG or
// WebM, optionally with a card of the parts it is made of.
namespace dingosdk::turnaround {
struct Options;
// What the overlay asks before drawing each frame (overlay::set_frame_grab); process lifetime.
const overlay::FrameGrabHost* frame_grab() noexcept;
// The options the next turnaround is made with (console thread and overlay; set before starting).
Options& options() noexcept;
// Starts one with options(), of the board worn now; what to tell the player.
std::string start_export();
void cancel_export() noexcept;
bool exporting() noexcept;
} // namespace dingosdk::turnaround
