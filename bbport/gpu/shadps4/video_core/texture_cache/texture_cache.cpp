// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <boost/container/small_vector.hpp>
#include <xxhash.h>

#include "bbport_toggles.h"
#include "bbport_free_check.h"
#include "bbport_memory_hash.h"
#include "bbport_portraits.h"
#include "bbport_prompts.h"
#include "bbport_settings.h"
#include "../bbcoop.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/gc_policy.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u32 MAX_IMAGES = std::numeric_limits<u16>::max();
static constexpr u32 MAX_IMAGE_VIEWS = std::numeric_limits<u16>::max();
static constexpr u32 MAX_SAMPLERS = std::numeric_limits<u16>::max();

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& page_manager_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, page_manager{page_manager_}, slot_images{MAX_IMAGES},
      slot_image_views{MAX_IMAGE_VIEWS}, slot_samplers{MAX_SAMPLERS},
      blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    const s64 min_pressure_floor =
        std::clamp<s64>(device_local_memory / 4, 256_MB, DEFAULT_PRESSURE_GC_MEMORY);
    const s64 min_critical_floor =
        std::clamp<s64>(device_local_memory / 2, 512_MB, DEFAULT_CRITICAL_GC_MEMORY);
    pressure_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      min_pressure_floor));
    critical_gc_memory = static_cast<u64>(
        std::max<s64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      min_critical_floor));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

std::string TextureCache::DescribeImagesIn(VAddr addr, u64 size) {
    std::unique_lock lock{mutex, std::try_to_lock}; // the caller may hold it already
    if (!lock.owns_lock()) {
        return " [images: not looked up]";
    }
    std::string out;
    int shown = 0;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (shown++ < 3) {
            out += fmt::format(" [image {:#x}+{:#x} {}x{} fmt {} tile {}{}{}]", image.info.guest_address,
                               image.info.guest_size, image.info.size.width,
                               image.info.size.height, u32(image.info.pixel_format),
                               u32(image.info.tile_mode), image.usage.render_target ? " RT" : "",
                               True(image.flags & ImageFlagBits::GpuModified) ? " gpu" : "");
        }
    });
    if (shown > 3) {
        out += fmt::format(" +{} more", shown - 3);
    }
    return out;
}

void TextureCache::ProcessDownloadImages() {
    // bbport: called for every fence (~450 a frame): the lock only when something is queued.
    if (!downloads_queued.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    downloads_queued.store(false, std::memory_order_relaxed);
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, DownloadMemoryFlags::Sync);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, DownloadMemoryFlags flags) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const auto image_addr = image.info.guest_address;
    const auto image_mips = image.info.resources.levels;
    const auto is_sync = True(flags & DownloadMemoryFlags::Sync);
    const auto is_priority = True(flags & DownloadMemoryFlags::Priority);
    const auto discard_buffer_cache = True(flags & DownloadMemoryFlags::DiscardBufferCache);
    ASSERT(!(is_sync && is_priority));

    u32 copy_size = 0;
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image_mips; ++mip) {
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> mip, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[mip];
        const u32 extent_width = mip_pitch ? std::min<u32>(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min<u32>(mip_height, height) : height;
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
        copy_size += mip_size;
    }
    if (buffer_copies.empty()) {
        return;
    }
    ASSERT(copy_size <= image.info.guest_size);

    // bbport BB_GUEST_IN_PLACE: the GPU copies the image into the game's memory itself, in stream
    // order: before the fences that follow, no wait here and no CPU copy afterwards.
    // Where the memory has a VRAM copy the GPU copies into that copy as well: buffers bound over
    // it read the image's data in stream order (issue #122: motion blur read a render target as a
    // buffer, 0x104ff00000, from a VRAM copy refreshed only once the CPU copy below landed, frames
    // later: the frame smeared while the camera turned). The game's memory still gets it below.
    if (GuestInPlace() && image_addr != 0) {
        const auto [arena, offset] =
            buffer_cache.ObtainBuffer(image_addr, copy_size, ObtainBufferFlags::IsWritten);
        auto arena_copies = buffer_copies;
        tile_manager.TileImage(image, arena_copies, arena, offset);
        if (buffer_cache.IsInPlace(image_addr, copy_size)) {
            return;
        }
    }

    const auto download =
        runtime.GetStagingPool().Request(copy_size, MemoryType::HostCached, 16, !is_sync);

    tile_manager.TileImage(image, buffer_copies, download.buffer, download.offset);

    const auto write_data = [this, image_addr, copy_size, download, discard_buffer_cache, is_sync] {
        download.Invalidate();
        BbFreeCheck::Check(image_addr, copy_size, download.mapped, BbFreeCheck::ImageDownload);
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image_addr), download.mapped,
                                                  copy_size);
        if (!WriteTracking()) {
            buffer_cache.InvalidateMemory(image_addr, copy_size);
        }
        if (discard_buffer_cache) {
            buffer_cache.UnmarkRegionAsGpuModified(image_addr, copy_size, true);
        }
        if (!is_sync) {
            runtime.GetStagingPool().FreeDeferred(download);
        }
    };

    if (is_priority) {
        scheduler.DeferPriorityOperation(std::move(write_data));
    } else if (is_sync) {
        scheduler.Finish();
        write_data();
    } else {
        scheduler.DeferOperation(std::move(write_data));
    }
}

