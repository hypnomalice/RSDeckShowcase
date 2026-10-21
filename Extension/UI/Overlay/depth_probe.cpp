#include "overlay_internal.h"
#include "Engine/Core/Log/logging.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// Research aid for Fit Check (`fitcheck depth`): where the game keeps its depth while it draws a
// frame, for cutting the board or skater out exactly. Each time a depth buffer the size of the
// screen (or near it) stops being written to over the next two frames (a transition barrier out of
// DEPTH_WRITE), a copy of it is put on that same command list, just before the barrier; a few frames
// later the copies are read back and written beside the fit checks as 16-bit PGM pictures (near
// and far stretched to black and white), with a log of what each was.
//
// ID3D12GraphicsCommandList::ResourceBarrier (slot 26) is hooked once, when the overlay starts
// (through its own command list: every list of the runtime shares it). Unarmed, the hook only
// passes the call on.
namespace dingosdk::overlay {
namespace {
using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
std::atomic<ResourceBarrierFn> original_barrier{};

struct Snapshot {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    D3D12_RESOURCE_DESC desc{};
    D3D12_RESOURCE_STATES after{};
    std::uintptr_t resource{};
    unsigned frame{}, order{};
};
struct Probe {
    std::mutex mutex;
    std::atomic<int> armed{};     // presents left in the window (frames recorded then are probed)
    std::atomic<int> settle{};    // presents to wait after the window before reading back
    std::atomic<bool> requested{};
    std::filesystem::path folder;
    std::uint32_t screen_width{}, screen_height{};
    unsigned frame{}, order{};
    std::vector<Snapshot> snapshots;
    unsigned barriers{}, depth_barriers{}, skipped_small{}, skipped_type{};
    std::vector<std::string> notes; // each depth barrier, and why one was not copied (the first few)
    std::vector<std::string> seen; // each depth resource's description, once
};
Probe& probe() { static auto* value = new Probe; return *value; }

bool depth_capable(const D3D12_RESOURCE_DESC& d) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && d.SampleDesc.Count == 1;
}

void STDMETHODCALLTYPE hooked_barrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    const auto original = original_barrier.load();
    auto& p = probe();
    if (p.armed.load(std::memory_order_relaxed) > 0 && list && barriers) {
        try {
            std::lock_guard lock(p.mutex);
            p.barriers += count;
            for (UINT i = 0; i < count; ++i) {
                const auto& b = barriers[i];
                if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) continue;
                if (!(b.Transition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE)) continue;
                const auto desc = b.Transition.pResource->GetDesc();
                if (!depth_capable(desc)) continue;
                ++p.depth_barriers;
                const auto key = reinterpret_cast<std::uintptr_t>(b.Transition.pResource);
                const auto line = std::format("{:#x} {}x{} format {} mips {} array {} flags {:#x}", key, desc.Width, desc.Height,
                                              static_cast<unsigned>(desc.Format), desc.MipLevels, desc.DepthOrArraySize,
                                              static_cast<unsigned>(desc.Flags));
                if (std::find(p.seen.begin(), p.seen.end(), line) == p.seen.end() && p.seen.size() < 64) p.seen.push_back(line);
                // Only near the screen's size, on a direct list, whole or its first subresource.
                const auto note = [&](const char* what) {
                    if (p.notes.size() < 80)
                        p.notes.push_back(std::format("frame {} {:#x} sub {:#x} {:#x}->{:#x} flags {:#x}: {}", p.frame, key, b.Transition.Subresource,
                                                      static_cast<unsigned>(b.Transition.StateBefore), static_cast<unsigned>(b.Transition.StateAfter),
                                                      static_cast<unsigned>(b.Flags), what));
                };
                if (desc.Width * 3 < std::uint64_t{p.screen_width} || desc.Height * 3 < p.screen_height) { ++p.skipped_small; continue; }
                if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) { ++p.skipped_type; continue; }
                // A split barrier's start leaves the buffer as it is until then (copied before it); its
                // end finds it already on its way.
                if (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) { note("end of a split barrier, not copied"); continue; }
                if (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0) { note("another plane, not copied"); continue; }
                if (p.snapshots.size() >= 16) { note("enough copies"); continue; }
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) { note("no device"); continue; }
                Snapshot snap;
                UINT rows{};
                UINT64 row_bytes{}, total{};
                device->GetCopyableFootprints(&desc, 0, 1, 0, &snap.layout, &rows, &row_bytes, &total);
                if (!total) { note("no copyable footprint"); continue; }
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bytes{};
                bytes.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bytes.Width = total;
                bytes.Height = 1;
                bytes.DepthOrArraySize = 1;
                bytes.MipLevels = 1;
                bytes.SampleDesc.Count = 1;
                bytes.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&snap.readback))))
                    { note("readback buffer not made"); continue; }
                // DEPTH_WRITE -> COPY_SOURCE, the copy, and back, before the game's own barrier.
                D3D12_RESOURCE_BARRIER ours[2]{};
                ours[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                ours[0].Transition.pResource = b.Transition.pResource;
                ours[0].Transition.Subresource = b.Transition.Subresource;
                ours[0].Transition.StateBefore = b.Transition.StateBefore;
                ours[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                ours[1] = ours[0];
                std::swap(ours[1].Transition.StateBefore, ours[1].Transition.StateAfter);
                original(list, 1, &ours[0]);
                D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                to.pResource = snap.readback.Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = snap.layout;
                from.pResource = b.Transition.pResource;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.SubresourceIndex = 0;
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                original(list, 1, &ours[1]);
                snap.desc = desc;
                snap.after = b.Transition.StateAfter;
                snap.resource = key;
                snap.frame = p.frame;
                snap.order = p.order++;
                p.snapshots.push_back(std::move(snap));
                note("copied");
            }
        } catch (...) {}
    }
    original(list, count, barriers);
}

