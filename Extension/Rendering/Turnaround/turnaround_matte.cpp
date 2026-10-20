#include "turnaround_matte.h"
#include <cmath>

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
} // namespace dingosdk::turnaround
