// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: screen-space ambient occlusion and one bounce of indirect light, shared by
// screen_lighting.comp; tests/test_screen_lighting.cpp compiles it as C++. The includer defines
// LightingViewPos (view-space position at a full-resolution pixel, z 0 for the sky) and
// LightingRadiance (the lit opaque scene there).
//
// Horizon based: along each screen direction, a sample that rises above the horizon reached so
// far lights the pixel with the sliver of the hemisphere it newly covers (in sine of its
// elevation over the surface plane), and what the final horizon leaves open is the ambient
// visibility. Everything on screen is taken as solid (thin objects darken a little too much);
// samples beyond `radius` fade out, so far walls neither occlude nor light.

vec3 LightingViewPos(vec2 pixel);
vec3 LightingRadiance(vec2 pixel);

const float LightingPi = 3.14159265;

/// View z from D3D depth 0..1 (depth = zs + zo / z, proj.zw = zs, zo; zo < 0).
float ViewZ(float depth, vec4 proj) {
    return proj.w / min(depth - proj.z, -1e-8);
}

/// The view-space position at normalized device coordinates (+y down the screen) and view z.
vec3 ViewPos(vec2 ndc, float z, vec4 proj) {
    return vec3(ndc.x * z / proj.x, ndc.y * z / proj.y, z);
}

/// Interleaved gradient noise (Jimenez 2014), 0..1.
float GradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

/// The surface normal from a pixel's position and its left, right, up and down neighbours: on
/// each axis the neighbour closer in depth (a silhouette does not bend it), facing the camera.
vec3 NormalFromNeighbours(vec3 c, vec3 l, vec3 r, vec3 u, vec3 d) {
    const vec3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    const vec3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    const vec3 n = normalize(cross(dy, dx));
    return dot(n, c) > 0.0 ? -n : n;
}

struct LightingResult {
    vec3 light; ///< indirect light arriving (radiance; times albedo for the bounce)
    float open; ///< ambient visibility, 1 = nothing occludes
};

/// `pixel`: full-resolution position; `p`, `n`: its view position and normal; `radius`: reach
/// in view units; `screen_scale`: pixels per view unit at z 1; `noise`, `noise2`: 0..1 for the
/// directions' rotation and the steps' offset; `max_radiance` caps a sample (lamps).
LightingResult TraceLighting(vec2 pixel, vec3 p, vec3 n, float radius, float screen_scale,
                             int directions, int steps, float noise, float noise2,
                             float max_radiance, vec2 size) {
    vec3 light = vec3(0.0);
    float open = 0.0;
    const float reach = clamp(radius * screen_scale / p.z, 2.0, 0.25 * size.x);
    for (int d = 0; d < directions; ++d) {
        const float angle = (float(d) + noise) * LightingPi / float(directions);
        const vec2 dir = vec2(cos(angle), sin(angle));
        for (int side = 0; side < 2; ++side) {
            const vec2 step_dir = side == 0 ? dir : -dir;
            float horizon = 0.0; // sine of the highest elevation so far
            for (int s = 0; s < steps; ++s) {
                const float t = (float(s) + noise2) / float(steps);
                const vec2 q = pixel + step_dir * max(t * t * reach, float(s) + 1.0);
                if (q.x < 0.0 || q.y < 0.0 || q.x >= size.x || q.y >= size.y) {
                    break;
                }
                const vec3 sample_pos = LightingViewPos(q);
                if (sample_pos.z <= 0.0) {
                    continue; // sky
                }
                const vec3 v = sample_pos - p;
                const float dist = length(v);
                if (dist < 1e-4) {
                    continue;
                }
                const float falloff = clamp(1.0 - dist * dist / (radius * radius), 0.0, 1.0);
                const float elevation = dot(v / dist, n) * falloff;
                if (elevation > horizon) {
                    vec3 radiance = LightingRadiance(q);
                    const float peak = max(max(radiance.x, radiance.y), radiance.z);
                    radiance *= min(1.0, max_radiance / max(peak, 1e-6));
                    light += radiance * (elevation - horizon);
                    horizon = elevation;
                }
            }
            open += 1.0 - horizon;
        }
    }
    const float count = float(2 * directions);
    return LightingResult(light / count, open / count);
}

