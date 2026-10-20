#include "turnaround_session.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>

namespace dingosdk::turnaround {
const char* extension(Format format) noexcept {
    switch (format) {
    case Format::apng: return "png";
    case Format::webm: return "webm";
    default: return "gif";
    }
}

namespace {
// Where the Board screen shows the board, as fractions of the game's menu frame (the whole width up
// to 21:9, a 21:9 frame in the middle of anything wider): right of the item card, below the search
// bar, left of the Back prompt. Measured from the game at 2000x837 (2026-10-20).
constexpr float area_left = 0.655f, area_right = 0.86f, area_top = 0.185f, area_bottom = 0.985f;

Rect board_area(std::uint32_t width, std::uint32_t height) {
    const auto menu_width = std::min<double>(width, height * 21.0 / 9.0), menu_left = (width - menu_width) / 2;
    const auto x0 = static_cast<std::uint32_t>(menu_left + menu_width * area_left);
    const auto x1 = static_cast<std::uint32_t>(menu_left + menu_width * area_right);
    const auto y0 = static_cast<std::uint32_t>(height * double{area_top}), y1 = static_cast<std::uint32_t>(height * double{area_bottom});
    return {x0, y0, std::min(x1, width) - x0, std::min(y1, height) - y0};
}

// `area` of a frame, copied out in the frame's own channel order.
std::vector<std::uint8_t> copy_area(const PixelView& frame, Rect area) {
    std::vector<std::uint8_t> out(std::size_t{area.width} * area.height * 4);
    for (std::uint32_t y = 0; y < area.height; ++y)
        std::copy_n(frame.data + std::size_t{area.y + y} * frame.pitch + std::size_t{area.x} * 4, std::size_t{area.width} * 4,
                    out.data() + std::size_t{y} * area.width * 4);
    return out;
}

Image opaque_image(const PixelView& view) {
    Image out(view.width, view.height);
    for (std::uint32_t y = 0; y < view.height; ++y)
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const auto rgb = view.rgb(x, y);
            auto* p = out.at(x, y);
            p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2], p[3] = 255;
        }
    return out;
}

// Without a background of its own to swap, the board is keyed out against the backdrop, which on
// the Board screen is a smooth vignette: each row's backdrop is taken from the area's left and right
// edges, where the board never goes, and blended across. Weaker than matting (a board as dark as the
// backdrop keys out with it), so it is only the fallback.
void key_against_edges(std::vector<Image>& frames) {
    if (frames.empty()) return;
    const auto w = frames.front().width, h = frames.front().height;
    constexpr std::uint32_t band = 6; // columns sampled at each edge
    if (w < band * 4) return;
    for (auto& frame : frames) {
        for (std::uint32_t y = 0; y < h; ++y) {
            std::array<std::array<std::uint8_t, band>, 3> left{}, right{};
            for (std::uint32_t k = 0; k < band; ++k)
                for (std::size_t c = 0; c < 3; ++c) left[c][k] = frame.at(k, y)[c], right[c][k] = frame.at(w - 1 - k, y)[c];
            std::array<float, 3> l{}, r{};
            for (std::size_t c = 0; c < 3; ++c) {
                std::nth_element(left[c].begin(), left[c].begin() + band / 2, left[c].end());
                std::nth_element(right[c].begin(), right[c].begin() + band / 2, right[c].end());
                l[c] = left[c][band / 2], r[c] = right[c][band / 2];
            }
            for (std::uint32_t x = 0; x < w; ++x) {
                auto* p = frame.at(x, y);
                const float t = static_cast<float>(x) / static_cast<float>(w - 1);
                std::array<float, 3> b{};
                float distance = 0;
                for (std::size_t c = 0; c < 3; ++c) {
                    b[c] = l[c] + (r[c] - l[c]) * t;
                    distance = std::max(distance, std::abs(p[c] - b[c]));
                }
                const float alpha = std::clamp((distance - 10.0f) / 30.0f, 0.0f, 1.0f);
                if (alpha <= 0) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
                for (std::size_t c = 0; c < 3; ++c)
                    p[c] = static_cast<std::uint8_t>(std::clamp(std::lround((p[c] - (1 - alpha) * b[c]) / alpha), 0L, 255L));
                p[3] = static_cast<std::uint8_t>(std::lround(alpha * 255));
            }
        }
    }
}

