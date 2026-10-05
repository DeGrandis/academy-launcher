#pragma once

namespace cw::d3d {

// HLSL for the Direct3D 11 renderer's frame effects (Device11.cpp, compositeScene). Everything works on the frame
// alone, with no history between frames, so nothing can smear, boil or flicker.
//
// Inputs: the 3D scene in high precision (t0), linear view depth written by every opaque 3D draw (t1, 0 where there
// is none), the sun's shadow atlas (t2, two cascades side by side), ambient occlusion (t3), bloom (t4).
constexpr char kPostHlsl[] = R"(
cbuffer Post : register(b0) {
    float4 screen;        // width, height, 1 / width, 1 / height
    float4 viewport;      // the main camera's viewport: x, y, width, height (pixels)
    float4 projection;    // P11, P22, P31, P32 of the main camera
    row_major float4x4 viewToShadow[2];
    float4 shadowParams;  // enabled, normal offset (view units), depth bias, cascade fade width
    float4 shadowColor;   // what a fully shadowed surface is multiplied by
    float4 sunView;       // direction towards the sun in view space; w = 1 with a sun
    float4 lighting;      // exposure, ambient occlusion strength, effect light strength, light count
    float4 grade;         // contrast, saturation, bloom strength, vignette
    float4 tint;          // rgb tint, bloom threshold
    float4 aoParams;      // radius (view units), 1 / radius, bias, glow strength
    float4 debugParams;   // x: CW_FX_DEBUG view (1 sun visibility, 2 occlusion, 3 normals, 4 depth, 5 effect lights, 7 bounce, 8 reflections)
    float4 surfaceParams; // bounce light strength, reflection strength, reflection reach (view units), 0
    float4 lightPos[48];  // view space position, radius
    float4 lightColor[48];
};

Texture2D scene : register(t0);
Texture2D<float> linearDepth : register(t1);
Texture2D<float> shadowAtlas : register(t2);
Texture2D<float> occlusion : register(t3);
Texture2D bloomTexture : register(t4);
Texture2D glowTexture : register(t5);  // what additive effects (lasers, explosions, engines) added to the scene
Texture2D bounceTexture : register(t6); // light bounced from nearby surfaces (half resolution)
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);
SamplerComparisonState shadowCompare : register(s2);

