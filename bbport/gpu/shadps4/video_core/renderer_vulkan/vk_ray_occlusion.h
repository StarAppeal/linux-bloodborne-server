// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced ambient occlusion for the screen lighting (Kaniel 2026-10-21, "continue with
// ray tracing"). The game never hands the renderer its world, only each frame's draws, so the
// rays go against the map's own models as the 3D map reads them from the game's files
// (BbMapCharts::GetMesh: the area's map pieces, thinned on a 0.5 m grid). Their coordinates are
// the game's world space on that map: the scene constants' inverse view puts the frame's depth
// within ~0.2 m of them (capture of 2026-10-21 in Central Yharnam).
//
// One bottom-level acceleration structure a map (the player's, bbcoop_map_view) and a top level
// with it alone, built once when the map's mesh is ready and kept until the player is on another
// map. Characters, enemies and small props are not in it: the screen-space trace still darkens
// their contacts; the rays add what the screen cannot see (walls behind the camera, ceilings,
// alcoves). Needs Vulkan ray queries (Instance::IsRayQueryEnabled: bbport.ini ray_traced_ao at
// start).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace BbMapArt {
struct AreaMesh;
}

namespace Vulkan {

class Instance;
class Scheduler;

class RayOcclusion {
public:
    RayOcclusion(const Instance& instance, Scheduler& scheduler);
    ~RayOcclusion();

    /// Once a frame while the screen lighting runs with ray_traced_ao: follows the player's map,
    /// asks for its mesh and builds its acceleration structures once the mesh is there.
    void Update();
    /// The top level of the player's map once built (else null): for the trace's ray queries.
    [[nodiscard]] vk::AccelerationStructureKHR Scene() const noexcept {
        if (built_map == 0 || built_map != map) {
            return {};
        }
        // A frame without its own build keeps the last one a few frames (their shadows blinked).
        return frame_tlas && frame_scene_frame != ~0u && frames - frame_scene_frame <= HoldFrames
                   ? *frame_tlas
                   : *tlas;
    }
    /// Kaniel 2026-10-21 ("do all steps", step 3): this frame's characters (their corners'
    /// clip positions in `positions` at the elements of `source`, three of those a triangle in
    /// `indices`) built into a second instance (custom index 1) beside the map; Scene() is then
    /// both, until the next frame. Records on the scheduler (before what traces them).
    /// BB_LIGHTING_DUMP: this frame's character corners (x, y, z floats, world) into `path`.
    void DumpCharacters(const std::string& path);
    /// `draw_of`: each corner's draw (of `draws`); a draw's clip depth carries a scale of its
    /// own, found against `depth` (the scene depth, General layout, `width` x `height`).
    void BuildCharacters(vk::Buffer positions, const std::vector<u32>& source,
                         const std::vector<u32>& indices, const std::vector<u32>& draw_of,
                         u32 draws, vk::ImageView depth, u32 width, u32 height,
                         const std::array<float, 4>& proj, const std::array<float, 12>& world);
    static constexpr u32 MaxCharacterDraws = 4096;
    /// Frames the last characters' build stands in for frames without one.
    static constexpr u32 HoldFrames = 4;
    /// Counts this frame as one with (or without) the characters' own build; logs the share now
    /// and then.
    void CountCharacterFrame(bool built);
    /// With Scene: the map's triangles' corner indices (three a triangle) and its corners' colours
    /// (0xAABBGGRR): a ray's hit brings back that surface's light (Kaniel 2026-10-21, "light to
    /// work more natural, not just darker").
    [[nodiscard]] vk::DescriptorBufferInfo Indices() const noexcept {
        return {index_buffer.buffer, 0, VK_WHOLE_SIZE};
    }
    [[nodiscard]] vk::DescriptorBufferInfo Colors() const noexcept {
        return {color_buffer.buffer, 0, VK_WHOLE_SIZE};
    }
    /// Kaniel 2026-10-22: each triangle's colour patch (AreaMesh::tri_patch) and the patches.
    [[nodiscard]] vk::DescriptorBufferInfo TriPatches() const noexcept {
        return {tri_patch_buffer.buffer, 0, VK_WHOLE_SIZE};
    }
    [[nodiscard]] vk::DescriptorBufferInfo Patches() const noexcept {
        return {patch_buffer.buffer, 0, VK_WHOLE_SIZE};
    }
    /// Its corners' positions (x, y, z floats, world): a hit's surface normal.
    [[nodiscard]] vk::DescriptorBufferInfo Positions() const noexcept {
        return {position_buffer.buffer, 0, VK_WHOLE_SIZE};
    }

private:
    struct Buffer {
        vk::Buffer buffer{};
        void* allocation{}; ///< VmaAllocation
        vk::DeviceAddress address = 0;
        void* mapped = nullptr;
    };
    Buffer MakeBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage, bool host);
    void Free(Buffer& buffer);
    void Drop();
    bool Build(const BbMapArt::AreaMesh& mesh);
    bool MakeCharacterParts();

    const Instance& instance;
    Scheduler& scheduler;
    u32 frames = 0;
    u32 map = 0;       ///< the player's map (packed), 0 unknown
    u32 built_map = 0; ///< the map the structures hold
    bool failed_map = false;
    Buffer blas_buffer, tlas_buffer, index_buffer, color_buffer, position_buffer;
    Buffer tri_patch_buffer, patch_buffer;
    vk::UniqueAccelerationStructureKHR blas, tlas;
    // Step 3 (characters), made once at their largest and reused every frame.
    static constexpr u32 MaxCharacterCorners = 1u << 20;
    static constexpr u32 FrameSlots = 3; ///< host-written inputs: one set a frame in flight
    std::array<Buffer, FrameSlots> char_source{}, char_draw_of{}, char_indices{}, frame_instances{};
    u32 char_slot = 0;
    Buffer char_world, char_factor, char_blas_buffer, frame_tlas_buffer, frame_scratch;
    Buffer char_kept; ///< the triangles the build reads (stretched ones made points)
    vk::UniqueDescriptorSetLayout char_tri_desc_layout;
    vk::UniquePipelineLayout char_tri_layout;
    vk::UniquePipeline char_tri_pipeline;
    vk::UniqueAccelerationStructureKHR char_blas, frame_tlas;
    vk::UniqueDescriptorSetLayout char_desc_layout;
    vk::UniquePipelineLayout char_layout;
    vk::UniquePipeline char_pipeline, char_ratio_pipeline;
    u32 frame_scene_frame = ~0u; ///< `frames` when frame_tlas holds this frame's characters
    bool char_failed = false;
    bool char_logged = false;
    u32 char_corners = 0;
    u32 counted_frames = 0, built_frames = 0;
};

} // namespace Vulkan
