#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragLightmapUV;
layout(location = 2) in float fragFogDist;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D diffuseTex;
layout(binding = 1) uniform sampler2D lightmapTex;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 camPos;
    vec4 fogColor; // rgb = fog colour, a = opaque distance (0 = no fog)
} pc;

void main() {
    // Baked lightmap only - no dynamic lights, no vertex color, see
    // rd-vulkan/README.md. pc.camPos.w carries a per-draw overbright factor
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
    vec4 diffuse = texture(diffuseTex, fragUV);
    vec4 lightmap = texture(lightmapTex, fragLightmapUV);
    vec3 shaded = diffuse.rgb * lightmap.rgb * pc.camPos.w;

    // World fog (see VK_LoadWorldFog, tr_world.cpp) - a simplified linear
    // distance ramp toward the BSP's global fogparms colour, see world.vert
    // for why this isn't rd-vanilla's exact falloff curve. fogColor.a == 0
    // means no fog volume in this map (or this is the sky draw, which
    // always passes 0 - see RE_RenderScene) - skip the mix entirely rather
    // than mixing by a meaningless factor of 0.
    if (pc.fogColor.a > 0.0) {
        float fogFactor = clamp(fragFogDist / pc.fogColor.a, 0.0, 1.0);
        shaded = mix(shaded, pc.fogColor.rgb, fogFactor);
    }

    outColor = vec4(shaded, diffuse.a);
}
