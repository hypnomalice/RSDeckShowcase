#pragma once

namespace dingosdk::overlay { struct FrameGrabHost; }
// Board turnarounds: a spinning picture of the board on the Board screen, saved as a GIF, APNG or
// WebM, optionally with a card of the parts it is made of.
namespace dingosdk::turnaround {
// What the overlay asks before drawing each frame (overlay::set_frame_grab); process lifetime.
const overlay::FrameGrabHost* frame_grab() noexcept;
} // namespace dingosdk::turnaround