// A depth value as 0..1 from a copied row: floats as they are, 24-bit and 16-bit as fractions.
double depth_at(const std::uint8_t* row, std::uint32_t x, DXGI_FORMAT format, std::uint32_t bytes) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: {
        float v;
        std::memcpy(&v, row + std::size_t{x} * 4, 4);
        return v;
    }
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: {
        std::uint32_t v;
        std::memcpy(&v, row + std::size_t{x} * 4, 4);
        return (v & 0xffffff) / 16777215.0;
    }
    default:
        if (bytes == 2) {
            std::uint16_t v;
            std::memcpy(&v, row + std::size_t{x} * 2, 2);
            return v / 65535.0;
        }
        float v;
        std::memcpy(&v, row + std::size_t{x} * 4, 4);
        return v;
    }
}

void write_snapshots(detail::State& s) {
    auto& p = probe();
    std::vector<Snapshot> snapshots;
    std::vector<std::string> seen, notes;
    unsigned barriers, depth_barriers, too_small, other_list;
    {
        std::lock_guard lock(p.mutex);
        snapshots.swap(p.snapshots);
        seen.swap(p.seen);
        notes.swap(p.notes);
        barriers = p.barriers, depth_barriers = p.depth_barriers, too_small = p.skipped_small, other_list = p.skipped_type;
    }
    std::error_code error;
    std::filesystem::create_directories(p.folder, error);
    std::ofstream log(p.folder / L"depth-probe.txt", std::ios::trunc);
    log << std::format("Screen {}x{}. Barriers seen: {}; out of DEPTH_WRITE on a depth texture: {} ({} too small, {} not on a direct list).\n",
                       p.screen_width, p.screen_height, barriers, depth_barriers, too_small, other_list);
    log << "Depth textures seen:\n";
    for (const auto& line : seen) log << "  " << line << "\n";
    log << "Depth barriers:\n";
    for (const auto& line : notes) log << "  " << line << "\n";
    // The copies are on the game's lists, submitted by now; this makes sure they are done.
    const UINT64 signal = ++s.next_fence;
    const bool done = SUCCEEDED(s.queue->Signal(s.fence.Get(), signal)) && detail::completed(signal, 5000);
    if (!done) log << "The GPU did not finish in time; the pictures may be empty.\n";
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto& snap = snapshots[i];
        const auto w = static_cast<std::uint32_t>(snap.desc.Width), h = snap.desc.Height;
        const auto format = snap.layout.Footprint.Format;
        const auto bytes = w ? static_cast<std::uint32_t>(snap.layout.Footprint.RowPitch / std::max<std::uint32_t>(w, 1)) : 4;
        void* mapped{};
        const D3D12_RANGE range{0, static_cast<SIZE_T>(snap.layout.Footprint.RowPitch) * h};
        if (FAILED(snap.readback->Map(0, &range, &mapped)) || !mapped) {
            log << std::format("#{}: could not be read back.\n", i);
            continue;
        }
        const auto* base = static_cast<const std::uint8_t*>(mapped) + snap.layout.Offset;
        std::vector<double> values(std::size_t{w} * h);
        double low = 1e30, high = -1e30;
        std::size_t zeros = 0, ones = 0;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto v = depth_at(base + std::size_t{y} * snap.layout.Footprint.RowPitch, x, format, bytes);
                values[std::size_t{y} * w + x] = v;
                if (v <= 0.0) ++zeros;
                else if (v >= 1.0) ++ones;
                else low = std::min(low, v), high = std::max(high, v);
            }
        const D3D12_RANGE nothing{};
        snap.readback->Unmap(0, &nothing);
        if (high < low) low = 0, high = 1;
        const auto name = std::format("depth-{:02}-frame{}-{}x{}.pgm", i, snap.frame, w, h);
        log << std::format("#{} {}: resource {:#x}, footprint format {}, then state {:#x}, order {}; values {:.6f}..{:.6f}, {} at 0, {} at 1.\n",
                           i, name, snap.resource, static_cast<unsigned>(format), static_cast<unsigned>(snap.after), snap.order, low, high, zeros,
                           ones);
        std::ofstream picture(p.folder / name, std::ios::binary | std::ios::trunc);
        picture << "P5\n" << w << " " << h << "\n65535\n";
        std::vector<std::uint8_t> row(std::size_t{w} * 2);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto v = std::clamp((values[std::size_t{y} * w + x] - low) / std::max(high - low, 1e-12), 0.0, 1.0);
                const auto q = static_cast<std::uint16_t>(std::lround(v * 65535));
                row[std::size_t{x} * 2] = static_cast<std::uint8_t>(q >> 8), row[std::size_t{x} * 2 + 1] = static_cast<std::uint8_t>(q);
            }
            picture.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
    }
    log << std::format("{} picture(s).\n", snapshots.size());
    logging::printf(logging::Level::info, logging::Channel::graphics, "Fit Check depth probe: %zu depth picture(s) and depth-probe.txt written.",
                    snapshots.size());
}
} // namespace

