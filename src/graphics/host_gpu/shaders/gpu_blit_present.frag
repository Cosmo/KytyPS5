#version 450 core

// Scales a prepared frame onto the swap chain (D3D12 presenter; Vulkan blits instead).

layout(binding = 0, set = 0) uniform texture2D frame;
layout(binding = 1, set = 0) uniform sampler frame_sampler;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
	color = texture(sampler2D(frame, frame_sampler), uv);
}
