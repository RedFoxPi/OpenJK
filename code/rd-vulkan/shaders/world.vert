#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec2 inLightmapUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec2 fragLightmapUV;
layout(location = 2) out float fragFogDist;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 camPos;   // xyz = camera world position, w = overbright factor (world.frag)
    vec4 fogColor; // rgb = fog colour, a = opaque distance (0 = no fog)
    // x = fog ramp start distance (world.frag); yz = this batch's tcMod
    // scroll UV offset (scrollSpeed * currentTimeSeconds, already computed
    // CPU-side - see tr_world.cpp's RE_RenderScene), 0,0 for the common
    // no-scroll case
    vec4 fogStart;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    // tcMod scroll only ever applies to the diffuse UV, never the lightmap
    // UV - real Quake3's lightmap coordinates are baked per-vertex from the
    // BSP compile and never move independently of the surface itself.
    fragUV = inUV + pc.fogStart.yz;
    fragLightmapUV = inLightmapUV;
    // World-space Euclidean camera distance - a simplified stand-in for
    // rd-vanilla's real fog (tr_shade_calc.cpp RB_CalcFogTexCoords, a
    // dot-product "depth along the fog plane's normal" measure fed through
    // a precomputed gradient texture, not a plain distance ramp). Same
    // "simplify the algorithm, keep the visual intent" tradeoff as the flat
    // skybox box and fixed-subdivision patches elsewhere in this renderer -
    // near is clear, saturating to fully fog-coloured at fogColor.a units,
    // just via a linear ramp instead of Quake3's actual falloff curve.
    fragFogDist = distance(inPos, pc.camPos.xyz);
}
