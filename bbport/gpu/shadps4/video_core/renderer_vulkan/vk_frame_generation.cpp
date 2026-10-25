// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "bbport_frame_gen_pace.h"
#include "bbport_settings.h"
#include "core/emulator_settings.h"
#include "ffx_vk_portable.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

/// The library's effect calls recycle their image views after 8 calls and the interpolation
/// makes two a frame: the GPU must have finished the frame three back before the next is recorded.
constexpr size_t kFramesInFlight = 3;

/// Frames DLSS Frame Generation can make per game frame on this system (0: none).
int DlssMaxCount() {
    const Dlss* dlss = Dlss::Get();
    return dlss ? dlss->FrameGenMaxCount() : 0;
}

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

/// An optional image that is not there: described but null.
FfxVkPortableImage Absent() {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    return out;
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Frame generation: %s refused:", what);
    for (u64 bit = 1; bit != 0 && bit <= issues; bit <<= 1) {
        if (issues & bit) {
            std::printf(" %s;", ffxVkPortableValidationIssueName(bit));
        }
    }
    std::printf("\n");
}

vk::ImageAspectFlags DepthAspects(vk::Format format) {
    const bool stencil = format == vk::Format::eD32SfloatS8Uint ||
                         format == vk::Format::eD24UnormS8Uint ||
                         format == vk::Format::eD16UnormS8Uint;
    return vk::ImageAspectFlagBits::eDepth |
           (stencil ? vk::ImageAspectFlagBits::eStencil : vk::ImageAspectFlags{});
}

FfxVkPortableFrameGenerationCreateInfo MakeCreateInfo(u32 width, u32 height, vk::Format source,
                                                      vk::Format output) {
    FfxVkPortableFrameGenerationCreateInfo info{};
    info.structSize = sizeof(info);
    // LDR, jitter-free motion vectors in render resolution pixels, depth 0..1 (near 0).
    info.flags = 0;
    info.maxRenderSize = {width, height};
    info.displaySize = {width, height};
    info.interpolationSourceFormat = static_cast<VkFormat>(source);
    info.outputFormat = static_cast<VkFormat>(output);
    return info;
}

} // namespace

FrameGeneration::FrameGeneration(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    auto& settings = BbSettings::Get();
    const int dlss_max = DlssMaxCount();
    settings.dlssg_max_multiplier = dlss_max + 1;
    // BB_DEPTH_INVERTED=1: the game's depth is 1 at the near plane (reversed Z). For DLSS; the
    // FSR 3.1 interface has no such option (0 at the near plane).
    if (const char* env = std::getenv("BB_DEPTH_INVERTED")) {
        depth_inverted = env[0] == '1';
    }
    std::printf("Frame generation: DLSS Frame Generation %s\n",
                dlss_max >= 3   ? "up to 4x (GeForce RTX 50)"
                : dlss_max >= 1 ? "2x (GeForce RTX 40)"
                : !Dlss::Get()  ? "unavailable (no DLSS bridge)"
                                : "unavailable (needs a GeForce RTX 40 or newer, nvngx_dlssg.dll and a "
                                  "bbport_dlss.dll with Frame Generation)");
}

FrameGeneration::~FrameGeneration() {
    if (context) {
        ffxVkPortableFrameGenerationContextDestroy(context);
        context = nullptr;
    }
    if (Dlss* dlss = Dlss::Get()) {
        dlss->ReleaseFrameGen();
    }
    const auto device = instance.GetDevice();
    for (const auto& entry : hudless_views) {
        device.destroyImageView(entry.view);
    }
    if (depth_copy_view) {
        device.destroyImageView(depth_copy_view);
    }
    if (motion_view) {
        device.destroyImageView(motion_view);
    }
}

bool FrameGeneration::Requested() {
    const auto& settings = BbSettings::Get();
    return settings.frame_generation.load(std::memory_order_relaxed) &&
           settings.upscaler.load(std::memory_order_relaxed) != BbSettings::UpscalerOff;
}

u32 FrameGeneration::WantedCount() {
    if (!Requested()) {
        return 0;
    }
    const auto& settings = BbSettings::Get();
    return BbFrameGenPace::WantedCount(settings.frame_gen_provider.load(std::memory_order_relaxed),
                                       settings.frame_gen_multiplier.load(std::memory_order_relaxed),
                                       DlssMaxCount());
}

