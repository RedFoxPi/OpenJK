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
    vec4 fogStart; // x = distance before which no fog applies (see world.frag)
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    fragUV = inUV;
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
