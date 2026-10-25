// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_game_menu.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_timeline.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include <miniz.h>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // bbport: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// bbport: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, frame_generation{instance, draw_scheduler},
      scene_hdr{instance, draw_scheduler},
      present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames. bbport: room for the ones frame generation adds while it is on
    // (AddPresentFrames); the vector never moves, the free queue points into it.
    base_frame_count = num_images;
    present_frames.reserve(std::max(num_images, BbFrameGenPace::MaxPresentFrames));
    AddPresentFrames(num_images);

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    hdr_pass.Create(instance, swapchain.GetSurfaceFormat().format, Swapchain::HdrFormat());
    // bbport: frame generation presents two images a frame: the overlay's buffers cover them.
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images + 2);
    rasterizer->GetUpscaler().SetFrameGeneration(&frame_generation);
    scene_hdr.SetSlots(u32(present_frames.capacity()));
    rasterizer->GetUpscaler().SetSceneHdr(&scene_hdr);
}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    for (auto& frame : generated_frames) {
        DestroyFrameImage(frame);
        if (frame.present_done) {
            device.destroyFence(frame.present_done);
        }
    }
    DestroyFrameImage(hudless_frame);
    hdr_pass.Destroy();
}

void Presenter::DestroyFrameImage(Frame& frame) {
    if (frame.image_view) {
        instance.GetDevice().destroyImageView(frame.image_view);
        frame.image_view = vk::ImageView{};
    }
    if (frame.image) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        frame.image = vk::Image{};
        frame.allocation = {};
    }
    frame.width = frame.height = 0;
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

bool Presenter::RecreateFrame(Frame* frame, u32 width, u32 height, vk::Format format_override,
                              vk::ImageUsageFlags extra_usage, bool may_fail) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
        frame->image_view = vk::ImageView{};
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
        frame->image = vk::Image{};
        frame->allocation = {};
    }

    const vk::Format format =
        format_override != vk::Format{} ? format_override : swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled |
                 extra_usage,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS && may_fail) {
        // bbport: generated frames stay within the VRAM budget (a 6 GB card past it lost the
        // device, playtest 2026-10-17): frame generation goes off instead.
        LOG_WARNING(Render_Vulkan, "No video memory for a {}x{} generated frame ({})", width,
                    height, vk::to_string(vk::Result{result}));
        frame->allocation = {};
        frame->width = frame->height = 0;
        return false;
    }
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    // bbport: frames stay SDR with HDR output; it is encoded at present (UpdateHdrOutput).
    frame->is_hdr = false;
    return true;
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a frame");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// bbport: raw copy of an image for BB_FRAME_DUMP_TRIGGER, written once the GPU is done. The
// copy is recorded through the scheduler (Record) like the presenter's other commands; the
// buffer is created and the file written on this thread's side.
struct RawDump {
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    VkDeviceSize size{};
    u32 width{}, height{};
    std::string path;
};

static bool BeginRawDump(const Instance& instance, u32 w, u32 h, const char* name, int index,
                         RawDump& dump, u32 bytes_per_pixel = 4) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * bytes_per_pixel,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &dump.buffer,
                        &dump.allocation, &dump.info) != VK_SUCCESS) {
        return false;
    }
    dump.size = buffer_ci.size;
    dump.width = w;
    dump.height = h;
    const char* dir = std::getenv("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    dump.path = path;
    return true;
}

static void CopyRawDump(vk::CommandBuffer cmdbuf, const RawDump& dump, vk::Image image,
                        vk::ImageLayout layout) {
    cmdbuf.copyImageToBuffer(image, layout, dump.buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {dump.width, dump.height, 1}});
}

/// After the Record() that copies: the file is written once the GPU is past the current tick.
static void EndRawDump(const Instance& instance, Scheduler& scheduler, RawDump dump) {
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), dump = std::move(dump)] {
        vmaInvalidateAllocation(allocator, dump.allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(dump.path.c_str(), "wb")) {
            std::fwrite(dump.info.pMappedData, 1, dump.size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, dump.buffer, dump.allocation);
    });
}

// bbport: the rig's pictures (a script's `shot <name>`, AutoplayShotPath in the co-op code):
// BB_SHOT_TRIGGER names a request file holding a picture's path, and the next presented frame,
// overlays and all, goes there as a PNG. Nothing without the setting (players).
static std::optional<std::string> TakeShotRequest() {
    static const char* request = std::getenv("BB_SHOT_TRIGGER");
    if (request == nullptr) {
        return std::nullopt;
    }
    FILE* file = std::fopen(request, "rb");
    if (file == nullptr) {
        return std::nullopt;
    }
    std::array<char, 1024> text{};
    const size_t size = std::fread(text.data(), 1, text.size(), file);
    std::fclose(file);
    std::remove(request);
    std::string path{text.data(), size};
    while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) {
        path.pop_back();
    }
    if (path.empty()) {
        return std::nullopt;
    }
    return path;
}

