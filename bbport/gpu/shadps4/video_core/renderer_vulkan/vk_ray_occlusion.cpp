// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced ambient occlusion, see vk_ray_occlusion.h.

#include "video_core/renderer_vulkan/vk_ray_occlusion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <vk_mem_alloc.h>

#include "../bbcoop.h"
#include "bbport_map_charts.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

RayOcclusion::RayOcclusion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

RayOcclusion::~RayOcclusion() {
    if (blas || tlas) {
        scheduler.Finish();
    }
    tlas.reset();
    blas.reset();
    Free(tlas_buffer);
    Free(blas_buffer);
    Free(index_buffer);
    Free(color_buffer);
}

RayOcclusion::Buffer RayOcclusion::MakeBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                                              bool host) {
    Buffer out;
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = static_cast<VkBufferUsageFlags>(usage |
                                                 vk::BufferUsageFlagBits::eShaderDeviceAddress),
    };
    const VmaAllocationCreateInfo alloc_ci{
        .flags = host ? VMA_ALLOCATION_CREATE_MAPPED_BIT |
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                      : VmaAllocationCreateFlags{},
        .usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return out;
    }
    out.buffer = buffer;
    out.allocation = allocation;
    out.mapped = info.pMappedData;
    out.address = instance.GetDevice().getBufferAddress({.buffer = out.buffer});
    return out;
}

void RayOcclusion::Free(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(instance.GetAllocator(), buffer.buffer,
                         static_cast<VmaAllocation>(buffer.allocation));
    }
    buffer = {};
}

void RayOcclusion::Drop() {
    if (!blas && !tlas) {
        return;
    }
    // The frames in flight may still trace them.
    scheduler.Finish();
    tlas.reset();
    blas.reset();
    Free(tlas_buffer);
    Free(blas_buffer);
    Free(index_buffer);
    Free(color_buffer);
    built_map = 0;
}

void RayOcclusion::Update() {
    if (!instance.IsRayQueryEnabled()) {
        return;
    }
    // The player's map, about twice a second (bbcoop_map_view reads the game's state).
    if (frames++ % 30 == 0) {
        bbcoop_map view{};
        if (bbcoop_map_view(&view, 0) && view.in_world && view.map != 0 && view.map != map) {
            map = view.map;
            failed_map = false;
            std::printf("Ray occlusion: on map %08x\n", map);
            std::fflush(stdout);
        }
    }
    if (map == 0 || map == built_map || failed_map) {
        return;
    }
    // Asking starts the mesh on the map worker (read from user/map/ when made before).
    const auto mesh = BbMapCharts::GetRayMesh(map);
    if (!mesh) {
        return;
    }
    Drop();
    if (!Build(*mesh)) {
        failed_map = true;
        std::printf("Ray occlusion: no acceleration structure for map %08x\n", map);
        std::fflush(stdout);
    }
}

