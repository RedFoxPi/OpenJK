#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec2 fragLightmapUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D diffuseTex;
layout(binding = 1) uniform sampler2D lightmapTex;

void main() {
    // Baked lightmap only - no dynamic lights, no vertex color, see
    // rd-vulkan/README.md. The *2.0 approximates Quake3's "overbright bits"
    // (lightmaps are baked assuming the renderer doubles them back out at
    // draw time); it's a reasonable approximation, not a verified match to
    // rd-vanilla's own tone mapping.
    vec4 diffuse = texture(diffuseTex, fragUV);
    vec4 lightmap = texture(lightmapTex, fragLightmapUV);
    outColor = vec4(diffuse.rgb * lightmap.rgb * 2.0, diffuse.a);
}