struct FsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FsOut fullscreenVS(uint id : SV_VertexID) {
    FsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float depthAt(int2 p) {
    p = clamp(p, int2(0, 0), int2(screen.xy) - 1);
    return linearDepth.Load(int3(p, 0));
}

float3 viewPosition(float2 pixel, float z) {
    float2 ndc = float2((pixel.x - viewport.x) / viewport.z * 2 - 1, 1 - (pixel.y - viewport.y) / viewport.w * 2);
    return float3((ndc.x - projection.z) * z / projection.x, (ndc.y - projection.w) * z / projection.y, z);
}

// Surface normal from the depth of the neighbours on each axis, taking the side that continues the same surface.
float3 normalAt(int2 p, float3 P) {
    float z = P.z;
    float zr = depthAt(p + int2(1, 0)), zl = depthAt(p - int2(1, 0));
    float zd = depthAt(p + int2(0, 1)), zu = depthAt(p - int2(0, 1));
    float3 dx = (zr > 0 && (abs(zr - z) <= abs(zl - z) || zl <= 0)) ? viewPosition(p + float2(1.5, 0.5), zr) - P
                                                                      : P - viewPosition(p + float2(-0.5, 0.5), zl);
    float3 dy = (zd > 0 && (abs(zd - z) <= abs(zu - z) || zu <= 0)) ? viewPosition(p + float2(0.5, 1.5), zd) - P
                                                                      : P - viewPosition(p + float2(0.5, -0.5), zu);
    float3 n = normalize(cross(dy, dx));
    return dot(n, P) > 0 ? -n : n;
}

// ---------------------------------------------------------------- ambient occlusion (half resolution)

static const float3 kAoKernel[12] = {
    float3(0.53, 0.20, 0.32), float3(-0.41, 0.48, 0.21), float3(0.12, -0.62, 0.44), float3(-0.58, -0.31, 0.18),
    float3(0.27, 0.71, 0.55), float3(-0.19, -0.18, 0.86), float3(0.80, -0.29, 0.25), float3(-0.73, 0.12, 0.47),
    float3(0.08, 0.31, 0.27), float3(0.36, -0.09, 0.12), float3(-0.22, 0.15, 0.10), float3(0.05, -0.27, 0.62)};

float aoPS(FsOut i) : SV_Target {
    int2 p = int2(i.pos.xy) * 2;
    float z = depthAt(p);
    if (z <= 0) return 1;
    float3 P = viewPosition(p + 0.5, z);
    float3 N = normalAt(p, P);
    // A fixed rotation per pixel of a 4x4 tile (the blur pass averages it away); the same every frame.
    int2 tile = int2(i.pos.xy) & 3;
    float angle = (tile.x * 4 + tile.y) * 0.3927 + (tile.y & 1) * 0.196;
    float3 t = normalize(abs(N.y) < 0.9 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
    float3 b = cross(N, t);
    float c = cos(angle), s = sin(angle);
    float3 t2 = t * c + b * s, b2 = b * c - t * s;
    float radius = aoParams.x;
    float occluded = 0;
    [unroll] for (int k = 0; k < 12; ++k) {
        float3 d = kAoKernel[k];
        float3 S = P + (t2 * d.x + b2 * d.y + N * d.z) * radius;
        if (S.z <= 0.01) continue;
        float2 ndc = float2(S.x * projection.x / S.z + projection.z, S.y * projection.y / S.z + projection.w);
        float2 pixel = float2(viewport.x + (ndc.x + 1) * 0.5 * viewport.z, viewport.y + (1 - ndc.y) * 0.5 * viewport.w);
        float zs = depthAt(int2(pixel));
        if (zs <= 0) continue;
        float closer = S.z - zs;
        occluded += (closer > aoParams.z) * saturate(1 - (closer - radius) * aoParams.y);
    }
    return saturate(1 - occluded / 12);
}

// 4x4 depth-aware average of the half-resolution occlusion.
float aoBlurPS(FsOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float z = depthAt(p * 2);
    float sum = 0, weight = 0;
    [unroll] for (int y = -2; y < 2; ++y) {
        [unroll] for (int x = -2; x < 2; ++x) {
            int2 q = p + int2(x, y);
            float zq = depthAt(q * 2);
            float w = zq > 0 ? saturate(1 - abs(zq - z) / max(z * 0.05, 0.1)) : 0;
            sum += occlusion.Load(int3(q, 0)) * w;
            weight += w;
        }
    }
    return weight > 0 ? sum / weight : 1;
}

)";

// (MSVC caps a string literal at 16 KB: the frame-effect HLSL is in parts, compiled together.)
constexpr char kPostHlsl2[] = R"(
// ---------------------------------------------------------------- bounce light (half resolution)

// Light reflected onto a surface by what is around it: the colors of the visible surfaces in its hemisphere (red rock
// tints the ground near it, the canopy greens the forest floor). The same fixed sample pattern as the occlusion.
float4 bouncePS(FsOut i) : SV_Target {
    int2 p = int2(i.pos.xy) * 2;
    float z = depthAt(p);
    if (z <= 0) return 0;
    float3 P = viewPosition(p + 0.5, z);
    float3 N = normalAt(p, P);
    int2 tile = int2(i.pos.xy) & 3;
    float angle = (tile.x * 4 + tile.y) * 0.3927 + (tile.y & 1) * 0.196;
    float3 t = normalize(abs(N.y) < 0.9 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
    float3 b = cross(N, t);
    float c = cos(angle), s = sin(angle);
    float3 t2 = t * c + b * s, b2 = b * c - t * s;
    float radius = aoParams.x * 4;
    float3 sum = 0;
    float weight = 0;
    [unroll] for (int k = 0; k < 12; ++k) {
        float3 d = kAoKernel[k];
        float3 S = P + (t2 * d.x + b2 * d.y + N * d.z) * radius;
        if (S.z <= 0.01) continue;
        float2 ndc = float2(S.x * projection.x / S.z + projection.z, S.y * projection.y / S.z + projection.w);
        float2 pixel = float2(viewport.x + (ndc.x + 1) * 0.5 * viewport.z, viewport.y + (1 - ndc.y) * 0.5 * viewport.w);
        if (any(pixel < 0) || any(pixel >= screen.xy)) continue;
        float zs = depthAt(int2(pixel));
        if (zs <= 0 || abs(zs - S.z) > radius) continue;  // no surface there to bounce from
        float w = d.z + 0.1;
        sum += scene.Load(int3(int2(pixel), 0)).rgb * w;
        weight += w;
    }
    return float4(weight > 0 ? sum / weight : 0, 1);
}

float4 bounceBlurPS(FsOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float z = depthAt(p * 2);
    float3 sum = 0;
    float weight = 0;
    [unroll] for (int y = -2; y < 2; ++y) {
        [unroll] for (int x = -2; x < 2; ++x) {
            int2 q = p + int2(x, y);
            float zq = depthAt(q * 2);
            float w = zq > 0 ? saturate(1 - abs(zq - z) / max(z * 0.05, 0.1)) : 0;
            sum += bounceTexture.Load(int3(q, 0)).rgb * w;
            weight += w;
        }
    }
    return float4(weight > 0 ? sum / weight : 0, 1);
}

// ---------------------------------------------------------------- reflections

// Screen-space reflection: march the reflected view ray through the depth buffer and take the color it meets. Fixed
// steps (no randomness), faded towards screen edges, with distance, and by the Fresnel term, so it stays steady.
float3 reflection(float3 P, float3 N, out float strength) {
    strength = 0;
    float3 V = normalize(P);
    float3 R = reflect(V, N);
    if (R.z < -0.2) return 0;  // towards the camera: not on screen
    float reach = surfaceParams.z;
    float3 previous = P;
    float3 sky = 0;   // the open sky the ray passes in front of: what it reflects if it meets nothing
    float skySeen = 0;
    float2 lastNdc = 0;
    [loop] for (int step = 1; step <= 32; ++step) {
        float travel = reach * pow(step / 32.0, 1.6);
        float3 Q = P + R * travel;
        if (Q.z <= 0.05) break;
        float2 ndc = float2(Q.x * projection.x / Q.z + projection.z, Q.y * projection.y / Q.z + projection.w);
        if (abs(ndc.x) > 1 || abs(ndc.y) > 1) break;
        float2 pixel = float2(viewport.x + (ndc.x + 1) * 0.5 * viewport.z, viewport.y + (1 - ndc.y) * 0.5 * viewport.w);
        float zs = depthAt(int2(pixel));
        lastNdc = ndc;
        if (zs <= 0) {
            sky = scene.Load(int3(int2(pixel), 0)).rgb;
            skySeen = 1;
        }
        float thickness = max(0.5, travel * 0.15);
        if (zs > 0 && Q.z > zs && Q.z - zs < thickness) {
            // Refine between the last two points.
            float3 low = previous, high = Q;
            [unroll] for (int k = 0; k < 4; ++k) {
                float3 mid = (low + high) * 0.5;
                float2 mn = float2(mid.x * projection.x / mid.z + projection.z, mid.y * projection.y / mid.z + projection.w);
                float2 mp = float2(viewport.x + (mn.x + 1) * 0.5 * viewport.z, viewport.y + (1 - mn.y) * 0.5 * viewport.w);
                if (mid.z > depthAt(int2(mp))) high = mid; else low = mid;
                pixel = mp;
                ndc = mn;
            }
            float edge = saturate((1 - max(abs(ndc.x), abs(ndc.y))) * 6);
            float fresnel = 0.04 + 0.96 * pow(1 - saturate(dot(-V, N)), 5);
            strength = edge * (1 - step / 32.0) * lerp(0.35, 1, fresnel);
            return scene.Load(int3(int2(pixel), 0)).rgb;
        }
        previous = Q;
    }
    if (skySeen > 0) {
        float edge = saturate((1 - max(abs(lastNdc.x), abs(lastNdc.y))) * 4);
        float fresnel = 0.04 + 0.96 * pow(1 - saturate(dot(-V, N)), 5);
        strength = lerp(0.6, 1, edge) * lerp(0.35, 1, fresnel);
        return sky;
    }
    return 0;
}

)";

// (MSVC caps a string literal at 16 KB: the frame-effect HLSL is in parts, compiled together.)
constexpr char kPostHlsl3[] = R"(
// ---------------------------------------------------------------- lighting: sun shadows, occlusion, effect lights

float cascadeShadow(int cascade, float3 P, float3 N, float ndl) {
    // Offset along the normal and a depth bias that grow at grazing sun angles keep surfaces from shadowing themselves.
    float grazing = sqrt(saturate(1 - ndl * ndl)) / max(ndl, 0.2);
    float4 s = mul(float4(P + N * shadowParams.y * (1 + cascade * 4) * (1 + grazing), 1), viewToShadow[cascade]);
    float local = s.x * 2 - cascade;  // 0..1 inside this cascade's half of the atlas
    if (local < 0.01 || local > 0.99 || s.y < 0.01 || s.y > 0.99 || s.z < 0 || s.z > 1) return -1;
    float2 texel = float2(0.5, 1) / 2048;
    float lit = 0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            lit += shadowAtlas.SampleCmpLevelZero(shadowCompare, s.xy + float2(x, y) * texel * 1.5, s.z - shadowParams.z * (1 + cascade * 3) * (1 + grazing));
        }
    }
    float edge = min(min(local, 1 - local), min(s.y, 1 - s.y));
    float result = lit / 9;
    return cascade == 1 ? lerp(1, result, saturate(edge / shadowParams.w)) : result;
}

