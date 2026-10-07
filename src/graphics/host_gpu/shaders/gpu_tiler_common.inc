#extension GL_EXT_shader_8bit_storage : require
#extension GL_EXT_shader_16bit_storage : require

layout(local_size_x = 64) in;

layout(set = 0, binding = 0, std430) readonly buffer Input { uint data[]; } input_buffer;
layout(set = 0, binding = 1, std430) buffer Output { uint data[]; } output_buffer;
layout(set = 0, binding = 0, std430) readonly buffer Input8 { uint8_t data[]; } input_buffer8;
layout(set = 0, binding = 1, std430) buffer Output8 { uint8_t data[]; } output_buffer8;
layout(set = 0, binding = 0, std430) readonly buffer Input16 { uint16_t data[]; } input_buffer16;
layout(set = 0, binding = 1, std430) buffer Output16 { uint16_t data[]; } output_buffer16;

struct Mip {
	uint src_base;
	uint dst_base;
	uint width;
	uint height;
	uint pitch_bytes;
	uint slice_bytes;
	uint tiled_slice_bytes;
	uint blocks_per_row;
	uint surface_z;
	uint tail_x;
	uint tail_y;
	uint num_texels;
};

layout(set = 0, binding = 2, std140) uniform TilingInfo {
	uint num_texels;
	uint color_transform;
	Mip mips[16];
} info;

#include "gpu_tiler_color.inc"

void copy_element(uint src, uint dst) {
	if ((ELEMENT_BYTES == 4u || ELEMENT_BYTES == 8u) && info.color_transform != 0u) {
		uvec2 value = uvec2(input_buffer.data[src >> 2], 0u);
		if (ELEMENT_BYTES == 8u) value.y = input_buffer.data[(src >> 2) + 1u];
		value = transform_color(value, info.color_transform, TILE == 0u);
		output_buffer.data[dst >> 2] = value.x;
		if (ELEMENT_BYTES == 8u) output_buffer.data[(dst >> 2) + 1u] = value.y;
	} else if (ELEMENT_BYTES >= 4) {
		for (uint i = 0; i < ELEMENT_BYTES; i += 4) {
			output_buffer.data[(dst + i) >> 2] = input_buffer.data[(src + i) >> 2];
		}
	} else if (ELEMENT_BYTES == 2) {
		output_buffer16.data[dst >> 1] = input_buffer16.data[src >> 1];
	} else {
		output_buffer8.data[dst] = input_buffer8.data[src];
	}
}

// Where the element at p of a mip level lies in the tiled surface, relative to the level's start.
uint tiled_offset(Mip params, uvec3 p) {
	uvec3 extent = block_extent();
	uvec3 block = p / extent;
	uvec3 swizzle = p + uvec3(params.tail_x, params.tail_y, params.surface_z);

	uint block_index = block.y * params.blocks_per_row + block.x;
	return block.z * params.tiled_slice_bytes + block_index * BLOCK_BYTES +
	       block_offset(swizzle);
}

// Elements smaller than a dword share a dword with their neighbours, and where the device has no
// byte and 16-bit stores (D3D12 below shader model 6.2, the Xbox Series X) they reach the output with
// atomics, which have no effect on this kernel's output buffer on the Series X (it stays at its
// initial value; plain stores work). So when the linear rows are dword aligned, one invocation per
// output dword gathers its elements and writes the whole dword. De-tiling clears its output first,
// which the padding bytes of a last, partial dword rely on.
bool gathers_dwords(Mip params) {
	return TILE == 0 && ELEMENT_BYTES < 4 &&
	       ((params.dst_base | params.slice_bytes | params.pitch_bytes) & 3u) == 0u;
}

void main() {
	uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
	// Reject padding groups before multiplying: the active range may approach 4 GiB.
	if (group > (info.num_texels - 1u) / 64u) {
		return;
	}
	uint texel = group * 64u + gl_LocalInvocationID.x;
	if (texel >= info.num_texels) {
		return;
	}
	uint mip = 0u;
	uint mip_texels = info.mips[mip].num_texels;
	while (texel >= mip_texels) {
		texel -= mip_texels;
		mip_texels = info.mips[++mip].num_texels;
	}
	Mip params = info.mips[mip];
	uint row = texel / params.width;
	uint z = row / params.height;
	uvec3 p = uvec3(texel - row * params.width, row - z * params.height, z);

	uint linear = p.z * params.slice_bytes + p.y * params.pitch_bytes +
	              p.x * ELEMENT_BYTES;
	if (gathers_dwords(params)) {
		const uint per_dword = ELEMENT_BYTES < 4 ? 4u / ELEMENT_BYTES : 1u;
		if (p.x % per_dword != 0u) {
			return;
		}
		const uint mask = ELEMENT_BYTES == 1 ? 0xffu : 0xffffu;
		uint word = 0u;
		for (uint i = 0u; i < per_dword && p.x + i < params.width; i++) {
			uint src = params.src_base + tiled_offset(params, uvec3(p.x + i, p.y, p.z));
			uint value = (input_buffer.data[src >> 2] >> ((src & 3u) * 8u)) & mask;
			word |= value << (i * ELEMENT_BYTES * 8u);
		}
		output_buffer.data[(params.dst_base + linear) >> 2] = word;
		return;
	}

	uint tiled = tiled_offset(params, p);
	if (TILE == 0) {
		copy_element(params.src_base + tiled, params.dst_base + linear);
	} else {
		copy_element(params.src_base + linear, params.dst_base + tiled);
	}
}
