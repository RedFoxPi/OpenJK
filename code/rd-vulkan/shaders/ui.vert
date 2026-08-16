#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

// x,y = viewport size in pixels; converts pixel-space vertices (as used
// throughout the engine's 2D draw calls) to Vulkan NDC without requiring
// callers to know about NDC at all.
layout(push_constant) uniform PushConstants {
	vec2 viewportSize;
	vec4 color;
} pc;

void main() {
	vec2 ndc = (inPos / pc.viewportSize) * 2.0 - 1.0;
	gl_Position = vec4(ndc, 0.0, 1.0);
	fragUV = inUV;
}