float sunVisibility(float3 P, float3 N, float ndl) {
    float v = cascadeShadow(0, P, N, ndl);
    if (v < 0) v = cascadeShadow(1, P, N, ndl);
    return v < 0 ? 1 : v;
}

float4 lightingPS(FsOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float3 glow = glowTexture.Load(int3(p, 0)).rgb;
    float3 color = max(scene.Load(int3(p, 0)).rgb - glow, 0);  // the surfaces; effects are added back unshaded
    float z = depthAt(p);
    float exposure = lighting.x;
    if (z <= 0) return float4(color * exposure + glow * aoParams.w, 1);  // sky and backgrounds
    float3 P = viewPosition(i.pos.xy, z);
    float3 N = normalAt(p, P);
    float3 shade = 1;
    if (shadowParams.x > 0 && sunView.w > 0) {
        float ndl = dot(N, sunView.xyz);
        float visibility = ndl > 0 ? sunVisibility(P, N, ndl) : 1;
        visibility = lerp(1, visibility, smoothstep(0.0, 0.25, ndl));
        shade = lerp(shadowColor.rgb, 1, visibility);
    }
    float ao = lerp(1, occlusion.SampleLevel(linearClamp, i.uv, 0), lighting.y);
    float3 lights = 0;
    int count = (int)lighting.w;
    for (int k = 0; k < count; ++k) {
        float3 L = lightPos[k].xyz - P;
        float d = length(L);
        float attenuation = saturate(1 - d / lightPos[k].w);
        attenuation *= attenuation;
        float facing = saturate(dot(N, L / max(d, 1e-3))) * 0.7 + 0.3;
        lights += lightColor[k].rgb * attenuation * facing;
    }
    int debugView = (int)debugParams.x;
    if (debugView == 1) return float4(shade, 1);
    if (debugView == 2) return float4(ao.xxx, 1);
    if (debugView == 3) return float4(N * 0.5 + 0.5, 1);
    if (debugView == 4) return float4(frac(z / 50).xxx, 1);
    if (debugView == 5) return float4(lights * 0.5, 1);
    if (debugView == 6) return float4(saturate(dot(N, sunView.xyz)).xxx, 1);
    float3 bounce = bounceTexture.SampleLevel(linearClamp, i.uv, 0).rgb;
    float3 surface = color * (exposure * shade * ao + lights * lighting.z + bounce * surfaceParams.x * exposure * ao);
    float reflected = 0;
    float3 mirror = 0;
    if (surfaceParams.y > 0) {
        mirror = reflection(P, N, reflected) * exposure;
        reflected *= surfaceParams.y;
    }
    if (debugView == 7) return float4(bounce, 1);
    if (debugView == 8) return float4(mirror * reflected, 1);
    return float4(lerp(surface, mirror, reflected) + glow * aoParams.w, 1);
}

