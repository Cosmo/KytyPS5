#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TILEDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TILEDISPATCH_H_

#include "graphics/guest_gpu/tile.h"

#include <cstdint>
#include <span>
#include <vector>

// Compute dispatches of the GPU tiling shaders (shaders/gpu_tiler_*.comp), shared by all host GPU
// backends. The shaders are compiled to SPIR-V at build time.

namespace Libs::Graphics {

struct GpuTileInfo {
	TileBlockFamily family              = TileBlockFamily::Count;
	uint32_t        bytes_per_element   = 0;
	uint64_t        linear_offset       = 0;
	uint64_t        linear_size         = 0;
	uint64_t        tiled_offset        = 0;
	uint64_t        tiled_size          = 0;
	uint64_t        linear_slice_stride = 0;
	uint32_t        width               = 0;
	uint32_t        height              = 0;
	uint32_t        depth               = 1;
	uint32_t        pitch               = 0;
	uint32_t        tail_x              = 0;
	uint32_t        tail_y              = 0;
	bool            tail                = false;
	uint32_t        tiled_width         = 0;
	uint32_t        tiled_height        = 0;
	uint32_t        surface_z           = 0;
};

// Parameter block of every tiler and conversion shader.
struct TileShaderParams {
	uint32_t src_base;
	uint32_t dst_base;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t surface_z;
	uint32_t pitch_bytes;
	uint32_t slice_bytes;
	uint32_t blocks_per_row;
	uint32_t blocks_per_slice;
	uint32_t tail_x;
	uint32_t tail_y;
	uint32_t tail;
};
static_assert(sizeof(TileShaderParams) == 52);

namespace TileShaders {

inline constexpr uint32_t FamilyCount          = static_cast<uint32_t>(TileBlockFamily::Count);
inline constexpr uint32_t BytesPerElementCount = 5;
inline constexpr uint32_t DirectionCount       = 2;
inline constexpr uint32_t PipelineCount = FamilyCount * BytesPerElementCount * DirectionCount;

// One 8x8-thread dispatch of a tiling shader. The pipeline slot selects the shader family and its
// specialization (element bytes, direction).
struct Dispatch {
	TileShaderParams params {};
	uint32_t         pipeline_slot = 0;
};

// Validates `infos` and computes their dispatches. The bases are added to the buffer offsets in
// the shader parameters.
[[nodiscard]] std::vector<Dispatch> Prepare(bool tile, uint64_t tiled_capacity,
                                            uint64_t linear_capacity,
                                            std::span<const GpuTileInfo> infos, uint64_t source_base,
                                            uint64_t target_base, const uint32_t max_groups[3]);

struct Specialization {
	std::span<const uint32_t> spirv;
	uint32_t                  element_bytes = 0; // specialization constant 0
	uint32_t                  tile          = 0; // specialization constant 1
};
[[nodiscard]] Specialization PipelineShader(uint32_t slot);

// Conversion shaders; their specialization constant 0 selects D32 (1) or D24 (0) depth.
[[nodiscard]] std::span<const uint32_t> PromoteD16();
[[nodiscard]] std::span<const uint32_t> DemoteD16();
[[nodiscard]] std::span<const uint32_t> SwapBgra16();

} // namespace TileShaders

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TILEDISPATCH_H_
