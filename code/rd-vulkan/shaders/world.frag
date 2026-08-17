#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D tex;

void main() {
    // Unlit - no lightmap/vertex-light term yet, see rd-vulkan/README.md.
    outColor = texture(tex, fragUV);
}
