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
    // rd-vulkan/README.md. The *2.0 approximates Quake3's "overbright bits"
    // (lightmaps are baked assuming the renderer doubles them back out at
    // draw time); it's a reasonable approximation, not a verified match to
    // rd-vanilla's own tone mapping.
    vec4 diffuse = texture(diffuseTex, fragUV);
    vec4 lightmap = texture(lightmapTex, fragLightmapUV);
    vec3 shaded = diffuse.rgb * lightmap.rgb * 2.0;

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