bool RayOcclusion::Build(const BbMapArt::AreaMesh& mesh) {
    const auto start = std::chrono::steady_clock::now();
    const u32 vertices = u32(mesh.positions.size() / 3);
    const u32 triangles = u32(mesh.indices.size() / 3);
    if (vertices == 0 || triangles == 0) {
        return false;
    }
    const auto device = instance.GetDevice();
    using Usage = vk::BufferUsageFlagBits;
    const auto input = Usage::eAccelerationStructureBuildInputReadOnlyKHR;
    // The mesh, the instance and the scratch: freed once the GPU has built from them.
    Buffer positions = MakeBuffer(mesh.positions.size() * sizeof(float), input, true);
    Buffer instances = MakeBuffer(sizeof(vk::AccelerationStructureInstanceKHR), input, true);
    // Kept with the structures: the trace reads a hit's corners and their colours.
    index_buffer = MakeBuffer(mesh.indices.size() * sizeof(u32), input | Usage::eStorageBuffer, true);
    color_buffer = MakeBuffer(std::max<std::size_t>(mesh.colors.size(), 1) * sizeof(u32),
                              Usage::eStorageBuffer, true);
    Buffer& indices = index_buffer;
    auto free_inputs = [this, positions, instances]() mutable {
        Free(positions);
        Free(instances);
    };
    if (!positions.mapped || !indices.mapped || !instances.mapped || !color_buffer.mapped ||
        mesh.colors.size() != vertices) {
        free_inputs();
        Free(index_buffer);
        Free(color_buffer);
        return false;
    }
    std::memcpy(positions.mapped, mesh.positions.data(), mesh.positions.size() * sizeof(float));
    std::memcpy(indices.mapped, mesh.indices.data(), mesh.indices.size() * sizeof(u32));
    std::memcpy(color_buffer.mapped, mesh.colors.data(), mesh.colors.size() * sizeof(u32));

    // Bottom level: the map's triangles, opaque (the first hit ends a ray).
    const vk::AccelerationStructureGeometryKHR geometry{
        .geometryType = vk::GeometryTypeKHR::eTriangles,
        .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                         .vertexFormat = vk::Format::eR32G32B32Sfloat,
                         .vertexData = {.deviceAddress = positions.address},
                         .vertexStride = 3 * sizeof(float),
                         .maxVertex = vertices - 1,
                         .indexType = vk::IndexType::eUint32,
                         .indexData = {.deviceAddress = indices.address},
                     }},
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };
    vk::AccelerationStructureBuildGeometryInfoKHR blas_info{
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const auto blas_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, blas_info, triangles);
    blas_buffer = MakeBuffer(blas_sizes.accelerationStructureSize,
                             Usage::eAccelerationStructureStorageKHR, false);
    if (!blas_buffer.buffer) {
        free_inputs();
        return false;
    }
    blas = Check(device.createAccelerationStructureKHRUnique({
        .buffer = blas_buffer.buffer,
        .size = blas_sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    }));

    // Top level: the map once, as it is (its coordinates are the world's).
    vk::TransformMatrixKHR identity{};
    identity.matrix = std::array<std::array<float, 4>, 3>{{{1.0f, 0.0f, 0.0f, 0.0f},
                                                          {0.0f, 1.0f, 0.0f, 0.0f},
                                                          {0.0f, 0.0f, 1.0f, 0.0f}}};
    vk::AccelerationStructureInstanceKHR placed{
        .transform = identity,
        .mask = 0xFF,
        .flags = VkGeometryInstanceFlagsKHR(
            vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable),
        .accelerationStructureReference =
            device.getAccelerationStructureAddressKHR({.accelerationStructure = *blas}),
    };
    std::memcpy(instances.mapped, &placed, sizeof(placed));
    const vk::AccelerationStructureGeometryKHR instance_geometry{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{
                         .data = {.deviceAddress = instances.address},
                     }},
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };
    vk::AccelerationStructureBuildGeometryInfoKHR tlas_info{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &instance_geometry,
    };
    const auto tlas_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, tlas_info, 1u);
    tlas_buffer = MakeBuffer(tlas_sizes.accelerationStructureSize,
                             Usage::eAccelerationStructureStorageKHR, false);
    if (!tlas_buffer.buffer) {
        free_inputs();
        blas.reset();
        Free(blas_buffer);
        return false;
    }
    tlas = Check(device.createAccelerationStructureKHRUnique({
        .buffer = tlas_buffer.buffer,
        .size = tlas_sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
    }));

    // One scratch for both builds (one after the other), aligned as the device asks.
    const u64 align = instance.AccelerationScratchAlignment();
    const vk::DeviceSize scratch_size =
        std::max(blas_sizes.buildScratchSize, tlas_sizes.buildScratchSize) + align;
    Buffer scratch = MakeBuffer(scratch_size, Usage::eStorageBuffer, false);
    if (!scratch.buffer) {
        free_inputs();
        Drop();
        return false;
    }
    const vk::DeviceAddress scratch_address = (scratch.address + align - 1) / align * align;
    blas_info.dstAccelerationStructure = *blas;
    blas_info.scratchData.deviceAddress = scratch_address;
    tlas_info.dstAccelerationStructure = *tlas;
    tlas_info.scratchData.deviceAddress = scratch_address;

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::AccelerationStructureBuildRangeInfoKHR blas_range{.primitiveCount = triangles};
    const vk::AccelerationStructureBuildRangeInfoKHR tlas_range{.primitiveCount = 1};
    cmdbuf.buildAccelerationStructuresKHR(blas_info, &blas_range);
    // The top level reads the bottom one, and reuses the scratch.
    const vk::MemoryBarrier2 between{
        .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                         vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &between});
    cmdbuf.buildAccelerationStructuresKHR(tlas_info, &tlas_range);
    const vk::MemoryBarrier2 to_trace{
        .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_trace});
    scheduler.DeferOperation([this, free_inputs, scratch]() mutable {
        free_inputs();
        Free(scratch);
    });
    built_map = map;
    std::printf("Ray occlusion: map %08x, %u triangles, %.1f MB, recorded in %.1f ms\n", map,
                triangles, double(blas_sizes.accelerationStructureSize) / 1e6,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
    std::fflush(stdout);
    return true;
}

} // namespace Vulkan