// ---------------------------------------------------------------- bloom

float4 bloomPrefilterPS(FsOut i) : SV_Target {
    float3 c = scene.SampleLevel(linearClamp, i.uv, 0).rgb;
    float brightness = max(c.r, max(c.g, c.b));
    float threshold = tint.w;
    float knee = threshold * 0.5;
    float soft = clamp(brightness - threshold + knee, 0, 2 * knee);
    soft = soft * soft / (4 * knee + 1e-4);
    float contribution = max(soft, brightness - threshold) / max(brightness, 1e-4);
    return float4(c * contribution, 1);
}

// 4-tap box from the level above (each tap a bilinear 2x2 average).
float4 bloomDownPS(FsOut i) : SV_Target {
    float2 texel = screen.zw;  // texel size of the source level
    float3 c = scene.SampleLevel(linearClamp, i.uv + texel * float2(-1, -1), 0).rgb + scene.SampleLevel(linearClamp, i.uv + texel * float2(1, -1), 0).rgb +
               scene.SampleLevel(linearClamp, i.uv + texel * float2(-1, 1), 0).rgb + scene.SampleLevel(linearClamp, i.uv + texel * float2(1, 1), 0).rgb;
    return float4(c * 0.25, 1);
}

// 3x3 tent from the level below, added onto this level.
float4 bloomUpPS(FsOut i) : SV_Target {
    float2 texel = screen.zw;  // texel size of the source level
    float3 c = scene.SampleLevel(linearClamp, i.uv, 0).rgb * 4;
    c += (scene.SampleLevel(linearClamp, i.uv + texel * float2(-1, 0), 0).rgb + scene.SampleLevel(linearClamp, i.uv + texel * float2(1, 0), 0).rgb +
          scene.SampleLevel(linearClamp, i.uv + texel * float2(0, -1), 0).rgb + scene.SampleLevel(linearClamp, i.uv + texel * float2(0, 1), 0).rgb) * 2;
    c += scene.SampleLevel(linearClamp, i.uv + texel * float2(-1, -1), 0).rgb + scene.SampleLevel(linearClamp, i.uv + texel * float2(1, -1), 0).rgb +
         scene.SampleLevel(linearClamp, i.uv + texel * float2(-1, 1), 0).rgb + scene.SampleLevel(linearClamp, i.uv + texel * float2(1, 1), 0).rgb;
    return float4(c / 16, 1);
}

