#include "turnaround_matte.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dingosdk::turnaround {
Backgrounds measure_backgrounds(const PixelView& dark, const PixelView& light, Rect area) noexcept {
    // The median of each channel along the edge: a stray UI element on it does not shift it.
    Backgrounds out;
    if (area.empty()) return out;
    std::array<std::array<std::uint32_t, 256>, 6> counts{};
    std::uint64_t total = 0;
    const auto add = [&](std::uint32_t x, std::uint32_t y) {
        const auto d = dark.rgb(x, y), l = light.rgb(x, y);
        for (std::size_t c = 0; c < 3; ++c) ++counts[c][d[c]], ++counts[3 + c][l[c]];
        ++total;
    };
    const auto right = area.x + area.width - 1, bottom = area.y + area.height - 1;
    for (auto x = area.x; x <= right; ++x) add(x, area.y), add(x, bottom);
    for (auto y = area.y + 1; y < bottom; ++y) add(area.x, y), add(right, y);
    for (std::size_t k = 0; k < 6; ++k) {
        std::uint64_t seen = 0;
        std::size_t value = 0;
        while (value < 255 && (seen += counts[k][value]) * 2 < total) ++value;
        (k < 3 ? out.dark : out.light)[k % 3] = static_cast<float>(value);
    }
    return out;
}

Image matte_pair(const PixelView& dark, const PixelView& light, Rect area, const Backgrounds& backgrounds,
                 float floor) {
    Image out(area.width, area.height);
    std::array<float, 3> step{};
    float step_power = 0;
    for (int c = 0; c < 3; ++c) {
        step[c] = backgrounds.light[c] - backgrounds.dark[c];
        step_power += step[c] * step[c];
    }
    // Backgrounds that look the same tell nothing apart: everything is taken as opaque.
    const bool usable = step_power > 3 * 16.0f * 16.0f;
    floor = std::clamp(floor, 0.0f, 0.45f);
    for (std::uint32_t y = 0; y < area.height; ++y)
        for (std::uint32_t x = 0; x < area.width; ++x) {
            const auto d = dark.rgb(area.x + x, area.y + y), l = light.rgb(area.x + x, area.y + y);
            auto* p = out.at(x, y);
            float seen = 0; // how much of the background shows: 0 opaque .. 1 empty
            if (usable) {
                // Least squares over the three channels of (light - dark) = seen * step.
                float dot = 0;
                for (int c = 0; c < 3; ++c) dot += (static_cast<float>(l[c]) - d[c]) * step[c];
                seen = std::clamp((dot / step_power - floor) / (1.0f - 2.0f * floor), 0.0f, 1.0f);
            }
            const float alpha = 1.0f - seen;
            if (alpha <= 0.0f) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
            for (int c = 0; c < 3; ++c) {
                // The board's own colour, from both captures with their background taken away.
                const float a = d[c] - seen * backgrounds.dark[c], b = l[c] - seen * backgrounds.light[c];
                p[c] = static_cast<std::uint8_t>(std::clamp(std::lround((a + b) * 0.5f / alpha), 0L, 255L));
            }
            p[3] = static_cast<std::uint8_t>(std::lround(alpha * 255.0f));
        }
    return out;
}
// Without a background of its own to swap, the board is keyed out against the backdrop. The
// backdrop is found from the turn itself: a pixel the board leaves for much of the turn keeps one
// colour then, the backdrop's; one the board (nearly) always covers has no steady colour, and there
// the backdrop is taken from the row's ends, where the board never goes (the Board screen's
// backdrop is a smooth vignette). An exact backdrop lets the key be tight, so a black grip against
// the dark backdrop still reads as board. Matting (a stage that swaps the background) is better
// still; this is for when there is none.
void key_against_backdrop(std::vector<Image>& frames) {
    if (frames.empty()) return;
    const auto raw = frames;
    const auto w = frames.front().width, h = frames.front().height;
    constexpr std::uint32_t band = 6; // columns sampled at each end of a row
    if (w < band * 4) return;
    const auto luma = [](const std::uint8_t* p) { return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8; };
    Image plate(w, h);
    std::vector<int> values(frames.size());
    for (std::uint32_t y = 0; y < h; ++y) {
        // The row's ends, over every frame.
        std::array<std::vector<std::uint8_t>, 6> ends;
        for (const auto& frame : frames)
            for (std::uint32_t k = 0; k < band; ++k)
                for (std::size_t c = 0; c < 3; ++c) ends[c].push_back(frame.at(k, y)[c]), ends[3 + c].push_back(frame.at(w - 1 - k, y)[c]);
        std::array<float, 6> end_median{};
        for (std::size_t k = 0; k < 6; ++k) {
            std::nth_element(ends[k].begin(), ends[k].begin() + static_cast<std::ptrdiff_t>(ends[k].size() / 2), ends[k].end());
            end_median[k] = ends[k][ends[k].size() / 2];
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            for (std::size_t f = 0; f < frames.size(); ++f) values[f] = luma(frames[f].at(x, y));
            auto sorted = values;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
            const int middle = sorted[sorted.size() / 2];
            std::array<std::uint64_t, 3> sum{};
            std::size_t steady = 0;
            for (std::size_t f = 0; f < frames.size(); ++f)
                if (std::abs(values[f] - middle) <= 4) {
                    ++steady;
                    for (std::size_t c = 0; c < 3; ++c) sum[c] += frames[f].at(x, y)[c];
                }
            auto* b = plate.at(x, y);
            if (steady * 100 >= frames.size() * 35) {
                for (std::size_t c = 0; c < 3; ++c) b[c] = static_cast<std::uint8_t>(sum[c] / steady);
            } else {
                const float t = static_cast<float>(x) / static_cast<float>(w - 1);
                for (std::size_t c = 0; c < 3; ++c) b[c] = static_cast<std::uint8_t>(std::lround(end_median[c] + (end_median[3 + c] - end_median[c]) * t));
            }
        }
    }
    for (auto& frame : frames)
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                auto* p = frame.at(x, y);
                const auto* b = plate.at(x, y);
                int distance = 0;
                for (std::size_t c = 0; c < 3; ++c) distance = std::max(distance, std::abs(p[c] - b[c]));
                const float alpha = std::clamp((static_cast<float>(distance) - 5.0f) / 15.0f, 0.0f, 1.0f);
                if (alpha <= 0) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
                for (std::size_t c = 0; c < 3; ++c)
                    p[c] = static_cast<std::uint8_t>(std::clamp(std::lround((p[c] - (1 - alpha) * b[c]) / alpha), 0L, 255L));
                p[3] = static_cast<std::uint8_t>(std::lround(alpha * 255));
            }
    // A board is solid across: anything between its outermost keyed pixels in a row is board, even
    // where part of a graphic matched the backdrop and keyed out.
    for (std::size_t f = 0; f < frames.size(); ++f) {
        auto& frame = frames[f];
        for (std::uint32_t y = 0; y < h; ++y) {
            std::int64_t first = -1, last = -1;
            for (std::uint32_t x = 0; x < w; ++x)
                if (frame.at(x, y)[3] >= 128) { if (first < 0) first = x; last = x; }
            for (auto x = first + 1; first >= 0 && x < last; ++x) {
                auto* p = frame.at(static_cast<std::uint32_t>(x), y);
                if (p[3] == 255) continue;
                std::copy_n(raw[f].at(static_cast<std::uint32_t>(x), y), 3, p);
                p[3] = 255;
            }
        }
    }
}

} // namespace dingosdk::turnaround
