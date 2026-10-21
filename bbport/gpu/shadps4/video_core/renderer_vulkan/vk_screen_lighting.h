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
        light_pass_seen = true;
    }
    /// A colour draw into `target` that is not a lighting draw: the first after this frame's
    /// lighting pass is the game's combine of its two targets, so OnLightingDone goes first
    /// (2026-10-21: at the first scene-colour draw with the scene depth it came after the
    /// combine, and the ray-traced shadows and bounce never reached the picture).
    void OnColorDraw(VideoCore::ImageId target) {
        if (light_pass_seen && !lamps_done && target != light_targets[0] &&
            target != light_targets[1]) {
            OnLightingDone();
        }
    }
    /// A lighting draw's lamp constants (a ring buffer of 144 bytes a lamp; vk_rasterizer): the
    /// frame's lamps, each once (Kaniel 2026-10-21, "do all steps": ray-traced lamp shadows).
    void OnLampConstants(const float* data, u32 size);
    /// The lighting pass is done (the first draw into the scene color): with ray_traced_ao the
    /// lamps' shadows are traced into its two targets before the game combines them. Once a frame.
    void OnLightingDone();
    /// A lighting draw's scene constants: the sun/moon's direction (80-82, view space, toward it)
    /// and colour (76-78), kept when the direction is a unit vector.
    void OnSunConstants(const float* data);
    /// Step 3: whether the rays want this frame's characters (ray_traced_ao on, the map built).
    [[nodiscard]] bool WantsCharacters() const;
    /// A skinned draw's triangles (its clip positions at element `store` of the object motion's
    /// positions, `vertices` from `first_vertex`; its index list as the game has it).
    /// `pass_depth`: the G-buffer pass's depth; a draw of another pass than the last drops the
    /// earlier pass's (the frame's camera goes with the last pass, CameraMotion).
    void OnCharacterDraw(u32 store, u32 first_vertex, u32 vertices, const void* indices,
                         u32 index_size, u32 count, s32 base_vertex,
                         VideoCore::ImageId pass_depth);
    /// The object motion's positions buffer (vk_rasterizer sets it).
    void SetCharacterPositions(vk::Buffer buffer) noexcept {
        character_positions = buffer;
    }
    /// The same pass's first target: the normals.
    void OnNormals(VideoCore::ImageId normals_id) noexcept {
        normals = normals_id;
    }
    /// The same pass's second target (red: gloss) and fourth (rgb: specular colour), for the
    /// ray-traced reflections.
    void OnGlossTargets(VideoCore::ImageId gloss_id, VideoCore::ImageId f0_id) noexcept {
        gloss = gloss_id;
        specular_colour = f0_id;
    }
    /// The scene color target (RGBA16F drawn with the scene depth).
    void OnSceneColor(VideoCore::ImageId color) noexcept {
        scene_color = color;
    }
    /// The opaque scene is complete: the first transparent draw, or else (fallback) the compute
    /// shader that starts the post-processing. Once a frame. `temporal`: an upscaler accumulates
    /// frames, so the noise may change every frame.
    void OnOpaqueSceneDone(bool temporal);
    /// Part 2: the lit opaque scene into the fourth bounce image (General layout) for the next
    /// frame's reflections.
    void KeepLastScene(const VideoCore::Image& color, bool reduced, u32 w, u32 h);
    /// Kaniel 2026-10-21 ("improve the fog"): the moon's and lamps' light in the air, shafts
    /// cut by the ray scene, into the opaque scene (General layout) before the game's fog.
    void RecordFogLight(vk::ImageView color_view, vk::ImageView depth_view, u32 w, u32 h);

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
    VideoCore::ImageId gloss{}, specular_colour{};
    std::array<VideoCore::ImageId, 2> light_targets{};
    std::vector<std::array<float, 16>> lamps; ///< this frame's, as the lighting pass has them
    bool lamps_done = false;
    bool light_pass_seen = false; ///< this frame's lighting pass has begun
    std::array<float, 4> sun_dir{};    ///< view space, toward the sun; w 1 when this frame has it
    std::array<float, 4> sun_colour{};
    std::vector<u32> character_source;  ///< step 3: a corner's element in the positions
    std::vector<u32> character_indices; ///< three corners (of character_source) a triangle
    std::vector<u32> character_draw_of; ///< a corner's draw (its own depth scale)
    u32 character_draws = 0;
    vk::Buffer character_positions{};
    VideoCore::ImageId character_pass{};
    bool lamps_logged = false;
    bool sun_logged = false; ///< whether the last log line had a sun
    u32 sun_age = 0;          ///< frames since the sun draw was last seen
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
    // The fog light: this frame's lamp buffer (the lamp pass's) and its pipeline.
    vk::Buffer fog_lamps{};
    u32 fog_lamp_count = 0;
    u32 fog_frame = ~0u; ///< frames_seen when fog_lamps is this frame's
    vk::UniqueDescriptorSetLayout fog_desc_layout;
    vk::UniquePipelineLayout fog_layout;
    vk::UniquePipeline fog_pipeline, fog_apply_pipeline;
    /// The march's half-resolution result (light rgb, transmittance), for fog_apply.
    VideoCore::UniqueImage fog_image;
    vk::UniqueImageView fog_view;
    u32 fog_width = 0, fog_height = 0;
    /// Step 2c: this frame's bounce and two histories (ping-pong), RGBA16F at light0's size;
    /// fourth, the last frame's opaque scene (the reflections' hits on screen).
    std::array<VideoCore::UniqueImage, 4> bounce_images;
    std::array<vk::UniqueImageView, 4> bounce_views;
    bool last_scene_valid = false;
    u32 bounce_width = 0, bounce_height = 0;
    u32 history_next = 0;  ///< which history the resolve writes (1 or 2)
    bool history_valid = false;
    vk::UniqueDescriptorSetLayout resolve_desc_layout;
    vk::UniquePipelineLayout resolve_layout;
    vk::UniquePipeline resolve_pipeline;

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
