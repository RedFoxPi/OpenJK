#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec2 inLightmapUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec2 fragLightmapUV;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    fragUV = inUV;
    fragLightmapUV = inLightmapUV;
}
