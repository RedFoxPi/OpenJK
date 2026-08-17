#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    fragUV = inUV;
}
