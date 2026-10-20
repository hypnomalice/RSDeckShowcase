#pragma once
#include "turnaround_image.h"
#include <optional>
#include <span>

// File formats for a board turnaround. Every frame of an animation is the same size.
namespace dingosdk::turnaround {
struct Animation {
    std::vector<Image> frames;
    std::uint32_t fps{30};
};

// A still PNG (RGBA).
std::vector<std::uint8_t> encode_png(const Image& image);
// An animated PNG that loops forever. Keeps full 8-bit alpha; flatten the frames over a matte
// (over_matte) first for an opaque one.
std::vector<std::uint8_t> encode_apng(const Animation& animation);

struct GifOptions {
    // GIF transparency is all or nothing. Pixels at least half opaque are kept, blended over this
    // colour so their soft edge does not go black; the rest are cut out. Pick something close to
    // the page the GIF will sit on.
    Rgb fringe{40, 40, 40};
    // A light, fixed (ordered) dither, which does not crawl between frames the way error diffusion does.
    bool dither{true};
};
// An animated GIF that loops forever: one 256-colour palette for every frame.
// GIF timing is in hundredths of a second, so the frame rate is kept on average, at most 50.
std::vector<std::uint8_t> encode_gif(const Animation& animation, const GifOptions& options = {});
} // namespace dingosdk::turnaround
