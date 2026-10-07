#version 450 core
#extension GL_GOOGLE_include_directive : require

// Scales a prepared frame onto the swap chain (D3D12 presenter; Vulkan blits instead). An overlay layer is blended as premultiplied alpha: a frame with
// straight alpha is premultiplied here. An HDR10 frame (PQ, or linear nits) becomes SDR as on upstream's SDR swap chains
// (gpu_video_out_overlay.frag).

#include "gpu_video_out_hdr10_to_sdr.inc"

layout(binding = 0, set = 0) uniform texture2D frame;
layout(binding = 1, set = 0) uniform sampler frame_sampler;
layout(push_constant) uniform AlphaControl {
	uint flags; // bit 0: premultiplied, 2: PQ source, 3: linear source, 4: opaque (as in gpu_video_out_overlay.frag)
};
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
	const uint PQ_SOURCE = 4u;
	color = texture(sampler2D(frame, frame_sampler), uv);
	if ((flags & 16u) != 0u) {
		color.a = 1.0;
	}
	bool premultiplied = (flags & 1u) != 0u;
	bool convert       = (flags & PQ_SOURCE) != 0u;
	if (convert && premultiplied) {
		color.rgb = color.a > 0.0 ? color.rgb / color.a : vec3(0.0);
	}
	if (convert) {
		color.rgb = Hdr10ToSdr(color.rgb, (flags & 8u) != 0u);
	}
	if (!premultiplied || convert) {
		color.rgb *= color.a;
	}
}
