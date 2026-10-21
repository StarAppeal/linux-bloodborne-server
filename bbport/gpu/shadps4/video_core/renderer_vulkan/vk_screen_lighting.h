// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: screen-space lighting (Kaniel 2026-10-19, "maybe there is a chance to add ray
// tracing"): ambient occlusion and one bounce of indirect light, added to Bloodborne's lit opaque
// scene (the RGBA16F scene color) before its transparents, fog and post-processing, so they and
// the HUD stay as the game draws them. Like ReShade's screen-space GI, but with the scene's own
// depth and albedo (the G-buffer's sRGB target) and in HDR.
//
// Two dispatches (host_shaders/screen_lighting.comp): a trace at half (low) or full (high) size
// into an RGBA16F image (rgb light, a visibility), then a depth-aware gather at full size that
// writes color * visibility + albedo * light into the scene color. bbport.ini screen_lighting
// off|low|high, screen_lighting_ao, screen_lighting_gi (BbSettings). BB_LIGHTING_VIEW=1 shows the
// visibility alone, 2 the bounced light.
//
// Checks without the menu: BB_LIGHTING_FILE=<file> holding "quality [view [rays]]" (0 off, 1 low, 2 high;
// view as BB_LIGHTING_VIEW; rays 0/1 ray_traced_ao), read about once a second. BB_LIGHTING_DUMP=<file>: creating it
// dumps the next lit frame's G-buffer targets, depth, scene color before and after and the trace
// as raw files into BB_DUMP_DIR (vk_image_dump.h); at native scene size only.

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;
class RayOcclusion;

class ScreenLighting {
public:
    ScreenLighting(const Instance& instance, Scheduler& scheduler,
                   VideoCore::TextureCache& texture_cache, Runtime& runtime,
                   CameraMotion& camera_motion, SceneTargets& scene_targets);
    ~ScreenLighting();

    /// The G-buffer's albedo target: its one sRGB format.
    static bool IsAlbedoFormat(vk::Format format);
    /// The G-buffer's normals: its first target, RGBA8 (rgb * 2 - 1, view space).
    static bool IsNormalFormat(vk::Format format) {
        return format == vk::Format::eR8G8B8A8Unorm;
    }

    /// The menu has it on (and the device can run it).
    [[nodiscard]] bool Enabled() const;

    /// A new frame begins (the display pass).
    void OnFrameStart();
    /// The color targets of the G-buffer pass with the scene depth (for BB_LIGHTING_DUMP).
    void OnGBufferTarget(u32 index, VideoCore::ImageId id) noexcept {
        if (index < gbuffer.size()) {
            gbuffer[index] = id;
        }
    }
    /// Frames seen (OnFrameStart), for once-in-a-while logs.
    [[nodiscard]] u64 FrameNumber() const noexcept {
        return frames_seen;
    }
    [[nodiscard]] bool DumpDue() const noexcept {
        return dump_due;
    }
    /// The G-buffer pass with the scene depth: its sRGB target is the albedo.
    void OnAlbedo(VideoCore::ImageId albedo_id) noexcept {
        albedo = albedo_id;
    }
    /// The lighting pass's two B10G11R11 targets (the lamps' light).
    void OnLightTargets(VideoCore::ImageId first, VideoCore::ImageId second) noexcept {
        light_targets = {first, second};
    }
    /// A lighting draw's lamp constants (a ring buffer of 144 bytes a lamp; vk_rasterizer): the
    /// frame's lamps, each once (Kaniel 2026-10-21, "do all steps": ray-traced lamp shadows).
    void OnLampConstants(const float* data, u32 size);
    /// The lighting pass is done (the first draw into the scene color): with ray_traced_ao the
    /// lamps' shadows are traced into its two targets before the game combines them. Once a frame.
    void OnLightingDone();
    /// The same pass's first target: the normals.
    void OnNormals(VideoCore::ImageId normals_id) noexcept {
        normals = normals_id;
    }
    /// The scene color target (RGBA16F drawn with the scene depth).
    void OnSceneColor(VideoCore::ImageId color) noexcept {
        scene_color = color;
    }
    /// The opaque scene is complete: the first transparent draw, or else (fallback) the compute
    /// shader that starts the post-processing. Once a frame. `temporal`: an upscaler accumulates
    /// frames, so the noise may change every frame.
    void OnOpaqueSceneDone(bool temporal);

private:
    struct ViewEntry {
        vk::Image image{};
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view{};
        u64 last_use = 0;
    };
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);
    bool EnsureTraceImage(u32 width, u32 height);
    void CreatePipelines();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;

    bool supported = true;
    bool done = false;
    bool logged = false;
    int view_mode = 0;
    u32 frame = 0;
    u32 frames_seen = 0;
    bool dump_due = false;
    int dumps = 0;
    std::array<VideoCore::ImageId, 8> gbuffer{};
    VideoCore::ImageId scene_color{};
    VideoCore::ImageId albedo{};
    VideoCore::ImageId normals{};
    std::array<VideoCore::ImageId, 2> light_targets{};
    std::vector<std::array<float, 16>> lamps; ///< this frame's, as the lighting pass has them
    bool lamps_done = false;
    bool lamps_logged = false;
    struct LampBuffer {
        vk::Buffer buffer{};
        void* allocation{}; ///< VmaAllocation
        void* mapped = nullptr;
    };
    std::array<LampBuffer, 3> lamp_buffers{}; ///< written in turn (frames in flight)
    u32 lamp_buffer_next = 0;
    vk::UniqueDescriptorSetLayout lamp_desc_layout;
    vk::UniquePipelineLayout lamp_layout;
    vk::UniquePipeline lamp_pipeline;

    vk::UniqueDescriptorSetLayout trace_desc_layout, apply_desc_layout;
    vk::UniquePipelineLayout trace_layout, apply_layout;
    vk::UniquePipeline trace_pipeline, apply_pipeline;
    /// Kaniel 2026-10-21: with ray_traced_ao, the trace's occlusion rays (vk_ray_occlusion.h).
    std::unique_ptr<RayOcclusion> rays;
    vk::UniqueDescriptorSetLayout trace_rt_desc_layout;
    vk::UniquePipelineLayout trace_rt_layout;
    vk::UniquePipeline trace_rt_pipeline;
    bool rays_logged = false;
    VideoCore::UniqueImage trace_image;
    vk::UniqueImageView trace_view;
    u32 trace_width = 0, trace_height = 0;
    std::array<ViewEntry, 6> view_cache{};
    u64 view_uses = 0;
};

} // namespace Vulkan
