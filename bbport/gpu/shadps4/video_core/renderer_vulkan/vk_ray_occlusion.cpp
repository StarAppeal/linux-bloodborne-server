// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: ray-traced ambient occlusion, see vk_ray_occlusion.h.

#include "video_core/renderer_vulkan/vk_ray_occlusion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vk_mem_alloc.h>

#include "../bbcoop.h"
#include "bbport_map_charts.h"
#include "video_core/host_shaders/character_ratio_comp.h"
#include "video_core/host_shaders/character_triangles_comp.h"
#include "video_core/host_shaders/character_world_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
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
    Free(position_buffer);
    Free(tri_patch_buffer);
    Free(patch_buffer);
    frame_tlas.reset();
    char_blas.reset();
    for (u32 i = 0; i < FrameSlots; ++i) {
        Free(char_source[i]);
        Free(char_draw_of[i]);
        Free(char_indices[i]);
        Free(frame_instances[i]);
    }
    Free(char_world);
    Free(char_kept);
    Free(char_factor);
    Free(char_blas_buffer);
    Free(frame_tlas_buffer);
    Free(frame_scratch);
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
    Free(position_buffer);
    Free(tri_patch_buffer);
    Free(patch_buffer);
    built_map = 0;
}

void RayOcclusion::Update() {
    if (!instance.IsRayQueryEnabled()) {
        return;
    }
    // The player's map, about twice a second (bbcoop_map_view reads the game's state); known
    // already while it loads, so its structure is made behind the loading screen (Kaniel 2026-10-22:
    // made after it, the GPU's build of 21.7M triangles froze the game ~2 s).
    if (frames++ % 30 == 0) {
        bbcoop_map view{};
        if (bbcoop_map_view(&view, 0) && view.map != 0 && view.map != map) {
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
    // The characters' parts first: in video memory while there is room (MakeCharacterParts).
    MakeCharacterParts();
    const u32 vertices = u32(mesh.positions.size() / 3);
    const u32 triangles = u32(mesh.indices.size() / 3);
    if (vertices == 0 || triangles == 0) {
        return false;
    }
    const auto device = instance.GetDevice();
    using Usage = vk::BufferUsageFlagBits;
    const auto input = Usage::eAccelerationStructureBuildInputReadOnlyKHR;
    // Kept with the structures, in video memory: the traces read a hit's corners and their
    // colours (Kaniel 2026-10-21, "lags really bad": read from host memory, every hit crossed PCIe).
    // They come in through host copies, freed with the instance and the scratch once built.
    const vk::DeviceSize position_bytes = mesh.positions.size() * sizeof(float);
    const vk::DeviceSize index_bytes = mesh.indices.size() * sizeof(u32);
    const vk::DeviceSize color_bytes = std::max<std::size_t>(mesh.colors.size(), 1) * sizeof(u32);
    // The patches (none: every triangle reads 0, its corners' colours).
    const vk::DeviceSize tri_patch_bytes = std::size_t(triangles) * sizeof(u32);
    const vk::DeviceSize patch_bytes = std::max<std::size_t>(mesh.patches.size(), 1) * sizeof(u32);
    const bool patched = mesh.tri_patch.size() == triangles;
    position_buffer = MakeBuffer(position_bytes,
                                 input | Usage::eStorageBuffer | Usage::eTransferDst, false);
    index_buffer =
        MakeBuffer(index_bytes, input | Usage::eStorageBuffer | Usage::eTransferDst, false);
    color_buffer = MakeBuffer(color_bytes, Usage::eStorageBuffer | Usage::eTransferDst, false);
    tri_patch_buffer = MakeBuffer(tri_patch_bytes, Usage::eStorageBuffer | Usage::eTransferDst, false);
    patch_buffer = MakeBuffer(patch_bytes, Usage::eStorageBuffer | Usage::eTransferDst, false);
    Buffer& positions = position_buffer;
    Buffer& indices = index_buffer;
    Buffer instances = MakeBuffer(sizeof(vk::AccelerationStructureInstanceKHR), input, true);
    const vk::DeviceSize patch_at = position_bytes + index_bytes + color_bytes;
    Buffer upload = MakeBuffer(patch_at + tri_patch_bytes + patch_bytes, Usage::eTransferSrc, true);
    auto free_inputs = [this, instances, upload]() mutable {
        Free(instances);
        Free(upload);
    };
    if (!positions.buffer || !indices.buffer || !color_buffer.buffer || !tri_patch_buffer.buffer ||
        !patch_buffer.buffer || !instances.mapped || !upload.mapped || mesh.colors.size() != vertices) {
        free_inputs();
        Free(position_buffer);
        Free(index_buffer);
        Free(color_buffer);
        Free(tri_patch_buffer);
        Free(patch_buffer);
        return false;
    }
    auto* staged = static_cast<u8*>(upload.mapped);
    std::memcpy(staged, mesh.positions.data(), position_bytes);
    std::memcpy(staged + position_bytes, mesh.indices.data(), index_bytes);
    std::memcpy(staged + position_bytes + index_bytes, mesh.colors.data(),
                mesh.colors.size() * sizeof(u32));
    if (patched) {
        std::memcpy(staged + patch_at, mesh.tri_patch.data(), tri_patch_bytes);
        std::memcpy(staged + patch_at + tri_patch_bytes, mesh.patches.data(),
                    mesh.patches.size() * sizeof(u32));
    } else {
        std::memset(staged + patch_at, 0, tri_patch_bytes);
    }

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
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
                 vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction,
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
    cmdbuf.copyBuffer(upload.buffer, positions.buffer, vk::BufferCopy{0, 0, position_bytes});
    cmdbuf.copyBuffer(upload.buffer, indices.buffer,
                      vk::BufferCopy{position_bytes, 0, index_bytes});
    cmdbuf.copyBuffer(upload.buffer, color_buffer.buffer,
                      vk::BufferCopy{position_bytes + index_bytes, 0, color_bytes});
    cmdbuf.copyBuffer(upload.buffer, tri_patch_buffer.buffer, vk::BufferCopy{patch_at, 0, tri_patch_bytes});
    cmdbuf.copyBuffer(upload.buffer, patch_buffer.buffer,
                      vk::BufferCopy{patch_at + tri_patch_bytes, 0, patch_bytes});
    const vk::MemoryBarrier2 uploaded{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR |
                        vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &uploaded});
    // Its compacted size asked for once built (Kaniel 2026-10-22: graphics memory for the rest).
    auto sizes = Check(device.createQueryPoolUnique({
        .queryType = vk::QueryType::eAccelerationStructureCompactedSizeKHR,
        .queryCount = 1,
    }));
    cmdbuf.resetQueryPool(*sizes, 0, 1);
    cmdbuf.buildAccelerationStructuresKHR(blas_info, &blas_range);
    // The top level (and the size query) read the bottom one; the top level reuses the scratch.
    const vk::MemoryBarrier2 between{
        .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR |
                        vk::PipelineStageFlagBits2::eAccelerationStructureCopyKHR,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                         vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &between});
    cmdbuf.writeAccelerationStructuresPropertiesKHR(
        *blas, vk::QueryType::eAccelerationStructureCompactedSizeKHR, *sizes, 0);
    scheduler.Finish();
    vk::DeviceSize compact_size = 0;
    const auto got = device.getQueryPoolResults(*sizes, 0, 1, sizeof(compact_size), &compact_size,
                                                sizeof(compact_size),
                                                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    const vk::DeviceSize full_size = blas_sizes.accelerationStructureSize;
    auto copy_cmdbuf = scheduler.CommandBuffer();
    if (got == vk::Result::eSuccess && compact_size > 0 && compact_size < full_size) {
        Buffer compact_buffer = MakeBuffer(compact_size, Usage::eAccelerationStructureStorageKHR, false);
        if (compact_buffer.buffer) {
            auto compact = Check(device.createAccelerationStructureKHRUnique({
                .buffer = compact_buffer.buffer,
                .size = compact_size,
                .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            }));
            copy_cmdbuf.copyAccelerationStructureKHR({
                .src = *blas,
                .dst = *compact,
                .mode = vk::CopyAccelerationStructureModeKHR::eCompact,
            });
            const vk::MemoryBarrier2 copied{
                .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureCopyKHR,
                .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            };
            copy_cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &copied});
            // The full one goes once the copy is done.
            scheduler.DeferOperation([this, old = std::make_shared<vk::UniqueAccelerationStructureKHR>(
                                                std::move(blas)),
                                      old_buffer = blas_buffer]() mutable {
                old->reset();
                Free(old_buffer);
            });
            blas = std::move(compact);
            blas_buffer = compact_buffer;
            placed.accelerationStructureReference =
                device.getAccelerationStructureAddressKHR({.accelerationStructure = *blas});
            std::memcpy(instances.mapped, &placed, sizeof(placed));
            std::printf("Ray occlusion: compacted %.1f MB to %.1f MB\n", double(full_size) / 1e6,
                        double(compact_size) / 1e6);
        }
    }
    copy_cmdbuf.buildAccelerationStructuresKHR(tlas_info, &tlas_range);
    const vk::MemoryBarrier2 to_trace{
        .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
    };
    copy_cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_trace});
    scheduler.DeferOperation([this, free_inputs, scratch]() mutable {
        free_inputs();
        Free(scratch);
    });
    built_map = map;
    std::printf("Ray occlusion: map %08x, %u triangles, %.1f MB, %zu patch colours, recorded in "
                "%.1f ms\n", map, triangles, double(blas_sizes.accelerationStructureSize) / 1e6,
                mesh.patches.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
    std::fflush(stdout);
    return true;
}