/// bbport co-op: the screen's height in pixels (the HUD is drawn at the output's size).
static int PortraitOutputHeight() {
    return BbSettings::CurrentOutput().height;
}

bool TextureCache::RefreshPortraitSlot(Image& image) {
    const auto format = image.info.pixel_format;
    const bool rgba = format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    // The HUD's placeholders are BbPortraits::Side square; an older HUD's (a mod layer's) 64.
    const int side = int(image.info.size.width);
    if ((side != BbPortraits::Side && side != 64) || image.info.size.height != u32(side) ||
        image.info.resources.layers != 1 || (!rgba && !bgra) || image.info.guest_address == 0) {
        image.portrait_slot = -1;
        return false;
    }
    std::vector<u8> level0(BbPortraits::MipOffset(1, side));
    runtime_memory_read_backing(image.info.guest_address + image.info.mips_layout[0].offset,
                                level0.data(), level0.size());
    const int slot = BbPortraits::PlaceholderSlot(level0, side);
    image.portrait_slot = slot;
    if (slot < 0) {
        return false;
    }
    std::vector<u8> picture(size_t(BbPortraits::PictureSide) * BbPortraits::PictureSide * 4);
    u32 version = 0;
    const bool shown = bbcoop_portrait_slot(slot, picture.data(), picture.size(), &version) != 0;
    image.portrait_version = version;
    // Kaniel 2026-10-17 ("really poor quality"): the picture as big as the screen shows it, so
    // that the texture's one level never shimmers (FitToScreen).
    image.portrait_output = PortraitOutputHeight();
    const auto fitted =
        shown ? BbPortraits::FitToScreen(picture, BbPortraits::PictureSide, side,
                                         BbPortraits::ShownPixels(image.portrait_output))
              : std::vector<u8>{};
    const int levels = int(image.info.resources.levels);
    const auto chain = BbPortraits::MipChain(fitted, bgra, levels, side);
    const auto staging =
        runtime.GetStagingPool().Request(chain.size(), MemoryType::HostUncached, 16);
    std::memcpy(staging.mapped, chain.data(), chain.size());
    staging.Flush();
    boost::container::small_vector<vk::BufferImageCopy, 14> copies;
    for (int m = 0; m < levels; ++m) {
        const u32 mip_side = std::max(u32(side) >> m, 1u);
        copies.push_back({
            .bufferOffset = staging.offset + BbPortraits::MipOffset(m, side),
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                              .mipLevel = u32(m),
                              .baseArrayLayer = 0,
                              .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip_side, mip_side, 1},
        });
    }
    scheduler.EndRendering();
    runtime.UploadImage(&image, staging.buffer, copies);
    image.flags &= ~ImageFlagBits::Dirty;
    LOG_INFO(Render_Vulkan,
             "Party portraits: slot {} texture {:#x} ({}, {} px, {} levels) shows {} (version {}, "
             "{} px on the screen)",
             slot, image.info.guest_address, vk::to_string(format), side, levels,
             shown ? "the member's picture" : "nothing", version,
             BbPortraits::ShownPixels(image.portrait_output));
    return true;
}

void TextureCache::NotePortraitTexture(Image& image) {
    const auto format = image.info.pixel_format;
    if (image.info.size.width == BbPortraits::ModelWidth &&
        image.info.size.height == BbPortraits::ModelHeight &&
        (format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb) &&
        True(image.flags & ImageFlagBits::GpuModified)) {
        CaptureStatusModel(image, format == vk::Format::eB8G8R8A8Srgb);
    }
}

void TextureCache::CaptureStatusModel(Image& image, bool bgra) {
    // One read back every half second while the page shows its model (the party code keeps the
    // last one of a showing).
    static std::atomic<s64> captured_at{0};
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = captured_at.load(std::memory_order_relaxed);
    if (now - last < 500 || !captured_at.compare_exchange_strong(last, now)) {
        return;
    }
    const u32 width = image.info.size.width, height = image.info.size.height;
    const u32 size = width * height * 4;
    const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16, true);
    const vk::BufferImageCopy copy = {
        .bufferOffset = download.offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
    scheduler.DeferPriorityOperation([this, download, size, width, height, bgra] {
        download.Invalidate();
        std::vector<u8> rgba(download.mapped, download.mapped + size);
        if (bgra) {
            for (size_t at = 0; at < rgba.size(); at += 4) {
                std::swap(rgba[at], rgba[at + 2]);
            }
        }
        bbcoop_portrait_model(rgba.data(), int(width), int(height));
        runtime.GetStagingPool().FreeDeferred(download);
    });
}

/// bbport: the guest memory a MaybeCpuDirty check compares, the same at marking and at
/// refresh (they hashed different ranges, the whole image and its first 8x8 pixels, so the
/// first check never matched). Such an image lies within the faulting page: cheap to hash
/// whole, and a CPU write past its first pixels still counts.
u64 TextureCache::MaybeDirtyHash(const Image& image) {
    return BbMemory::HashBacking(image.info.guest_address, image.info.guest_size);
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.cpu_hash == 0) {
        // Initialize hash
        image.cpu_hash = MaybeDirtyHash(image);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);

    SmallVector<ImageId, 8> image_ids;
    ForEachImageInRegion(pages_start, pages_end - pages_start,
                         [&](ImageId image_id, Image&) { image_ids.push_back(image_id); });

    for (const auto image_id : image_ids) {
        Image& image = slot_images[image_id];
        std::scoped_lock lk{image.mutex};

        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            PageManager::NoteImageFault(image.info.guest_address, image.info.guest_size,
                                        image.info.size.width, image.info.size.height,
                                        u32(image.info.pixel_format), u32(image.info.tile_mode));
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    }
}

