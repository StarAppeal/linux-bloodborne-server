// SPDX-License-Identifier: GPL-2.0-or-later
// Kaniel's log of 2026-10-11 (RTX 4070 Ti, 4K output): at the first memory pressure report the
// collector evicted 5339 images, none written back, and magenta and white blocks covered the
// scene. A GPU-drawn image whose page the CPU also wrote (MaybeCpuDirty) looked like a plain
// texture to the collector: its contents, which exist only on the GPU (RefreshImage keeps the
// mips whose guest bytes did not change), were dropped and later rebuilt from stale memory.
#include <cassert>
#include "gpu/shadps4/video_core/texture_cache/gc_policy.h"
#include "gpu/shadps4/video_core/texture_cache/vram_placement.h"

using VideoCore::GcAction;
using VideoCore::GcActionFor;
using VideoCore::GcImageState;
using VideoCore::HeapUse;
using VideoCore::ImageEvent;
using VideoCore::ArenaReserveBytes;
using VideoCore::BufferWriteMarkAfter;
using VideoCore::ArenaSpillsOutsideVram;
using VideoCore::ObjectMotionFitsVram;
using VideoCore::SystemMemoryTypesNearVramLimit;
using VideoCore::TexturesPreferHost;
using VideoCore::VramPlacement;
using VideoCore::VramPlacementFrom;

