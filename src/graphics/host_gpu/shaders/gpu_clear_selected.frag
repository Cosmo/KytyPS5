#version 450 core
// The D3D12 renderer's color clears selected on the GPU where predication does not work (the Series X: BlitHelper::ClearSelected):
// a fullscreen draw per slice and key whose pixels keep their color unless the color metadata check selected the key. The check's
// predicate is 64 bits per slice and key, read as two words of a uint texel buffer.

layout(binding = 0, set = 0) uniform utextureBuffer predicates;

layout(push_constant) uniform Constants {
	vec4 color_float;
	uvec4 color_bits;
	uint predicate;
} constants;

#if defined(CLEAR_UINT)
layout(location = 0) out uvec4 color;
#elif defined(CLEAR_SINT)
layout(location = 0) out ivec4 color;
#else
layout(location = 0) out vec4 color;
#endif

void main() {
	const int word = int(constants.predicate) * 2;
	if ((texelFetch(predicates, word).x | texelFetch(predicates, word + 1).x) == 0u) {
		discard;
	}
#if defined(CLEAR_UINT)
	color = constants.color_bits;
#elif defined(CLEAR_SINT)
	color = ivec4(constants.color_bits);
#else
	color = constants.color_float;
#endif
}