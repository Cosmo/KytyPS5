layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, std430) readonly buffer Input { uint data[]; } input_buffer;
layout(set = 0, binding = 1, std430) buffer Output { uint data[]; } output_buffer;

layout(set = 0, binding = 2, std140) uniform Push {
	uint src_base;
	uint dst_base;
	uint width;
	uint height;
	uint depth;
	uint surface_z;
	uint pitch_bytes;
	uint slice_bytes;
	uint blocks_per_row;
	uint blocks_per_slice;
	uint tail_x;
	uint tail_y;
	uint tail;
} params;

void copy_element(uint src, uint dst) {
	if (ELEMENT_BYTES >= 4) {
		for (uint i = 0; i < ELEMENT_BYTES; i += 4) {
			output_buffer.data[(dst + i) >> 2] = input_buffer.data[(src + i) >> 2];
		}
	} else {
		uint mask = ELEMENT_BYTES == 1 ? 0xffu : 0xffffu;
		uint value = (input_buffer.data[src >> 2] >> ((src & 3u) * 8u)) & mask;
		uint shift = (dst & 3u) * 8u;
		atomicAnd(output_buffer.data[dst >> 2], ~(mask << shift));
		atomicOr(output_buffer.data[dst >> 2], value << shift);
	}
}

// Where the element at p lies in the tiled surface, relative to the surface start.
uint tiled_offset(uvec3 p) {
	uvec3 extent = block_extent();
	uvec3 swizzle = p;
	uvec3 block = uvec3(0);
	if (params.tail != 0) {
		swizzle.xy += uvec2(params.tail_x, params.tail_y);
	} else {
		block = p / extent;
	}

	uint block_index = block.z * params.blocks_per_slice +
	                   block.y * params.blocks_per_row + block.x;
	return block_index * BLOCK_BYTES +
	       block_offset(uvec3(swizzle.xy, swizzle.z + params.surface_z));
}

// Elements smaller than a dword normally reach the output with atomics, because neighbouring elements
// share a dword. On the Xbox Series X the atomics on this kernel's output buffer have no effect (it
// stays at its initial value; plain stores work). So when the linear rows are dword aligned, one
// invocation per output dword gathers its elements and writes the whole dword. De-tiling clears its
// output first, which the padding bytes of a last, partial dword rely on.
bool gathers_dwords() {
	return TILE == 0 && ELEMENT_BYTES < 4 &&
	       ((params.dst_base | params.slice_bytes | params.pitch_bytes) & 3u) == 0u;
}

void main() {
	uvec3 p = gl_GlobalInvocationID;
	if (p.x >= params.width || p.y >= params.height || p.z >= params.depth) {
		return;
	}

	uint linear = p.z * params.slice_bytes + p.y * params.pitch_bytes +
	              p.x * ELEMENT_BYTES;
	if (gathers_dwords()) {
		const uint per_dword = ELEMENT_BYTES < 4 ? 4u / ELEMENT_BYTES : 1u;
		if (p.x % per_dword != 0u) {
			return;
		}
		const uint mask = ELEMENT_BYTES == 1 ? 0xffu : 0xffffu;
		uint word = 0u;
		for (uint i = 0u; i < per_dword && p.x + i < params.width; i++) {
			uint src = params.src_base + tiled_offset(uvec3(p.x + i, p.y, p.z));
			uint value = (input_buffer.data[src >> 2] >> ((src & 3u) * 8u)) & mask;
			word |= value << (i * ELEMENT_BYTES * 8u);
		}
		output_buffer.data[(params.dst_base + linear) >> 2] = word;
		return;
	}

	uint tiled = tiled_offset(p);
	if (TILE == 0) {
		copy_element(params.src_base + tiled, params.dst_base + linear);
	} else {
		copy_element(params.src_base + linear, params.dst_base + tiled);
	}
}
