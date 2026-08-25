#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragLightmapUV;
layout(location = 2) in float fragFogDist;
layout(location = 3) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D diffuseTex;
layout(binding = 1) uniform sampler2D lightmapTex;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 camPos;
    vec4 fogColor; // rgb = fog colour, a = opaque distance (0 = no fog)
    // x = distance before which no fog applies (see below); yz = tcMod
    // scroll UV offset, already applied to fragUV in world.vert - not read
    // here
    vec4 fogStart;
} pc;

void main() {
    // Baked lightmap, plus real per-vertex colour for vertex-lit surfaces
    // (fragColor, see below) - still no dynamic lights, see rd-vulkan/
    // README.md. pc.camPos.w carries a per-draw overbright factor
    // (see vkWorldPushConstants_t's comment, tr_local.h) rather than a
    // hardcoded 2.0: real BSP lightmaps are baked assuming the renderer
    // doubles them back out at draw time ("overbright bits"), so world/sky
    // draws pass 2.0 here - a reasonable approximation, not a verified match
    // to rd-vanilla's own tone mapping - but Ghoul2 draws are paired with a
    // plain white placeholder in the lightmap slot (this renderer has no
    // real per-character lighting yet, see README.md), not an actual baked-
    // and-compensated lightmap, so doubling it too would just be wrong: it
    // silently rendered every character twice as bright as its own diffuse
    // texture, not merely "unlit" - passing 1.0 there removes that extra,
    // unearned doubling without pretending to add the real ambient/
    // directional lighting rd-vanilla applies instead (still not
    // implemented - this only removes a bug, it isn't the missing feature).
    // fragColor (WorldVertex::color) is real baked per-vertex colour for
    // vertex-lit surfaces, and a hardcoded (1,1,1,1) no-op for everything
    // else (real lightmapped world geometry, sky, Ghoul2) - see
    // WorldVertex::color's own comment (tr_local.h) for why that hardcoded
    // default is correct rather than a placeholder, and why no shader-side
    // branch is needed to tell the two cases apart.
    vec4 diffuse = texture(diffuseTex, fragUV);
    vec4 lightmap = texture(lightmapTex, fragLightmapUV);
    vec3 shaded = diffuse.rgb * lightmap.rgb * fragColor.rgb * pc.camPos.w;

    // World fog (see VK_LoadWorldFog, tr_world.cpp) - a simplified linear
    // distance ramp toward this batch's own assigned fog's colour (global
    // or local, see WorldSurfaceBatch::fogIndex), see world.vert for why
    // this isn't rd-vanilla's exact falloff curve. fogColor.a == 0 means
    // this batch isn't in any fog volume (or this is the sky draw, which
    // always passes 0 - see RE_RenderScene) - skip the mix entirely rather
    // than mixing by a meaningless factor of 0. fogStart.x shifts where the
    // ramp begins away from the camera (0 in the common case - see
    // VK_ComputeRangedFogStart, tr_world.cpp); guard the divide since a
    // pathological fogStart.x >= fogColor.a would otherwise divide by a
    // non-positive span (CPU side already clamps this, this is just
    // defense in depth against a mismatched push).
    if (pc.fogColor.a > 0.0) {
        float span = max(pc.fogColor.a - pc.fogStart.x, 1.0);
        float fogFactor = clamp((fragFogDist - pc.fogStart.x) / span, 0.0, 1.0);
        shaded = mix(shaded, pc.fogColor.rgb, fogFactor);
    }

    outColor = vec4(shaded, diffuse.a);
}
