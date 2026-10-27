// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace VideoCore {

/// bbport: what the texture collector does with an image old enough to go.
enum class GcAction {
    Evict,             ///< rebuilt from guest memory or the buffer cache when used again
    Keep,              ///< its contents exist only on the GPU and cannot be written back now
    WriteBackAndEvict, ///< downloaded to guest memory first
};

struct GcImageState {
    bool gpu_modified = false; ///< ImageFlagBits::GpuModified
    bool cpu_dirty = false;    ///< MaybeCpuDirty or CpuDirty
    bool gpu_dirty = false;    ///< GpuDirty: the GPU wrote its memory through buffers since
    bool tiled = false;
};

/// An image the GPU drew holds its only copy even when the CPU wrote to its pages since
/// (cpu_dirty): RefreshImage re-uploads only the mips whose guest bytes changed. Treating it like
/// a plain texture (Image::SafeToDownload is false then) dropped what the GPU drew; the image came
/// back from stale guest memory as blocks of garbage (Kaniel, 2026-10-11, 4K under VRAM pressure).
constexpr GcAction GcActionFor(const GcImageState& image, bool pressured) {
    if (!image.gpu_modified || image.gpu_dirty) {
        return GcAction::Evict;
    }
    // Tiled ones cannot be written back; CPU-touched ones would get old bytes over new ones.
    if (image.tiled || image.cpu_dirty || !pressured) {
        return GcAction::Keep;
    }
    return GcAction::WriteBackAndEvict;
}

/// What happens to an image that ends its mark of a buffer write (below).
enum class ImageEvent {
    Use,    ///< TouchImage: bound or sampled again
    Reload, ///< RefreshImage: rebuilt from guest memory
};

/// The mark a buffer bound for writing leaves on an image it overlaps (Image::gpu_hash): guest
/// memory's hash at that time, 0 for none. The collector writes such an image back only while
/// guest memory still matches it. shadPS4 #5504: only a reload ends the mark. A use used to, and
/// the collector then wrote the stale image back over what the buffer had written.
constexpr std::uint64_t BufferWriteMarkAfter(ImageEvent event, std::uint64_t gpu_hash) {
    return event == ImageEvent::Reload ? 0 : gpu_hash;
}

} // namespace VideoCore