void request_depth_probe(const wchar_t* folder) noexcept {
    try {
        auto& p = probe();
        std::lock_guard lock(p.mutex);
        p.folder = folder;
        p.requested.store(true);
    } catch (...) {}
}

namespace detail {
void install_depth_probe(ID3D12GraphicsCommandList* list) noexcept {
    static std::atomic<bool> installed{};
    if (!list || installed.exchange(true)) return;
    auto* const table = *reinterpret_cast<void***>(list);
    void* original{};
    if (hook_prepare(table[26], reinterpret_cast<void*>(&hooked_barrier), &original) != HookOk) return;
    original_barrier.store(reinterpret_cast<ResourceBarrierFn>(original));
    if (hook_enable(table[26]) != HookOk)
        logging::printf(logging::Level::warning, logging::Channel::graphics, "Fit Check depth probe: the barrier hook could not start.");
}

bool depth_probe_pending() {
    auto& p = probe();
    return p.requested.load() || p.armed.load() > 0 || p.settle.load() > 0;
}

void depth_probe_present(State& s, std::uint32_t width, std::uint32_t height) {
    auto& p = probe();
    if (!original_barrier.load()) {
        if (p.requested.exchange(false))
            logging::printf(logging::Level::warning, logging::Channel::graphics, "Fit Check depth probe: no barrier hook, nothing to probe.");
        return;
    }
    if (p.requested.exchange(false)) {
        std::lock_guard lock(p.mutex);
        p.screen_width = width, p.screen_height = height;
        p.snapshots.clear(), p.seen.clear(), p.notes.clear();
        p.barriers = p.depth_barriers = p.skipped_small = p.skipped_type = 0;
        p.frame = 0, p.order = 0;
        p.armed.store(3); // this frame's recording onwards, two whole frames
        return;
    }
    if (p.armed.load() > 0) {
        std::lock_guard lock(p.mutex);
        ++p.frame;
        if (p.armed.fetch_sub(1) == 1) p.settle.store(4);
        return;
    }
    if (p.settle.load() > 0 && p.settle.fetch_sub(1) == 1) {
        try { write_snapshots(s); } catch (...) {}
    }
}
} // namespace detail
} // namespace dingosdk::overlay
