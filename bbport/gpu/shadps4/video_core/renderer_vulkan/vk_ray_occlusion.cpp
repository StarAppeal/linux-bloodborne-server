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
#include "video_core/host_shaders/character_world_comp.h"
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
    frame_tlas.reset();
    char_blas.reset();
    for (u32 i = 0; i < FrameSlots; ++i) {
        Free(char_source[i]);
        Free(char_draw_of[i]);
        Free(char_indices[i]);
        Free(frame_instances[i]);
    }
    Free(char_world);
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
    position_buffer =
        MakeBuffer(mesh.positions.size() * sizeof(float), input | Usage::eStorageBuffer, true);
    Buffer& positions = position_buffer;
    Buffer instances = MakeBuffer(sizeof(vk::AccelerationStructureInstanceKHR), input, true);
    // Kept with the structures: the trace reads a hit's corners and their colours.
    index_buffer = MakeBuffer(mesh.indices.size() * sizeof(u32), input | Usage::eStorageBuffer, true);
    color_buffer = MakeBuffer(std::max<std::size_t>(mesh.colors.size(), 1) * sizeof(u32),
                              Usage::eStorageBuffer, true);
    Buffer& indices = index_buffer;
    auto free_inputs = [this, instances]() mutable {
        Free(instances);
    };
    if (!positions.mapped || !indices.mapped || !instances.mapped || !color_buffer.mapped ||
        mesh.colors.size() != vertices) {
        free_inputs();
        Free(position_buffer);
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
    if (!char_pipeline) {
        // Made once: the inputs (three sets), the corners in the world, the structures.
        for (u32 i = 0; i < FrameSlots; ++i) {
            char_source[i] = MakeBuffer(MaxCharacterCorners * sizeof(u32), Usage::eStorageBuffer, true);
            char_draw_of[i] = MakeBuffer(MaxCharacterCorners * sizeof(u32), Usage::eStorageBuffer, true);
            char_indices[i] = MakeBuffer(3ull * MaxCharacterCorners * sizeof(u32), input, true);
            frame_instances[i] = MakeBuffer(2 * sizeof(vk::AccelerationStructureInstanceKHR), input, true);
        }
        // Host visible: BB_LIGHTING_DUMP writes the corners out (DumpCharacters).
        char_world = MakeBuffer(3ull * MaxCharacterCorners * sizeof(float),
                                input | Usage::eStorageBuffer, true);
        char_factor = MakeBuffer(MaxCharacterDraws * sizeof(u32),
                                 Usage::eStorageBuffer | Usage::eTransferDst, false);
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
        bool ok = char_world.buffer && char_factor.buffer && char_blas_buffer.buffer &&
                  frame_tlas_buffer.buffer &&
                  frame_scratch.buffer;
        for (u32 i = 0; i < FrameSlots; ++i) {
            ok = ok && char_source[i].mapped && char_draw_of[i].mapped && char_indices[i].mapped &&
                 frame_instances[i].mapped;
        }
        if (!ok) {
            char_failed = true;
            std::printf("Ray occlusion: no memory for the characters\n");
            return;
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
                            index_address = char_indices[slot].address,
                            instance_address = frame_instances[slot].address;
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
    scheduler.RecordCrumb(
        {.name = "ray characters"},
        [=, pipeline = *char_pipeline, ratio_pipeline = *char_ratio_pipeline,
         layout = *char_layout](vk::CommandBuffer cmdbuf) {
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