// The part of the area the board turned through: every pixel that is at all opaque and changes
// over the turn. The game's own UI stays the same, so it falls outside.
Rect turned_through(const std::vector<Image>& frames) {
    const auto& first = frames.front();
    Image moving(first.width, first.height);
    for (const auto& frame : frames)
        for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
            if (moving.rgba[i + 3] || frame.rgba[i + 3] < 16) continue;
            int change = 0;
            for (std::size_t c = 0; c < 4; ++c) change = std::max(change, std::abs(frame.rgba[i + c] - first.rgba[i + c]));
            if (change > 24) moving.rgba[i + 3] = 255;
        }
    return content_bounds(moving, 1);
}
} // namespace

bool Session::start(Inputs inputs) {
    if (busy_.exchange(true)) return false;
    // The render thread reads none of this until the phase below says it is capturing.
    cancelled_.store(false);
    in_ = std::move(inputs);
    in_.options.frames = std::clamp<std::uint32_t>(in_.options.frames, 8, 240);
    in_.options.size = std::clamp<std::uint32_t>(in_.options.size, 64, 2048);
    angle_ = 0, wait_ = 0, light_ = false, posed_ = false, area_ = {}, work_scale_ = 1;
    captured_.clear();
    dark_.clear();
    std::string why;
    bool began = false;
    try { began = in_.stage && in_.stage->begin(why); } catch (const std::exception& error) { why = error.what(); }
    if (!began) {
        finish(why.empty() ? "The board on show could not be taken over." : why, true);
        return true;
    }
    std::lock_guard lock(mutex_);
    progress_ = {Phase::capturing, 0, in_.options.frames, "Turning the board...", {}};
    return true;
}

void Session::cancel() noexcept { cancelled_.store(true); }

Progress Session::progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

bool Session::busy() const noexcept { return busy_.load(); }

void Session::finish(std::string message, bool failed) noexcept {
    try {
        std::lock_guard lock(mutex_);
        progress_.phase = failed ? Phase::failed : Phase::done;
        progress_.message = std::move(message);
    } catch (...) {}
    captured_.clear();
    captured_.shrink_to_fit();
    dark_.clear();
    busy_.store(false);
}

bool Session::wants_frame() noexcept {
    if (!busy_.load()) return false;
    {
        std::lock_guard lock(mutex_);
        if (progress_.phase != Phase::capturing) return false;
    }
    if (cancelled_.load()) {
        try { in_.stage->end(); } catch (...) {}
        finish("Turnaround cancelled.", true);
        return false;
    }
    if (!posed_) {
        try {
            in_.stage->pose(static_cast<double>(angle_) / in_.options.frames, light_);
        } catch (...) {
            try { in_.stage->end(); } catch (...) {}
            finish("The board could not be turned.", true);
            return false;
        }
        posed_ = true;
        wait_ = in_.options.settle;
        return false;
    }
    if (wait_) {
        --wait_;
        return false;
    }
    return true;
}