// The swapchain image as it will be presented (in `layout` now): copied to the host by this
// frame's commands and left ready to present; the PNG is written once the GPU is done.
static void RecordShot(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                       vk::Image image, vk::ImageLayout layout, vk::Extent2D extent,
                       vk::Format format, std::string path) {
    const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    const bool rgba = format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(extent.width) * extent.height * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    // An 8-bit swapchain only (an HDR one gets no picture).
    const bool copied = (bgra || rgba) && vmaCreateBuffer(instance.GetAllocator(), &buffer_ci,
                                                          &alloc_ci, &buffer, &allocation,
                                                          &info) == VK_SUCCESS;
    const vk::ImageSubresourceRange color_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier to_copy{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = layout,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {}, to_copy);
    if (copied) {
        cmdbuf.copyImageToBuffer(
            image, vk::ImageLayout::eTransferSrcOptimal, buffer,
            vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                .imageExtent = {extent.width, extent.height, 1}});
    }
    const vk::ImageMemoryBarrier to_present{
        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
        .dstAccessMask = vk::AccessFlagBits::eNone,
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = vk::ImageLayout::ePresentSrcKHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eBottomOfPipe,
                           vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
    if (!copied) {
        LOG_WARNING(Render_Vulkan, "Shot: no picture for {} ({} swapchain)", path,
                    vk::to_string(format));
        return;
    }
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation,
                                      info, width = extent.width, height = extent.height, bgra,
                                      path = std::move(path)] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        auto* pixels = static_cast<u8*>(info.pMappedData);
        const size_t bytes = size_t(width) * height * 4;
        for (size_t at = 0; at < bytes; at += 4) {
            if (bgra) {
                std::swap(pixels[at], pixels[at + 2]);
            }
            pixels[at + 3] = 0xFF; // the swapchain's alpha means nothing on screen
        }
        size_t png_size = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(
            pixels, static_cast<int>(width), static_cast<int>(height), 4, &png_size,
            MZ_DEFAULT_LEVEL, MZ_FALSE);
        bool saved = false;
        if (png != nullptr) {
            if (FILE* file = std::fopen(path.c_str(), "wb")) {
                saved = std::fwrite(png, 1, png_size, file) == png_size;
                saved = std::fclose(file) == 0 && saved;
            }
            mz_free(png);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
        if (saved) {
            LOG_INFO(Render_Vulkan, "Shot: saved {}", path);
        } else {
            LOG_WARNING(Render_Vulkan, "Shot: could not save {}", path);
        }
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    BbTimeline::Note(BbTimeline::PipeTask, 4, draw_scheduler.CurrentTick());
    // bbport: BB_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to BB_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    const bool frame_dump = frame_dump_trigger && std::remove(frame_dump_trigger) == 0;
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();
    frame->generated = {};
    frame->generated_count = 0;

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    // bbport: recorded like draws, on the recording threads. CommandBuffer() would first wait
    // for them to record everything handed over before (~90 us a frame of the draw recording
    // thread) and then submit from this thread.
    draw_scheduler.EndRendering();

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = AmdGpu::CompSwizzle::One;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        draw_scheduler.Record([image = display.image](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
        });
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            RawDump dump{};
            if (BeginRawDump(instance, image.info.size.width, image.info.size.height, "display",
                             frame_dump_index, dump)) {
                draw_scheduler.Record([dump, source = image.GetImage()](vk::CommandBuffer cmdbuf) {
                    CopyRawDump(cmdbuf, dump, source, vk::ImageLayout::eTransferSrcOptimal);
                });
                EndRawDump(instance, draw_scheduler, std::move(dump));
            }
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        // bbport: BB_PRESENT_DUMP_TRIGGER for the game's own display buffer too.
        if (auto& upscaler = rasterizer->GetUpscaler(); upscaler.PresentDumpDue()) {
            runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            upscaler.DumpPresented(image.GetImage(), image_size.width, image_size.height,
                                   view_info.format);
        }
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;

    // bbport: HDR output with scene highlights (vk_scene_hdr.h): this frame's gain map.
    const auto hdr_job = scene_hdr.Prepare(frame->id, view_info.format, image_size);
    frame->gain_slot = hdr_job.valid ? int(frame->id) : -1;

    // bbport: frame generation (vk_frame_generation.h): the frames between the previous game
    // frame and this one, made from this finished frame and its twin without the UI (FSR 3.1
    // puts this frame's UI back over its frame, DLSS takes the twin as its HUD-less input).
    auto fg_plan = frame_generation.PlanFrame(frame->width, frame->height,
                                              swapchain.GetSurfaceFormat().format, frame->is_hdr,
                                              image_size);
    if (fg_plan.off) {
        ReleaseFrameGenImages();
    }
    std::array<Frame*, FrameGeneration::MaxGenerated> generated{};
    u32 generated_count = 0;
    Frame* hudless = nullptr;
    vk::ImageView hudless_input{};
    if (fg_plan.generate) {
        for (u32 i = 0; i < fg_plan.count; ++i) {
            generated[i] = AcquireGeneratedFrame(frame->width, frame->height,
                                                 frame_generation.OutputFormat(),
                                                 4 * fg_plan.count);
            if (!generated[i]) {
                frame_generation.OutOfMemory();
                generated = {};
                fg_plan.generate = false;
                break;
            }
        }
        generated_count = fg_plan.generate ? fg_plan.count : 0;
    }
    if (fg_plan.generate) {
        if (fg_plan.hudless) {
            hudless = AcquireHudlessFrame(frame->width, frame->height,
                                          swapchain.GetSurfaceFormat().format);
            if (hudless) {
                hudless_input = frame_generation.HudlessView(view_info.format).view;
            }
        }
    }

    // Numbered here, in command order; marked where the commands are recorded.
    const u32 stream = draw_scheduler.CrumbStream();
    const bool crumbs = Breadcrumbs::Enabled();
    const u32 fsr_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter FSR"}) : 0;
    const u32 pp_crumb = crumbs ? Breadcrumbs::Note(stream, {.name = "presenter post process"}) : 0;
    draw_scheduler.Record([this, frame, image_view, image_size, frame_subresources, stream,
                           fsr_crumb, pp_crumb, fsr = fsr_settings, pp = pp_settings,
                           has_hudless = hudless != nullptr,
                           hudless_target = hudless ? *hudless : Frame{},
                           hudless_input](vk::CommandBuffer cmdbuf) mutable {
        // Frames of different submissions may be recorded on two threads at once.
        std::scoped_lock lock{passes_mutex};
        const auto pre_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = frame->image,
            .subresourceRange{frame_subresources},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, false);
        }
        const vk::ImageView input = fsr_pass.Render(cmdbuf, image_view, image_size,
                                                    {frame->width, frame->height}, fsr,
                                                    frame->is_hdr);
        if (fsr_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, fsr_crumb, true);
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, false);
        }
        pp_pass.Render(cmdbuf, input, image_size, *frame, pp);
        if (pp_crumb) {
            Breadcrumbs::Mark(cmdbuf, stream, pp_crumb, true);
        }
        if (has_hudless) {
            // The same passes over the scene without the UI. The previous frame's
            // interpolation may still read this image.
            const vk::ImageMemoryBarrier2 hudless_barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                                 vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = hudless_target.image,
                .subresourceRange{frame_subresources},
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &hudless_barrier,
            });
            const vk::ImageView hudless_pass = fsr_pass.Render(
                cmdbuf, hudless_input, image_size, {hudless_target.width, hudless_target.height},
                fsr, frame->is_hdr);
            pp_pass.Render(cmdbuf, hudless_pass, image_size, hudless_target, pp);
        }
    });
    if (hdr_job.valid) {
        const auto& s = BbSettings::Get();
        const float headroom = BbSettings::HdrPeak() / BbSettings::HdrPaperWhite();
        const float highlights = s.hdr_highlights.load(std::memory_order_relaxed);
        draw_scheduler.Record([this, hdr_job, image_view, headroom,
                               highlights](vk::CommandBuffer cmdbuf) {
            scene_hdr.Record(cmdbuf, hdr_job, image_view, headroom, highlights);
        });
    }
    if (frame_dump) {
        // A raw command buffer: the dump records straight into it (direct recording), after the
        // recorded presenter passes above.
        const vk::CommandBuffer cmdbuf = draw_scheduler.CommandBuffer();
        // The presented image as the swapchain blit reads it (General after the pass).
        RawDump dump{};
        if (BeginRawDump(instance, frame->width, frame->height, "output", frame_dump_index, dump)) {
            draw_scheduler.Record([dump, image = frame->image](vk::CommandBuffer cmdbuf) {
                const vk::MemoryBarrier2 done{
                    .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
                cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
                CopyRawDump(cmdbuf, dump, image, vk::ImageLayout::eGeneral);
            });
            EndRawDump(instance, draw_scheduler, std::move(dump));
        }
        // HDR scene highlights: the gain map (RG16F: gain, UI coverage).
        RawDump gain_dump{};
        if (hdr_job.valid && BeginRawDump(instance, hdr_job.size.width, hdr_job.size.height,
                                          "hdr_gain", frame_dump_index, gain_dump)) {
            draw_scheduler.Record([gain_dump, image = hdr_job.gain_image](vk::CommandBuffer cmdbuf) {
                CopyRawDump(cmdbuf, gain_dump, image, vk::ImageLayout::eTransferSrcOptimal);
            });
            EndRawDump(instance, draw_scheduler, std::move(gain_dump));
            // The gain's inputs: the 16-bit scene and the UI-free SDR scene (hdr_scene_*, raw
            // RGBA16F / RGBA8), for tuning the highlights offline.
            const auto captures = scene_hdr.CapturedImages();
            for (const auto& [capture, name] :
                 {std::pair{captures.scene, "hdr_scene"}, std::pair{captures.hudless, "hdr_hudless"}}) {
                RawDump capture_dump{};
                if (!capture.image || !BeginRawDump(instance, capture.width, capture.height, name,
                                                    frame_dump_index, capture_dump,
                                                    capture.bytes_per_pixel)) {
                    continue;
                }
                draw_scheduler.Record([capture_dump, image = capture.image](vk::CommandBuffer cmdbuf) {
                    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to) {
                        const vk::ImageMemoryBarrier2 b{
                            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                            .oldLayout = from,
                            .newLayout = to,
                            .image = image,
                            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                                   .pImageMemoryBarriers = &b});
                    };
                    barrier(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal);
                    CopyRawDump(cmdbuf, capture_dump, image, vk::ImageLayout::eTransferSrcOptimal);
                    barrier(vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
                });
                EndRawDump(instance, draw_scheduler, std::move(capture_dump));
            }
        }
        ++frame_dump_index;
    }
    if (generated_count) {
        // Direct recording, after the host passes above, in game frame order: the providers
        // keep the previous frame's state between calls.
        const vk::CommandBuffer cmdbuf = draw_scheduler.CommandBuffer();
        FrameGeneration::Targets targets{
            .current = frame->image,
            .current_view = frame->image_view,
            .hudless = hudless ? hudless->image : vk::Image{},
            .hudless_view = hudless ? hudless->image_view : vk::ImageView{},
            .count = generated_count,
            .format = swapchain.GetSurfaceFormat().format,
            .width = frame->width,
            .height = frame->height,
        };
        for (u32 i = 0; i < generated_count; ++i) {
            targets.outputs[i] = generated[i]->image;
            targets.output_views[i] = generated[i]->image_view;
        }
        const u32 made = frame_generation.Record(cmdbuf, targets);
        if (made && frame_dump) {
            // BB_FRAME_DUMP_TRIGGER: the generated frames (between the previous game frame
            // and the dumped "output") and the game frame without the UI.
            const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                          .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                          .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                          .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
            const auto dump_image = [&](vk::Image image, u32 w, u32 h, const char* name) {
                RawDump dump{};
                if (BeginRawDump(instance, w, h, name, frame_dump_index - 1, dump)) {
                    CopyRawDump(cmdbuf, dump, image, vk::ImageLayout::eGeneral);
                    EndRawDump(instance, draw_scheduler, std::move(dump));
                }
            };
            for (u32 i = 0; i < made; ++i) {
                char name[16];
                std::snprintf(name, sizeof(name), i ? "generated%u" : "generated", i);
                dump_image(generated[i]->image, generated[i]->width, generated[i]->height, name);
            }
            if (hudless) {
                dump_image(hudless->image, hudless->width, hudless->height, "hudless");
            }
        }
        generated_count = made;
    }

    // Flush frame creation commands.
    BbStats::frame_number.fetch_add(1, std::memory_order_relaxed); // bbport: game frames shown
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    BbTimeline::Note(BbTimeline::PipeTask, 5, frame->ready_tick);
    if (fg_plan.generate) {
        frame_generation.EndFrame(frame->ready_tick);
        for (u32 i = 0; i < generated_count; ++i) {
            // Shown right before this frame (Present); they wait for the same submission.
            generated[i]->ready_semaphore = frame->ready_semaphore;
            generated[i]->ready_tick = frame->ready_tick;
            generated[i]->gain_slot = frame->gain_slot;
        }
        if (fg_plan.present && generated_count) {
            frame->generated = generated;
            frame->generated_count = generated_count;
            ++generated_sent;
        }
    }

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                BbStats::WaitTimer timer{BbStats::present_wait_ns};
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->gain_slot = -1;

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

