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
// Where the game shows what is turned, as fractions of its menu frame (the whole width up to 21:9, a
// 21:9 frame in the middle of anything wider). Measured from the game at 2000x837 (2026-10-20):
//  - the Board screen's board: right of the item card, below the search bar, left of Back;
//  - the Skaters screen's skater: between the Apparel and Tattoos columns, below the tab icons.
//    The page numbers under the skater's feet are in it, and are cleared as UI (keep_turning).
struct Area { float left, right, top, bottom; };
constexpr Area board_fractions{0.655f, 0.86f, 0.185f, 0.985f};
constexpr Area skater_fractions{0.27f, 0.735f, 0.14f, 0.985f};

Rect subject_area(Subject subject, std::uint32_t width, std::uint32_t height) {
    const auto& a = subject == Subject::skater ? skater_fractions : board_fractions;
    const auto menu_width = std::min<double>(width, height * 21.0 / 9.0), menu_left = (width - menu_width) / 2;
    const auto x0 = static_cast<std::uint32_t>(menu_left + menu_width * a.left);
    const auto x1 = static_cast<std::uint32_t>(menu_left + menu_width * a.right);
    const auto y0 = static_cast<std::uint32_t>(height * double{a.top}), y1 = static_cast<std::uint32_t>(height * double{a.bottom});
    return {x0, y0, std::min(x1, width) - x0, std::min(y1, height) - y0};
}
const char* noun(Subject subject) noexcept { return subject == Subject::skater ? "skater" : "board"; }
std::string screen_hint(Subject subject) {
    return subject == Subject::skater ? "Is the Skaters screen open, with the whole skater on show?"
                                      : "Is the Board screen open, with the board on show?";
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

// Keeps only what turned, and says where it is. What moves over the turn (opaque and unlike the
// first frame) is the subject; what is solid in any frame and joined to that is the rest of it (the
// nose and tail sit on the axis the board turns about and hardly change); everything else solid in
// the area is the game's UI, and is cleared from every frame. A rim two pixels wide is kept round it
// for the soft edges.
Rect keep_turning(std::vector<Image>& frames) {
    const auto& first = frames.front();
    const auto w = first.width, h = first.height;
    std::vector<std::uint8_t> moved(std::size_t{w} * h), solid(std::size_t{w} * h);
    for (const auto& frame : frames)
        for (std::size_t p = 0, i = 0; p < moved.size(); ++p, i += 4) {
            const auto alpha = frame.rgba[i + 3];
            if (alpha >= 64) solid[p] = 1;
            if (moved[p] || alpha < 16) continue;
            int change = 0;
            for (std::size_t c = 0; c < 4; ++c) change = std::max(change, std::abs(frame.rgba[i + c] - first.rgba[i + c]));
            if (change > 24) moved[p] = 1;
        }
    // Flood from what moved over what is solid.
    std::vector<std::uint8_t> kept(moved.size());
    std::vector<std::uint32_t> todo;
    for (std::uint32_t p = 0; p < moved.size(); ++p)
        if (moved[p]) kept[p] = 1, todo.push_back(p);
    while (!todo.empty()) {
        const auto p = todo.back();
        todo.pop_back();
        const auto x = static_cast<std::int64_t>(p % w), y = static_cast<std::int64_t>(p / w);
        for (std::int64_t dy = -1; dy <= 1; ++dy)
            for (std::int64_t dx = -1; dx <= 1; ++dx) {
                const auto nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const auto q = static_cast<std::uint32_t>(ny * w + nx);
                if (!kept[q] && solid[q]) kept[q] = 1, todo.push_back(q);
            }
    }
    // The rim.
    for (int pass = 0; pass < 2; ++pass) {
        auto grown = kept;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                if (!kept[std::size_t{y} * w + x]) continue;
                if (x > 0) grown[std::size_t{y} * w + x - 1] = 1;
                if (x + 1 < w) grown[std::size_t{y} * w + x + 1] = 1;
                if (y > 0) grown[std::size_t{y - 1} * w + x] = 1;
                if (y + 1 < h) grown[std::size_t{y + 1} * w + x] = 1;
            }
        kept = std::move(grown);
    }
    std::uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            if (kept[std::size_t{y} * w + x] && solid[std::size_t{y} * w + x])
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    for (auto& frame : frames)
        for (std::size_t p = 0; p < kept.size(); ++p)
            if (!kept[p]) std::fill_n(frame.rgba.data() + p * 4, 4, std::uint8_t{0});
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
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
    kept_at_.clear(), difference_.clear(), thumbnails_.clear();
    taken_ = 0, stride_ = 1, warmup_ = 45, period_ = 0;
    started_ = std::chrono::steady_clock::now();
    std::string why;
    bool began = false;
    try { began = in_.stage && in_.stage->begin(why); } catch (const std::exception& error) { why = error.what(); }
    if (!began) {
        finish(why.empty() ? std::string("The ") + noun(in_.options.subject) + " on show could not be taken over." : why, true);
        return true;
    }
    std::lock_guard lock(mutex_);
    progress_ = {Phase::capturing, 0, 0, std::string("Turning the ") + noun(in_.options.subject) + "...", {}};
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
    // However it goes, a turnaround gives the game back within a minute.
    if (std::chrono::steady_clock::now() - started_ > std::chrono::seconds(60)) {
        try { in_.stage->end(); } catch (...) {}
        finish("The turnaround took too long and was stopped. " + screen_hint(in_.options.subject), true);
        return false;
    }
    if (in_.stage->continuous()) return next_continuous();
    if (!posed_) {
        try {
            in_.stage->pose(static_cast<double>(angle_) / in_.options.frames, light_);
        } catch (...) {
            try { in_.stage->end(); } catch (...) {}
            finish(std::string("The ") + noun(in_.options.subject) + " could not be turned.", true);
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
            area_ = subject_area(in_.options.subject, frame.width, frame.height);
            if (area_.empty()) throw std::runtime_error("The screen is too small.");
            work_scale_ = area_.height > 1000 ? 2 : 1;
        }
        if (in_.stage->continuous()) {
            take_continuous(frame);
            return;
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

namespace {
// A 24x48 grey thumbnail of a frame's area, for telling how far round the board is and whether it
// has stopped.
std::vector<float> thumbnail(const PixelView& frame, Rect area) {
    constexpr std::uint32_t tw = 24, th = 48;
    std::vector<float> out(tw * th);
    for (std::uint32_t ty = 0; ty < th; ++ty)
        for (std::uint32_t tx = 0; tx < tw; ++tx) {
            const auto x0 = tx * area.width / tw, x1 = std::max(x0 + 1, (tx + 1) * area.width / tw);
            const auto y0 = ty * area.height / th, y1 = std::max(y0 + 1, (ty + 1) * area.height / th);
            float sum = 0;
            for (auto y = y0; y < y1; y += 2)
                for (auto x = x0; x < x1; x += 2) {
                    const auto rgb = frame.rgb(area.x + x, area.y + y);
                    sum += 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
                }
            out[ty * tw + tx] = sum / static_cast<float>(((x1 - x0 + 1) / 2) * ((y1 - y0 + 1) / 2));
        }
    return out;
}
float unlike(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) sum += std::abs(a[i] - b[i]);
    return sum / static_cast<float>(std::max<std::size_t>(a.size(), 1));
}
constexpr std::size_t most_kept = 240;       // pictures held at once, at most...
constexpr std::size_t kept_bytes = 700u << 20; // ...and in bytes
constexpr std::uint32_t longest_turn = 2400; // frames (40 s at 60) to measure a turn in
constexpr float moving = 6.0f;               // grey levels of change that say the board turns
} // namespace

// Come round: the last few views match the first few again, after clearly different ones. Matching
// a run of views, not one, keeps a board that started edge-on from "coming round" half way, when it
// is edge-on again but turning to its other face. The place of the best match (with a parabola
// through its neighbours), or 0.
double Session::come_round(const std::vector<float>& small) {
    constexpr std::size_t run = 4;
    thumbnails_.push_back(small);
    const auto n = thumbnails_.size();
    if (n < run) return 0;
    const auto p = n - run; // the candidate start of the second turn
    float sum = 0;
    for (std::size_t k = 0; k < run; ++k) sum += unlike(thumbnails_[p + k], thumbnails_[k]);
    difference_.push_back(sum / run); // difference_[p]
    const auto d = difference_.size();
    if (d < 12) return 0;
    const auto peak = *std::max_element(difference_.begin(), difference_.end());
    if (peak <= moving) return 0;
    const float a = difference_[d - 3], b = difference_[d - 2], c = difference_[d - 1];
    if (!(b < 0.2f * peak && b <= a && b < c)) return 0;
    const float curve = a - 2 * b + c;
    return static_cast<double>(d - 2) + (curve > 0 ? 0.5 * (a - c) / curve : 0.0);
}

// Render thread, every presented frame, for a continuous stage: whether this frame is wanted. Every
// one is, once the board is up to speed.
bool Session::next_continuous() {
    if (warmup_) { --warmup_; return false; }
    return true;
}

// A continuous stage turns the board at a steady speed: every frame of one whole turn is kept (every
// other one, and so on, past most_kept), and where the turn comes round decides which of them make
// the evenly spaced frames of the animation.
void Session::take_continuous(const PixelView& frame) {
    const auto small = thumbnail(frame, area_);
    const auto copy = copy_area(frame, area_);
    auto picture = opaque_image({copy.data(), area_.width, area_.height, area_.width * 4, frame.bgra});
    // Kept at about the size the board will be shown at (it fills most of the area's height), not
    // bigger: no detail is lost, and a whole turn fits in memory.
    const auto keep_height = std::clamp<std::uint32_t>(in_.options.size * 5 / 4, 480, picture.height);
    if (keep_height < picture.height)
        picture = resized(picture, std::max<std::uint32_t>(1, picture.width * keep_height / picture.height), keep_height);
    const auto most = std::clamp<std::size_t>(kept_bytes / std::max<std::size_t>(picture.rgba.size(), 1), 60, most_kept);
    const auto n = taken_++;
    if (n % stride_ == 0) {
        kept_at_.push_back(n);
        captured_.push_back(std::move(picture));
        if (captured_.size() > most) { // keep every other one, and from now on every other
            std::size_t w = 0;
            for (std::size_t r = 0; r < captured_.size(); r += 2) captured_[w] = std::move(captured_[r]), kept_at_[w++] = kept_at_[r];
            captured_.resize(w), kept_at_.resize(w);
            stride_ *= 2;
        }
    }
    {
        std::lock_guard lock(mutex_);
        progress_.step = taken_;
    }
    period_ = come_round(small);
    if (period_ <= 0) {
        const auto peak = difference_.empty() ? 0.0f : *std::max_element(difference_.begin(), difference_.end());
        if (taken_ > 240 && peak <= moving)
            throw std::runtime_error(std::string("The ") + noun(in_.options.subject) + " did not turn. " + screen_hint(in_.options.subject));
        if (taken_ >= longest_turn) throw std::runtime_error(std::string("The ") + noun(in_.options.subject) + " did not come all the way round.");
        return;
    }
    in_.stage->end();
    {
        std::lock_guard lock(mutex_);
        progress_.phase = Phase::encoding;
        progress_.message = "Saving the turnaround...";
    }
    std::thread([this] { encode(); }).detach();
}

void Session::encode() noexcept {
    try {
        auto frames = std::move(captured_);
        if (!in_.stage->changes_background()) key_against_backdrop(frames);
        if (in_.stage->continuous() && period_ > 0) {
            // The frames nearest to evenly spaced points of the one turn.
            std::vector<Image> even;
            for (std::uint32_t j = 0; j < in_.options.frames; ++j) {
                const double at = period_ * j / in_.options.frames;
                std::size_t best = 0;
                for (std::size_t k = 1; k < kept_at_.size(); ++k)
                    if (std::abs(kept_at_[k] - at) < std::abs(kept_at_[best] - at)) best = k;
                even.push_back(frames[best]);
            }
            frames = std::move(even);
        }
        const auto& o = in_.options;
        
        const auto through = keep_turning(frames);
        if (through.empty()) throw std::runtime_error(std::string("No ") + noun(o.subject) + " was found on screen.");
        const auto margin = std::max(through.width, through.height) / 25 + 2;
        std::optional<Image> card;
        if (o.parts && !in_.parts.empty()) card = render_parts_panel(o.subject == Subject::skater ? "Outfit" : "Board", in_.parts, o.size, in_.fonts);
        Animation animation;
        animation.fps = o.fps;
        for (auto& frame : frames) {
            auto shown = fitted(square_around(frame, through, margin), o.size);
            frame = {};
            if (card) shown = side_by_side(shown, *card);
            if (!o.transparent) shown = over_matte(shown, o.matte);
            animation.frames.push_back(std::move(shown));
        }
        std::vector<std::uint8_t> bytes;
        switch (o.format) {
        case Format::apng: bytes = encode_apng(animation); break;
        case Format::webm: bytes = encode_webm(animation, {.quality = 1, .alpha_quality = 0}); break; // the highest VP8 has
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
        if (name.empty()) name = noun(o.subject);
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
