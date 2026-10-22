// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: screen-space lighting, see vk_screen_lighting.h.

#include "video_core/renderer_vulkan/vk_screen_lighting.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "video_core/host_shaders/screen_lighting_apply_comp.h"
#include "video_core/host_shaders/screen_lighting_trace_comp.h"
#include "video_core/host_shaders/screen_lighting_trace_rt_comp.h"
#include "video_core/host_shaders/lamp_shadows_comp.h"
#include "video_core/host_shaders/lamp_bounce_resolve_comp.h"
#include "video_core/host_shaders/fog_apply_comp.h"
#include "video_core/host_shaders/fog_light_comp.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_image_dump.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_ray_occlusion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {
/// BB_RT_OFF (speed checks): ray-traced parts left out, any of "fog", "bounce", "lamps", "chars".
bool RayPartOff(const char* part) {
    static const char* off = std::getenv("BB_RT_OFF");
    return off && std::strstr(off, part);
}

/// BB_GPU_PROFILE: a ray pass's segment (`key`), then the game's work after it under its own.
template <typename Record>
void Profiled(u64 key, const char* name, Record&& record) {
    auto* profiler = GpuProfiler::Get();
    if (profiler) {
        profiler->Mark(key, [name] { return std::string{name}; });
    }
    record();
    if (profiler) {
        profiler->Mark(0x5A7E0ull, [] { return std::string{"(game work after a ray pass)"}; });
    }
}

/// screen_lighting.comp's push constants (std430).
struct PushConstants {
    std::array<float, 4> proj;
    std::array<float, 2> size, trace_size, albedo_size;
    float radius, max_radiance, noise, ao_strength, gi_strength;
    s32 directions, steps, view, normals;
    s32 rays;                    ///< RAY_QUERY: occlusion rays a pixel
    std::array<float, 12> world; ///< RAY_QUERY: view to world, 3x4 rows
};
static_assert(sizeof(PushConstants) == 128 && offsetof(PushConstants, world) == 80);

/// Reach of the occlusion and the bounce, in the game's units (metres): a room's corners and
/// a body's contact with the ground, not the far side of a street.
constexpr float Radius = 1.5f;
/// A sample's brightest channel is capped here (the scene's lamps reach ~1-4): no fireflies.
constexpr float MaxRadiance = 4.0f;

/// Bytes a texel of the formats the scene's targets use (BB_LIGHTING_DUMP), 0 others.
u32 BytesPerTexel(vk::Format format) {
    switch (format) {
    case vk::Format::eR8Unorm:
        return 1;
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR16Sfloat:
        return 2;
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR16G16B16A16Unorm:
    case vk::Format::eR32G32Sfloat:
        return 8;
    case vk::Format::eR32G32B32A32Sfloat:
        return 16;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eA2B10G10R10UnormPack32:
    case vk::Format::eA2R10G10B10UnormPack32:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eR16G16Sfloat:
    case vk::Format::eR16G16Unorm:
    case vk::Format::eR32Sfloat:
    case vk::Format::eR32Uint:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return 4;
    default:
        return 0;
    }
}

} // namespace

bool ScreenLighting::IsAlbedoFormat(vk::Format format) {
    return format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb ||
           format == vk::Format::eA8B8G8R8SrgbPack32;
}