// Sleeps until `time`: coarsely, then yielding (the scheduler's tick is longer than the margin
// frame generation places its images by).
static void SleepUntil(std::chrono::steady_clock::time_point time) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (time - now > microseconds(2500)) {
        std::this_thread::sleep_for(time - now - microseconds(1500));
    }
    while (steady_clock::now() < time) {
        std::this_thread::yield();
    }
}

void Presenter::WaitRendered(const Frame* frame) {
    if (!frame || !frame->ready_semaphore) {
        return;
    }
    const vk::SemaphoreWaitInfo info{
        .semaphoreCount = 1,
        .pSemaphores = &frame->ready_semaphore,
        .pValues = &frame->ready_tick,
    };
    // A lost device ends the wait (the presenter reports it); a timeout waits again.
    while (instance.GetDevice().waitSemaphores(&info, 1'000'000'000) == vk::Result::eTimeout) {
    }
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    const auto generated = std::exchange(frame->generated, {});
    const u32 count = std::exchange(frame->generated_count, 0);
    // Redraws of the last frame and blank frames never generate; with the switch off this is
    // the plain presentation.
    if (is_reusing_frame || !is_game_frame) {
        if (count) {
            generated_done.fetch_add(1, std::memory_order_release);
        }
        PresentFrame(frame, is_reusing_frame, is_game_frame);
        return;
    }
    if (!count && !FrameGeneration::Requested()) {
        frame_generation.OnSwitchedOff();
        fg_limiter.Reset();
        PresentFrame(frame, is_reusing_frame, is_game_frame);
        return;
    }
    PresentGenerated(frame, generated, count);
}

// bbport: frame generation (vk_frame_generation.h), from Supermedo's bbport (026f8ff): the
// generated frames first, then the game's own, evenly spaced over the interval between the
// game's frames (the generated ones belong to the time before this frame).
void Presenter::PresentGenerated(Frame* frame,
                                 const std::array<Frame*, FrameGeneration::MaxGenerated>& generated,
                                 u32 count) {
    using namespace std::chrono;
    static auto last_refresh = steady_clock::time_point{};
    static u32 hz = 0;
    // The generated frames are placed from the moment the GPU has finished them (they come
    // ready with the game frame, the same submission). Placed from the moment the frame was
    // queued instead, a GPU busy for more than the spacing had both ready at once, and Mailbox
    // replaced the generated one before the display showed it: frame generation "on" with
    // nothing to see, at the GPU's cost (Kaniel 2026-10-25).
    if (count) {
        WaitRendered(frame);
    }
    const auto start = steady_clock::now();
    if (start - last_refresh > seconds(2)) {
        last_refresh = start;
        hz = 0;
        if (SDL_Window* sdl_window = window.GetSDLWindow()) {
            if (const SDL_DisplayID display = SDL_GetDisplayForWindow(sdl_window)) {
                if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display)) {
                    hz = u32(mode->refresh_rate + 0.5f);
                }
            }
        }
    }
    const float interval_ms = frame_generation.OnPresent(count);
    const auto step = duration_cast<steady_clock::duration>(
        duration<double, std::milli>(double(interval_ms) / (count + 1)));
    // With V-Sync (FIFO) the display paces the images itself.
    const bool fifo = swapchain.GetPresentMode() == vk::PresentModeKHR::eFifo ||
                      swapchain.GetPresentMode() == vk::PresentModeKHR::eFifoRelaxed;
    // The game must never wait for generated frames: when every presentation frame is in use
    // (the GPU command thread would block on the next one) the rest are dropped and the game's
    // frame goes out at once.
    const auto behind = [this] {
        std::scoped_lock lock{free_mutex};
        return free_queue.empty();
    };
    // When each image went out, to tell which generated ones the display had time to show.
    std::array<steady_clock::time_point, FrameGeneration::MaxGenerated> sent{};
    u32 shown = 0;
    for (; shown < count; ++shown) {
        if (shown > 0) {
            if (behind()) {
                break;
            }
            if (!fifo) {
                SleepUntil(start + step * shown);
            }
        }
        PresentFrame(generated[shown], true, false);
        sent[shown] = steady_clock::now();
    }
    if (count) {
        generated_done.fetch_add(1, std::memory_order_release);
    }
    // Generated frames whose spacing the game cut (it needed every presentation frame): not
    // sent, or sent right before the game frame. The log counts them (PresentFramesNeeded).
    u32 starved = count - shown;
    if (shown && !fifo) {
        if (behind()) {
            ++starved;
        } else {
            SleepUntil(start + step * shown);
        }
    }
    PresentFrame(frame, false, true);
    const auto game_sent = steady_clock::now();
    u32 seen = 0;
    for (u32 i = 0; i < shown; ++i) {
        const auto next = i + 1 < shown ? sent[i + 1] : game_sent;
        seen += BbFrameGenPace::GeneratedShown(duration<double, std::milli>(next - sent[i]).count(),
                                               hz, fifo);
    }
    frame_generation.OnPresented(shown, seen, starved);

    // How many the display has room for from the next frames on (fit to display; always with
    // V-Sync, where each image more waits for the display).
    auto& settings = BbSettings::Get();
    const double present_ms = duration<double, std::milli>(steady_clock::now() - start).count();
    const double now_s = duration<double>(start.time_since_epoch()).count();
    const bool fit = settings.frame_gen_fit_display.load(std::memory_order_relaxed) || fifo;
    const u32 limit = fg_limiter.OnGameFrame(now_s, hz, fit, interval_ms, present_ms);
    frame_generation.SetDisplayLimit(limit, fg_limiter.Paused(now_s));
    settings.frame_gen_display_hz.store(int(hz), std::memory_order_relaxed);
}

