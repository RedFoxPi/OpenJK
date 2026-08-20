#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D diffuseTex;

void main() {
    // No lightmap (polys have none) and no world fog (see README.md - the
    // same scope cut as Ghoul2 models) - just the texture modulated by the
    // per-vertex colour polyVert_t::modulate provides, matching rd-vanilla's
    // RB_SurfacePolychain (tr_surface.cpp).
    outColor = texture(diffuseTex, fragUV) * fragColor;
}