void RayOcclusion::CountCharacterFrame(bool built) {
    built_frames += built ? 1 : 0;
    if (++counted_frames == 600) {
        std::printf("Ray occlusion: characters built in %u of the last 600 frames\n", built_frames);
        std::fflush(stdout);
        counted_frames = built_frames = 0;
    }
}

void RayOcclusion::DumpCharacters(const std::string& path) {
    if (!char_world.mapped || frame_scene_frame != frames || char_corners == 0) {
        return;
    }
    const u32 corners = char_corners;
    const void* data = char_world.mapped;
    void* allocation = char_world.allocation;
    VmaAllocator allocator = instance.GetAllocator();
    scheduler.DeferPriorityOperation([=] {
        vmaInvalidateAllocation(allocator, static_cast<VmaAllocation>(allocation), 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(data, 3 * sizeof(float), corners, f);
            std::fclose(f);
        }
    });
}

/// Kaniel 2026-10-22: the characters' buffers, structures and pipelines, made once and before the
/// map's structures (made after them, with graphics memory tight, they landed in host memory).
bool RayOcclusion::MakeCharacterParts() {
    if (char_pipeline || char_failed) {
        return !char_failed;
    }
    const auto device = instance.GetDevice();
    using Usage = vk::BufferUsageFlagBits;
    const auto input = Usage::eAccelerationStructureBuildInputReadOnlyKHR;
    const u32 max_triangles = MaxCharacterCorners;
    auto char_geometry = [&](vk::DeviceAddress vertices, vk::DeviceAddress index_data) {
        return vk::AccelerationStructureGeometryKHR{
            .geometryType = vk::GeometryTypeKHR::eTriangles,
            .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                             .vertexFormat = vk::Format::eR32G32B32Sfloat,
                             .vertexData = {.deviceAddress = vertices},
                             .vertexStride = 3 * sizeof(float),
                             .maxVertex = MaxCharacterCorners - 1,
                             .indexType = vk::IndexType::eUint32,
                             .indexData = {.deviceAddress = index_data},
                         }},
            .flags = vk::GeometryFlagBitsKHR::eOpaque,
        };
    };
    // Made once: the inputs (three sets), the corners in the world, the structures.
    for (u32 i = 0; i < FrameSlots; ++i) {
        char_source[i] = MakeBuffer(MaxCharacterCorners * sizeof(u32), Usage::eStorageBuffer, true);
        char_draw_of[i] = MakeBuffer(MaxCharacterCorners * sizeof(u32), Usage::eStorageBuffer, true);
        char_indices[i] = MakeBuffer(3ull * MaxCharacterCorners * sizeof(u32), input, true);
        frame_instances[i] = MakeBuffer(2 * sizeof(vk::AccelerationStructureInstanceKHR), input, true);
    }
    // In video memory (written and built from each frame); host visible only for
    // BB_LIGHTING_DUMP, which writes the corners out (DumpCharacters).
    char_world = MakeBuffer(3ull * MaxCharacterCorners * sizeof(float),
                            input | Usage::eStorageBuffer,
                            std::getenv("BB_LIGHTING_DUMP") != nullptr);
    char_factor = MakeBuffer(MaxCharacterDraws * sizeof(u32),
                             Usage::eStorageBuffer | Usage::eTransferDst, false);
    char_kept = MakeBuffer(3ull * MaxCharacterCorners * sizeof(u32),
                           input | Usage::eStorageBuffer, false);
    const auto geometry = char_geometry(0, 0);
    vk::AccelerationStructureBuildGeometryInfoKHR info{
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const auto blas_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, info, max_triangles);
    const vk::AccelerationStructureGeometryKHR instances_geometry{
        .geometryType = vk::GeometryTypeKHR::eInstances,
        .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{}},
        .flags = vk::GeometryFlagBitsKHR::eOpaque,
    };
    vk::AccelerationStructureBuildGeometryInfoKHR top{
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
        .geometryCount = 1,
        .pGeometries = &instances_geometry,
    };
    const auto top_sizes = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice, top, 2u);
    char_blas_buffer = MakeBuffer(blas_sizes.accelerationStructureSize,
                                  Usage::eAccelerationStructureStorageKHR, false);
    frame_tlas_buffer = MakeBuffer(top_sizes.accelerationStructureSize,
                                   Usage::eAccelerationStructureStorageKHR, false);
    const u64 align = instance.AccelerationScratchAlignment();
    frame_scratch = MakeBuffer(blas_sizes.buildScratchSize + top_sizes.buildScratchSize + 2 * align,
                               Usage::eStorageBuffer, false);
    bool ok = char_world.buffer && char_world.address && char_kept.buffer && char_factor.buffer && char_blas_buffer.buffer &&
              frame_tlas_buffer.buffer &&
              frame_scratch.buffer;
    for (u32 i = 0; i < FrameSlots; ++i) {
        ok = ok && char_source[i].mapped && char_draw_of[i].mapped && char_indices[i].mapped &&
             frame_instances[i].mapped;
    }
    if (!ok) {
        char_failed = true;
        std::printf("Ray occlusion: no memory for the characters\n");
        return false;
    }
    char_blas = Check(device.createAccelerationStructureKHRUnique({
        .buffer = char_blas_buffer.buffer,
        .size = blas_sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
    }));
    frame_tlas = Check(device.createAccelerationStructureKHRUnique({
        .buffer = frame_tlas_buffer.buffer,
        .size = top_sizes.accelerationStructureSize,
        .type = vk::AccelerationStructureTypeKHR::eTopLevel,
    }));
    const std::array<vk::DescriptorSetLayoutBinding, 6> bindings{{
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {5, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    char_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                     .offset = 0,
                                     .size = 80};
    char_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*char_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto compute = [&](std::span<const u32> code) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *char_layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    char_pipeline = compute(CHARACTER_WORLD_COMP);
    char_ratio_pipeline = compute(CHARACTER_RATIO_COMP);
    const std::array<vk::DescriptorSetLayoutBinding, 3> tri_bindings{{
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    char_tri_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(tri_bindings.size()),
        .pBindings = tri_bindings.data(),
    }));
    const vk::PushConstantRange tri_push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = sizeof(u32)};
    char_tri_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*char_tri_desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &tri_push,
    }));
    const auto module = CompileSPV(CHARACTER_TRIANGLES_COMP, device);
    char_tri_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *char_tri_layout,
            }));
    device.destroyShaderModule(module);
    return true;
}