int main() {
    // Where VRAM pressure goes (Supermedo 5f65e2d): by default textures stay in VRAM and game
    // memory has a reserve; "system" (bbport.ini vram_placement, BB_VRAM_PLACEMENT wins) is
    // #187's rule, new textures in system memory near the limit.
    assert(VramPlacementFrom(nullptr, std::nullopt) == VramPlacement::Reserve);
    assert(VramPlacementFrom(nullptr, std::string("system")) == VramPlacement::System);
    assert(VramPlacementFrom(nullptr, std::string("reserve")) == VramPlacement::Reserve);
    assert(VramPlacementFrom(nullptr, std::string("anything")) == VramPlacement::Reserve);
    assert(VramPlacementFrom("system", std::string("reserve")) == VramPlacement::System);
    assert(VramPlacementFrom("reserve", std::string("system")) == VramPlacement::Reserve);
    assert(VramPlacementFrom("", std::string("system")) == VramPlacement::System);
    // The reserve: 512 MiB, an eighth of smaller heaps, on discrete GPUs only, block aligned.
    constexpr std::uint64_t MB = 1ull << 20;
    assert(ArenaReserveBytes(true, false, 12288 * MB, 64 * 1024) == 512 * MB);
    assert(ArenaReserveBytes(true, false, 4096 * MB, 64 * 1024) == 512 * MB);
    assert(ArenaReserveBytes(true, false, 2048 * MB, 64 * 1024) == 256 * MB);
    assert(ArenaReserveBytes(true, false, 2048 * MB + 100, 64 * 1024) == 256 * MB);
    assert(ArenaReserveBytes(false, false, 12288 * MB, 64 * 1024) == 0);
    assert(ArenaReserveBytes(true, false, 0, 64 * 1024) == 0);
    // Only where the game's memory cannot spill outside VRAM (NVIDIA, Supermedo 859f9e4): AMD and
    // Intel take system memory for it first, and the RX 6500 XT report kept 478 MiB of its 4 GB
    // as a reserve the whole session, 0 used.
    assert(ArenaReserveBytes(true, true, 4096 * MB, 64 * 1024) == 0);
    {
        // NVIDIA: the arena binds to the VRAM types only (heap 0).
        const std::uint32_t heaps[] = {1, 0, 0, 1, 1};
        assert(!ArenaSpillsOutsideVram(0b00110, heaps, 0));
        // AMD: also to a host-visible type of heap 1.
        assert(ArenaSpillsOutsideVram(0b01010, heaps, 0));
        // Bits past the type count are ignored.
        assert(!ArenaSpillsOutsideVram(0b100010, heaps, 0));
    }
    // Object motion under 6 GB of VRAM (Supermedo d257021): its ~225 MiB go to textures on
    // discrete GPUs, unless BB_OBJECT_MOTION=1 asks for it.
    assert(ObjectMotionFitsVram(true, 8192 * MB, false));
    assert(ObjectMotionFitsVram(true, 6144 * MB - 200 * MB, false)); // a 6 GB card as reported
    assert(!ObjectMotionFitsVram(true, 4096 * MB, false));
    assert(ObjectMotionFitsVram(true, 4096 * MB, true));
    assert(ObjectMotionFitsVram(false, 2048 * MB, false)); // integrated: shared memory

    // Uploaded from guest memory only: rebuilt the same from it.
    assert(GcActionFor({}, false) == GcAction::Evict);
    assert(GcActionFor({}, true) == GcAction::Evict);
    assert(GcActionFor({.cpu_dirty = true, .tiled = true}, true) == GcAction::Evict);

    // GPU-drawn, tiled: cannot be written back, so it stays.
    assert(GcActionFor({.gpu_modified = true, .tiled = true}, true) == GcAction::Keep);
    // The case from the log: also touched by the CPU on its pages. Before the fix it went.
    assert(GcActionFor({.gpu_modified = true, .cpu_dirty = true, .tiled = true}, true) ==
           GcAction::Keep);
    assert(GcActionFor({.gpu_modified = true, .cpu_dirty = true, .tiled = true}, false) ==
           GcAction::Keep);
    // Linear and CPU-touched: a write-back would put old bytes over the CPU's new ones.
    assert(GcActionFor({.gpu_modified = true, .cpu_dirty = true}, true) == GcAction::Keep);

    // Linear, clean: written back under pressure, kept otherwise.
    assert(GcActionFor({.gpu_modified = true}, true) == GcAction::WriteBackAndEvict);
    assert(GcActionFor({.gpu_modified = true}, false) == GcAction::Keep);

    // The GPU wrote the memory through buffers since: the buffer cache holds the data the image
    // is rebuilt from, as RefreshImage does on its next use.
    assert(GcActionFor({.gpu_modified = true, .gpu_dirty = true, .tiled = true}, true) ==
           GcAction::Evict);
    assert(GcActionFor({.gpu_modified = true, .gpu_dirty = true}, true) == GcAction::Evict);

    // shadPS4 #5504: a buffer bound for writing over an image leaves guest memory's hash on it.
    // Using the image again keeps the mark, so the collector still checks guest memory before
    // writing the image back over what the buffer wrote; only a reload from guest memory ends it.
    assert(BufferWriteMarkAfter(ImageEvent::Use, 0x1234) == 0x1234);
    assert(BufferWriteMarkAfter(ImageEvent::Use, 0) == 0);
    assert(BufferWriteMarkAfter(ImageEvent::Reload, 0x1234) == 0);

    // krolebord's play test 2026-10-17 (RTX 4050 Laptop, 5920 MiB): 5834 MiB in use of a ~5150
    // MiB budget, the GPU stopped. NVIDIA: heap 0 VRAM, heap 1 system memory; types 0-1 in
    // system memory, 2-3 in VRAM.
    constexpr std::uint64_t MiB = 1ull << 20;
    const std::uint32_t type_heaps[] = {1, 1, 0, 0};
    const HeapUse full[] = {{.size = 5920 * MiB, .device_local = true, .usage = 5834 * MiB,
                             .budget = 5152 * MiB},
                            {.size = 16000 * MiB, .usage = 200 * MiB, .budget = 15000 * MiB}};
    // New images go to the system memory types.
    assert(SystemMemoryTypesNearVramLimit(true, full, type_heaps) == 0b0011);
    // Within the reserve (512 MiB) of the budget: already there.
    HeapUse close[] = {full[0], full[1]};
    close[0].usage = 4700 * MiB;
    assert(SystemMemoryTypesNearVramLimit(true, close, type_heaps) == 0b0011);
    // Room left: VRAM as before.
    close[0].usage = 4500 * MiB;
    assert(SystemMemoryTypesNearVramLimit(true, close, type_heaps) == 0);
    // Integrated GPUs (Steam Deck) keep their own rules.
    assert(SystemMemoryTypesNearVramLimit(false, full, type_heaps) == 0);
    // A 3 GB card keeps an eighth free (384 MiB; 512 would be too much of it).
    const HeapUse small[] = {{.size = 3072 * MiB, .device_local = true, .usage = 2700 * MiB,
                              .budget = 3100 * MiB},
                             full[1]};
    assert(SystemMemoryTypesNearVramLimit(true, small, type_heaps) == 0);

    // Supermedo a1235ee: a Ryzen 5600H (Radeon Vega, 15 GB RAM) reports the BIOS's 256 MiB frame
    // buffer as its only device-local heap, the rest is shared system memory. Textures filled the
    // small heap and a render target failed later (ErrorOutOfDeviceMemory). There textures prefer
    // the shared heap; discrete GPUs and bigger heaps are unchanged.
    const HeapUse apu[] = {{.size = 256 * MiB, .device_local = true}, {.size = 7800 * MiB}};
    assert(TexturesPreferHost(nullptr, apu));
    const HeapUse apu_512[] = {{.size = 512 * MiB, .device_local = true}, {.size = 7800 * MiB}};
    assert(TexturesPreferHost(nullptr, apu_512));
    const HeapUse one_gib[] = {{.size = 1024 * MiB, .device_local = true}, {.size = 7800 * MiB}};
    assert(!TexturesPreferHost(nullptr, one_gib));
    assert(!TexturesPreferHost(nullptr, full));
    // No shared heap beside it: nowhere else to go.
    const HeapUse local_only[] = {{.size = 256 * MiB, .device_local = true}};
    assert(!TexturesPreferHost(nullptr, local_only));
    // BB_IMAGES_PREFER_HOST=1 or 0 forces the rule; empty leaves it.
    assert(TexturesPreferHost("1", full));
    assert(!TexturesPreferHost("0", apu));
    assert(TexturesPreferHost("", apu));
    return 0;
}
