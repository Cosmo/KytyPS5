#version 450 core
#extension GL_GOOGLE_include_directive : require

// Experiment: AMD FidelityFX FSR 1's edge adaptive spatial upsampling (EASU, 3rdparty/fidelityfx-fsr) scales a prepared frame up to the swap chain's size
// (D3D12 presenter, Fsr1Scaler). The constants are FsrEasuCon's; gpu_blit_fsr1_rcas.frag sharpens the result.

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

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) {
	return textureGather(sampler2D(frame, frame_sampler), p, 0);
}
AF4 FsrEasuGF(AF2 p) {
	return textureGather(sampler2D(frame, frame_sampler), p, 1);
}
AF4 FsrEasuBF(AF2 p) {
	return textureGather(sampler2D(frame, frame_sampler), p, 2);
}
#include "ffx_fsr1.h"

void main() {
	AF3 pixel;
	FsrEasuF(pixel, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
	color = vec4(pixel, 1.0);
}