// bbport: HDR output when it is switched on and both the swapchain and the display offer it;
// the menus show the HDR rows only then (BbSettings::hdr_supported).
void Presenter::UpdateHdrOutput() {
    auto& s = BbSettings::Get();
    const bool display_hdr = window.IsHdrDisplay();
    if (display_hdr != last_display_hdr) {
        last_display_hdr = display_hdr;
        swapchain.RefreshFormats();
    }
    // The display's HDR state can blink off for a moment (display or mode changes, the swapchain
    // switching format): HDR output stays on until it has been off for a while, so a blink does
    // not rebuild the swapchain twice.
    constexpr u32 OffPresents = 90;
    const bool now_supported = swapchain.HasHDR() && display_hdr;
    hdr_unsupported_presents = now_supported ? 0 : hdr_unsupported_presents + 1;
    const bool supported = now_supported || (swapchain.GetHDR() && hdr_unsupported_presents < OffPresents);
    s.hdr_supported.store(supported, std::memory_order_relaxed);
    const bool setting = s.hdr.load(std::memory_order_relaxed);
    const bool wanted = supported && setting;
    // BB_HDR_SCENE_TEST=1: the gain maps are made with SDR output too (BB_FRAME_DUMP_TRIGGER
    // saves them as hdr_gain), for checking them on a display without HDR.
    static const bool scene_test = [] {
        const char* env = std::getenv("BB_HDR_SCENE_TEST");
        return env && *env == '1';
    }();
    scene_hdr.SetActive((wanted || scene_test) && s.hdr_scene.load(std::memory_order_relaxed));
    if (wanted != swapchain.GetHDR()) {
        std::printf("Presenter: HDR output %s (setting %d, display HDR %d, HDR10 swapchain %d)\n",
                    wanted ? "on (HDR10)" : "off (SDR)", int(setting), int(display_hdr),
                    int(swapchain.HasHDR()));
        // Changing HDR recreates the swapchain and waits for the device: no queue submits from
        // the recording threads meanwhile (deadinside28 PR #211).
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        swapchain.SetHDR(wanted);
    }
}