void Session::take_frame(const PixelView& frame) noexcept {
    try {
        if (!frame.data || !frame.width || !frame.height) throw std::runtime_error("The game's picture could not be read.");
        if (area_.empty()) {
            area_ = board_area(frame.width, frame.height);
            if (area_.empty()) throw std::runtime_error("The screen is too small.");
            work_scale_ = area_.height > 1000 ? 2 : 1;
        }
        const bool pairs = in_.stage->changes_background();
        Image picture;
        if (pairs && !light_) {
            dark_ = copy_area(frame, area_);
            light_ = true;
            posed_ = false;
            return;
        }
        if (pairs) {
            const auto light = copy_area(frame, area_);
            const PixelView d{dark_.data(), area_.width, area_.height, area_.width * 4, frame.bgra};
            const PixelView l{light.data(), area_.width, area_.height, area_.width * 4, frame.bgra};
            const Rect all{0, 0, area_.width, area_.height};
            picture = matte_pair(d, l, all, measure_backgrounds(d, l, all));
            light_ = false;
        } else {
            const auto copy = copy_area(frame, area_);
            picture = opaque_image({copy.data(), area_.width, area_.height, area_.width * 4, frame.bgra});
        }
        if (work_scale_ > 1) picture = fitted(picture, std::max(picture.width, picture.height) / work_scale_);
        captured_.push_back(std::move(picture));
        ++angle_;
        posed_ = false;
        {
            std::lock_guard lock(mutex_);
            progress_.step = angle_;
        }
        if (angle_ < in_.options.frames) return;
        in_.stage->end();
        {
            std::lock_guard lock(mutex_);
            progress_.phase = Phase::encoding;
            progress_.message = "Saving the turnaround...";
        }
        std::thread([this] { encode(); }).detach();
    } catch (const std::exception& error) {
        try { in_.stage->end(); } catch (...) {}
        finish(error.what(), true);
    } catch (...) {
        try { in_.stage->end(); } catch (...) {}
        finish("The turnaround failed.", true);
    }
}

void Session::encode() noexcept {
    try {
        auto frames = std::move(captured_);
        const auto& o = in_.options;
        if (!in_.stage->changes_background()) key_against_edges(frames);
        const auto through = turned_through(frames);
        if (through.empty()) throw std::runtime_error("No board was found on screen.");
        const auto margin = std::max(through.width, through.height) / 25 + 2;
        const auto square = padded_square(through, margin, frames.front().width, frames.front().height);
        std::optional<Image> card;
        if (o.parts && !in_.parts.empty()) card = render_parts_panel("Board", in_.parts, o.size, in_.fonts);
        Animation animation;
        animation.fps = o.fps;
        for (auto& frame : frames) {
            auto shown = fitted(cropped(frame, square), o.size);
            frame = {};
            if (card) shown = side_by_side(shown, *card);
            if (!o.transparent) shown = over_matte(shown, o.matte);
            animation.frames.push_back(std::move(shown));
        }
        std::vector<std::uint8_t> bytes;
        switch (o.format) {
        case Format::apng: bytes = encode_apng(animation); break;
        case Format::webm: bytes = encode_webm(animation); break;
        default: bytes = encode_gif(animation, {.fringe = o.matte, .dither = true}); break;
        }
        // The deck's name, kept to what a file name may hold.
        std::string name;
        for (const char c : in_.title) {
            const auto u = static_cast<unsigned char>(c);
            if (u >= 0x80 || std::isalnum(u)) name += c;
            else if ((c == ' ' || c == '-' || c == '_') && !name.empty() && name.back() != '-') name += '-';
        }
        while (!name.empty() && name.back() == '-') name.pop_back();
        if (name.empty()) name = "board";
        const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::filesystem::create_directories(in_.folder);
        const auto u8name = name + "-" + std::to_string(stamp) + "." + extension(o.format);
        const auto path = in_.folder / std::filesystem::path(std::u8string(u8name.begin(), u8name.end()));
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("The file could not be written.");
        {
            std::lock_guard lock(mutex_);
            progress_.file = path;
        }
        finish("Saved " + u8name + " in the Turnarounds folder.", false);
    } catch (const std::exception& error) {
        finish(std::string("The turnaround could not be saved: ") + error.what(), true);
    } catch (...) {
        finish("The turnaround could not be saved.", true);
    }
}
} // namespace dingosdk::turnaround
