#version 450 core
#extension GL_GOOGLE_include_directive : require

// Experiment: AMD FidelityFX FSR 1's robust contrast adaptive sharpening (RCAS, 3rdparty/fidelityfx-fsr) sharpens the frame gpu_blit_fsr1_easu.frag
// scaled up, onto the swap chain (D3D12 presenter, Fsr1Scaler). con0 is FsrRcasCon's, con1.xy the top left of the game's picture on the swap chain (black
// bars around it); the block matches EASU's so that both share a root signature.

layout(binding = 0, set = 0) uniform texture2D frame;
layout(binding = 1, set = 0) uniform sampler frame_sampler;
layout(push_constant) uniform Constants {
	uvec4 con0;
	uvec4 con1;
	uvec4 con2;
	uvec4 con3;
};
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

#define A_GPU  1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_RCAS_F 1
// Neighbours outside the image repeat its edge.
AF4 FsrRcasLoadF(ASU2 p) {
	const ivec2 last = textureSize(sampler2D(frame, frame_sampler), 0) - ivec2(1);
	return texelFetch(sampler2D(frame, frame_sampler), clamp(p, ivec2(0), last), 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main() {
	AF3 pixel;
	FsrRcasF(pixel.r, pixel.g, pixel.b, AU2(gl_FragCoord.xy) - con1.xy, con0);
	color = vec4(pixel, 1.0);
}