// bbport BB_IMAGE_WATCH=addr (diagnostics): how the image at that address is used (first 300).
void TextureCache::WatchImage(const Image& image, const char* what) {
    static const u64 watched = [] {
        const char* env = std::getenv("BB_IMAGE_WATCH");
        return env ? std::strtoull(env, nullptr, 0) : 0ull;
    }();
    if (!watched || image.info.guest_address != watched) {
        return;
    }
    static std::atomic<u32> printed{0};
    if (printed.fetch_add(1) < 300) {
        std::printf("Image watch %#llx: %s, flags %#x\n", (unsigned long long)watched, what,
                    u32(image.flags));
    }
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    // bbport: only an image starting at `address` is affected, so the images over its first byte
    // are all there is to look at (the whole range was walked for every GPU write binding).
    ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
        static const bool log = std::getenv("BB_IMAGE_SYNC_LOG") != nullptr;
        if (log) {
            static std::atomic<u32> printed{0};
            if (printed.fetch_add(1) < 200) {
                std::printf("Image sync: GPU write %#llx+%#llx over image %#llx %ux%u fmt %u flags %#x%s\n",
                            (unsigned long long)address, (unsigned long long)max_size,
                            (unsigned long long)image.info.guest_address, image.info.size.width,
                            image.info.size.height, u32(image.info.pixel_format), u32(image.flags),
                            image.info.guest_address == address ? "" : " (not at its start: kept)");
            }
        }
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        WatchImage(image, "GPU write (marked for refresh)");
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::MarkAsMaybeGpuDirty(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(addr, size, [&](ImageId image_id, Image& image) {
        if (image.gpu_hash == 0) {
            // Read backing pages directly, without faulting through the guest readback path.
            // An unavailable baseline keeps the conservative GC behavior.
            image.gpu_hash = BbMemory::HashBacking(image.info.guest_address, image.info.guest_size);
        }
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lock{mutex};
    SmallVector<ImageId, 16> deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        new_info.UpdateSize();
        const auto new_image_id = InsertImage(new_info);
        RegisterImage(new_image_id);

        // bbport: insert() may have grown slot_images and moved every image: `cache_image` would
        // then point at freed storage, so look it up again (Supermedo's bbport 3a7cde83).
        auto& old_image = slot_images[cache_image_id];

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = old_image.usage;
        if (new_info.num_samples == 1 &&
            (new_info.resources.layers > old_image.info.resources.layers ||
             new_info.resources.levels > old_image.info.resources.levels)) {
            RefreshImage(new_image);
        }
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&old_image, &new_image);

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    static constexpr u64 NUM_FRAMES_BEFORE_REMOVAL = 32;

    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NUM_FRAMES_BEFORE_REMOVAL;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // If we don't have an allocated image, we create one here
                // Otherwise it's later created from guest memory and the GPU data is lost
                // And reads/writes go to different images.
                if (!merged_image_id) {
                    merged_image_id =
                        slot_images.Insert(instance, runtime, slot_image_views, image_info);
                    RegisterImage(merged_image_id);
                    RefreshImage(slot_images[merged_image_id]);
                    TrackImage(merged_image_id);
                }
                auto& merged_image = slot_images[merged_image_id];
                runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                FreeImage(cache_image_id);
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = InsertImage(info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    // Only carry over the old contents when they hold GPU-written data that guest memory lacks.
    // A CPU-dirty source is stale (e.g. a streamed texture rewritten in a reused pool slot);
    // copying it would overwrite the fresh upload with old data.
    const bool src_has_gpu_data = src_image.HasGpuValidData();

    RefreshImage(new_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    if (src_has_gpu_data) {
        // As upstream (#5484): the copy carries GpuModified over.
        runtime.CopyImage(&src_image, &new_image);
    }
    FreeImage(image_id);

    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;

    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format;
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level = cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer = cached.view_slice;
        }
        return cached.image_id;
    }

    SmallVector<ImageId, 8> image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = InsertImage(info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    SmallVector<ImageId, 4> image_ids;
    ForEachImageWithAddress(address, [&](ImageId image_id, Image& image) {
        if (ensure_valid && !image.HasGpuValidData()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        Image& image = slot_images[image_ids[0]];
        TouchImage(image);
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                TouchImage(image);
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    WatchImage(image, refresh ? "texture (refresh)" : "texture (no refresh)");
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        image.gpu_write_tick = scheduler.CurrentTick();
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
            downloads_queued.store(true, std::memory_order_release);
        }
    }
    // bbport co-op: a portrait slot whose picture (or the screen's size) changed is uploaded again.
    if (image.portrait_slot >= 0 &&
        (bbcoop_portrait_version(image.portrait_slot) != image.portrait_version ||
         PortraitOutputHeight() != image.portrait_output)) {
        std::scoped_lock lock{mutex};
        image.flags |= ImageFlagBits::CpuDirty;
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (desc.type != BindingType::Storage) {
        NotePortraitTexture(image);
    }
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

void TextureCache::RegisterMeta(VAddr address, MetaDataInfo info) {
    const auto it = surface_metas.find(address);
    if (it == surface_metas.end()) {
        surface_metas.emplace(address, info);
        return;
    }

    auto& registered = it->second;
    if (registered.type != info.type || registered.owner_uid != info.owner_uid) {
        registered = info;
        return;
    }

    // Preserve the current clear state when the same render target is rebound.
    registered.owner_id = info.owner_id;
    registered.num_samples = info.num_samples;
}

void TextureCache::UnregisterMeta(VAddr address, u64 owner_uid) {
    const auto it = surface_metas.find(address);
    if (it != surface_metas.end() && it->second.owner_uid == owner_uid) {
        surface_metas.erase(address);
    }
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "render target");
    image.flags |= ImageFlagBits::GpuModified;
    image.gpu_write_tick = scheduler.CurrentTick();
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
        downloads_queued.store(true, std::memory_order_release);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);
    // bbport: the off-screen status model (the party portrait) is read back as it is drawn.
    if (bbcoop_portrait_offscreen_wanted() != 0) {
        NotePortraitTexture(image);
    }

    // Register meta data for this color buffer
    const VAddr cmask_addr = desc.info.meta_info.cmask_addr;
    if (image.info.meta_info.cmask_addr != cmask_addr) {
        UnregisterMeta(image.info.meta_info.cmask_addr, image.image_uid);
        image.info.meta_info.cmask_addr = cmask_addr;
    }
    if (cmask_addr) {
        RegisterMeta(cmask_addr, MetaDataInfo{
                                     .type = MetaType::CMask,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                 });
    }

    const VAddr fmask_addr = desc.info.meta_info.fmask_addr;
    if (image.info.meta_info.fmask_addr != fmask_addr) {
        UnregisterMeta(image.info.meta_info.fmask_addr, image.image_uid);
        image.info.meta_info.fmask_addr = fmask_addr;
    }
    if (fmask_addr) {
        RegisterMeta(fmask_addr, MetaDataInfo{
                                     .type = MetaType::FMask,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                 });
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "depth target");
    image.flags |= ImageFlagBits::GpuModified;
    image.gpu_write_tick = scheduler.CurrentTick();
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    const VAddr htile_addr = desc.info.meta_info.htile_addr;
    if (image.info.meta_info.htile_addr != htile_addr) {
        UnregisterMeta(image.info.meta_info.htile_addr, image.image_uid);
        image.info.meta_info.htile_addr = htile_addr;
    }
    if (htile_addr) {
        RegisterMeta(htile_addr, MetaDataInfo{
                                     .type = MetaType::HTile,
                                     .owner_id = image_id,
                                     .owner_uid = image.image_uid,
                                     .num_samples = image.info.num_samples,
                                     .clear_mask = image.info.meta_info.htile_clear_mask,
                                 });
    }

    AssociateStencil(image_id, desc.info);

    return slot_images[image_id].FindView(desc.view_info, false);
}

void TextureCache::AssociateStencil(ImageId depth_id, const ImageInfo& depth_info) {
    if (depth_info.stencil_addr == 0 || depth_info.stencil_addr == depth_info.guest_address) {
        return;
    }
    ImageId stencil_id{};
    ForEachImageInRegion(
        depth_info.stencil_addr, depth_info.stencil_size, [&](ImageId image_id, Image& image) {
            if (image.info.guest_address != depth_info.stencil_addr) {
                return;
            }
            if (image.info.pixel_format == vk::Format::eUndefined ||
                Vulkan::LiverpoolToVK::IsFormatStencilCompatible(image.info.pixel_format)) {
                stencil_id = image_id;
            }
        });
    if (!stencil_id) {
        ImageInfo info{};
        info.guest_address = depth_info.stencil_addr;
        info.guest_size = depth_info.stencil_size;
        info.size = depth_info.size;
        stencil_id = InsertImage(info);
        RegisterImage(stencil_id);
    }
    Image& stencil_image = slot_images[stencil_id];
    TouchImage(stencil_image);
    stencil_image.AssociateDepth(depth_id, slot_images[depth_id].image_uid);
}

void TextureCache::RefreshImage(Image& image) {
    WatchImage(image, "refresh entered");
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        const u64 hash = MaybeDirtyHash(image);
        if (image.cpu_hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.cpu_hash = hash;
    }
    image.gpu_hash = BufferWriteMarkAfter(ImageEvent::Reload, image.gpu_hash);

    if (RefreshPortraitSlot(image)) {
        return;
    }
    // bbport: the game's button pictures, for icons that follow the device in use
    // (bbport_prompts.cpp, which writes them from the window thread).
    BbPrompts::OnUpload(image.info.guest_address, image.info.mips_layout[0].size,
                        image.info.size.width, image.info.size.height, image.info.resources.levels,
                        image.info.num_bits);

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    static const bool sync_log = std::getenv("BB_IMAGE_SYNC_LOG") != nullptr;
    if (sync_log && is_gpu_dirty) {
        static std::atomic<u32> printed{0};
        if (printed.fetch_add(1) < 100) {
            std::printf("Image sync: refresh of %#llx (%ux%u) from memory the GPU wrote, flags %#x\n",
                        (unsigned long long)image.info.guest_address, image.info.size.width,
                        image.info.size.height, u32(image.flags));
        }
    }

    if (sync_log) {
        // BB_IMAGE_SYNC_LOG: the images refreshed most per 5 s (any reason), with their flags.
        struct Refreshes {
            u64 count = 0, bytes = 0;
            u32 width = 0, height = 0, format = 0, tiling = 0, flags = 0;
        };
        static std::mutex refresh_mutex;
        static std::unordered_map<VAddr, Refreshes> refreshes;
        static auto window_start = std::chrono::steady_clock::now();
        std::scoped_lock lk{refresh_mutex};
        auto& r = refreshes[image.info.guest_address];
        r = {r.count + 1,
             r.bytes + image.info.guest_size,
             image.info.size.width,
             image.info.size.height,
             u32(image.info.pixel_format),
             u32(image.info.tile_mode),
             r.flags | u32(image.flags)};
        if (const auto now = std::chrono::steady_clock::now();
            now - window_start >= std::chrono::seconds(5)) {
            std::vector<std::pair<VAddr, Refreshes>> top(refreshes.begin(), refreshes.end());
            std::ranges::sort(top, std::greater{}, [](const auto& e) { return e.second.bytes; });
            std::string text;
            for (size_t i = 0; i < std::min<size_t>(top.size(), 5); ++i) {
                const auto& [address, e] = top[i];
                text += fmt::format(" {:#x} {}x{} fmt {} tile {}: {}x {:.0f} MB flags {:#x};",
                                    address, e.width, e.height, e.format, e.tiling, e.count,
                                    e.bytes / 1e6, e.flags);
            }
            std::printf("Image refreshes (5 s, %zu images):%s\n", refreshes.size(), text.c_str());
            refreshes.clear();
            window_start = now;
        }
    }
    BbStats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    SmallVector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        // bbport: every upload records the guest memory it saw, not only uploads of images
        // the GPU had already written. An image the GPU writes after an upload from the buffer
        // cache (GpuDirty: a storage image's first binding) or from a plain texture otherwise
        // had no reference, and the first CPU write anywhere in its page replaced the GPU's
        // contents with stale guest memory (a 1x1 exposure texture computed once: the
        // character creation preview went black after one frame).
        // Only for images the GPU writes or may write (small ones, targets, storage images): a
        // streamed texture hashed here cost the GPU thread its whole size again on every upload
        // (Supermedo's bbport 136a72d0).
        const bool track = (is_gpu_modified && !is_gpu_dirty) || mip_size <= 64_KB ||
                           image.usage.storage || image.usage.render_target ||
                           image.usage.depth_target;
        if (track) {
            const u64 mip_hash =
                BbMemory::HashBacking(image.info.guest_address + mip_offset, mip_size);
            if (is_gpu_modified && !is_gpu_dirty && image.mip_hashes[m] == mip_hash) {
                continue;
            }
            image.mip_hashes[m] = mip_hash;
        }

        const u32 extent_width = mip_pitch ? std::min<u32>(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min<u32>(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    WatchImage(image, image_copies.empty() ? "refresh: nothing to copy" : "refresh: uploading");
    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sharp,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sharp, sizeof(sharp)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    // bbport: the draw-preparation workers ask for samplers concurrently.
    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash);
    if (new_sampler) {
        it->second =
            slot_samplers.Insert(instance, sharp, border_color_base, is_depth, extra_lod_bias);
        Sampler& sampler = slot_samplers[it->second];
        sampler.hash = hash;
        sampler_lru_cache.Insert(sampler, gc_tick);
    }

    Sampler& sampler = slot_samplers[it->second];
    sampler_lru_cache.Touch(sampler, gc_tick);
    return sampler.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    image.memory_usage_counter = &total_used_memory;
    total_used_memory += Common::AlignUp(image.SizeBytes(), 1024);
    BbStats::live_image_bytes.fetch_add(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_add(1, std::memory_order_relaxed);
    image_lru_cache.Insert(image, gc_tick);
    image.lru_touched_tick = gc_tick;
    const auto& info = image.info;
    ASSERT_MSG((info.guest_address & 0xff) == 0, "Trying to register an unaligned image");
    ForEachPage(info.guest_address, info.guest_size, [this, image_id, info](u64 page) {
        page_table[page].entries.emplace_back(BucketEntry{
            .key = u32(info.guest_address >> 8),
            .size = info.guest_size,
            .id = image_id,
        });
    });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    image.memory_usage_counter = nullptr;
    image_lru_cache.Free(image);
    total_used_memory -= Common::AlignUp(image.SizeBytes(), 1024);
    BbStats::live_image_bytes.fetch_sub(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_sub(1, std::memory_order_relaxed);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        ASSERT_MSG(page_it, "Unregistering unregistered page={:#x}", page << Traits::PAGE_BITS);
        auto& entries = page_it->entries;
        const auto vector_it = std::ranges::find(entries, image_id, &BucketEntry::id);
        ASSERT_MSG(vector_it != entries.end(), "Unregistering unregistered image in page={:#x}",
                   page << Traits::PAGE_BITS);
        entries.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    std::scoped_lock lk{image.mutex};
    if (image.IsUntracked()) {
        page_manager.UpdatePageWatchers(image_begin, image.info.guest_size, PageOp::Track);
    } else {
        if (image_begin < image.track_addr) {
            page_manager.UpdatePageWatchers(image_begin, image.track_addr - image_begin,
                                            PageOp::Track);
        }
        if (image.track_addr_end < image_end) {
            page_manager.UpdatePageWatchers(image.track_addr_end, image_end - image.track_addr_end,
                                            PageOp::Track);
        }
    }
    image.track_addr = image_begin;
    image.track_addr_end = image_end;
    MarkDirtyIfEdgesModified(image);
    image.head_hash = 0;
    image.tail_hash = 0;
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (image.IsUntracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    if (size != 0) {
        page_manager.UpdatePageWatchers(addr, size, PageOp::Untrack);
    }
    image.track_addr = 0;
    image.track_addr_end = 0;
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (image.IsUntracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = page_manager.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.head_hash = BbMemory::HashBacking(image_begin, size);
    page_manager.UpdatePageWatchers(image_begin, size, PageOp::Untrack);

    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image.IsUntracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = page_manager.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.tail_hash = BbMemory::HashBacking(addr, size);
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    page_manager.UpdatePageWatchers(addr, size, PageOp::Untrack);
}

void TextureCache::MarkDirtyIfEdgesModified(Image& image) {
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image.head_hash != 0) {
        const auto head_end = page_manager.GetNextPageAddr(image_begin);
        if (BbMemory::HashBacking(image_begin, head_end - image_begin) != image.head_hash) {
            image.flags |= ImageFlagBits::CpuDirty;
        }
    }
    if (image.tail_hash != 0) {
        const auto tail_begin = page_manager.GetPageAddr(image_end);
        if (BbMemory::HashBacking(tail_begin, image_end - tail_begin) != image.tail_hash) {
            image.flags |= ImageFlagBits::CpuDirty;
        }
    }
}

void TextureCache::GarbageCollectImages() {
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        // bbport: the buffer cache's VRAM reserve is not in use until VRAM is full: counted, it
        // put cards half a gigabyte closer to the marks than textures make them (a02c38f).
        total_used_memory -= std::min(total_used_memory, buffer_cache.UnusedVramReserve());
        // bbport: on integrated GPUs (Steam Deck) the usage covers system-memory heaps holding
        // much more than images (buffers backing guest memory), and the startup budget left
        // ~1 GB after its 8 GB system reserve: usage stayed above the critical mark, so the
        // collector evicted images used two or three frames ago on every submission and wrote
        // GPU-written ones back. Compare with the driver's current budget instead.
        // BB_GC_BUDGET_MB=N: this rule with a fixed budget on any GPU (tests on a desktop).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        // bbport: on Windows the desktop and other programs hold 1.3-1.7 GB of a discrete card
        // before the game starts (deadinside28/bloodborne_pc#39): limits from the startup heap
        // size let a 6 GB card run out of memory with nothing evicted. WDDM's live budget
        // already leaves out what others hold, so it is used on every Windows GPU.
#ifdef _WIN32
        constexpr bool live_budget = true;
#else
        constexpr bool live_budget = false;
#endif
        if (live_budget || instance.IsIntegrated() || forced_budget) {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                trigger_gc_memory = budget / 10 * 7;
                pressure_gc_memory = budget / 100 * 85;
                critical_gc_memory = budget / 100 * 95;
            }
        }
    }
    BbStats::gc_used_bytes.store(total_used_memory, std::memory_order_relaxed);
    BbStats::gc_trigger_bytes.store(trigger_gc_memory, std::memory_order_relaxed);
    BbStats::gc_critical_bytes.store(critical_gc_memory, std::memory_order_relaxed);
    // bbport: gc_tick (one per guest submission, hundreds a second) at each of the last 64
    // seconds, for ages in seconds. The usage compared is all of our VRAM, not only images: on a
    // discrete GPU it stays over the trigger, where an age of 16 ticks (~3 frames) evicted every
    // texture briefly off screen and uploaded it again. Below the pressure mark an image goes
    // once unused for BB_GC_IDLE_SECONDS (20): the textures of an area left behind.
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    if (second != gc_second) {
        gc_second = second;
        // bbport: the driver's budget for our process, once a second: near it the collector evicts
        // by submissions (as before 0.5) rather than keeping textures for seconds (NoteVramShort).
        if (instance.CanReportMemoryUsage() && !instance.IsIntegrated()) {
            const u64 budget = instance.GetDeviceMemoryBudgetNow();
            if (budget != 0 && total_used_memory >= budget / 100 * 92 &&
                vram_short_until.load(std::memory_order_relaxed) < second + 2) {
                vram_short_until.store(second + 2, std::memory_order_relaxed);
            }
        }
        BbStats::coarse_second.store(u32(second), std::memory_order_relaxed);
        gc_tick_at_second[second % gc_tick_at_second.size()] = gc_tick;
    }
    if (BbToggle::Disabled(BbToggle::TextureCollector)) {
        return;
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    static const u64 idle_seconds = [] {
        const char* env = std::getenv("BB_GC_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 20, 1, 63);
    }();
    // Under pressure an image goes once unused for BB_GC_PRESSURE_IDLE_SECONDS (5), over the
    // critical mark for 1 s first, and only then by submissions as before (80/160 ticks, a
    // fraction of a second). On cards up to 8 GB the pressure mark is 40 % of the budget: the
    // usage stayed over it, so textures off screen for a moment were evicted and uploaded again
    // when the camera turned back (9000 a minute with a 4.5 GB budget), read over the bus each
    // time: NVIDIA testers saw the frame rate fall the longer they played.
    static const u64 pressure_idle_seconds = [] {
        const char* env = std::getenv("BB_GC_PRESSURE_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 5, 0, 63);
    }();
    // The tick at the start of that second (an older one where no submission came then).
    const auto tick_seconds_ago = [&](u64 seconds) {
        return gc_tick_at_second[(second - seconds) % gc_tick_at_second.size()];
    };
    const u64 idle_tick = tick_seconds_ago(idle_seconds);
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 below_tick = 0;
    size_t num_deletions = 0;
    u32 visited = 0;

    // emergency: over the critical mark after evicting what was unused for a second.
    const auto configure = [&](bool allow_aggressive, bool emergency = false) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        const u64 ticks_to_destroy = std::min<u64>(aggresive ? 160 : pressured ? 80 : 16, gc_tick);
        below_tick = gc_tick - ticks_to_destroy;
        if (!pressured && !aggresive) {
            below_tick = std::min(below_tick, idle_tick);
        } else if (!emergency && second >= vram_short_until.load(std::memory_order_relaxed)) {
            const u64 seconds = aggresive ? 1 : pressure_idle_seconds;
            below_tick = std::min(below_tick, tick_seconds_ago(seconds));
        }
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
        visited = 0;
    };
    const auto clean_up = [&](Image& image) {
        if (num_deletions == 0 || ++visited > 256) {
            return true;
        }
        const auto image_id = slot_images.GetSlotId(image);
        // As upstream (#5450): an edge write means the guest reused the memory, so the image is
        // freed without a write-back.
        MarkDirtyIfEdgesModified(image);
        constexpr auto CpuWritten = ImageFlagBits::MaybeCpuDirty | ImageFlagBits::CpuDirty;
        // `tiled` is "cannot be written back" (gc_policy.h): since shadPS4 0.20.1 tiled images
        // are written back too (CanBeDownloaded); multisampled ones, ones with stencil or in a
        // format the copy cannot reinterpret still cannot.
        // bbport (Supermedo f91543a): GPU-written tiled images stay, as before 0.20.1
        // (BB_GC_WRITE_BACK_TILED=1: written back under pressure). Each write-back is a
        // synchronous GPU->CPU copy, and on 6-8 GB cards, always over the pressure mark, the
        // collector evicted render targets still in use: stalls and lost contents (a black
        // Hunter's Dream). Over the critical mark one unused for a minute is written back still,
        // or the card would run out of memory.
        static const bool write_back_tiled = [] {
            const char* env = std::getenv("BB_GC_WRITE_BACK_TILED");
            return env && env[0] == '1';
        }();
        const bool unused_minute = aggresive && image.lru_touched_tick < tick_seconds_ago(60);
        const bool keep_tiled = image.info.IsTiled() && !write_back_tiled && !unused_minute;
        const GcImageState state{.gpu_modified = True(image.flags & ImageFlagBits::GpuModified),
                                 .cpu_dirty = True(image.flags & CpuWritten),
                                 .gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty),
                                 .tiled = !image.CanBeDownloaded() || keep_tiled};
        const GcAction action = GcActionFor(state, pressured);
        // bbport: images that cannot go now (GPU-written: their contents exist only here, and
        // some cannot be written back at all) neither use up the deletions nor stay first in
        // line. The oldest ten being such render targets stopped the collector for good, and
        // VRAM grew with every area visited up to the pressure mark (~10 GB on 16 GB).
        if (action == GcAction::Keep) {
            image_lru_cache.Touch(image, gc_tick);
            BbStats::gc_kept_images.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        --num_deletions;
        if (action == GcAction::WriteBackAndEvict) {
            bool write_back = true;
            if (image.gpu_hash != 0) {
                // As upstream: a buffer bound for writing overlapped the image since its last
                // use. Its contents are read back first; the image holds the current data only
                // while guest memory still matches what it held then.
                buffer_cache.ReadMemory(image.info.guest_address, image.info.guest_size, false,
                                        false, true);
                write_back = image.gpu_hash ==
                             BbMemory::HashBacking(image.info.guest_address, image.info.guest_size);
            }
            if (write_back) {
                // bbport: synchronously, while the image still protects its pages. A deferred
                // write-back landed after FreeImage had unprotected them, over whatever the game
                // had meanwhile stored there (e.g. its heap after unloading an area).
                DownloadImageMemory(image_id, DownloadMemoryFlags::Sync |
                                                  DownloadMemoryFlags::DiscardBufferCache);
                ++gc_downloads;
            }
        }
        ++gc_evictions;
        BbStats::gc_freed_images.fetch_add(1, std::memory_order_relaxed);
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    image_lru_cache.ForEachItemBelow(below_tick, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        image_lru_cache.ForEachItemBelow(below_tick, clean_up);
    }
    if (total_used_memory >= critical_gc_memory) {
        // Still over it: what was used a moment ago goes too (out of memory otherwise).
        configure(true, true);
        image_lru_cache.ForEachItemBelow(below_tick, clean_up);
    }
    // bbport: evictions under memory pressure, at most every 5 s (BB_FRAME_STATS or not).
    if (pressured || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back since the last report\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads);
            gc_report_time = now;
            gc_evictions = gc_downloads = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](Sampler& sampler) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        sampler_lru_cache.Free(sampler);
        samplers.erase(sampler.hash);
        const auto sampler_id = slot_samplers.GetSlotId(sampler);
        slot_samplers.Erase(sampler_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::NoteVramShort() {
    const u64 now = u64(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    vram_short_until.store(now + 30, std::memory_order_relaxed);
}

void TextureCache::RunGarbageCollector() {
    GarbageCollectImages();
    GarbageCollectSamplers();
    ++gc_tick;
}

void TextureCache::TouchImage(Image& image) {
    image.gpu_hash = BufferWriteMarkAfter(ImageEvent::Use, image.gpu_hash);
    // bbport: skips the LRU list (a cache miss) when already touched in this GC period.
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    image_lru_cache.Touch(image, gc_tick);
}

ImageId TextureCache::InsertImage(const ImageInfo& info) {
    const ImageId image_id = slot_images.Insert(instance, runtime, slot_image_views, info);
    if (!slot_images[image_id].allocation_failed) [[likely]] {
        return image_id;
    }
    // bbport: the GPU has no memory left for it, past the driver's budget too (4 GB cards, on
    // entering a big area or the Hunter's Dream). Unused textures go at once, more each round,
    // before the image is given up on: a moment's stutter instead of the game ending.
    u64 freed = 0;
    for (u64 want : {u64(64) << 20, u64(256) << 20, u64(1024) << 20}) {
        want = std::max<u64>(want, u64(info.guest_size) * 2);
        freed += ReclaimMemoryFor(info.guest_address, info.guest_size, want);
        if (slot_images[image_id].RetryAllocation()) {
            static int reported = 0;
            if (reported < 8) {
                ++reported;
                std::printf("GPU: video memory full: %llu MiB of unused textures freed for a "
                            "new %ux%u image\n",
                            (unsigned long long)(freed >> 20), info.size.width, info.size.height);
                std::fflush(stdout);
            }
            return image_id;
        }
    }
    std::printf("GPU: video memory full: no memory for a %ux%u image after freeing %llu MiB of "
                "unused textures\n",
                info.size.width, info.size.height, (unsigned long long)(freed >> 20));
    std::fflush(stdout);
    UNREACHABLE_MSG("Failed allocating image with error {}",
                    vk::to_string(vk::Result::eErrorOutOfDeviceMemory));
    return image_id;
}

u64 TextureCache::ReclaimMemoryFor(VAddr addr, size_t size, u64 bytes) {
    // Unused for 2 s: in no command buffer still being recorded, and with WaitSubmitted in
    // none the GPU is still running, so destroying it at once is safe. Images holding GPU
    // results (render targets) stay: writing them back here would stall in the middle of a
    // draw; textures are uploaded again from guest memory when next used.
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    const u64 unused_tick = std::min(gc_tick, gc_tick_at_second[(second - 2) % gc_tick_at_second.size()]);
    boost::container::small_vector<ImageId, 64> victims;
    u64 freed = 0;
    image_lru_cache.ForEachItemBelow(unused_tick, [&](Image& image) {
        if (freed >= bytes) {
            return true;
        }
        if (image.lru_touched_tick >= unused_tick || image.HasGpuValidData() ||
            image.scene_proxy || image.usage.vo_surface || image.binding.is_bound ||
            image.binding.is_target ||
            image.allocation_failed || image.Overlaps(addr, size)) {
            return false;
        }
        victims.push_back(slot_images.GetSlotId(image));
        freed += Common::AlignUp(image.SizeBytes(), 1024);
        return false;
    });
    if (victims.empty()) {
        return 0;
    }
    scheduler.WaitSubmitted();
    for (const ImageId image_id : victims) {
        {
            std::scoped_lock lk{slot_images[image_id].mutex};
            UntrackImage(image_id);
        }
        UnregisterImage(image_id);
        DeleteImage(image_id, true);
        ++gc_evictions;
    }
    return freed;
}

void TextureCache::DeleteImage(ImageId image_id, bool now) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(image.IsUntracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        UnregisterMeta(meta_info.cmask_addr, image.image_uid);
    }
    if (meta_info.fmask_addr) {
        UnregisterMeta(meta_info.fmask_addr, image.image_uid);
    }
    if (meta_info.htile_addr) {
        UnregisterMeta(meta_info.htile_addr, image.image_uid);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    if (now) {
        EraseImage(image_id);
        return;
    }
    scheduler.DeferOperation([this, image_id] { EraseImage(image_id); });
}

void TextureCache::EraseImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    for (auto& backing : image.backing_images) {
        for (const ImageViewId image_view_id : backing.image_view_ids) {
            slot_image_views.Erase(image_view_id);
        }
    }
    slot_images.Erase(image_id);
}

} // namespace VideoCore
