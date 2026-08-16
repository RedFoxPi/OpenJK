#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PushConstants {
	vec2 viewportSize;
	vec4 color;
} pc;

void main() {
	outColor = texture(tex, fragUV) * pc.color;
}
