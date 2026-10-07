#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_TILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_TILER_H_

#include "common/common.h"
#include "graphics/host_gpu/d3d12/buffer.h"
#include "graphics/host_gpu/renderer/image/tileDispatch.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class Image;
class StreamBuffer;
struct GraphicContext;

// Converts guest tiled surfaces to linear buffer data and back on the GPU, with the shared tiling
// shaders (renderer/image/tileDispatch.h). Same interface as the Vulkan tile manager.
class TileManager final {
public:
	enum class D16Direction { Promote, Demote };

	struct Result {
		BufferHandle buffer = nullptr;
		uint64_t     offset = 0;
		uint64_t     size   = 0;
	};
	struct D16Layout {
		uint32_t width               = 0;
		uint32_t height              = 0;
		uint32_t layers              = 0;
		uint64_t source_row_stride   = 0;
		uint64_t target_row_stride   = 0;
		uint64_t source_slice_stride = 0;
		uint64_t target_slice_stride = 0;
	};

	TileManager(GraphicContext& graphics, CommandScheduler& scheduler, StreamBuffer& stream_buffer);
	KYTY_CLASS_NO_COPY(TileManager);

	// The returned buffer remains alive through the current scheduler tick.
	[[nodiscard]] Result Detile(BufferHandle tiled, uint64_t tiled_offset, const ImageInfo& info);
	void Tile(BufferHandle linear, uint64_t linear_offset, uint64_t linear_capacity,
	          BufferHandle tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
	          const ImageInfo& info, uint32_t levels = 0);
	void TileImage(Image& image, std::span<const vk::BufferImageCopy> regions, BufferHandle tiled,
	               uint64_t tiled_offset, uint64_t tiled_capacity, uint32_t levels = 0);
	// A scratch buffer is created per call here, so it is never the `input` the caller reads from (the Vulkan tile manager reuses two).
	[[nodiscard]] Result GetScratchBuffer(uint64_t size, BufferHandle input = nullptr);
	void                 ConvertD16(Result source, Result target, D16Direction direction, bool d32,
	                                const D16Layout& layout);
	[[nodiscard]] Result TransformColor(Result input, ColorTransform transform, bool to_host);
	void                 TransformColor(Result input, Result output, ColorTransform transform, bool to_host);
	// Color-clear keys are multiples of 0x20, so key >> 5 selects one of these slots (the texture cache's clear keys).
	static constexpr uint32_t ColorClearSlots = 8;

private:
	// One dispatch over all levels of an image, its parameters in the stream buffer.
	struct Dispatch {
		uint64_t params_offset = 0;
		uint32_t num_texels    = 0;
		uint32_t pipeline_slot = 0;
	};

	[[nodiscard]] Dispatch Prepare(bool tile, uint64_t tiled_capacity, uint64_t linear_capacity,
	                               const ImageInfo& info, uint32_t levels, uint64_t source_base,
	                               uint64_t target_base);
	void Record(BufferHandle source, uint64_t source_offset, BufferHandle target,
	            uint64_t target_offset, uint64_t target_capacity, const Dispatch& dispatch,
	            bool clear_target);

	GraphicContext&   m_graphics;
	CommandScheduler& m_scheduler;
	StreamBuffer&     m_stream_buffer;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_TILER_H_