// bbport: the SDR picture is in hdr_pass.Game() (a transfer destination). The overlay goes into
// its own layer, then both are encoded into the HDR10 swapchain image (host_passes/hdr_pass.h).
void Presenter::RecordHdrOutput(vk::CommandBuffer cmdbuf, Frame* frame, vk::Extent2D extent,
                                bool overlay, std::optional<std::string> shot) {
    const vk::ImageSubresourceRange color_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto transition = [&](vk::Image image, vk::AccessFlags from_access, vk::AccessFlags to_access,
                                vk::ImageLayout from, vk::ImageLayout to) {
        return vk::ImageMemoryBarrier{
            .srcAccessMask = from_access,
            .dstAccessMask = to_access,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = color_range,
        };
    };
    const auto barrier = [&](std::span<const vk::ImageMemoryBarrier> barriers) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, barriers);
    };
    using Access = vk::AccessFlagBits;
    using Layout = vk::ImageLayout;
    // The rig's picture: the game without the overlay, in SDR (RecordShot leaves it PresentSrc).
    Layout game_layout = Layout::eTransferDstOptimal;
    if (shot) {
        RecordShot(instance, present_scheduler, cmdbuf, hdr_pass.Game(), game_layout, extent,
                   swapchain.GetSurfaceFormat().format, std::move(*shot));
        game_layout = Layout::ePresentSrcKHR;
    }
    const std::array to_compose{
        transition(hdr_pass.Game(), Access::eTransferWrite, Access::eShaderRead, game_layout,
                   Layout::eShaderReadOnlyOptimal),
        transition(hdr_pass.Overlay(), Access::eNone, Access::eTransferWrite, Layout::eUndefined,
                   Layout::eTransferDstOptimal),
        transition(frame->image, Access::eTransferRead, Access::eColorAttachmentWrite,
                   Layout::eTransferSrcOptimal, Layout::eGeneral),
        transition(swapchain.Image(), Access::eNone, Access::eColorAttachmentWrite, Layout::eUndefined,
                   Layout::eColorAttachmentOptimal),
    };
    barrier(to_compose);
    const vk::ClearColorValue transparent{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
    cmdbuf.clearColorImage(hdr_pass.Overlay(), Layout::eTransferDstOptimal, transparent, color_range);
    const std::array to_draw{transition(hdr_pass.Overlay(), Access::eTransferWrite,
                                        Access::eColorAttachmentRead | Access::eColorAttachmentWrite,
                                        Layout::eTransferDstOptimal, Layout::eColorAttachmentOptimal)};
    barrier(to_draw);
    if (overlay) {
        BbOverlay::Render(cmdbuf, hdr_pass.OverlayView(), extent,
                          FitImage(frame->width, frame->height, extent.width, extent.height));
    }
    // The frame's gain map (vk_scene_hdr.h), placed like the frame; 1 in the borders.
    const auto gain = frame->gain_slot >= 0 ? scene_hdr.GainImage(u32(frame->gain_slot))
                                            : SceneHdr::Gain{};
    const std::array to_fill{transition(hdr_pass.Gain(), Access::eNone, Access::eTransferWrite,
                                        Layout::eUndefined, Layout::eTransferDstOptimal)};
    barrier(to_fill);
    const vk::ClearColorValue no_gain{std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}};
    cmdbuf.clearColorImage(hdr_pass.Gain(), Layout::eTransferDstOptimal, no_gain, color_range);
    if (gain.image) {
        const vk::MemoryBarrier clear_done{.srcAccessMask = Access::eTransferWrite,
                                           .dstAccessMask = Access::eTransferWrite};
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(gain.image, Layout::eTransferSrcOptimal, hdr_pass.Gain(),
                         Layout::eTransferDstOptimal,
                         MakeImageBlitFit(gain.width, gain.height, extent.width, extent.height),
                         vk::Filter::eLinear);
    }
    const std::array to_read{
        transition(hdr_pass.Overlay(), Access::eColorAttachmentWrite, Access::eShaderRead,
                   Layout::eColorAttachmentOptimal, Layout::eShaderReadOnlyOptimal),
        transition(hdr_pass.Gain(), Access::eTransferWrite, Access::eShaderRead,
                   Layout::eTransferDstOptimal, Layout::eShaderReadOnlyOptimal)};
    barrier(to_read);
    const auto& s = BbSettings::Get();
    hdr_pass.Render(cmdbuf, swapchain.ImageView(), extent,
                    {.paper_white = BbSettings::HdrPaperWhite(),
                     .peak = BbSettings::HdrPeak(),
                     .highlights = s.hdr_highlights.load(std::memory_order_relaxed),
                     .use_gain = gain.image ? 1.0f : 0.0f});
    const std::array to_present{transition(swapchain.Image(), Access::eColorAttachmentWrite,
                                           Access::eNone, Layout::eColorAttachmentOptimal,
                                           Layout::ePresentSrcKHR)};
    barrier(to_present);
}