// ---------------------------------------------------------------- tone mapping, grading, anti-aliasing

float3 shoulder(float3 x) {
    // Identity up to 0.9 (the game's own colors stay as authored), then a smooth roll-off for what effects push past 1.
    float3 over = max(x - 0.9, 0);
    return min(x, 0.9) + 0.1 * (1 - exp(-over / 0.1)) + over * 0.08;
}

float4 tonemapPS(FsOut i) : SV_Target {
    float3 c = scene.SampleLevel(pointClamp, i.uv, 0).rgb + bloomTexture.SampleLevel(linearClamp, i.uv, 0).rgb * grade.z;
    c = shoulder(c * tint.rgb);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(luma.xxx, c, grade.y);
    c = saturate((c - 0.5) * grade.x + 0.5);
    float2 d = i.uv - 0.5;
    c *= 1 - grade.w * dot(d, d) * 1.6;
    return float4(c, dot(c, float3(0.299, 0.587, 0.114)));
}

float4 copyPS(FsOut i) : SV_Target {
    return float4(saturate(scene.Load(int3(int2(i.pos.xy), 0)).rgb), 1);
}

float lumaAt(float2 uv) {
    return scene.SampleLevel(linearClamp, uv, 0).a;
}

float4 fxaaPS(FsOut i) : SV_Target {
    float2 rcp = screen.zw;
    float2 uv = i.uv;
    float3 rgbM = scene.SampleLevel(pointClamp, uv, 0).rgb;
    float lM = scene.SampleLevel(pointClamp, uv, 0).a;
    float lNW = lumaAt(uv + float2(-1, -1) * rcp), lNE = lumaAt(uv + float2(1, -1) * rcp);
    float lSW = lumaAt(uv + float2(-1, 1) * rcp), lSE = lumaAt(uv + float2(1, 1) * rcp);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return float4(rgbM, 1);
    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128);
    float rcpMin = 1 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, -8, 8) * rcp;
    float3 a = 0.5 * (scene.SampleLevel(linearClamp, uv + dir * (1.0 / 3 - 0.5), 0).rgb + scene.SampleLevel(linearClamp, uv + dir * (2.0 / 3 - 0.5), 0).rgb);
    float3 b = a * 0.5 + 0.25 * (scene.SampleLevel(linearClamp, uv - dir * 0.5, 0).rgb + scene.SampleLevel(linearClamp, uv + dir * 0.5, 0).rgb);
    float lB = dot(b, float3(0.299, 0.587, 0.114));
    return float4((lB < lMin || lB > lMax) ? a : b, 1);
}
)";

// Appended to a copy of each scene vertex shader for the sun's shadow map: runs the original shader, turns its clip
// position back into world space with the camera it was drawn with, and projects that from the sun. (The game's
// vertex shaders only output clip space; the world position is never given.)
constexpr char kShadowVertexHlsl[] = R"(
cbuffer ShadowDraw : register(b3) {
    float4 shadowCameraProjection;  // P11, P22, P31, P32 of the camera the draw used
    row_major float4x4 shadowInverseView;
    row_major float4x4 shadowSunProjection;
};
)";

constexpr char kShadowPixelHlsl[] = R"(
Texture2D shadowAlphaTexture : register(t0);
SamplerState shadowAlphaSampler : register(s0);
cbuffer ShadowAlpha : register(b4) { float4 shadowAlpha; };  // reference, enabled
void main(VSOut i) {
    if (shadowAlpha.y > 0 && shadowAlphaTexture.Sample(shadowAlphaSampler, i.t0.xy).a < shadowAlpha.x) discard;
}
)";

}  // namespace cw::d3d
