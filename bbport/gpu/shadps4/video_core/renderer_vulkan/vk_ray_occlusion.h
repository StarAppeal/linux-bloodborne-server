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
        return built_map != 0 && built_map == map ? *tlas : vk::AccelerationStructureKHR{};
    }
    /// With Scene: the map's triangles' corner indices (three a triangle) and its corners' colours
    /// (0xAABBGGRR): a ray's hit brings back that surface's light (Kaniel 2026-10-21, "light to
    /// work more natural, not just darker").
    [[nodiscard]] vk::DescriptorBufferInfo Indices() const noexcept {
        return {index_buffer.buffer, 0, VK_WHOLE_SIZE};
    }
    [[nodiscard]] vk::DescriptorBufferInfo Colors() const noexcept {
        return {color_buffer.buffer, 0, VK_WHOLE_SIZE};
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

    const Instance& instance;
    Scheduler& scheduler;
    u32 frames = 0;
    u32 map = 0;       ///< the player's map (packed), 0 unknown
    u32 built_map = 0; ///< the map the structures hold
    bool failed_map = false;
    Buffer blas_buffer, tlas_buffer, index_buffer, color_buffer;
    vk::UniqueAccelerationStructureKHR blas, tlas;
};

} // namespace Vulkan