void Presenter::PresentFrame(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // bbport: nothing to present to while the window is minimised (a 0x0 surface, issue #31).
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }
    UpdateHdrOutput();
    // Recreate the swapchain if the window was resized (or was minimised at the last try).
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight() ||
        !swapchain.IsPresentable() ||
        swapchain.HasFrameGenerationImages() != swapchain.WantsFrameGenerationImages()) {
        // Recreate waits for the device: recording threads must not submit to its queues
        // meanwhile (deadinside28 PR #211), as for the recreation after a failed present.
        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
        if (!swapchain.IsPresentable()) {
            free_frame();
            return;
        }
    }

    if (!swapchain.AcquireNextImage()) {
        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // bbport: the game frame is blitted (letterboxed) straight into the swapchain image; with
    // HDR output into an SDR image of the same size, encoded into the swapchain image after
    // the overlay (host_passes/hdr_pass.h).
    const vk::Image swapchain_image = swapchain.Image();
    const bool hdr = swapchain.GetHDR();
    if (hdr) {
        hdr_pass.EnsureImages(swapchain.GetExtent());
    }
    const vk::Image target_image = hdr ? hdr_pass.Game() : swapchain_image;
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    DumpFinalFrameIfDue(instance, scheduler, cmdbuf, frame->image, frame->width, frame->height,
                        swapchain.GetSurfaceFormat().format);

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(target_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, target_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        // bbport: the settings menu / FPS counter over the frame, at display resolution.
        BbGameMenu::Poll(); // the port's pages in the game's System menu
        const bool overlay = BbOverlay::Visible();
        auto shot = TakeShotRequest(); // the rig's picture of this frame, if asked for
        if (hdr) {
            RecordHdrOutput(cmdbuf, frame, extent, overlay, std::move(shot));
        } else {
            const std::array post_barriers{
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                                   vk::AccessFlagBits::eColorAttachmentWrite
                                             : vk::AccessFlagBits::eNone,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                         : vk::ImageLayout::ePresentSrcKHR,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swapchain_image,
                    .subresourceRange = color_range,
                },
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                    .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                    .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = frame->image,
                    .subresourceRange = color_range,
                },
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eAllCommands,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
            if (overlay) {
                BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent,
                                  FitImage(frame->width, frame->height, extent.width, extent.height));
            }
            if (shot) {
                RecordShot(instance, scheduler, cmdbuf, swapchain_image,
                           overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                   : vk::ImageLayout::ePresentSrcKHR,
                           extent, swapchain.GetSurfaceFormat().format, std::move(*shot));
            } else if (overlay) {
                const vk::ImageMemoryBarrier to_present{
                    .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                    .dstAccessMask = vk::AccessFlagBits::eNone,
                    .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .newLayout = vk::ImageLayout::ePresentSrcKHR,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swapchain_image,
                    .subresourceRange = color_range,
                };
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                       vk::PipelineStageFlagBits::eBottomOfPipe,
                                       vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
            }
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    // bbport: the frame's submission goes out from a recording thread (BB_ASYNC_SUBMIT); this
    // one, waiting for it on the same queue, must not get there first.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick);
    }
    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

