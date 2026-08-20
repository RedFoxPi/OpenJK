#version 450

// Runtime polys (RE_AddPolyToScene) - particles, sparks, decals, and other
// per-frame dynamic effect geometry. Unlike world.vert's WorldVertex (which
// carries a second, lightmap UV set and no per-vertex color), a polyVert_t
// (rd-common/tr_types.h) is just a world-space position, one UV set, and a
// per-vertex RGBA modulate colour - closer to ui.vert's UiVertex than to
// world.vert, except in world space (a real mvp, not a 2D ortho transform)
// and with real depth testing against world/Ghoul2 geometry - see
// tr_init.cpp's VK_CreatePolyPipeline for why this needs its own small
// pipeline rather than reusing either existing one directly.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}