void RayOcclusion::BuildCharacters(vk::Buffer positions, const std::vector<u32>& source,
                                   const std::vector<u32>& indices,
                                   const std::vector<u32>& draw_of, u32 draws,
                                   vk::ImageView depth, u32 width, u32 height,
                                   const std::array<float, 4>& proj,
                                   const std::array<float, 12>& world) {
    if (char_failed || !blas || indices.empty() || source.size() > MaxCharacterCorners ||
        indices.size() > 3 * MaxCharacterCorners || draw_of.size() != source.size() ||
        draws == 0 || draws > MaxCharacterDraws || !depth) {
        return;
    }
    const auto device = instance.GetDevice();
    using Usage = vk::BufferUsageFlagBits;
    const auto input = Usage::eAccelerationStructureBuildInputReadOnlyKHR;
    const u32 max_triangles = MaxCharacterCorners;
    // The geometry the builds describe (sizes at the largest).
    auto char_geometry = [&](vk::DeviceAddress vertices, vk::DeviceAddress index_data) {
        return vk::AccelerationStructureGeometryKHR{
            .geometryType = vk::GeometryTypeKHR::eTriangles,
            .geometry = {.triangles = vk::AccelerationStructureGeometryTrianglesDataKHR{
                             .vertexFormat = vk::Format::eR32G32B32Sfloat,
                             .vertexData = {.deviceAddress = vertices},
                             .vertexStride = 3 * sizeof(float),
                             .maxVertex = MaxCharacterCorners - 1,
                             .indexType = vk::IndexType::eUint32,
                             .indexData = {.deviceAddress = index_data},
                         }},
            .flags = vk::GeometryFlagBitsKHR::eOpaque,
        };
    };
    if (!MakeCharacterParts()) {
        return;
    }
    const u32 slot = char_slot;
    char_slot = (char_slot + 1) % FrameSlots;
    std::memcpy(char_source[slot].mapped, source.data(), source.size() * sizeof(u32));
    std::memcpy(char_draw_of[slot].mapped, draw_of.data(), draw_of.size() * sizeof(u32));
    std::memcpy(char_indices[slot].mapped, indices.data(), indices.size() * sizeof(u32));
    // The two instances: the map, the characters (custom index 1: no map colours for them).
    vk::TransformMatrixKHR identity{};
    identity.matrix = std::array<std::array<float, 4>, 3>{{{1.0f, 0.0f, 0.0f, 0.0f},
                                                          {0.0f, 1.0f, 0.0f, 0.0f},
                                                          {0.0f, 0.0f, 1.0f, 0.0f}}};
    const std::array<vk::AccelerationStructureInstanceKHR, 2> placed{{
        {.transform = identity,
         .instanceCustomIndex = 0,
         // BB_RAY_CHARACTERS_ONLY=1 (checks): the rays see the characters alone.
         .mask = u8(std::getenv("BB_RAY_CHARACTERS_ONLY") ? 0x00 : 0x01),
         .flags = VkGeometryInstanceFlagsKHR(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable),
         .accelerationStructureReference =
             device.getAccelerationStructureAddressKHR({.accelerationStructure = *blas})},
        {.transform = identity,
         .instanceCustomIndex = 1,
         .mask = 0x02,
         .flags = VkGeometryInstanceFlagsKHR(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable),
         .accelerationStructureReference =
             device.getAccelerationStructureAddressKHR({.accelerationStructure = *char_blas})},
    }};
    std::memcpy(frame_instances[slot].mapped, placed.data(), sizeof(placed));
    const u32 corners = u32(source.size()), triangles = u32(indices.size() / 3);
    struct {
        std::array<float, 12> world;
        std::array<float, 4> proj;
        float width, height;
        u32 count;
        u32 pad;
    } push{world, proj, float(width), float(height), corners, 0};
    static_assert(sizeof(push) == 80);
    const u64 align = instance.AccelerationScratchAlignment();
    const vk::DeviceAddress scratch0 = (frame_scratch.address + align - 1) / align * align;
    const vk::AccelerationStructureKHR char_as = *char_blas, top_as = *frame_tlas;
    const vk::DeviceAddress world_address = char_world.address,
                            index_address = char_kept.address,
                            instance_address = frame_instances[slot].address;
    const vk::Buffer indices_buffer = char_indices[slot].buffer, kept_buffer = char_kept.buffer;
    const vk::Buffer source_buffer = char_source[slot].buffer, world_buffer = char_world.buffer,
                     draw_of_buffer = char_draw_of[slot].buffer, factor_buffer = char_factor.buffer;
    const auto geometry_now = char_geometry(world_address, index_address);
    const vk::DeviceSize blas_scratch_size = device.getAccelerationStructureBuildSizesKHR(
        vk::AccelerationStructureBuildTypeKHR::eDevice,
        vk::AccelerationStructureBuildGeometryInfoKHR{
            .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries = &geometry_now},
        max_triangles).buildScratchSize;
    const vk::DeviceAddress scratch1 = (scratch0 + blas_scratch_size + align - 1) / align * align;
    scheduler.EndRendering();
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0x5A7E4ull, [] { return std::string{"ray: characters built"}; });
    }
    scheduler.RecordCrumb(
        {.name = "ray characters"},
        [=, pipeline = *char_pipeline, ratio_pipeline = *char_ratio_pipeline,
         layout = *char_layout, tri_pipeline = *char_tri_pipeline,
         tri_layout = *char_tri_layout](vk::CommandBuffer cmdbuf) {
            // The skinned draws wrote the positions; the last frame may still trace the old.
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                                vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                                 vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
            // Each draw's factor starts at infinity (none: its corners stay out).
            cmdbuf.fillBuffer(factor_buffer, 0, draws * sizeof(u32), 0x7F800000u);
            const vk::MemoryBarrier2 filled{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &filled});
            const vk::DescriptorBufferInfo positions_info{positions, 0, VK_WHOLE_SIZE};
            const vk::DescriptorBufferInfo source_info{source_buffer, 0, VK_WHOLE_SIZE};
            const vk::DescriptorBufferInfo world_info{world_buffer, 0, VK_WHOLE_SIZE};
            const vk::DescriptorBufferInfo draw_of_info{draw_of_buffer, 0, VK_WHOLE_SIZE};
            const vk::DescriptorBufferInfo factor_info{factor_buffer, 0, VK_WHOLE_SIZE};
            const vk::DescriptorImageInfo depth_info{.imageView = depth,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 6> writes = {{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &positions_info},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &source_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &draw_of_info},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &factor_info},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &depth_info},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &world_info},
            }};
            // RATIO: each draw's depth scale against the depth buffer; then the corners placed.
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, ratio_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmdbuf.dispatch((corners + 63) / 64, 1, 1);
            const vk::MemoryBarrier2 ratios{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &ratios});
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmdbuf.dispatch((corners + 63) / 64, 1, 1);
            // The placed corners' triangles, stretched ones made points (character_triangles).
            const vk::MemoryBarrier2 placed_corners{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &placed_corners});
            const vk::DescriptorBufferInfo indices_info{indices_buffer, 0, VK_WHOLE_SIZE};
            const vk::DescriptorBufferInfo kept_info{kept_buffer, 0, VK_WHOLE_SIZE};
            const std::array<vk::WriteDescriptorSet, 3> tri_writes = {{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &world_info},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &indices_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &kept_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, tri_pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, tri_layout, 0, tri_writes);
            cmdbuf.pushConstants(tri_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(u32),
                                 &triangles);
            cmdbuf.dispatch((triangles + 63) / 64, 1, 1);
            const vk::MemoryBarrier2 to_build{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR |
                                 vk::AccessFlagBits2::eShaderRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_build});
            const auto geometry = geometry_now;
            const vk::AccelerationStructureBuildGeometryInfoKHR bottom{
                .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
                .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .dstAccelerationStructure = char_as,
                .geometryCount = 1,
                .pGeometries = &geometry,
                .scratchData = {.deviceAddress = scratch0},
            };
            const vk::AccelerationStructureBuildRangeInfoKHR bottom_range{.primitiveCount = triangles};
            cmdbuf.buildAccelerationStructuresKHR(bottom, &bottom_range);
            const vk::MemoryBarrier2 between{
                .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &between});
            const vk::AccelerationStructureGeometryKHR instances_geometry{
                .geometryType = vk::GeometryTypeKHR::eInstances,
                .geometry = {.instances = vk::AccelerationStructureGeometryInstancesDataKHR{
                                 .data = {.deviceAddress = instance_address}}},
                .flags = vk::GeometryFlagBitsKHR::eOpaque,
            };
            const vk::AccelerationStructureBuildGeometryInfoKHR top{
                .type = vk::AccelerationStructureTypeKHR::eTopLevel,
                .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .dstAccelerationStructure = top_as,
                .geometryCount = 1,
                .pGeometries = &instances_geometry,
                .scratchData = {.deviceAddress = scratch1},
            };
            const vk::AccelerationStructureBuildRangeInfoKHR top_range{.primitiveCount = 2};
            cmdbuf.buildAccelerationStructuresKHR(top, &top_range);
            const vk::MemoryBarrier2 to_trace{
                .srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_trace});
        });
    frame_scene_frame = frames;
    char_corners = corners;
    if (!char_logged) {
        std::printf("Ray occlusion: characters in the ray scene (%u corners, %u triangles)\n",
                    corners, triangles);
        std::fflush(stdout);
        char_logged = true;
    }
}

} // namespace Vulkan
