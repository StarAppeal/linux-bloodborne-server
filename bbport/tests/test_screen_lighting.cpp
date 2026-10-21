// SPDX-License-Identifier: GPL-2.0-or-later
// host_shaders/screen_lighting.glsl, compiled as C++: screen-space ambient occlusion and one
// bounce of indirect light (Kaniel 2026-10-19, "maybe there is a chance to add ray tracing").
// The scene is traced analytically: a camera with Bloodborne's projection over a grey floor that
// meets a red wall, sky above.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

namespace glsl {
struct vec2 {
    float x = 0, y = 0;
    vec2() = default;
    vec2(double v) : x(float(v)), y(float(v)) {}
    vec2(double a, double b) : x(float(a)), y(float(b)) {}
};
struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3() = default;
    vec3(double v) : x(float(v)), y(float(v)), z(float(v)) {}
    vec3(double a, double b, double c) : x(float(a)), y(float(b)), z(float(c)) {}
    float& r() { return x; }
};
struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};
inline vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline vec2 operator-(vec2 a) { return {-a.x, -a.y}; }
inline vec2 operator*(vec2 a, float s) { return {a.x * s, a.y * s}; }
inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator-(vec3 a) { return {-a.x, -a.y, -a.z}; }
inline vec3 operator*(vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline vec3 operator/(vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline vec3& operator+=(vec3& a, vec3 b) { return a = a + b; }
inline vec3& operator*=(vec3& a, float s) { return a = a * s; }
inline float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(vec3 a) { return std::sqrt(dot(a, a)); }
inline vec3 normalize(vec3 a) { return a / length(a); }
inline vec3 cross(vec3 a, vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float abs(float v) { return std::fabs(v); }
inline float cos(float v) { return std::cos(v); }
inline float sin(float v) { return std::sin(v); }
inline float fract(float v) { return v - std::floor(v); }
inline float sqrt(float v) { return std::sqrt(v); }
using uint = unsigned;
inline float pow(float x, double y) { return std::pow(x, float(y)); }
inline float max(float a, double b) { return std::max(a, float(b)); }
inline float min(float a, double b) { return std::min(a, float(b)); }
inline float clamp(float x, double lo, double hi) { return std::min(std::max(x, float(lo)), float(hi)); }
inline float smoothstep(double e0, double e1, float x) {
    const float t = clamp((x - float(e0)) / float(e1 - e0), 0.0, 1.0);
    return t * t * (3.0f - 2.0f * t);
}

vec3 LightingViewPos(vec2 pixel);
vec3 LightingRadiance(vec2 pixel);
#include "screen_lighting.glsl"
} // namespace glsl

using namespace glsl;

namespace {
// Bloodborne's projection (docs/upscaler.md): x 1.42799, y 2.53865, near 0.05, far 3000; +y down.
const vec4 Proj{1.42799f, 2.53865f, 1.00002f, -0.0500679f};
const vec2 Size{640.0f, 360.0f};
const float FloorY = 1.6f;   ///< the floor, 1.6 below the camera (+y down the screen)
float WallZ = 6.0f;          ///< the red wall straight ahead
bool wall_on = true;
float wall_radiance = 1.0f;

vec3 Ray(vec2 pixel) {
    const float nx = (pixel.x + 0.5f) / Size.x * 2.0f - 1.0f;
    const float ny = (pixel.y + 0.5f) / Size.y * 2.0f - 1.0f;
    return vec3(nx / Proj.x, ny / Proj.y, 1.0);
}
} // namespace

namespace glsl {
vec3 LightingViewPos(vec2 pixel) {
    const vec3 d = Ray(vec2(std::floor(pixel.x), std::floor(pixel.y)));
    float t = 1e9f;
    if (d.y > 0.0f) t = FloorY / d.y;
    if (wall_on) t = std::min(t, WallZ);
    return t < 1e8f ? d * t : vec3(0.0); // z 0: sky
}
vec3 LightingRadiance(vec2 pixel) {
    const vec3 p = LightingViewPos(pixel);
    if (p.z <= 0.0f) return vec3(0.0);
    if (wall_on && std::fabs(p.z - WallZ) < 1e-3f) return vec3(wall_radiance, 0.0, 0.0);
    return vec3(0.2);
}
} // namespace glsl

namespace {
/// The pixel of a floor point at view depth z, in the middle column.
vec2 FloorPixel(float z) {
    const float ny = Proj.y * FloorY / z;
    return vec2(Size.x * 0.5f, std::floor((ny + 1.0f) * 0.5f * Size.y));
}

LightingResult Trace(vec2 pixel, float radius = 1.0f, float max_radiance = 8.0f) {
    const vec3 p = LightingViewPos(pixel);
    const vec3 n = NormalFromNeighbours(p, LightingViewPos(pixel + vec2(-1.0, 0.0)),
                                        LightingViewPos(pixel + vec2(1.0, 0.0)),
                                        LightingViewPos(pixel + vec2(0.0, -1.0)),
                                        LightingViewPos(pixel + vec2(0.0, 1.0)));
    return TraceLighting(pixel + vec2(0.5), p, n, radius, Proj.x * Size.x * 0.5f, 4, 12, 0.37f,
                         0.61f, max_radiance, Size);
}
} // namespace

int main() {
    // Depth to view z and back (D3D depth: zs + zo / z).
    for (const float z : {0.1f, 1.0f, 7.5f, 250.0f}) {
        const float depth = Proj.z + Proj.w / z;
        assert(std::fabs(ViewZ(depth, Proj) - z) < z * 1e-3f && "view z from depth");
    }

    // The floor's normal points up (toward -y, the camera's side) and faces the viewer.
    const vec2 open_floor = FloorPixel(2.5f);
    {
        const vec3 p = LightingViewPos(open_floor);
        const vec3 n = NormalFromNeighbours(p, LightingViewPos(open_floor + vec2(-1.0, 0.0)),
                                            LightingViewPos(open_floor + vec2(1.0, 0.0)),
                                            LightingViewPos(open_floor + vec2(0.0, -1.0)),
                                            LightingViewPos(open_floor + vec2(0.0, 1.0)));
        assert(std::fabs(p.y - FloorY) < 0.05f && "a floor pixel");
        assert(n.y < -0.99f && "floor normal up");
        // At the wall's foot the pixel above is the wall: the floor side is taken, no smearing.
        const vec2 foot = FloorPixel(WallZ - 0.02f);
        const vec3 pf = LightingViewPos(foot);
        const vec3 nf = NormalFromNeighbours(pf, LightingViewPos(foot + vec2(-1.0, 0.0)),
                                             LightingViewPos(foot + vec2(1.0, 0.0)),
                                             LightingViewPos(foot + vec2(0.0, -1.0)),
                                             LightingViewPos(foot + vec2(0.0, 1.0)));
        assert(nf.y < -0.9f && "the wall's foot is still floor");
        // A silhouette: the left and lower neighbours are far background; a wall facing the
        // camera keeps its normal (the closer neighbour on each axis).
        const vec3 ns = NormalFromNeighbours(vec3(0.0, 0.0, 5.0), vec3(-0.01, 0.0, 40.0),
                                             vec3(0.01, 0.0, 5.0), vec3(0.0, -0.01, 5.0),
                                             vec3(0.0, 0.01, 40.0));
        assert(ns.z < -0.99f && "silhouette: the closer neighbour");
    }

    // Open floor far from the wall: nothing occludes it, nothing bounces onto it.
    const LightingResult open = Trace(open_floor);
    assert(open.open > 0.97f && "open floor is not occluded");
    assert(open.light.x < 0.02f && "no wall light far from the wall");

    // The floor right at the red wall: occluded, and lit red by it.
    const LightingResult corner = Trace(FloorPixel(WallZ - 0.15f));
    assert(corner.open < 0.9f && "the corner is darker");
    assert(corner.light.x > 0.05f && corner.light.x > 2.0f * corner.light.y && "red bounce from the wall");
    // Energy: the light is at most the covered part of the hemisphere times the wall's radiance.
    assert(corner.light.x <= (1.0f - corner.open) * 1.0001f && "no light counted twice");
    // The bounce weakens with distance from the wall.
    const LightingResult near_wall = Trace(FloorPixel(WallZ - 0.6f));
    assert(near_wall.light.x < corner.light.x && near_wall.open > corner.open);

    // The radius: a wall beyond it neither occludes nor lights.
    const LightingResult small = Trace(FloorPixel(WallZ - 0.6f), 0.3f);
    assert(small.open > 0.97f && small.light.x < 0.01f && "beyond the radius");

    // A blinding wall (a lamp) is clamped: no fireflies.
    wall_radiance = 1000.0f;
    const LightingResult lamp = Trace(FloorPixel(WallZ - 0.15f), 1.0f, 8.0f);
    assert(lamp.light.x <= 8.0f && "radiance clamped");
    wall_radiance = 1.0f;

    // Sky: no wall at all, the floor stays open and nothing lights it.
    wall_on = false;
    const LightingResult sky = Trace(FloorPixel(WallZ - 0.15f));
    assert(sky.open > 0.97f && sky.light.x < 1e-3f && "sky neither occludes nor lights");
    wall_on = true;

    // Kaniel 2026-10-21 (ray tracing): the occlusion rays' directions are unit, never under the
    // surface, and cosine-weighted (their mean cosine to the normal is 2/3).
    for (const vec3 n : {vec3(0.0, -1.0, 0.0), normalize(vec3(0.3, 0.2, -0.9)), vec3(0.0, 1.0, 0.0)}) {
        double cos_sum = 0.0;
        const int count = 4096;
        for (int i = 0; i < count; ++i) {
            const vec3 d = HemisphereDir(n, fract(i * 0.618034f + 0.5f / count),
                                         fract(i * 0.7548777f + 0.25f));
            assert(std::fabs(length(d) - 1.0f) < 1e-4f && "unit");
            assert(dot(d, n) >= -1e-5f && "above the surface");
            cos_sum += dot(d, n);
        }
        assert(std::fabs(cos_sum / count - 2.0 / 3.0) < 0.02 && "cosine-weighted");
    }
    // A hit right at the surface occludes fully, one at the reach not at all; a miss is open.
    assert(RayOcclusionWeight(0.0f, 4.0f) == 1.0f && RayOcclusionWeight(4.0f, 4.0f) == 0.0f);
    assert(RayOcclusionWeight(1.0f, 4.0f) > RayOcclusionWeight(3.0f, 4.0f));
    assert(RayOcclusionWeight(-1.0f, 4.0f) == 0.0f && "no hit");
    // Kaniel 2026-10-21 ("I would have expected light to work more natural, not just be darker"):
    // the rays' visibility multiplies the screen's (halfway between the first, too weak, and the
    // squared, too dark); it never brightens what either sees as occluded.
    assert(std::fabs(CombineOcclusion(1.0f, 0.5f) - 0.5f) < 1e-6f && "halfway");
    assert(CombineOcclusion(0.8f, 0.5f) <= 0.8f * 0.5f + 1e-6f && "no lighter than both");
    assert(CombineOcclusion(0.8f, 1.0f) == 0.8f && "open sky: the screen's alone");
    assert(CombineOcclusion(0.3f, 0.9f) <= 0.3f);
    // A ray's hit brings back the surface's light: the map mesh's colour (0xAABBGGRR, sRGB: the
    // texture in the game's baked light) at the scene's brightness (~0.05 of it, the 2026-10-21
    // capture at Central Yharnam's lamp).
    {
        const vec3 white = MeshRadiance(0xFFFFFFFFu);
        assert(std::fabs(white.x - MeshRadianceScale) < 1e-6f && white.x == white.y && white.y == white.z);
        const vec3 red = MeshRadiance(0xFF0000FFu);
        assert(red.x > 0.0f && red.y == 0.0f && red.z == 0.0f && "red is the low byte");
        const vec3 mid = MeshRadiance(0xFF808080u);
        assert(std::fabs(mid.x / MeshRadianceScale - 0.2158605f) < 1e-3f && "sRGB decoded");
        assert(MeshRadianceScale > 0.02f && MeshRadianceScale < 0.2f);
    }

    // Kaniel 2026-10-21 ("do all steps": ray-traced lamp shadows). A lamp of the game's lighting
    // pass lights a surface by its colour, N.L and (1 - d^2/r^2)^2 (a fit to the captured light:
    // the lamp within half a metre of its logged place, its reach 6 m against the logged 7).
    {
        const vec3 p(0.0, 0.0, 5.0), up(0.0, -1.0, 0.0);
        const float inv_r2 = 1.0f / 49.0f;
        const float near = LampWeight(p, up, vec3(0.0, -1.0, 5.0), inv_r2, 1.0f);
        const float far = LampWeight(p, up, vec3(0.0, -4.0, 5.0), inv_r2, 1.0f);
        assert(near > far && far > 0.0f && "closer is brighter");
        assert(std::fabs(near - (1.0f - 1.0f / 49.0f) * (1.0f - 1.0f / 49.0f)) < 1e-5f);
        assert(LampWeight(p, up, vec3(0.0, -8.0, 5.0), inv_r2, 1.0f) == 0.0f && "beyond its reach");
        assert(LampWeight(p, up, vec3(0.0, 2.0, 5.0), inv_r2, 1.0f) == 0.0f && "behind the surface");
        assert(std::fabs(LampWeight(p, up, vec3(0.0, -1.0, 5.0), inv_r2, 3.0f) - 3.0f * near) < 1e-5f);
        // The light that stays: what the unblocked lamps give, of all the lamps give; no lamp, all.
        assert(LampShadowFactor(4.0f, 1.0f) == 0.75f && LampShadowFactor(0.0f, 0.0f) == 1.0f);
        assert(LampShadowFactor(2.0f, 2.0f) == 0.0f && LampShadowFactor(1.0f, 3.0f) == 0.0f);
    }

    // Kaniel 2026-10-21 ("let's do 1 and 2": ray-traced reflections). A glossy surface reflects its
    // specular colour head on and nearly all at grazing angles (Schlick); a rough one nothing.
    {
        assert(std::fabs(ReflectionWeight(0.04f, 1.0f, 1.0f) - 0.04f) < 1e-5f && "head on: F0");
        assert(ReflectionWeight(0.04f, 0.0f, 1.0f) > 0.99f && "grazing: all of it");
        assert(ReflectionWeight(0.04f, 0.3f, 1.0f) > ReflectionWeight(0.04f, 0.8f, 1.0f));
        assert(ReflectionWeight(0.5f, 1.0f, 0.2f) == 0.0f && "rough: no reflection");
        assert(ReflectionWeight(0.5f, 1.0f, 0.6f) < ReflectionWeight(0.5f, 1.0f, 0.9f));
    }

    // The noise is in 0..1.
    for (int i = 0; i < 100; ++i) {
        const float v = GradientNoise(vec2(i * 7.0, i * 3.0));
        assert(v >= 0.0f && v < 1.0f);
    }
    std::printf("PASS: screen lighting: open %.3f corner %.3f (red %.3f) near %.3f\n", open.open,
                corner.open, corner.light.x, near_wall.open);
    return 0;
}