// ---- Capture --------------------------------------------------------------------------------

void FrameGeneration::EnsureDepthCopy(vk::Format format, u32 width, u32 height) {
    if (depth_copy && depth_copy_format == format && depth_copy_width >= width &&
        depth_copy_height >= height) {
        return;
    }
    // Earlier frames may still read the old one.
    const auto device = instance.GetDevice();
    scheduler.DeferOperation([device, old = std::move(depth_copy), view = depth_copy_view] {
        if (view) {
            device.destroyImageView(view);
        }
    });
    depth_copy_view = vk::ImageView{};
    depth_copy = VideoCore::UniqueImage(device, instance.GetAllocator());
    depth_copy.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    depth_copy_format = format;
    depth_copy_width = width;
    depth_copy_height = height;
    // DLSS reads it through a view of its depth aspect.
    depth_copy_view = Check(device.createImageView({
        .image = vk::Image(depth_copy),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    // The interpolation context holds no reference to the image between frames.
}

void FrameGeneration::EnsureHudless(vk::Format format, u32 width, u32 height) {
    if (hudless_raw && hudless_format == format && hudless_width == width &&
        hudless_height == height) {
        return;
    }
    const auto device = instance.GetDevice();
    for (const auto& entry : hudless_views) {
        scheduler.DeferOperation([device, view = entry.view] { device.destroyImageView(view); });
    }
    hudless_views.clear();
    scheduler.DeferOperation([old = std::move(hudless_raw)] {});
    hudless_raw = VideoCore::UniqueImage(device, instance.GetAllocator());
    hudless_raw.Create(vk::ImageCreateInfo{
        // The host passes read it through the display buffer's view format (sRGB or not).
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    hudless_format = format;
    hudless_width = width;
    hudless_height = height;
}

void FrameGeneration::CaptureScene(const Scene& scene) {
    pending = {};
    if (!Requested() || !scene.depth || !scene.motion) {
        return;
    }
    EnsureDepthCopy(scene.depth_format, scene.width, scene.height);
    const vk::ImageAspectFlags all_aspects = DepthAspects(scene.depth_format);
    scheduler.Record([src = scene.depth, layout = scene.depth_layout, dst = vk::Image(depth_copy),
                      w = scene.width, h = scene.height,
                      all_aspects](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 source_ready{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        // Whatever read the previous frame's copy (the interpolation) is done before it is
        // overwritten.
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = dst,
            .subresourceRange = {all_aspects, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1,
                              .pMemoryBarriers = &source_ready,
                              .imageMemoryBarrierCount = 1,
                              .pImageMemoryBarriers = &barrier});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .extent = {w, h, 1},
        };
        cmd.copyImage(src, layout, dst, vk::ImageLayout::eTransferDstOptimal, region);
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    pending.valid = true;
    pending.motion = scene.motion;
    pending.width = scene.width;
    pending.height = scene.height;
    pending.depth_format = scene.depth_format;
    pending.jitter = scene.jitter;
    pending.frame_ms = scene.frame_ms;
    pending.camera_near = scene.camera_near;
    pending.camera_fov = scene.camera_fov;
    pending.camera_far = scene.camera_far;
    pending.has_matrices = scene.has_matrices;
    pending.view_to_clip = scene.view_to_clip;
    pending.clip_to_prev_clip = scene.clip_to_prev_clip;
    pending.prev_clip_to_clip = scene.prev_clip_to_clip;
    pending.reset = scene.reset;
}

void FrameGeneration::CaptureHudless(vk::Image image, vk::ImageLayout layout, vk::Format format,
                                     u32 width, u32 height) {
    if (!WantsHudless()) {
        return;
    }
    EnsureHudless(format, width, height);
    scheduler.Record([src = image, layout, dst = vk::Image(hudless_raw), width,
                      height](vk::CommandBuffer cmd) {
        constexpr vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::MemoryBarrier2 source_ready{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = dst,
            .subresourceRange = color,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1,
                              .pMemoryBarriers = &source_ready,
                              .imageMemoryBarrierCount = 1,
                              .pImageMemoryBarriers = &barrier});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {width, height, 1},
        };
        cmd.copyImage(src, layout, dst, vk::ImageLayout::eTransferDstOptimal, region);
        // The host passes sample it next.
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
        barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    });
    pending.hudless = true;
    pending.hudless_width = width;
    pending.hudless_height = height;
}

// ---- Context --------------------------------------------------------------------------------

void FrameGeneration::Report(std::string_view state) {
    if (state == reported) {
        return;
    }
    reported = state;
    std::printf("Frame generation: %.*s\n", int(state.size()), state.data());
}

bool FrameGeneration::ChooseOutputFormat(vk::Format format) {
    // The generated frames are written by compute shaders: storage support in the frame's
    // format, else RGBA8 (the swapchain blit converts).
    const auto physical = instance.GetPhysicalDevice();
    const auto storage = [&](vk::Format candidate) {
        const auto features = physical.getFormatProperties(candidate).optimalTilingFeatures;
        return (features & vk::FormatFeatureFlagBits::eStorageImage) &&
               (features & vk::FormatFeatureFlagBits::eSampledImage) &&
               (features & vk::FormatFeatureFlagBits::eTransferSrc);
    };
    output_format = storage(format) ? format : vk::Format::eR8G8B8A8Unorm;
    if (!storage(output_format)) {
        Report("off (no storage image format for the generated frames)");
        return false;
    }
    return true;
}

bool FrameGeneration::CreateContext(u32 width, u32 height, vk::Format format) {
    const auto physical = instance.GetPhysicalDevice();
    FfxVkPortableDeviceCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    if (ffxVkPortableQueryDeviceCapabilities(physical, &capabilities) != FFX_VK_PORTABLE_OK ||
        !capabilities.fsr3FrameGenerationPrerequisites) {
        Report("off (the GPU lacks what AMD's frame interpolation needs)");
        return false;
    }
    if (!ChooseOutputFormat(format)) {
        return false;
    }

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = physical;
    device_info.device = instance.GetDevice();
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    const auto create_info = MakeCreateInfo(width, height, format, output_format);
    if (const u64 issues = ffxVkPortableValidateFrameGenerationCreateInfo(&create_info)) {
        PrintIssues("create info", issues);
        return false;
    }
    const auto result = ffxVkPortableFrameGenerationContextCreate(&device_info, &create_info,
                                                                 &context);
    if (result != FFX_VK_PORTABLE_OK) {
        context = nullptr;
        std::printf("Frame generation: off (the AMD frame interpolation context could not be "
                    "created, error %d)\n", int(result));
        reported = "failed";
        return false;
    }
    context_width = width;
    context_height = height;
    context_format = format;
    std::printf("Frame generation: on (AMD frame interpolation, %ux%u, interpolated frame %s, "
                "optical flow)\n", width, height, vk::to_string(output_format).c_str());
    reported = "on";
    return true;
}

void FrameGeneration::DestroyContext() {
    if (!context) {
        return;
    }
    // Submitted work may still use the context's images.
    scheduler.Finish();
    ffxVkPortableFrameGenerationContextDestroy(context);
    context = nullptr;
    ticks.clear();
}

void FrameGeneration::ReleaseDlssFrameGen(Dlss& dlss) {
    // Submitted frames may still use the feature; then no present beside its release
    // (BbFrameGenPace::ReleaseBesidePresent).
    scheduler.WaitSubmitted();
    BbFrameGenPace::ReleaseBesidePresent(
        Scheduler::submit_mutex,
        [this] {
            (void)instance.GetPresentQueue().waitIdle();
            (void)instance.GetGraphicsQueue().waitIdle();
        },
        [&dlss] { dlss.ReleaseFrameGen(); });
}

void FrameGeneration::ReleaseAll() {
    // Submitted frames may still use them; the open command buffer does not.
    DestroyContext();
    if (provider == BbFrameGenPace::ProviderDlss) {
        if (Dlss* dlss = Dlss::Get()) {
            ReleaseDlssFrameGen(*dlss);
        }
    }
    provider = 0;
    if (!depth_copy && !hudless_raw && !motion_view) {
        return;
    }
    const auto device = instance.GetDevice();
    scheduler.DeferOperation([device, depth = std::move(depth_copy), depth_view = depth_copy_view,
                              hudless = std::move(hudless_raw), views = std::move(hudless_views),
                              motion = motion_view] {
        for (const auto& entry : views) {
            device.destroyImageView(entry.view);
        }
        if (depth_view) {
            device.destroyImageView(depth_view);
        }
        if (motion) {
            device.destroyImageView(motion);
        }
    });
    depth_copy = VideoCore::UniqueImage{};
    depth_copy_view = vk::ImageView{};
    depth_copy_format = {};
    depth_copy_width = depth_copy_height = 0;
    hudless_raw = VideoCore::UniqueImage{};
    hudless_views.clear();
    hudless_format = {};
    hudless_width = hudless_height = 0;
    motion_image = vk::Image{};
    motion_view = vk::ImageView{};
    std::printf("Frame generation: off, its memory freed\n");
}

int FrameGeneration::ChooseProvider() {
    auto& settings = BbSettings::Get();
    if (settings.frame_gen_provider.load(std::memory_order_relaxed) != BbSettings::FrameGenDlss) {
        dlss_failed = false; // tried again when it is chosen again
        return BbFrameGenPace::ProviderFsr3;
    }
    if (dlss_failed) {
        return BbFrameGenPace::ProviderFsr3;
    }
    if (DlssMaxCount() < 1) {
        dlss_failed = true;
        settings.frame_gen_problem = "DLSS Frame Generation needs a GeForce RTX 40 or newer";
        std::printf("Frame generation: DLSS Frame Generation unavailable; using FSR 3.1\n");
        return BbFrameGenPace::ProviderFsr3;
    }
    return BbFrameGenPace::ProviderDlss;
}

void FrameGeneration::OutOfMemory() {
    failed = true;
    failed_idle = BbFrameGenPace::IdleNoMemory;
    SetIdle(failed_idle);
    history_broken = true;
    BbSettings::Get().frame_gen_problem = "Not enough video memory for the generated frames";
    Report("off (not enough video memory for the generated frames)");
}

FrameGeneration::Plan FrameGeneration::PlanFrame(u32 width, u32 height, vk::Format format,
                                                 bool hdr, vk::Extent2D display) {
    Plan plan;
    current = std::exchange(pending, Pending{});
    auto& settings = BbSettings::Get();
    const bool switched_on = settings.frame_generation.load(std::memory_order_relaxed);
    if (!switched_on || !Requested()) {
        if (was_requested) {
            was_requested = false;
            ReleaseAll();
            failed = false;
            dlss_failed = false;
            paused = false;
            history_broken = true;
            settings.frame_gen_problem = nullptr;
            plan.off = true;
        }
        SetIdle(BbFrameGenPace::IdleStarting);
        if (switched_on) {
            Report("off (no temporal upscaler selected)");
        } else {
            reported.clear();
        }
        return plan;
    }
    if (!was_requested) {
        was_requested = true;
        failed = false;
        failed_idle = BbFrameGenPace::IdleFailed;
        history_broken = true;
    }
    if (failed) {
        SetIdle(failed_idle);
        skipped.fetch_add(1, std::memory_order_relaxed);
        return plan;
    }
    if (hdr) {
        Report("off (HDR output)");
        history_broken = true;
        return plan;
    }
    if (!current.valid) {
        // Menus, loading screens, movies: no scene to interpolate.
        SetIdle(BbFrameGenPace::IdleNoScene);
        history_broken = true;
        skipped.fetch_add(1, std::memory_order_relaxed);
        return plan;
    }
    if (current.width > width || current.height > height) {
        Report("off (the scene is rendered larger than the window)");
        SetIdle(BbFrameGenPace::IdleSceneLarger);
        history_broken = true;
        skipped.fetch_add(1, std::memory_order_relaxed);
        return plan;
    }
    // The provider of this frame; a change restarts the history and frees the other one's.
    const int chosen = ChooseProvider();
    if (chosen != provider) {
        if (provider == BbFrameGenPace::ProviderFsr3) {
            DestroyContext();
        } else if (provider == BbFrameGenPace::ProviderDlss) {
            if (Dlss* dlss = Dlss::Get()) {
                ReleaseDlssFrameGen(*dlss);
            }
        }
        provider = chosen;
        history_broken = true;
        std::printf("Frame generation: %s\n",
                    provider == BbFrameGenPace::ProviderDlss ? "DLSS Frame Generation"
                                                             : "FSR 3.1 frame interpolation");
    }
    if (provider == BbFrameGenPace::ProviderFsr3) {
        if (!context || context_width != width || context_height != height ||
            context_format != format) {
            if (context) {
                DestroyContext();
            }
            if (!CreateContext(width, height, format)) {
                failed = true;
                SetIdle(failed_idle);
                return plan;
            }
            history_broken = true;
        } else if (reported != "on") {
            Report("on");
        }
    } else {
        if (!ChooseOutputFormat(format)) {
            failed = true;
            SetIdle(failed_idle);
            return plan;
        }
        if (reported != "on (DLSS)") {
            Report("on (DLSS)");
        }
    }
    // As many as the settings ask for, within what the display has room for (swap thread).
    const u32 wanted = BbFrameGenPace::WantedCount(
        provider, settings.frame_gen_multiplier.load(std::memory_order_relaxed),
        provider == BbFrameGenPace::ProviderDlss ? DlssMaxCount() : 0);
    const u32 count = std::min(wanted, display_limit.load(std::memory_order_relaxed));
    if (count == 0) {
        SetIdle(display_paused.load(std::memory_order_relaxed) ? BbFrameGenPace::IdlePresentBehind
                                                               : BbFrameGenPace::IdleNoRoom);
        if (!paused) {
            paused = true;
            std::printf("Frame generation: paused, no room on the display at this frame rate\n");
        }
        history_broken = true;
        skipped.fetch_add(1, std::memory_order_relaxed);
        return plan;
    }
    if (paused) {
        paused = false;
        std::printf("Frame generation: active again, %u per game frame\n", count);
    }
    while (ticks.size() >= kFramesInFlight) {
        scheduler.Wait(ticks.front());
        ticks.pop_front();
    }
    SetIdle(BbFrameGenPace::IdleNone);
    plan.generate = true;
    plan.count = count;
    reset_frame = current.reset || history_broken;
    history_broken = false;
    plan.present = !reset_frame;
    if (!plan.present) {
        skipped.fetch_add(1, std::memory_order_relaxed);
    }
    plan.hudless = current.hudless && hudless_raw && current.hudless_width == display.width &&
                   current.hudless_height == display.height;
    return plan;
}

FrameGeneration::Hudless FrameGeneration::HudlessView(vk::Format view_format) {
    for (const auto& entry : hudless_views) {
        if (entry.format == view_format) {
            return {entry.view, {hudless_width, hudless_height}};
        }
    }
    const vk::ImageView view = Check(instance.GetDevice().createImageView({
        .image = vk::Image(hudless_raw),
        .viewType = vk::ImageViewType::e2D,
        .format = view_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    hudless_views.push_back({view_format, view});
    return {view, {hudless_width, hudless_height}};
}

u32 FrameGeneration::Record(vk::CommandBuffer cmdbuf, const Targets& targets) {
    if (targets.count == 0) {
        return 0;
    }
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    // The host passes of the frame (and the last frames' reads) come first.
    const vk::MemoryBarrier2 before{
        .srcStageMask = all,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
    };
    std::array<vk::ImageMemoryBarrier2, MaxGenerated> outputs_to_general{};
    const u32 count = std::min(targets.count, MaxGenerated);
    for (u32 i = 0; i < count; ++i) {
        outputs_to_general[i] = {
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = targets.outputs[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                             .pMemoryBarriers = &before,
                             .imageMemoryBarrierCount = count,
                             .pImageMemoryBarriers = outputs_to_general.data()});
    u32 made = 0;
    if (provider == BbFrameGenPace::ProviderDlss) {
        made = RecordDlss(cmdbuf, targets);
        if (made == 0) {
            // FSR 3.1 takes over from the next frame (ChooseProvider).
            dlss_failed = true;
            history_broken = true;
            BbSettings::Get().frame_gen_problem = "DLSS Frame Generation failed; FSR 3.1 is used";
        }
    } else {
        made = RecordFsr(cmdbuf, targets) ? 1 : 0;
    }
    if (made == 0) {
        return 0;
    }
    ++frame_id;
    const vk::MemoryBarrier2 after{
        .srcStageMask = all,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    return made;
}

vk::ImageView FrameGeneration::MotionView(vk::Image image) {
    if (image == motion_image && motion_view) {
        return motion_view;
    }
    const auto device = instance.GetDevice();
    if (motion_view) {
        scheduler.DeferOperation([device, view = motion_view] { device.destroyImageView(view); });
    }
    motion_image = image;
    motion_view = Check(device.createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    return motion_view;
}

u32 FrameGeneration::RecordDlss(vk::CommandBuffer cmdbuf, const Targets& targets) {
    Dlss* dlss = Dlss::Get();
    if (!dlss || dlss->FrameGenMaxCount() < 1 || !depth_copy_view || !targets.current_view) {
        std::printf("Frame generation: DLSS Frame Generation is unavailable; using FSR 3.1\n");
        return 0;
    }
    const Dlss::FrameGenDesc desc{targets.width, targets.height, targets.format, current.width,
                                  current.height};
    if (!dlss->HasFrameGen(desc)) {
        // The previous feature may still be used by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        dlss->ReleaseFrameGen();
        if (!dlss->CreateFrameGen(cmdbuf, desc)) {
            std::printf("Frame generation: DLSS Frame Generation feature creation failed (%ux%u "
                        "%s); using FSR 3.1\n",
                        targets.width, targets.height, vk::to_string(targets.format).c_str());
            return 0;
        }
        std::printf("Frame generation: DLSS Frame Generation %ux%u (depth and motion %ux%u)\n",
                    targets.width, targets.height, current.width, current.height);
        reset_frame = true;
    }
    const auto resource = [](vk::Image image, vk::ImageView view, vk::Format format,
                             vk::ImageAspectFlags aspect, u32 w, u32 h) {
        return Dlss::Resource{image, view, format, aspect, w, h};
    };
    const auto color = vk::ImageAspectFlagBits::eColor;
    const u32 count = std::min(targets.count, MaxGenerated);
    for (u32 index = 1; index <= count; ++index) {
        Dlss::FrameGenFrame frame{
            .backbuffer = resource(targets.current, targets.current_view, targets.format, color,
                                   targets.width, targets.height),
            .depth = resource(vk::Image(depth_copy), depth_copy_view, depth_copy_format,
                              vk::ImageAspectFlagBits::eDepth, depth_copy_width, depth_copy_height),
            .motion = resource(current.motion, MotionView(current.motion),
                               vk::Format::eR16G16Sfloat, color, current.width, current.height),
            .output = resource(targets.outputs[index - 1], targets.output_views[index - 1],
                               output_format, color, targets.width, targets.height),
            .hudless = targets.hudless ? resource(targets.hudless, targets.hudless_view,
                                                  targets.format, color, targets.width,
                                                  targets.height)
                                       : Dlss::Resource{},
            .jitter_x = current.jitter[0],
            .jitter_y = current.jitter[1],
            .camera_near = current.camera_near,
            .camera_far = current.camera_far,
            .camera_fov = current.camera_fov,
            .depth_inverted = depth_inverted,
            .has_matrices = current.has_matrices,
            .view_to_clip = current.view_to_clip,
            .clip_to_prev_clip = current.clip_to_prev_clip,
            .prev_clip_to_clip = current.prev_clip_to_clip,
            .reset = reset_frame,
            .count = count,
            .index = index,
        };
        if (!dlss->EvaluateFrameGen(cmdbuf, frame)) {
            std::printf("Frame generation: DLSS Frame Generation evaluation failed; using FSR "
                        "3.1\n");
            return 0;
        }
    }
    return count;
}

bool FrameGeneration::RecordFsr(vk::CommandBuffer cmdbuf, const Targets& targets) {
    const bool hudless = bool(targets.hudless);
    const auto sampled = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;
    const auto create_info = MakeCreateInfo(context_width, context_height, context_format,
                                            output_format);
    const FfxVkPortableImage current_color =
        Describe(targets.current, targets.format, targets.width, targets.height, sampled,
                 vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    const FfxVkPortableImage hudless_color =
        hudless ? Describe(targets.hudless, targets.format, targets.width, targets.height,
                           sampled, vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ)
                : Absent();
    const FfxVkPortableImage& source = hudless ? hudless_color : current_color;

    FfxVkPortableFrameGenerationPrepareInfo prepare{};
    prepare.structSize = sizeof(prepare);
    prepare.commandBuffer = cmdbuf;
    prepare.depth = Describe(vk::Image(depth_copy), depth_copy_format, depth_copy_width,
                             depth_copy_height,
                             vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                             vk::ImageAspectFlagBits::eDepth,
                             FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.motionVectors = Describe(current.motion, vk::Format::eR16G16Sfloat, current.width,
                                     current.height,
                                     vk::ImageUsageFlagBits::eStorage |
                                         vk::ImageUsageFlagBits::eSampled,
                                     vk::ImageAspectFlagBits::eColor,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.renderSize = {current.width, current.height};
    prepare.jitterOffset = {current.jitter[0], current.jitter[1]};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeMilliseconds = current.frame_ms;
    prepare.cameraNear = current.camera_near;
    prepare.cameraFar = current.camera_far;
    prepare.cameraVerticalFovRadians = current.camera_fov;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f;
    prepare.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    // The library (1.1.4) only checks that the camera is given; the port tracks no basis.
    prepare.cameraUp = {0.0f, 1.0f, 0.0f};
    prepare.cameraRight = {1.0f, 0.0f, 0.0f};
    prepare.cameraForward = {0.0f, 0.0f, 1.0f};
    prepare.reset = reset_frame ? VK_TRUE : VK_FALSE;
    prepare.frameId = frame_id;

    FfxVkPortableFrameGenerationDispatchInfo dispatch{};
    dispatch.structSize = sizeof(dispatch);
    dispatch.commandBuffer = cmdbuf;
    dispatch.currentColor = current_color;
    dispatch.hudlessColor = hudless_color;
    dispatch.distortionField = Absent();
    dispatch.output = Describe(targets.outputs[0], output_format, targets.width, targets.height,
                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.displaySize = {targets.width, targets.height};
    dispatch.interpolationRect = {0, 0, targets.width, targets.height};
    dispatch.frameTimeMilliseconds = current.frame_ms;
    dispatch.cameraNear = current.camera_near;
    dispatch.cameraFar = current.camera_far;
    dispatch.cameraVerticalFovRadians = current.camera_fov;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.minLuminance = 0.0f;
    dispatch.maxLuminance = 1.0f;
    dispatch.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    dispatch.reset = reset_frame ? VK_TRUE : VK_FALSE;
    dispatch.frameId = frame_id;

    auto refused = [&](const char* what, u64 issues) {
        PrintIssues(what, issues);
        failed = true;
        return false;
    };
    // The command buffer is real here: the checks see the same structs the library gets.
    if (const u64 issues = ffxVkPortableValidateFrameGenerationPrepareInfo(&create_info, &prepare)) {
        return refused("prepare", issues);
    }
    if (const u64 issues = ffxVkPortableValidateFrameGenerationDispatchInfo(&create_info, &dispatch)) {
        return refused("dispatch", issues);
    }
    if (const auto result = ffxVkPortableFrameGenerationContextPrepare(context, &prepare, &source);
        result != FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: off (optical flow failed, error %d)\n", int(result));
        failed = true;
        return false;
    }
    if (const auto result = ffxVkPortableFrameGenerationContextRecordDispatch(context, &dispatch);
        result != FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: off (interpolation failed, error %d)\n", int(result));
        failed = true;
        return false;
    }
    return true;
}

void FrameGeneration::EndFrame(u64 submission_tick) {
    ticks.push_back(submission_tick);
}

// ---- Pacing and statistics (swap thread) ----------------------------------------------------

float FrameGeneration::OnPresent(u32 generated) {
    const auto now = std::chrono::steady_clock::now();
    if (last_present.time_since_epoch().count() != 0) {
        const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
        // A frame of a menu or a hitch is not the rate the generated frames are placed by.
        const float clamped = std::clamp(ms, 1.0f, 33.0f);
        const float previous = base_interval_ms.load(std::memory_order_relaxed);
        base_interval_ms.store(previous > 0.0f ? previous * 0.9f + clamped * 0.1f : clamped,
                               std::memory_order_relaxed);
    }
    last_present = now;
    ++stats_base;
    return generated ? std::clamp(base_interval_ms.load(std::memory_order_relaxed), 1.0f, 33.0f)
                     : 0.0f;
}

void FrameGeneration::PrintStats(std::chrono::steady_clock::time_point now) {
    static const bool stats = EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false);
    const double seconds = std::chrono::duration<double>(now - stats_start).count();
    if (!stats || seconds < 5.0) {
        return;
    }
    if (BbSettings::Get().frame_generation.load(std::memory_order_relaxed)) {
        std::printf("Frame generation: %.1f generated/s (%.1f shown), %.1f presented/s, base %.1f "
                    "FPS, %u skipped\n",
                    stats_generated / seconds, stats_shown / seconds, stats_images / seconds,
                    stats_base / seconds, skipped.exchange(0, std::memory_order_relaxed));
    }
    stats_start = now;
    stats_base = stats_images = stats_generated = stats_shown = 0;
}

void FrameGeneration::OnSwitchedOff() {
    auto& settings = BbSettings::Get();
    if (settings.fg_active.load(std::memory_order_relaxed)) {
        settings.fg_active.store(false, std::memory_order_relaxed);
    }
    settings.frame_gen_idle.store(BbFrameGenPace::IdleStarting, std::memory_order_relaxed);
    overlay = summary = {};
    overlay_start = summary_start = {};
    tracker.Reset();
}

void FrameGeneration::OnPresented(u32 generated, u32 shown, u32 starved) {
    const auto now = std::chrono::steady_clock::now();
    if (stats_start.time_since_epoch().count() == 0) {
        stats_start = now;
    }
    if (overlay_start.time_since_epoch().count() == 0) {
        overlay_start = summary_start = now;
    }
    stats_images += generated + 1;
    stats_generated += generated;
    stats_shown += shown;
    PrintStats(now);
    for (auto* window : {&overlay, &summary}) {
        ++window->game;
        window->generated += generated;
        window->shown += shown;
        window->starved += starved;
    }
    // Twice a second: the menu's status and the FPS counter's "120 FPS (game 60, frame gen 2x)",
    // from the generated frames that reached the display, not the ones handed to the driver.
    auto& settings = BbSettings::Get();
    overlay.seconds = std::chrono::duration<double>(now - overlay_start).count();
    if (overlay.seconds >= 0.5) {
        const auto status =
            BbFrameGenPace::Judge(overlay, plan_idle.load(std::memory_order_relaxed));
        settings.fg_active.store(status.active, std::memory_order_relaxed);
        settings.fg_base_fps.store(status.game_fps, std::memory_order_relaxed);
        settings.fg_presented_fps.store(status.shown_fps, std::memory_order_relaxed);
        settings.frame_gen_idle.store(status.idle, std::memory_order_relaxed);
        if (tracker.Update(status)) {
            if (status.active) {
                std::printf("Frame generation: active, %dx (game %.0f FPS, shown %.0f FPS)\n",
                            status.multiplier, status.game_fps, status.shown_fps);
            } else {
                std::printf("Frame generation: switched on, not generating: %s\n",
                            BbFrameGenPace::IdleText(status.idle));
            }
        }
        overlay = {};
        overlay_start = now;
    }
    // Every 30 seconds, beside the "Perf:" line, so a player's log says what it really did.
    summary.seconds = std::chrono::duration<double>(now - summary_start).count();
    if (summary.seconds >= 30.0) {
        const auto status =
            BbFrameGenPace::Judge(summary, plan_idle.load(std::memory_order_relaxed));
        std::printf("Frame generation: %.1f generated/s made, %.1f shown, %.1f cut short (no free "
                    "presentation frame), game %.1f FPS, shown %.1f FPS over %.0f s (%s)\n",
                    summary.generated / summary.seconds, summary.shown / summary.seconds,
                    summary.starved / summary.seconds, status.game_fps, status.shown_fps,
                    summary.seconds, BbFrameGenPace::IdleText(status.idle));
        summary = {};
        summary_start = now;
    }
}

} // namespace Vulkan