// Under free_mutex (or before the swap thread runs): `count` more presentation frames, within the
// reserved capacity; their images are made at first use (GetRenderFrame).
void Presenter::AddPresentFrames(u32 count) {
    const vk::Device device = instance.GetDevice();
    for (u32 i = 0; i < count && present_frames.size() < present_frames.capacity(); ++i) {
        Frame& frame = present_frames.emplace_back();
        frame.id = u8(present_frames.size() - 1);
        frame.present_done = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        free_queue.push(&frame);
    }
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        // bbport: with frame generation the swap thread holds each frame through the generated
        // frames' spacing; more frames, so the game never takes the last one and cuts it short.
        const u32 needed =
            BbFrameGenPace::PresentFramesNeeded(base_frame_count, FrameGeneration::Requested());
        if (present_frames.size() < needed) {
            const u32 before = u32(present_frames.size());
            AddPresentFrames(needed - before);
            std::printf("Presenter: %u presentation frames for frame generation (were %u)\n",
                        u32(present_frames.size()), before);
        }
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a frame");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

// Destroys the image once the GPU has caught up with everything submitted so far.
static void DeferFrameImage(Scheduler& scheduler, const Instance& instance, Frame& frame) {
    if (frame.image) {
        scheduler.DeferOperation([device = instance.GetDevice(), allocator = instance.GetAllocator(),
                                  image = frame.image, allocation = frame.allocation,
                                  view = frame.image_view] {
            device.destroyImageView(view);
            vmaDestroyImage(allocator, image, allocation);
        });
    }
    frame.image = vk::Image{};
    frame.image_view = vk::ImageView{};
    frame.allocation = {};
}

Frame* Presenter::AcquireGeneratedFrame(u32 width, u32 height, vk::Format format, u32 ring) {
    ring = std::clamp<u32>(ring, 1, u32(generated_frames.size()));
    if (next_generated >= ring) {
        next_generated = 0;
    }
    Frame* frame = &generated_frames[next_generated];
    next_generated = (next_generated + 1) % ring;
    const vk::Device device = instance.GetDevice();
    if (!frame->present_done) {
        frame->present_done = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame->id = u8(frame - generated_frames.data());
    }
    // Its last presentation is over (the blit reads it).
    vk::Result result{};
    while ((result = device.waitForFences(frame->present_done, false,
                                          std::numeric_limits<u64>::max())) !=
           vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for an interpolated frame");
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for an interpolated frame");
    }
    frame->generated = {};
    frame->generated_count = 0;
    if (!frame->image || frame->width != width || frame->height != height) {
        DeferFrameImage(draw_scheduler, instance, *frame);
        if (!RecreateFrame(frame, width, height, format, vk::ImageUsageFlagBits::eStorage,
                           true)) {
            return nullptr;
        }
    }
    // Fewer frames per game frame now: the entries past the ring are freed (their last
    // presentation waited for like this one's).
    for (u32 i = ring; i < generated_frames.size(); ++i) {
        Frame& unused = generated_frames[i];
        if (unused.image && unused.present_done &&
            device.getFenceStatus(unused.present_done) == vk::Result::eSuccess) {
            DeferFrameImage(draw_scheduler, instance, unused);
            unused.width = unused.height = 0;
        }
    }
    return frame;
}

void Presenter::ReleaseFrameGenImages() {
    // The swap thread may still show the last generated frames: wait until it has shown or
    // dropped every game frame that had some (bounded: a frame it never got must not hang the
    // GPU thread; the images are then freed the next time generation is switched off).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (generated_done.load(std::memory_order_acquire) < generated_sent) {
        if (std::chrono::steady_clock::now() > deadline) {
            LOG_WARNING(Render_Vulkan, "Frame generation: generated frames still queued, kept");
            return;
        }
        std::this_thread::yield();
    }
    const vk::Device device = instance.GetDevice();
    u64 freed = 0;
    for (Frame& generated : generated_frames) {
        if (!generated.image) {
            continue;
        }
        // Its blit is over (the fence is signalled after it), then the GPU work that wrote it.
        while (generated.present_done &&
               device.waitForFences(generated.present_done, false, 1'000'000'000) ==
                   vk::Result::eTimeout) {
        }
        freed += u64(generated.width) * generated.height * 4;
        DeferFrameImage(draw_scheduler, instance, generated);
        generated.width = generated.height = 0;
    }
    if (hudless_frame.image) {
        freed += u64(hudless_frame.width) * hudless_frame.height * 4;
        DeferFrameImage(draw_scheduler, instance, hudless_frame);
        hudless_frame.width = hudless_frame.height = 0;
    }
    next_generated = 0;
    if (freed) {
        std::printf("Frame generation: %.0f MB of generated frames freed\n", freed / 1e6);
    }
}

Frame* Presenter::AcquireHudlessFrame(u32 width, u32 height, vk::Format format) {
    if (!hudless_frame.image || hudless_frame.width != width || hudless_frame.height != height) {
        DeferFrameImage(draw_scheduler, instance, hudless_frame);
        if (!RecreateFrame(&hudless_frame, width, height, format, {}, true)) {
            return nullptr; // VRAM full: the generated frames are made with the UI
        }
    }
    return &hudless_frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // minimised: keep the last size (a 0x0 frame image cannot be made, issue #31)
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