ScreenLighting::ScreenLighting(const Instance& instance_, Scheduler& scheduler_,
                               VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                               CameraMotion& camera_motion_, SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, camera_motion{camera_motion_}, scene_targets{scene_targets_} {
    if (const char* view = std::getenv("BB_LIGHTING_VIEW")) {
        view_mode = std::clamp(std::atoi(view), 0, 2);
    }
    const auto features = instance.GetPhysicalDevice()
                              .getFormatProperties(vk::Format::eR16G16B16A16Sfloat)
                              .optimalTilingFeatures;
    supported = bool(features & vk::FormatFeatureFlagBits::eStorageImage);
    if (instance.IsRayQueryEnabled()) {
        rays = std::make_unique<RayOcclusion>(instance, scheduler);
    }
}

ScreenLighting::~ScreenLighting() {
    for (auto& b : lamp_buffers) {
        if (b.buffer) {
            scheduler.Finish();
            vmaDestroyBuffer(instance.GetAllocator(), b.buffer, static_cast<VmaAllocation>(b.allocation));
            b = {};
        }
    }
    if (trace_pipeline || trace_image.image) {
        scheduler.Finish();
    }
    const auto device = instance.GetDevice();
    for (auto& entry : view_cache) {
        if (entry.view) {
            device.destroyImageView(entry.view);
        }
    }
}

void ScreenLighting::OnFrameStart() {
    done = false;
    lamps.clear();
    lamps_done = false;
    light_pass_seen = false;
    // The sun draw is not in every frame's lighting pass (2026-10-21 log: found, none, found):
    // the last one stands for a few frames.
    if (++sun_age > 4) {
        sun_dir = {};
    }
    character_source.clear();
    character_indices.clear();
    character_draw_of.clear();
    character_draws = 0;
    character_pass = {};
    if (rays && Enabled() && BbSettings::Get().ray_traced_ao) {
        rays->Update();
    }
    scene_color = {};
    static const char* control = std::getenv("BB_LIGHTING_FILE");
    ++frames_seen;
    if (control && frames_seen % 64 == 0) {
        if (FILE* f = std::fopen(control, "r")) {
            int quality = -1, view = -1, traced = -1;
            const int fields = std::fscanf(f, "%d %d %d", &quality, &view, &traced);
            std::fclose(f);
            if (fields >= 1 && quality >= 0 && quality < BbSettings::ScreenLightingCount &&
                BbSettings::Get().screen_lighting != quality) {
                BbSettings::Get().screen_lighting = quality;
                std::printf("Screen lighting: %s (control file)\n",
                            BbSettings::ScreenLightingNames[quality]);
            }
            if (fields == 3 && traced >= 0) {
                BbSettings::Get().ray_traced_ao = traced != 0;
            }
            if (fields >= 2 && view >= 0 && view <= 2) {
                view_mode = view;
            }
        }
    }
    static const char* dump_trigger = std::getenv("BB_LIGHTING_DUMP");
    if (dump_trigger && !dump_due && frames_seen % 16 == 0 && std::remove(dump_trigger) == 0) {
        dump_due = true;
        gbuffer = {};
    }
}

namespace {
/// Lamps a frame at most (the Central Yharnam hall has 91).
constexpr std::size_t MaxLamps = 512;
/// lamp_shadows.comp's push constants.
struct LampPush {
    std::array<float, 4> proj;
    std::array<float, 2> size;
    s32 count;
    float strength;
    std::array<float, 12> world;
    s32 bounce_rays;
    float bounce_strength;
    float noise;
    float sky; ///< the open sky's light, a share of the sun's
    std::array<float, 4> sun_dir;    ///< view, toward the sun; w 1 when there is one
    std::array<float, 4> sun_colour; ///< rgb, a: its strength in light0 units
};
static_assert(sizeof(LampPush) == 128 && offsetof(LampPush, sun_dir) == 96);
/// lamp_bounce_resolve.comp's push constants.
struct ResolvePush {
    std::array<float, 12> reproject;
    std::array<float, 4> proj, prev_proj;
    std::array<float, 2> size;
    float blend;
    s32 history_valid;
};
static_assert(sizeof(ResolvePush) == 96);
} // namespace

void ScreenLighting::OnLampConstants(const float* data, u32 size) {
    // 144 bytes a lamp: 16 floats each first (position, 1/r^2, diffuse, specular, ...), then the
    // lamps' other data. Only plausible lamps are kept (another layout reads as nonsense).
    const u32 count = size / 144;
    for (u32 i = 0; i < count && lamps.size() < MaxLamps; ++i) {
        const float* l = data + i * 16;
        bool ok = l[3] > 1e-5f && l[3] < 100.0f && l[2] > -1000.0f && l[2] < 1000.0f;
        for (int c = 0; c < 16 && ok; ++c) {
            ok = std::isfinite(l[c]);
        }
        for (int c = 4; c < 7 && ok; ++c) {
            ok = l[c] >= 0.0f && l[c] < 1000.0f;
        }
        if (!ok || (l[4] == 0.0f && l[5] == 0.0f && l[6] == 0.0f)) {
            continue;
        }
        const bool seen = std::any_of(lamps.begin(), lamps.end(), [&](const auto& k) {
            return std::abs(k[0] - l[0]) < 1e-3f && std::abs(k[1] - l[1]) < 1e-3f &&
                   std::abs(k[2] - l[2]) < 1e-3f;
        });
        if (!seen) {
            std::array<float, 16> lamp{};
            std::copy(l, l + 16, lamp.begin());
            lamps.push_back(lamp);
        }
    }
}

bool ScreenLighting::WantsCharacters() const {
    return rays && Enabled() && BbSettings::Get().ray_traced_ao && rays->Scene();
}

void ScreenLighting::OnCharacterDraw(u32 store, u32 first_vertex, u32 vertices,
                                     const void* indices, u32 index_size, u32 count,
                                     s32 base_vertex, VideoCore::ImageId pass_depth) {
    if (pass_depth != character_pass) {
        character_source.clear();
        character_indices.clear();
        character_draw_of.clear();
        character_draws = 0;
        character_pass = pass_depth;
    }
    // At most a million corners and triangles a frame (a party and its enemies).
    constexpr std::size_t MaxCorners = 1u << 20;
    if (vertices == 0 || character_source.size() + vertices > MaxCorners ||
        character_indices.size() + count > 3 * MaxCorners ||
        character_draws >= RayOcclusion::MaxCharacterDraws) {
        return;
    }
    const u32 base = u32(character_source.size());
    for (u32 v = 0; v < vertices; ++v) {
        character_source.push_back(store + v);
        character_draw_of.push_back(character_draws);
    }
    ++character_draws;
    const auto corner = [&](u32 i) -> s64 {
        const u32 raw = index_size == 2 ? static_cast<const u16*>(indices)[i]
                                        : static_cast<const u32*>(indices)[i];
        const s64 v = s64(raw) + base_vertex - s64(first_vertex);
        return v >= 0 && v < s64(vertices) ? s64(base) + v : -1;
    };
    for (u32 t = 0; t + 2 < count; t += 3) {
        const s64 a = corner(t), b = corner(t + 1), c = corner(t + 2);
        if (a < 0 || b < 0 || c < 0) {
            continue;
        }
        character_indices.push_back(u32(a));
        character_indices.push_back(u32(b));
        character_indices.push_back(u32(c));
    }
}

void ScreenLighting::OnSunConstants(const float* data) {
    const float x = data[80], y = data[81], z = data[82];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (std::abs(length - 1.0f) > 0.01f || !std::isfinite(data[76]) || data[76] < 0.0f) {
        return;
    }
    // The constant is the way the light travels: toward the sun is its opposite (Kaniel 2026-10-21,
    // "check if the global light lights from the correct direction": the game's light0 on the
    // street rises with N.L against -v, corr 0.77, and not with +v, corr -0.11; rtgi-sunfit.py).
    sun_dir = {-x, -y, -z, 1.0f};
    sun_age = 0;
    sun_colour = {data[76], data[77], data[78], 0.0f};
}

void ScreenLighting::OnLightingDone() {
    if (lamps_done) {
        return;
    }
    lamps_done = true;
    if (!rays || !Enabled() || !BbSettings::Get().ray_traced_ao || lamps.empty() || RayPartOff("lamps") ||
        !light_targets[0] || !light_targets[1] || !normals) {
        return;
    }
    const auto depth_id = camera_motion.Depth();
    const auto proj = camera_motion.SceneProjection();
    const auto world = camera_motion.SceneInverseView();
    vk::AccelerationStructureKHR scene_as = rays->Scene();
    if (!scene_as || !depth_id || proj[0] == 0.0f || world[0] == 0.0f && world[1] == 0.0f &&
                                                        world[2] == 0.0f) {
        return;
    }
    auto& diffuse = texture_cache.GetImage(light_targets[0]);
    auto& specular = texture_cache.GetImage(light_targets[1]);
    auto& depth = texture_cache.GetImage(depth_id);
    auto& normal = texture_cache.GetImage(normals);
    if (!gloss || !specular_colour) {
        return;
    }
    auto& gloss_image = texture_cache.GetImage(gloss);
    auto& f0_image = texture_cache.GetImage(specular_colour);
    const u32 w = diffuse.info.size.width, h = diffuse.info.size.height;
    // Native scene size only for now (live resolution draws into reduced proxies).
    if (scene_targets.Reduced() || !(diffuse.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        !(specular.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        depth.info.size.width != w || depth.info.size.height != h ||
        normal.info.size.width != w || normal.info.size.height != h ||
        gloss_image.info.size.width != w || gloss_image.info.size.height != h ||
        f0_image.info.size.width != w || f0_image.info.size.height != h) {
        if (!lamps_logged) {
            std::printf("Lamp shadows: not at this size or format (reduced %d)\n",
                        int(scene_targets.Reduced()));
            lamps_logged = true;
        }
        return;
    }
    const auto device = instance.GetDevice();
    if (!lamp_pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 15> bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {4, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eCompute},
            {5, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {6, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {7, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {8, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {9, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {10, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {11, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {12, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {13, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {14, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        lamp_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = sizeof(LampPush)};
        lamp_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*lamp_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
        const auto module = CompileSPV(LAMP_SHADOWS_COMP, device);
        lamp_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *lamp_layout,
                }));
        device.destroyShaderModule(module);
        const std::array<vk::DescriptorSetLayoutBinding, 4> resolve_bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        resolve_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(resolve_bindings.size()),
            .pBindings = resolve_bindings.data(),
        }));
        const vk::PushConstantRange resolve_push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                                 .offset = 0,
                                                 .size = sizeof(ResolvePush)};
        resolve_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*resolve_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &resolve_push,
        }));
        const auto resolve_module = CompileSPV(LAMP_BOUNCE_RESOLVE_COMP, device);
        resolve_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = resolve_module,
                              .pName = "main"},
                    .layout = *resolve_layout,
                }));
        device.destroyShaderModule(resolve_module);
    }
    // Step 2c: the bounce's images at light0's size (a new size starts the history again).
    if (bounce_width != w || bounce_height != h) {
        if (bounce_images[0].image) {
            scheduler.Finish();
        }
        for (u32 i = 0; i < bounce_images.size(); ++i) {
            bounce_views[i].reset();
            bounce_images[i] = VideoCore::UniqueImage(device, instance.GetAllocator());
            bounce_images[i].Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .extent = {w, h, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            });
            bounce_views[i] = Check(device.createImageViewUnique({
                .image = vk::Image(bounce_images[i]),
                .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        bounce_width = w;
        bounce_height = h;
        history_valid = false;
        last_scene_valid = false;
        history_next = 1;
    }
    // The lamps into the next of three host buffers (the frames before may still read theirs).
    auto& buf = lamp_buffers[lamp_buffer_next];
    lamp_buffer_next = (lamp_buffer_next + 1) % u32(lamp_buffers.size());
    if (!buf.buffer) {
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = MaxLamps * 16 * sizeof(float),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer handle{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &handle, &allocation,
                            &info) != VK_SUCCESS) {
            return;
        }
        buf = {handle, allocation, info.pMappedData};
    }
    std::memcpy(buf.mapped, lamps.data(), lamps.size() * sizeof(lamps[0]));
    vmaFlushAllocation(instance.GetAllocator(), static_cast<VmaAllocation>(buf.allocation), 0,
                       VK_WHOLE_SIZE);

    scheduler.EndRendering();
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    const vk::ImageView depth_view =
        CachedView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
    const vk::ImageView normal_view =
        CachedView(normal, normal.info.pixel_format, vk::ImageAspectFlagBits::eColor);
    const vk::ImageView gloss_view =
        CachedView(gloss_image, gloss_image.info.pixel_format, vk::ImageAspectFlagBits::eColor);
    const vk::ImageView f0_view =
        CachedView(f0_image, f0_image.info.pixel_format, vk::ImageAspectFlagBits::eColor);
    const vk::ImageView diffuse_view =
        CachedView(diffuse, diffuse.info.pixel_format, vk::ImageAspectFlagBits::eColor);
    const vk::ImageView specular_view =
        CachedView(specular, specular.info.pixel_format, vk::ImageAspectFlagBits::eColor);
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&normal, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&gloss_image, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&f0_image, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&diffuse, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    rw);
    runtime.Transit(&specular, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader, rw);
    runtime.FlushBarriers();
    // Step 3: this frame's characters join the map in the ray scene (their depth set against
    // the depth buffer's, character_world.comp).
    const bool characters = !RayPartOff("chars") && !character_indices.empty() && character_positions &&
                            character_pass == depth_id;
    rays->CountCharacterFrame(characters);
    if (characters) {
        rays->BuildCharacters(character_positions, character_source, character_indices,
                              character_draw_of, character_draws, depth_view, w, h, proj, world);
    }
    scene_as = rays->Scene();
    if (!lamps_logged || (sun_dir[3] > 0.0f) != sun_logged) {
        sun_logged = sun_dir[3] > 0.0f;
        std::printf("Lamp shadows: %zu lamps, %ux%u, sun %s (%.3f %.3f %.3f)\n", lamps.size(), w, h,
                    sun_logged ? "found" : "none", sun_colour[0], sun_colour[1], sun_colour[2]);
        std::fflush(stdout);
        lamps_logged = true;
    }
    const LampPush push{
        .proj = proj,
        .size = {float(w), float(h)},
        .count = s32(lamps.size()),
        .strength = 1.0f, // the blocked light taken out whole (Kaniel 2026-10-21: "almost identical")
        .world = world,
        // Step 2: the lamps' bounce (Bounced light; 2 rays a pixel, the upscaler averages frames).
        // A quarter of the pixels a frame, in turn (Kaniel 2026-10-21, "lags really bad").
        .bounce_rays = (RayPartOff("bounce")                                          ? 0
                        : BbSettings::Get().screen_lighting == BbSettings::ScreenLightingHigh ? 4
                                                                                        : 2) |
                       s32((frames_seen % 4) << 8),
        // The game's look (Kaniel 2026-10-21, "preserve its style"): x2.5 bounce and sky 30 were
        // the stylised try; outdoors the sky then gave ~200x the sun's light and washed out the
        // game's own shadows.
        .bounce_strength = std::clamp(BbSettings::Get().screen_lighting_gi.load(), 0.0f, 4.0f),
        .noise = std::fmod(float(frames_seen % 64) * 0.618034f, 1.0f),
        .sky = 1.5f,
        .sun_dir = sun_dir,
        // The street at dusk: light0 rose ~0.04 per unit N.L of the directional light.
        .sun_colour = {sun_colour[0], sun_colour[1], sun_colour[2], 0.05f},
    };
    ResolvePush resolve{
        .proj = proj,
        .size = {float(w), float(h)},
        .blend = 0.06f, // ~16 frames (0.12 flickered)
        .history_valid = 0,
    };
    resolve.history_valid =
        history_valid && camera_motion.SceneReprojection(resolve.reproject, resolve.prev_proj) ? 1 : 0;
    const u32 write_history = history_next, read_history = 3 - history_next;
    history_next = read_history;
    const bool first_use = !history_valid;
    history_valid = true;
    const std::array<vk::Image, 4> bounce_imgs{vk::Image(bounce_images[0]), vk::Image(bounce_images[1]),
                                               vk::Image(bounce_images[2]),
                                               vk::Image(bounce_images[3])};
    // Part 2: the last frame's scene (its first frame: this frame's bounce, never read as one).
    const vk::ImageView last_scene_v = last_scene_valid ? *bounce_views[3] : *bounce_views[0];
    const s32 last_scene_on = last_scene_valid ? 1 : 0;
    const vk::ImageView current_v = *bounce_views[0], history_v = *bounce_views[read_history],
                        next_v = *bounce_views[write_history];
    const vk::DescriptorBufferInfo map_positions = rays->Positions();
    const vk::DescriptorBufferInfo map_indices = rays->Indices();
    const vk::DescriptorBufferInfo map_colors = rays->Colors();
    const vk::DescriptorBufferInfo map_tri_patches = rays->TriPatches();
    const vk::DescriptorBufferInfo map_patches = rays->Patches();
    const vk::Buffer lamp_buffer = buf.buffer;
    fog_lamps = lamp_buffer;
    fog_lamp_count = u32(lamps.size());
    fog_frame = frames_seen;
    Profiled(0x5A7E1ull, "ray: lamp shadows, bounce, reflections", [&] { scheduler.RecordCrumb(
        {.name = "lamp shadows"},
        [=, pipeline = *lamp_pipeline, layout = *lamp_layout, resolve_pipe = *resolve_pipeline,
         resolve_lay = *resolve_layout](vk::CommandBuffer cmdbuf) {
            // The bounce images to General (their first use: from Undefined).
            for (const vk::Image image : bounce_imgs) {
                const vk::ImageMemoryBarrier2 to_general{
                    .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                    .oldLayout = first_use ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .image = image,
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                };
                cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
            }
            const vk::DescriptorImageInfo current_info{.imageView = current_v,
                                                       .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo normal_info{.imageView = normal_view,
                                                      .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo diffuse_info{.imageView = diffuse_view,
                                                       .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo specular_info{.imageView = specular_view,
                                                        .imageLayout = vk::ImageLayout::eGeneral};
            const vk::WriteDescriptorSetAccelerationStructureKHR scene_info{
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &scene_as,
            };
            const vk::DescriptorBufferInfo lamps_info{lamp_buffer, 0, VK_WHOLE_SIZE};
            const vk::DescriptorImageInfo gloss_info{.imageView = gloss_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo f0_info{.imageView = f0_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo last_scene_info{.imageView = last_scene_v,
                                                          .imageLayout = vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 15> writes = {{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &depth_info},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &normal_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &diffuse_info},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &specular_info},
                {.pNext = &scene_info, .dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eAccelerationStructureKHR},
                {.dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lamps_info},
                {.dstBinding = 6, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &map_positions},
                {.dstBinding = 7, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &map_indices},
                {.dstBinding = 8, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &map_colors},
                {.dstBinding = 9, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &current_info},
                {.dstBinding = 10, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &gloss_info},
                {.dstBinding = 11, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &f0_info},
                {.dstBinding = 12, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &last_scene_info},
                {.dstBinding = 13, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &map_tri_patches},
                {.dstBinding = 14, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &map_patches},
            }};
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmdbuf.dispatch((push.size[0] > 0 ? (u32(push.size[0]) + 7) / 8 : 0),
                            (u32(push.size[1]) + 7) / 8, 1);
            // Step 2c: the bounce through the history, into light0.
            const vk::MemoryBarrier2 traced{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &traced});
            const vk::DescriptorImageInfo history_info{.imageView = history_v,
                                                       .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo next_info{.imageView = next_v,
                                                    .imageLayout = vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 4> resolve_writes = {{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &current_info},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &history_info},
                {.dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &next_info},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &diffuse_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, resolve_pipe);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, resolve_lay, 0,
                                        resolve_writes);
            cmdbuf.pushConstants(resolve_lay, vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(resolve), &resolve);
            cmdbuf.dispatch((u32(push.size[0]) + 7) / 8, (u32(push.size[1]) + 7) / 8, 1);
            const vk::MemoryBarrier2 after{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        }); });
}

bool ScreenLighting::Enabled() const {
    return supported && BbSettings::Get().screen_lighting != BbSettings::ScreenLightingOff;
}

vk::ImageView ScreenLighting::CachedView(const VideoCore::Image& image, vk::Format format,
                                         vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

void ScreenLighting::CreatePipelines() {
    if (trace_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const auto layout = [&](std::initializer_list<vk::DescriptorType> types,
                            vk::UniqueDescriptorSetLayout& set_layout,
                            vk::UniquePipelineLayout& pipeline_layout) {
        std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
        u32 count = 0;
        for (const auto type : types) {
            bindings[count] = {.binding = count,
                               .descriptorType = type,
                               .descriptorCount = 1,
                               .stageFlags = vk::ShaderStageFlagBits::eCompute};
            ++count;
        }
        set_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = count,
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = sizeof(PushConstants)};
        pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    };
    const auto compute = [&](const auto& code, vk::PipelineLayout pipeline_layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = pipeline_layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    using T = vk::DescriptorType;
    layout({T::eSampledImage, T::eStorageImage, T::eStorageImage, T::eSampledImage},
           trace_desc_layout, trace_layout);
    layout({T::eSampledImage, T::eStorageImage, T::eSampledImage, T::eSampledImage},
           apply_desc_layout, apply_layout);
    trace_pipeline = compute(SCREEN_LIGHTING_TRACE_COMP, *trace_layout);
    if (rays) {
        layout({T::eSampledImage, T::eStorageImage, T::eStorageImage, T::eSampledImage,
                T::eAccelerationStructureKHR, T::eStorageBuffer, T::eStorageBuffer},
               trace_rt_desc_layout, trace_rt_layout);
        trace_rt_pipeline = compute(SCREEN_LIGHTING_TRACE_RT_COMP, *trace_rt_layout);
    }
    apply_pipeline = compute(SCREEN_LIGHTING_APPLY_COMP, *apply_layout);
}

bool ScreenLighting::EnsureTraceImage(u32 width, u32 height) {
    if (trace_image.image && trace_width == width && trace_height == height) {
        return true;
    }
    // A size change (quality or resolution): the previous frames' dispatches may still read it.
    if (trace_image.image) {
        scheduler.Finish();
    }
    const auto device = instance.GetDevice();
    trace_view.reset();
    trace_image = VideoCore::UniqueImage(device, instance.GetAllocator());
    trace_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    trace_view = Check(device.createImageViewUnique({
        .image = vk::Image(trace_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    trace_width = width;
    trace_height = height;
    return true;
}

void ScreenLighting::RecordFogLight(vk::ImageView color_view, vk::ImageView depth_view, u32 w,
                                    u32 h) {
    if (!rays || !BbSettings::Get().ray_traced_ao || fog_frame != frames_seen || !fog_lamps ||
        RayPartOff("fog")) {
        return;
    }
    const vk::AccelerationStructureKHR scene_as = rays->Scene();
    const auto proj = camera_motion.SceneProjection();
    const auto world = camera_motion.SceneInverseView();
    if (!scene_as || proj[0] == 0.0f) {
        return;
    }
    const auto device = instance.GetDevice();
    if (!fog_pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 5> bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
            {4, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        fog_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = 128};
        fog_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*fog_desc_layout,
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
                        .layout = *fog_layout,
                    }));
            device.destroyShaderModule(module);
            return pipeline;
        };
        fog_pipeline = compute(FOG_LIGHT_COMP);
        fog_apply_pipeline = compute(FOG_APPLY_COMP);
    }
    // The march's image at half the scene's size (a new size: the frames before may still read it).
    const u32 fw = (w + 1) / 2, fh = (h + 1) / 2;
    if (fog_width != fw || fog_height != fh) {
        if (fog_image.image) {
            scheduler.Finish();
        }
        fog_view.reset();
        fog_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        fog_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {fw, fh, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        fog_view = Check(device.createImageViewUnique({
            .image = vk::Image(fog_image),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        fog_width = fw;
        fog_height = fh;
    }
    const vk::Image fog_img = vk::Image(fog_image);
    const vk::ImageView fog_v = *fog_view;
    struct {
        std::array<float, 4> proj;
        std::array<float, 2> size;
        s32 count;
        float density;
        std::array<float, 12> world;
        std::array<float, 4> sun_dir;
        std::array<float, 4> sun_colour;
        float noise;
        float lamp_strength;
        float sun_strength;
        float pad;
    } push{proj,
           {float(w), float(h)},
           s32(fog_lamp_count),
           0.015f, // the game's look (0.03 was the stylised try, "let's try 2")
           world,
           sun_dir,
           {sun_colour[0], sun_colour[1], sun_colour[2], 0.05f},
           std::fmod(float(frames_seen % 64) * 0.618034f, 1.0f),
           0.12f, // lamps: a room full of them hazed white at 1 (2026-10-21 hall shot)
           0.35f,
           0.0f};
    static_assert(sizeof(push) == 128);
    const vk::Buffer lamps_buffer = fog_lamps;
    Profiled(0x5A7E2ull, "ray: fog light", [&] { scheduler.RecordCrumb(
        {.name = "fog light"},
        [=, pipeline = *fog_pipeline, apply = *fog_apply_pipeline,
         layout = *fog_layout](vk::CommandBuffer cmdbuf) {
            const vk::MemoryBarrier2 before{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            };
            // The march overwrites all of the half image (the last frame's apply read it).
            const vk::ImageMemoryBarrier2 fog_to_general{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = fog_img,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                                     .pMemoryBarriers = &before,
                                     .imageMemoryBarrierCount = 1,
                                     .pImageMemoryBarriers = &fog_to_general});
            const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::WriteDescriptorSetAccelerationStructureKHR scene_info{
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &scene_as,
            };
            const vk::DescriptorBufferInfo lamps_info{lamps_buffer, 0, VK_WHOLE_SIZE};
            const vk::DescriptorImageInfo fog_info{.imageView = fog_v,
                                                   .imageLayout = vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 5> writes = {{
                {.dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &depth_info},
                {.dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &color_info},
                {.pNext = &scene_info, .dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eAccelerationStructureKHR},
                {.dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &lamps_info},
                {.dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &fog_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmdbuf.dispatch((fw + 7) / 8, (fh + 7) / 8, 1);
            const vk::MemoryBarrier2 marched{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &marched});
            // The same descriptors and constants; the apply reads the half image back.
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, apply);
            cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
            const vk::MemoryBarrier2 after{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        }); });
}

void ScreenLighting::KeepLastScene(const VideoCore::Image& color, bool reduced, u32 w, u32 h) {
    if (reduced || !bounce_images[3].image || w != bounce_width || h != bounce_height ||
        !BbSettings::Get().ray_traced_ao) {
        last_scene_valid = false;
        return;
    }
    const vk::Image source = color.GetImage(), target = vk::Image(bounce_images[3]);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {w, h, 1},
    };
    scheduler.RecordCrumb({.name = "keep last scene"}, [=](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        cmdbuf.copyImage(source, vk::ImageLayout::eGeneral, target, vk::ImageLayout::eGeneral, region);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    });
    last_scene_valid = true;
}

void ScreenLighting::OnOpaqueSceneDone(bool temporal) {
    if (done || !scene_color || !Enabled()) {
        return;
    }
    done = true;
    const auto depth_id = camera_motion.Depth();
    const auto proj = camera_motion.SceneProjection();
    if (!depth_id || proj[0] == 0.0f || proj[1] == 0.0f) {
        return;
    }
    auto& color = texture_cache.GetImage(scene_color);
    auto& depth = texture_cache.GetImage(depth_id);
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        return;
    }
    // With live scene resolution the scene is drawn into reduced proxies: read and write those.
    const bool reduced = scene_targets.Reduced() && scene_targets.EligibleScene(color) &&
                         scene_targets.EligibleScene(depth);
    const u32 w = reduced ? scene_targets.Size().width : color.info.size.width;
    const u32 h = reduced ? scene_targets.Size().height : color.info.size.height;
    if (!reduced && !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        if (!logged) {
            std::printf("Screen lighting: the scene color is not a storage image, off\n");
            logged = true;
        }
        return;
    }
    const bool high = BbSettings::Get().screen_lighting == BbSettings::ScreenLightingHigh;
    const u32 tw = high ? w : std::max(1u, w / 2), th = high ? h : std::max(1u, h / 2);
    CreatePipelines();
    EnsureTraceImage(tw, th);

    scheduler.EndRendering();
    vk::ImageView color_view{}, depth_view{}, albedo_view{};
    vk::ImageLayout albedo_layout = vk::ImageLayout::eGeneral;
    std::array<float, 2> albedo_size{};
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto c = scene_targets.Read(scene_color, ci, vk::PipelineStageFlagBits2::eComputeShader,
                                          rw);
        ci.format = depth.info.pixel_format;
        const auto d = scene_targets.Read(depth_id, ci);
        if (!(c.usage & vk::ImageUsageFlagBits::eStorage)) {
            return;
        }
        color_view = c.view;
        depth_view = d.view;
    } else {
        color_view = CachedView(color, vk::Format::eR16G16B16A16Sfloat,
                                vk::ImageAspectFlagBits::eColor);
        depth_view = CachedView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
        runtime.Transit(&depth, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader, rw);
    }
    if (albedo) {
        auto& image = texture_cache.GetImage(albedo);
        if (IsAlbedoFormat(image.info.pixel_format) &&
            (image.usage_flags & vk::ImageUsageFlagBits::eSampled)) {
            VideoCore::ImageViewInfo ci;
            ci.format = image.info.pixel_format;
            if (const auto proxy = reduced ? scene_targets.SampleProxy(image, ci) : std::nullopt) {
                albedo_view = proxy->view;
                albedo_layout = proxy->layout;
                albedo_size = {float(w), float(h)};
            } else {
                albedo_view = CachedView(image, image.info.pixel_format,
                                         vk::ImageAspectFlagBits::eColor);
                runtime.Transit(&image, vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eComputeShader,
                                vk::AccessFlagBits2::eShaderRead);
                albedo_size = {float(image.info.size.width), float(image.info.size.height)};
            }
        }
    }
    vk::ImageView normals_view{};
    vk::ImageLayout normals_layout = vk::ImageLayout::eGeneral;
    if (normals) {
        auto& image = texture_cache.GetImage(normals);
        if (IsNormalFormat(image.info.pixel_format) &&
            (image.usage_flags & vk::ImageUsageFlagBits::eSampled) &&
            (reduced ? image.info.size.width == color.info.size.width &&
                           image.info.size.height == color.info.size.height
                     : image.info.size.width == w && image.info.size.height == h)) {
            VideoCore::ImageViewInfo ci;
            ci.format = image.info.pixel_format;
            if (const auto proxy = reduced ? scene_targets.SampleProxy(image, ci) : std::nullopt) {
                normals_view = proxy->view;
                normals_layout = proxy->layout;
            } else if (!reduced) {
                normals_view = CachedView(image, image.info.pixel_format,
                                          vk::ImageAspectFlagBits::eColor);
                runtime.Transit(&image, vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eComputeShader,
                                vk::AccessFlagBits2::eShaderRead);
            }
        }
    }
    // BB_LIGHTING_DUMP: the G-buffer targets as they are (native scene size only).
    const bool dump = dump_due && !reduced;
    if (dump) {
        for (const auto id : light_targets) {
            if (id) {
                runtime.Transit(&texture_cache.GetImage(id), vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferRead);
            }
        }
        for (const auto id : gbuffer) {
            if (id && id != scene_color) {
                runtime.Transit(&texture_cache.GetImage(id), vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eTransfer,
                                vk::AccessFlagBits2::eTransferRead);
            }
        }
    }
    runtime.FlushBarriers();
    if (!logged) {
        std::printf("Screen lighting: %s, scene %ux%u, trace %ux%u, albedo %s, normals %s\n",
                    high ? "high" : "low", w, h, tw, th, albedo_view ? "found" : "none (grey)",
                    normals_view ? "the G-buffer's" : "from depth");
        std::fflush(stdout);
        logged = true;
    }

    const auto& settings = BbSettings::Get();
    ++frame;
    const PushConstants push{
        .proj = proj,
        .size = {float(w), float(h)},
        .trace_size = {float(tw), float(th)},
        .albedo_size = albedo_size,
        .radius = Radius,
        .max_radiance = MaxRadiance,
        // Golden-ratio steps: each frame's directions fall between the last ones'.
        .noise = temporal ? std::fmod(float(frame % 64) * 0.618034f, 1.0f) : 0.0f,
        .ao_strength = std::clamp(settings.screen_lighting_ao.load(), 0.0f, 1.0f),
        .gi_strength = std::clamp(settings.screen_lighting_gi.load(), 0.0f, 4.0f),
        .directions = high ? 3 : 2,
        .steps = high ? 10 : 6,
        .view = view_mode,
        .normals = normals_view ? 1 : 0,
        .rays = 0,
        .world = camera_motion.SceneInverseView(),
    };
    // The map's acceleration structure, once built: the trace's rays go against it too.
    const vk::AccelerationStructureKHR scene_as =
        rays && settings.ray_traced_ao ? rays->Scene() : vk::AccelerationStructureKHR{};
    const vk::DescriptorBufferInfo map_indices =
        scene_as ? rays->Indices() : vk::DescriptorBufferInfo{};
    const vk::DescriptorBufferInfo map_colors = scene_as ? rays->Colors() : vk::DescriptorBufferInfo{};
    // No camera yet (all zero): no rays.
    const bool has_world = push.world[0] != 0.0f || push.world[1] != 0.0f || push.world[2] != 0.0f;
    PushConstants push_rt = push;
    push_rt.rays = scene_as && has_world ? (high ? 6 : 3) : 0;
    if (scene_as && !rays_logged) {
        std::printf("Screen lighting: ray-traced occlusion on (%d rays a pixel)\n", push_rt.rays);
        std::fflush(stdout);
        rays_logged = true;
    }
    const vk::Image trace = vk::Image(trace_image);
    const vk::ImageView trace_v = *trace_view;
    // The apply pass samples the trace; without an albedo the trace stands in (unread).
    const vk::ImageView albedo_v = albedo_view ? albedo_view : trace_v;
    const vk::ImageLayout albedo_l = albedo_view ? albedo_layout : vk::ImageLayout::eGeneral;
    const vk::ImageView normals_v = normals_view ? normals_view : trace_v;
    const vk::ImageLayout normals_l = normals_view ? normals_layout : vk::ImageLayout::eGeneral;
    const auto record =
        [=, trace_pipe = scene_as ? *trace_rt_pipeline : *trace_pipeline,
         trace_lay = scene_as ? *trace_rt_layout : *trace_layout,
         apply_pipe = *apply_pipeline, apply_lay = *apply_layout](vk::CommandBuffer cmdbuf) {
            const auto image_barrier = [&](vk::PipelineStageFlags2 src_stage,
                                           vk::AccessFlags2 src_access, vk::ImageLayout old_layout,
                                           vk::AccessFlags2 dst_access) {
                const vk::ImageMemoryBarrier2 barrier{
                    .srcStageMask = src_stage,
                    .srcAccessMask = src_access,
                    .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                    .dstAccessMask = dst_access,
                    .oldLayout = old_layout,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .image = trace,
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                };
                cmdbuf.pipelineBarrier2(
                    {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
            };
            // The last frame's apply read it: wait for that before the trace overwrites it.
            image_barrier(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone,
                          vk::ImageLayout::eUndefined, vk::AccessFlagBits2::eShaderWrite);
            const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo trace_info{.imageView = trace_v,
                                                     .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo albedo_info{.imageView = albedo_v,
                                                      .imageLayout = albedo_l};
            const vk::DescriptorImageInfo normals_info{.imageView = normals_v,
                                                       .imageLayout = normals_l};
            const vk::WriteDescriptorSetAccelerationStructureKHR scene_info{
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &scene_as,
            };
            const std::array<vk::WriteDescriptorSet, 7> trace_writes = {{
                {.dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &depth_info},
                {.dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &color_info},
                {.dstBinding = 2,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &trace_info},
                {.dstBinding = 3,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &normals_info},
                {.pNext = &scene_info,
                 .dstBinding = 4,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eAccelerationStructureKHR},
                {.dstBinding = 5,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &map_indices},
                {.dstBinding = 6,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageBuffer,
                 .pBufferInfo = &map_colors},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, trace_pipe);
            cmdbuf.pushDescriptorSetKHR(
                vk::PipelineBindPoint::eCompute, trace_lay, 0,
                vk::ArrayProxy<const vk::WriteDescriptorSet>(scene_as ? 7u : 4u,
                                                             trace_writes.data()));
            cmdbuf.pushConstants(trace_lay, vk::ShaderStageFlagBits::eCompute, 0,
                                 sizeof(push_rt), &push_rt);
            cmdbuf.dispatch((u32(push.trace_size[0]) + 7) / 8, (u32(push.trace_size[1]) + 7) / 8, 1);
            // The trace is read, and the scene color it read is written next.
            image_barrier(vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral,
                          vk::AccessFlagBits2::eShaderRead);
            const vk::MemoryBarrier2 scene_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &scene_read});
            const std::array<vk::WriteDescriptorSet, 4> apply_writes = {{
                {.dstBinding = 0,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &depth_info},
                {.dstBinding = 1,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eStorageImage,
                 .pImageInfo = &color_info},
                {.dstBinding = 2,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &trace_info},
                {.dstBinding = 3,
                 .descriptorCount = 1,
                 .descriptorType = vk::DescriptorType::eSampledImage,
                 .pImageInfo = &albedo_info},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, apply_pipe);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, apply_lay, 0,
                                        apply_writes);
            cmdbuf.pushConstants(apply_lay, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                 &push);
            cmdbuf.dispatch((u32(push.size[0]) + 7) / 8, (u32(push.size[1]) + 7) / 8, 1);
            // The game's next draws and dispatches read and write the scene color.
            const vk::MemoryBarrier2 after{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask =
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
        };
    if (!dump) {
        Profiled(0x5A7E3ull, "ray: screen lighting (occlusion)",
                 [&] { scheduler.RecordCrumb({.name = "screen lighting"}, record); });
        if (!reduced) {
            RecordFogLight(color_view, depth_view, w, h);
        }
        KeepLastScene(color, reduced, w, h);
        return;
    }
    dump_due = false;
    const int index = dumps++;
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto dump_image = [&](const VideoCore::Image& image, const std::string& name,
                                vk::ImageAspectFlagBits aspect) {
        const u32 bytes = BytesPerTexel(image.info.pixel_format);
        if (!bytes) {
            std::printf("Screen lighting dump: %s has %s, skipped\n", name.c_str(),
                        vk::to_string(image.info.pixel_format).c_str());
            return;
        }
        const std::string format = vk::to_string(image.info.pixel_format);
        DumpImages(instance, scheduler, cmdbuf, index,
                   {{image.GetImage(), image.info.size.width, image.info.size.height, bytes,
                     name.c_str(), format.c_str(), aspect}});
    };
    for (u32 i = 0; i < gbuffer.size(); ++i) {
        if (gbuffer[i] && gbuffer[i] != scene_color) {
            dump_image(texture_cache.GetImage(gbuffer[i]), "gbuffer" + std::to_string(i),
                       vk::ImageAspectFlagBits::eColor);
        }
    }
    for (u32 i = 0; i < light_targets.size(); ++i) {
        if (light_targets[i]) {
            dump_image(texture_cache.GetImage(light_targets[i]), "light" + std::to_string(i),
                       vk::ImageAspectFlagBits::eColor);
        }
    }
    dump_image(depth, "depth", vk::ImageAspectFlagBits::eDepth);
    dump_image(color, "scene_before", vk::ImageAspectFlagBits::eColor);
    record(cmdbuf);
    dump_image(color, "scene_after", vk::ImageAspectFlagBits::eColor);
    DumpImages(instance, scheduler, cmdbuf, index,
               {{trace, tw, th, 8, "trace", "R16G16B16A16Sfloat"}});
    if (rays) {
        const char* dir = std::getenv("BB_DUMP_DIR");
        rays->DumpCharacters(std::string(dir && dir[0] ? dir : "out/dump") + "/f" +
                             (index < 10 ? "00" : index < 100 ? "0" : "") + std::to_string(index) +
                             "_characters.raw");
    }
    std::printf("Screen lighting dump %d: proj %.5f %.5f %.6f %.6f, %s\n", index, proj[0],
                proj[1], proj[2], proj[3], albedo_view ? "albedo found" : "no albedo");
    // The camera (view to world, 3x4 rows): the dump's depth placed in the world, offline.
    const auto inv = camera_motion.SceneInverseView();
    std::printf("Screen lighting dump %d: inverse view", index);
    for (const float v : inv) {
        std::printf(" %.6f", v);
    }
    std::printf("\n");
    std::fflush(stdout);
}

} // namespace Vulkan
