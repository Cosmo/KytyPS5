#include "graphics/host_gpu/renderer/image/tileDispatch.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "gpu_tiler_shaders/gpu_tiler_color_clear_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_color_transform_spv.h"
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

#include <algorithm>
#include <array>
#include <bit>

namespace Libs::Graphics::TileShaders {

Tiling Prepare(bool tile, uint64_t tiled_capacity, uint64_t linear_capacity, const ImageInfo& info,
               uint32_t levels, uint64_t source_base, uint64_t target_base) {
	levels = levels != 0 ? levels : info.resources.levels;
	EXIT_IF(levels == 0 || levels > MaxMipLevels || levels > info.resources.levels ||
	        tiled_capacity == 0 || linear_capacity == 0);
	EXIT_NOT_IMPLEMENTED(tiled_capacity > UINT32_MAX || linear_capacity > UINT32_MAX);

	const auto& block     = info.tiling.block;
	const auto  transform = info.GetColorTransform();
	EXIT_NOT_IMPLEMENTED(
	    !info.IsTiled() || info.samples != 1 || Prospero::IsFmaskTextureFormat(info.guest_format) ||
	    block.family >= TileBlockFamily::Count || !std::has_single_bit(block.bytes_per_element) ||
	    block.bytes_per_element > 16 || block.block_width == 0 || block.block_height == 0 ||
	    block.block_depth == 0 || block.block_size == 0 || info.tiling.texel_width == 0 ||
	    info.tiling.texel_height == 0 || info.tiled_slice_stride == 0);
	EXIT_IF((transform == ColorTransform::SwapBgra16 && block.bytes_per_element != 8u) ||
	        (transform == ColorTransform::Reverse10_11_11 && block.bytes_per_element != 4u));
	const auto checked_multiply = [](uint64_t left, uint64_t right, uint64_t& result) {
		return (left == 0 || right <= UINT64_MAX / left) && (result = left * right, true);
	};
	const auto checked_add = [](uint64_t left, uint64_t right, uint64_t& result) {
		return right <= UINT64_MAX - left && (result = left + right, true);
	};
	const auto valid_range = [](uint64_t offset, uint64_t size, uint64_t capacity) {
		return size != 0 && offset <= capacity && size <= capacity - offset;
	};

	// ImageInfo owns the layout. Upload only the header and active mip records.
	Tiling tiling;
	auto&  params          = tiling.params;
	params.color_transform = static_cast<uint32_t>(transform);
	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip    = info.mip_layout[level];
		const auto  extent = info.MipExtent(level);
		const auto  width  = static_cast<uint32_t>(
		    (uint64_t {extent.width} + info.tiling.texel_width - 1u) / info.tiling.texel_width);
		const auto height = static_cast<uint32_t>(
		    (uint64_t {extent.height} + info.tiling.texel_height - 1u) / info.tiling.texel_height);
		const auto depth = extent.depth;
		const bool tail  = level >= info.first_tail_level;
		EXIT_NOT_IMPLEMENTED(width == 0 || height == 0 || depth == 0 || mip.linear_pitch < width ||
		                     mip.linear_size == 0 || mip.size == 0 ||
		                     (!tail && (mip.pitch < width || mip.height < height)) ||
		                     (!info.IsVolume() && mip.size % depth != 0));

		uint64_t pitch_bytes = 0;
		EXIT_NOT_IMPLEMENTED(
		    !checked_multiply(mip.linear_pitch, block.bytes_per_element, pitch_bytes) ||
		    pitch_bytes > UINT32_MAX);
		const uint64_t slice_bytes =
		    info.linear_slice_stride != 0 ? info.linear_slice_stride : mip.linear_size;
		uint64_t minimum_slice = 0;
		uint64_t linear_used   = 0;
		uint64_t bytes         = 0;
		uint64_t linear_span   = 0;
		EXIT_NOT_IMPLEMENTED(!checked_multiply(pitch_bytes, height, minimum_slice) ||
		                     (depth > 1 && slice_bytes < minimum_slice) ||
		                     !checked_multiply(height - 1u, pitch_bytes, linear_used) ||
		                     !checked_multiply(width, block.bytes_per_element, bytes) ||
		                     !checked_add(linear_used, bytes, linear_used) ||
		                     linear_used > mip.linear_size || slice_bytes > UINT32_MAX ||
		                     !checked_multiply(depth - 1u, slice_bytes, linear_span) ||
		                     !checked_add(linear_span, mip.linear_size, linear_span) ||
		                     !valid_range(mip.linear_offset, linear_span, linear_capacity));

		const uint64_t columns = (uint64_t {mip.pitch} + block.block_width - 1u) / block.block_width;
		if (tail) {
			EXIT_NOT_IMPLEMENTED(
			    block.family == TileBlockFamily::Standard256B || mip.tail_x >= block.block_width ||
			    width > block.block_width - mip.tail_x || mip.tail_y >= block.block_height ||
			    height > block.block_height - mip.tail_y);
		}
		const uint64_t tiled_plane = info.IsVolume() ? mip.size : mip.size / depth;
		const uint64_t tiled_slice = info.tiled_slice_stride;
		const uint64_t slices      = (uint64_t {depth} + block.block_depth - 1u) / block.block_depth;
		uint64_t       tiled_span  = 0;
		EXIT_NOT_IMPLEMENTED(columns > UINT32_MAX || tiled_slice > UINT32_MAX ||
		                     (slices > 1 && tiled_slice < tiled_plane) ||
		                     !checked_multiply(slices - 1u, tiled_slice, tiled_span) ||
		                     !checked_add(tiled_span, tiled_plane, tiled_span) ||
		                     !valid_range(mip.offset, tiled_span, tiled_capacity));

		const uint32_t alignment = std::min(block.bytes_per_element, 4u);
		EXIT_NOT_IMPLEMENTED(
		    ((mip.linear_offset | mip.offset | pitch_bytes | slice_bytes | tiled_slice) &
		     (alignment - 1u)) != 0);
		uint64_t src = 0, dst = 0, texels = 0;
		EXIT_NOT_IMPLEMENTED(
		    !checked_add(source_base, tile ? mip.linear_offset : mip.offset, src) ||
		    !checked_add(target_base, tile ? mip.offset : mip.linear_offset, dst) ||
		    src > UINT32_MAX || dst > UINT32_MAX || !checked_multiply(width, height, texels) ||
		    !checked_multiply(texels, depth, texels) || texels > UINT32_MAX - params.num_texels);
		params.mips[level] = {static_cast<uint32_t>(src),
		                      static_cast<uint32_t>(dst),
		                      width,
		                      height,
		                      static_cast<uint32_t>(pitch_bytes),
		                      static_cast<uint32_t>(slice_bytes),
		                      static_cast<uint32_t>(tiled_slice),
		                      static_cast<uint32_t>(columns),
		                      mip.surface_z,
		                      tail ? mip.tail_x : 0u,
		                      tail ? mip.tail_y : 0u,
		                      static_cast<uint32_t>(texels)};
		params.num_texels += static_cast<uint32_t>(texels);
	}

	tiling.params_size   = static_cast<uint16_t>(offsetof(TilingParams, mips) + levels * sizeof(Mip));
	tiling.pipeline_slot = static_cast<uint16_t>(
	    ((tile ? FamilyCount : 0u) + static_cast<uint32_t>(block.family)) * BytesPerElementCount +
	    std::countr_zero(block.bytes_per_element));
	return tiling;
}

bool Groups(uint32_t num_texels, uint32_t max_groups_x, uint32_t max_groups_y, uint32_t* groups_x,
            uint32_t* groups_y) {
	EXIT_IF(num_texels == 0 || max_groups_x == 0);
	const uint64_t groups = (uint64_t {num_texels} + 63u) / 64u;
	*groups_y             = static_cast<uint32_t>((groups + max_groups_x - 1u) / max_groups_x);
	*groups_x             = static_cast<uint32_t>((groups + *groups_y - 1u) / *groups_y);
	return *groups_y <= max_groups_y;
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

std::span<const uint32_t> ColorTransformShader() {
	return GPU_TILER_COLOR_TRANSFORM_SPV;
}

std::span<const uint32_t> ColorClearShader() {
	return GPU_TILER_COLOR_CLEAR_SPV;
}

} // namespace Libs::Graphics::TileShaders
