#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TILEDISPATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TILEDISPATCH_H_

#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Compute dispatches of the GPU tiling shaders (shaders/gpu_tiler_*.comp), shared by all host GPU
// backends. The shaders are compiled to SPIR-V at build time.

namespace Libs::Graphics::TileShaders {

inline constexpr uint32_t FamilyCount          = static_cast<uint32_t>(TileBlockFamily::Count);
inline constexpr uint32_t BytesPerElementCount = 5;
inline constexpr uint32_t DirectionCount       = 2;
inline constexpr uint32_t PipelineCount = FamilyCount * BytesPerElementCount * DirectionCount;
inline constexpr uint32_t MaxMipLevels         = 16;

// One mip level of a tiling dispatch (std140, shaders/gpu_tiler_common.inc).
struct alignas(16) Mip {
	uint32_t src_base;
	uint32_t dst_base;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_bytes;
	uint32_t slice_bytes;
	uint32_t tiled_slice_bytes;
	uint32_t blocks_per_row;
	uint32_t surface_z;
	uint32_t tail_x;
	uint32_t tail_y;
	uint32_t num_texels;
};
// The tiling shaders' uniform buffer; only the header and the active levels' records are uploaded.
struct TilingParams {
	uint32_t                      num_texels      = 0;
	uint32_t                      color_transform = 0;
	std::array<uint32_t, 2>        padding {};
	std::array<Mip, MaxMipLevels> mips;
};
static_assert(sizeof(Mip) == 48);
static_assert(offsetof(TilingParams, mips) == 16);
static_assert(sizeof(TilingParams) == 784);

// One dispatch over all levels of an image: its parameters, how many bytes of them the shader reads,
// and the pipeline slot (the shader family and its specialization: element bytes, direction).
struct Tiling {
	TilingParams params;
	uint16_t     params_size   = 0;
	uint16_t     pipeline_slot = 0;
};

// Push constants of the conversion shaders (D16 depth, color transform).
struct Conversion {
	uint32_t src_base;
	uint32_t dst_base;
	uint32_t width;
	uint32_t height;
	uint32_t source_pitch;
	uint32_t target_pitch;
	uint32_t color_transform;
	uint32_t to_host;
};
static_assert(sizeof(Conversion) == 32);

// Validates the first `levels` levels of `info` (0: all) and computes their dispatch. The bases are
// added to the buffer offsets in the shader parameters.
[[nodiscard]] Tiling Prepare(bool tile, uint64_t tiled_capacity, uint64_t linear_capacity,
                             const ImageInfo& info, uint32_t levels, uint64_t source_base,
                             uint64_t target_base);

// The workgroups (64 invocations each) of a tiling dispatch over `num_texels`, spread over x and y.
// Returns false where they need more than `max_groups_y` rows.
[[nodiscard]] bool Groups(uint32_t num_texels, uint32_t max_groups_x, uint32_t max_groups_y,
                          uint32_t* groups_x, uint32_t* groups_y);

struct Specialization {
	std::span<const uint32_t> spirv;
	uint32_t                  element_bytes = 0; // specialization constant 0
	uint32_t                  tile          = 0; // specialization constant 1
};
[[nodiscard]] Specialization PipelineShader(uint32_t slot);

// Conversion shaders; their specialization constant 0 selects D32 (1) or D24 (0) depth.
[[nodiscard]] std::span<const uint32_t> PromoteD16();
[[nodiscard]] std::span<const uint32_t> DemoteD16();
[[nodiscard]] std::span<const uint32_t> ColorTransformShader();

} // namespace Libs::Graphics::TileShaders

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_TILEDISPATCH_H_
