// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deque>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/hdr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_hdr.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    /// bbport: frame generation: the generated frames shown right before this one (Present),
    /// oldest first, the first `generated_count` of them.
    std::array<Frame*, FrameGeneration::MaxGenerated> generated{};
    u32 generated_count{0};
    /// bbport: HDR output: the SceneHdr gain image of this frame (an interpolated frame: of the
    /// frame after it), -1 for none.
    int gain_slot{-1};
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    /// The game's own HDR request (VideoOut colorimetry). bbport: Bloodborne renders SDR and
    /// never asks; HDR output follows the port's setting instead (UpdateHdrOutput).
    void SetHDR(bool) {}

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    /// bbport: waits until the GPU has finished the commands that render `frame` (any thread).
    void WaitRendered(const Frame* frame);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();
    void AddPresentFrames(u32 count);

    /// `may_fail`: false instead of stopping when video memory is full (generated frames).
    bool RecreateFrame(Frame* frame, u32 width, u32 height, vk::Format format = {},
                       vk::ImageUsageFlags extra_usage = {}, bool may_fail = false);
    void DestroyFrameImage(Frame& frame);

    /// One image of the swapchain: the frame blitted, the overlay drawn over it, presented.
    void PresentFrame(Frame* frame, bool is_reusing_frame, bool is_game_frame);
    /// bbport: HDR output on or off as the setting and the display allow (swapchain recreated).
    void UpdateHdrOutput();
    /// bbport: the overlay and HDR encode of a frame composed into hdr_pass.Game().
    void RecordHdrOutput(vk::CommandBuffer cmdbuf, Frame* frame, vk::Extent2D extent, bool overlay,
                         std::optional<std::string> shot);
    /// bbport: the next image of the ring of generated frames, free and of this size; null
    /// when video memory is full. `ring`: the entries in use (the rest is freed).
    Frame* AcquireGeneratedFrame(u32 width, u32 height, vk::Format format, u32 ring);
    /// bbport: frame generation was switched off: the generated frames and the frame without
    /// UI are freed once the swap thread has shown (or dropped) the last of them.
    void ReleaseFrameGenImages();
    /// bbport: swap thread: the generated frames of `frame` evenly over the game's frame
    /// interval, then the frame (Present with frame generation on).
    void PresentGenerated(Frame* frame, const std::array<Frame*, FrameGeneration::MaxGenerated>& generated,
                          u32 count);
    /// bbport: the frame without UI, made by the host passes like the presented one.
    Frame* AcquireHudlessFrame(u32 width, u32 height, vk::Format format);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::HdrOutputPass hdr_pass;
    bool last_display_hdr{false}; ///< UpdateHdrOutput: the display's HDR state last seen
    u32 hdr_unsupported_presents{0}; ///< UpdateHdrOutput: presents since HDR became unavailable
    HostPasses::PostProcessingPass pp_pass;
    std::mutex passes_mutex; ///< bbport: the passes are recorded on the recording threads
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    /// bbport: FSR 3.1 frame generation; before the rasterizer, whose upscaler feeds it.
    FrameGeneration frame_generation;
    SceneHdr scene_hdr;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    /// bbport: presentation frames made at start (the swapchain's images); frame generation adds
    /// up to BbFrameGenPace::PresentFramesNeeded (AddPresentFrames).
    u32 base_frame_count{0};
    /// bbport: generated frames (Presenter::Present shows them before their game frame) and the
    /// game frame without UI; their images exist only while frame generation is on. Four game
    /// frames' worth of the ring are in use (4 x the frames generated per game frame).
    std::array<Frame, 4 * FrameGeneration::MaxGenerated> generated_frames{};
    u32 next_generated{0};
    Frame hudless_frame{};
    /// Game frames handed to the swap thread with generated frames, and those it has shown or
    /// dropped (ReleaseFrameGenImages waits for them to match).
    u64 generated_sent{0};
    std::atomic<u64> generated_done{0};
    /// Swap thread: how many generated frames the display has room for (fit to display, V-Sync).
    BbFrameGenPace::Limiter fg_limiter;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
};

} // namespace Vulkan
