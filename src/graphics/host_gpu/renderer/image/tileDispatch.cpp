#include "graphics/host_gpu/renderer/image/tileDispatch.h"

#include "common/assert.h"
#include "gpu_tiler_shaders/gpu_tiler_demote_d16_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_depth_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_promote_d16_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_render_target_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard256_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_3d_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_swap_bgra16_spv.h"

#include <array>
#include <bit>

namespace Libs::Graphics::TileShaders {

std::vector<Dispatch> Prepare(bool tile, uint64_t tiled_capacity, uint64_t linear_capacity,
                              std::span<const GpuTileInfo> infos, uint64_t source_base,
                              uint64_t target_base, const uint32_t max_groups[3]) {
	EXIT_IF(infos.empty() || tiled_capacity == 0 || linear_capacity == 0);
	EXIT_NOT_IMPLEMENTED(tiled_capacity > UINT32_MAX || linear_capacity > UINT32_MAX);

	const auto checked_multiply = [](uint64_t left, uint64_t right, uint64_t& result) {
		return (left == 0 || right <= UINT64_MAX / left) && (result = left * right, true);
	};
	const auto checked_add = [](uint64_t left, uint64_t right, uint64_t& result) {
		return right <= UINT64_MAX - left && (result = left + right, true);
	};
	const auto valid_range = [](uint64_t offset, uint64_t size, uint64_t capacity) {
		return size != 0 && offset <= capacity && size <= capacity - offset;
	};

	std::vector<Dispatch> dispatches;
	dispatches.reserve(infos.size());
	for (const auto& info: infos) {
		TileBlockLayout block {};
		const uint32_t  tiled_width  = info.tiled_width != 0 ? info.tiled_width : info.pitch;
		const uint32_t  tiled_height = info.tiled_height != 0 ? info.tiled_height : info.height;
		const uint64_t  groups_x     = (static_cast<uint64_t>(info.width) + 7u) / 8u;
		const uint64_t  groups_y     = (static_cast<uint64_t>(info.height) + 7u) / 8u;
		EXIT_NOT_IMPLEMENTED(
		    !TileGetBlockLayout(info.family, info.bytes_per_element, block) || info.width == 0 ||
		    info.height == 0 || info.depth == 0 || info.pitch < info.width ||
		    groups_x > max_groups[0] || groups_y > max_groups[1] || info.depth > max_groups[2] ||
		    (!info.tail && (tiled_width < info.width || tiled_height < info.height)) ||
		    !valid_range(info.linear_offset, info.linear_size, linear_capacity) ||
		    !valid_range(info.tiled_offset, info.tiled_size, tiled_capacity) ||
		    (block.block_depth == 1 && info.depth != 1));

		uint64_t pitch_bytes = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(info.pitch, info.bytes_per_element, pitch_bytes) ||
		                     pitch_bytes > UINT32_MAX);
		uint64_t slice_bytes   = info.linear_slice_stride;
		uint64_t minimum_slice = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(pitch_bytes, info.height, minimum_slice));
		if (slice_bytes == 0) {
			slice_bytes = minimum_slice;
		}
		uint64_t linear_used = 0;
		uint64_t bytes       = 0;
		EXIT_NOT_IMPLEMENTED((info.depth > 1 && slice_bytes < minimum_slice) ||
		                     !checked_multiply(info.depth - 1u, slice_bytes, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     !checked_multiply(info.height - 1u, pitch_bytes, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     !checked_multiply(info.width, info.bytes_per_element, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     linear_used > info.linear_size || slice_bytes > UINT32_MAX);

		const uint64_t columns =
		    (static_cast<uint64_t>(tiled_width) + block.block_width - 1u) / block.block_width;
		const uint64_t rows =
		    (static_cast<uint64_t>(tiled_height) + block.block_height - 1u) / block.block_height;
		uint64_t blocks_per_slice = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(columns, rows, blocks_per_slice) ||
		                     columns > UINT32_MAX || blocks_per_slice > UINT32_MAX);
		if (info.tail) {
			EXIT_NOT_IMPLEMENTED(
			    info.family == TileBlockFamily::Standard256B || info.depth > block.block_depth ||
			    info.tail_x >= block.block_width || info.width > block.block_width - info.tail_x ||
			    info.tail_y >= block.block_height ||
			    info.height > block.block_height - info.tail_y ||
			    info.tiled_size < block.block_size);
		} else {
			const uint64_t slices =
			    (static_cast<uint64_t>(info.depth) + block.block_depth - 1u) / block.block_depth;
			uint64_t tiled_used = 0;
			EXIT_NOT_IMPLEMENTED(!checked_multiply(blocks_per_slice, slices, tiled_used) ||
			                     !checked_multiply(tiled_used, block.block_size, tiled_used) ||
			                     tiled_used > info.tiled_size);
		}

		const uint32_t alignment = std::min(info.bytes_per_element, 4u);
		EXIT_NOT_IMPLEMENTED(((info.linear_offset | info.tiled_offset | pitch_bytes | slice_bytes) &
		                      (alignment - 1u)) != 0);
		const uint64_t src = source_base + (tile ? info.linear_offset : info.tiled_offset);
		const uint64_t dst = target_base + (tile ? info.tiled_offset : info.linear_offset);
		EXIT_NOT_IMPLEMENTED(src > UINT32_MAX || dst > UINT32_MAX);

		const uint32_t family_index  = static_cast<uint32_t>(info.family);
		const uint32_t element_index = std::countr_zero(info.bytes_per_element);
		EXIT_NOT_IMPLEMENTED(family_index >= FamilyCount || element_index >= BytesPerElementCount);
		Dispatch dispatch {};
		dispatch.pipeline_slot =
		    ((tile ? FamilyCount : 0u) + family_index) * BytesPerElementCount + element_index;
		dispatch.params.src_base         = static_cast<uint32_t>(src);
		dispatch.params.dst_base         = static_cast<uint32_t>(dst);
		dispatch.params.width            = info.width;
		dispatch.params.height           = info.height;
		dispatch.params.depth            = info.depth;
		dispatch.params.surface_z        = info.surface_z;
		dispatch.params.pitch_bytes      = static_cast<uint32_t>(pitch_bytes);
		dispatch.params.slice_bytes      = static_cast<uint32_t>(slice_bytes);
		dispatch.params.blocks_per_row   = static_cast<uint32_t>(columns);
		dispatch.params.blocks_per_slice = static_cast<uint32_t>(blocks_per_slice);
		dispatch.params.tail_x           = info.tail_x;
		dispatch.params.tail_y           = info.tail_y;
		dispatch.params.tail             = info.tail;
		dispatches.push_back(dispatch);
	}
	return dispatches;
}

Specialization PipelineShader(uint32_t slot) {
	EXIT_IF(slot >= PipelineCount);
	static constexpr std::array<std::span<const uint32_t>, FamilyCount> shaders {{
	    GPU_TILER_STANDARD256_SPV,
	    GPU_TILER_STANDARD4_SPV,
	    GPU_TILER_STANDARD4_3D_SPV,
	    GPU_TILER_STANDARD64_SPV,
	    GPU_TILER_STANDARD64_3D_SPV,
	    GPU_TILER_PRT_SPV,
	    GPU_TILER_PRT_3D_SPV,
	    GPU_TILER_RENDER_TARGET_SPV,
	    GPU_TILER_DEPTH_SPV,
	}};
	const uint32_t element_index   = slot % BytesPerElementCount;
	const uint32_t direction_index = slot / (FamilyCount * BytesPerElementCount);
	const uint32_t family_index    = (slot / BytesPerElementCount) % FamilyCount;
	return {shaders[family_index], 1u << element_index, direction_index};
}

std::span<const uint32_t> PromoteD16() {
	return GPU_TILER_PROMOTE_D16_SPV;
}

std::span<const uint32_t> DemoteD16() {
	return GPU_TILER_DEMOTE_D16_SPV;
}

std::span<const uint32_t> SwapBgra16() {
	return GPU_TILER_SWAP_BGRA16_SPV;
}

} // namespace Libs::Graphics::TileShaders