/// Kaniel 2026-10-21 (ray tracing, vk_ray_occlusion.h): a cosine-weighted direction around the
/// unit normal `n` from two numbers 0..1, for the occlusion rays against the map's models.
vec3 HemisphereDir(vec3 n, float u1, float u2) {
    const float r = sqrt(u1);
    const float phi = 2.0 * LightingPi * u2;
    const vec3 t = normalize(abs(n.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), n)
                                             : cross(vec3(1.0, 0.0, 0.0), n));
    const vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

/// How much a ray's hit at distance `t` occludes (1 at the surface, 0 at `reach`); a miss (t < 0)
/// nothing.
float RayOcclusionWeight(float t, float reach) {
    return t < 0.0 ? 0.0 : clamp(1.0 - t / reach, 0.0, 1.0);
}

/// The visibility with the rays: the rays' times the screen's; open sky leaves the screen's alone
/// (Kaniel 2026-10-21: squared was too dark, "not just darker").
float CombineOcclusion(float screen_open, float ray_open) {
    return screen_open * ray_open;
}

/// The scene's radiance of a map mesh colour: the 2026-10-21 capture at Central Yharnam's lamp had
/// the lit scene at ~0.05 of the mesh's linear colour where the mesh lies on the visible surface.
const float MeshRadianceScale = 0.05;

float SrgbToLinear(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/// A ray's hit brings back the surface's light (Kaniel 2026-10-21, "light to work more natural"):
/// the map mesh's colour (0xAABBGGRR, sRGB: its texture in the game's baked light) at the scene's
/// brightness.
vec3 MeshRadiance(uint packed) {
    return vec3(SrgbToLinear(float(packed & 255u) / 255.0),
                SrgbToLinear(float((packed >> 8u) & 255u) / 255.0),
                SrgbToLinear(float((packed >> 16u) & 255u) / 255.0)) *
           MeshRadianceScale;
}

/// Kaniel 2026-10-21 ("do all steps", ray-traced lamp shadows): how much a lamp of the game's
/// lighting pass lights the surface at `p` (normal `n`, both in view space): its strength, N.L and
/// (1 - d^2 / r^2)^2 (`inv_r2` = 1 / r^2, the lamp's own value). A fit to the captured light put the
/// lamp within half a metre of its logged place; only the lamps' shares of a pixel's light are
/// used, never their sum against the game's.
float LampWeight(vec3 p, vec3 n, vec3 lamp, float inv_r2, float strength) {
    const vec3 v = lamp - p;
    const float d2 = dot(v, v);
    const float reach = clamp(1.0 - d2 * inv_r2, 0.0, 1.0);
    const float ndl = max(dot(n, v) / sqrt(max(d2, 1e-8)), 0.0);
    return strength * ndl * reach * reach;
}

/// Kaniel 2026-10-21 ("let's do 1 and 2"): how much of what a ray-traced reflection finds a surface
/// shows. Its specular colour `f0` head on, nearly all at grazing angles (Schlick, `n_dot_v` the
/// cosine to the eye), faded out on rough surfaces (`gloss` from the G-buffer, 0..1).
float ReflectionWeight(float f0, float n_dot_v, float gloss) {
    const float grazing = pow(1.0 - clamp(n_dot_v, 0.0, 1.0), 5.0);
    return (f0 + (1.0 - f0) * grazing) * smoothstep(0.35, 0.8, gloss);
}

/// The share of a pixel's lamp light that stays when lamps worth `occluded` of `total` are blocked.
float LampShadowFactor(float total, float occluded) {
    return total > 1e-6 ? clamp((total - occluded) / total, 0.0, 1.0) : 1.0;
}
