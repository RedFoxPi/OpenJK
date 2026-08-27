#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec2 inLightmapUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec3 inNormal;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec2 fragLightmapUV;
layout(location = 2) out float fragFogDist;
layout(location = 3) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 camPos;   // xyz = camera world position, w = overbright factor (world.frag)
    vec4 fogColor; // rgb = fog colour, a = opaque distance (0 = no fog)
    // x = fog ramp start distance (world.frag); yz = this batch's tcMod
    // scroll UV offset (scrollSpeed * currentTimeSeconds, already computed
    // CPU-side - see tr_world.cpp's RE_RenderScene), 0,0 for the common
    // no-scroll case
    vec4 fogStart;
    // xy = this batch's tcMod scale multiplier, 1,1 for the common
    // no-scale case (see vkWorldPushConstants_t's own comment, tr_local.h,
    // for why scale multiplies first and the scroll offset above is
    // already pre-scaled at parse time when a shader's tcMod order needs
    // that). z = tcGen-environment flag (1.0 = generate reflection UVs
    // below instead of using inUV at all, 0.0 = the common ordinary-UV
    // case). w unused.
    vec4 uvScale;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    if ( pc.uvScale.z > 0.5 )
    {
        // Real per-vertex reflection-mapped UV generation - ports
        // rd-vanilla's RB_CalcEnvironmentTexCoords (tr_shade_calc.cpp)
        // exactly, including its curious "only x/y, z never touched"
        // convention (real Quake3 code, not a simplification here). World
        // geometry has no per-entity transform, so inPos/inNormal are
        // already in the same world space as pc.camPos - matches real
        // rd-vanilla's own backEnd.ori-identity case for non-viewmodel
        // entities exactly (the RF_FIRST_PERSON branch of the real
        // function, which uses a light direction instead of the viewer
        // vector, has no real per-map world-surface user and is not
        // ported). No remap to 0..1 is applied - real Quake3 doesn't
        // either, relying on the texture sampler's wrap addressing to
        // tile the roughly [-1,1]-ish range this produces.
        vec3 viewer = normalize( pc.camPos.xyz - inPos );
        float d = dot( inNormal, viewer );
        fragUV = vec2( inNormal.x * d - 0.5 * viewer.x, inNormal.y * d - 0.5 * viewer.y );
    }
    else
    {
        // tcMod scroll/scale only ever apply to the diffuse UV, never the
        // lightmap UV - real Quake3's lightmap coordinates are baked
        // per-vertex from the BSP compile and never move or rescale
        // independently of the surface itself.
        fragUV = inUV * pc.uvScale.xy + pc.fogStart.yz;
    }
    fragLightmapUV = inLightmapUV;
    fragColor = inColor;
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
