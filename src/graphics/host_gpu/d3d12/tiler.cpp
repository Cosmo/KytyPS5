#include "graphics/host_gpu/d3d12/tiler.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "graphics/host_gpu/gpuBackend.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace Libs::Graphics {

namespace {

// Root descriptors address buffers at this alignment; shader parameters carry the remainder.
constexpr uint64_t RootAlignment = 16;

// Workgroup counts per dimension of a D3D12 dispatch.
constexpr uint32_t MaxGroups[3] = {D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION,
                                   D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION,
                                   D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION};

} // namespace

TileManager::TileManager(GraphicContext& graphics, CommandScheduler& scheduler,
                         StreamBuffer& stream_buffer)
    : m_graphics(graphics), m_scheduler(scheduler), m_stream_buffer(stream_buffer) {}

TileManager::Result TileManager::GetScratchBuffer(uint64_t size, BufferHandle /*input*/) {
	auto  scratch = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
	                                         Common::AlignUp<uint64_t>(std::max<uint64_t>(size, 4), 4));
	auto* result  = scratch.get();
	m_scheduler.DeferOperation([owner = std::move(scratch)]() mutable { owner.reset(); });
	return {result, 0, result->Size()};
}

std::vector<TileManager::Dispatch> TileManager::Prepare(bool tile, uint64_t tiled_capacity,
                                                        uint64_t linear_capacity,
                                                        std::span<const GpuTileInfo> infos,
                                                        uint64_t source_base,
                                                        uint64_t target_base) {
	const auto prepared = TileShaders::Prepare(tile, tiled_capacity, linear_capacity, infos,
	                                           source_base, target_base, MaxGroups);
	const auto stride   = Common::AlignUp<uint64_t>(sizeof(TileShaderParams),
	                                                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	auto [mapped, offset] = m_stream_buffer.Map(prepared.size() * stride,
	                                            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
	EXIT_IF(mapped == nullptr);
	std::vector<Dispatch> dispatches;
	dispatches.reserve(prepared.size());
	for (size_t i = 0; i < prepared.size(); i++) {
		std::memcpy(mapped + i * stride, &prepared[i].params, sizeof(TileShaderParams));
		dispatches.push_back({prepared[i], offset + i * stride});
	}
	m_stream_buffer.Commit();
	return dispatches;
}

void TileManager::Record(BufferHandle source, uint64_t source_offset, BufferHandle target,
                         uint64_t target_offset, uint64_t target_capacity,
                         std::span<const Dispatch> dispatches, bool clear_target) {
	auto& command = m_scheduler.Current();
	if (clear_target) {
		target->Fill(target_offset, Common::AlignUp<uint64_t>(target_capacity, 4), 0);
	}
	auto& kernels = command.GetContext().GetComputeKernels();
	ComputeKernels::Bindings bindings;
	bindings.input         = source;
	bindings.input_offset  = Common::AlignDown(source_offset, RootAlignment);
	bindings.output        = target;
	bindings.output_offset = Common::AlignDown(target_offset, RootAlignment);
	for (const auto& dispatch: dispatches) {
		const auto shader = TileShaders::PipelineShader(dispatch.shader.pipeline_slot);
		const D3D12::SpecializationConstant constants[] = {{0, shader.element_bytes},
		                                                    {1, shader.tile}};
		bindings.params        = &m_stream_buffer;
		bindings.params_offset = dispatch.params_offset;
		const auto& params = dispatch.shader.params;
		kernels.Dispatch(command, kernels.Get(shader.spirv, constants), bindings,
		                 (params.width + 7u) / 8u, (params.height + 7u) / 8u, params.depth);
	}
}

TileManager::Result TileManager::Detile(BufferHandle tiled, uint64_t tiled_offset,
                                        uint64_t tiled_capacity, uint64_t linear_capacity,
                                        std::span<const GpuTileInfo> infos) {
	const auto dispatches = Prepare(false, tiled_capacity, linear_capacity, infos,
	                                tiled_offset % RootAlignment, 0);
	const auto linear = GetScratchBuffer(linear_capacity);
	Record(tiled, tiled_offset, linear.buffer, 0, linear.size, dispatches, true);
	return {linear.buffer, 0, linear_capacity};
}

void TileManager::Tile(BufferHandle linear, uint64_t linear_offset, uint64_t linear_capacity,
                       BufferHandle tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                       std::span<const GpuTileInfo> infos) {
	const auto dispatches = Prepare(true, tiled_capacity, linear_capacity, infos,
	                                linear_offset % RootAlignment, tiled_offset % RootAlignment);
	Record(linear, linear_offset, tiled, tiled_offset, tiled_capacity, dispatches, false);
}

void TileManager::TileImage(Image& image, std::span<const vk::BufferImageCopy> regions,
                            BufferHandle tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                            uint64_t linear_capacity, std::span<const GpuTileInfo> infos,
                            ColorTransform transform) {
	EXIT_IF(regions.empty());
	// Reserve the stream parameters first: StreamBuffer::Map may submit the current tick.
	const auto dispatches =
	    Prepare(true, tiled_capacity, linear_capacity, infos, 0, tiled_offset % RootAlignment);
	auto linear = GetScratchBuffer(linear_capacity);
	image.Download(regions, linear.buffer, 0, linear.size);
	if (transform == ColorTransform::SwapBgra16) {
		linear = SwapBgra16(linear);
	}
	Record(linear.buffer, linear.offset, tiled, tiled_offset, tiled_capacity, dispatches, false);
}

void TileManager::ConvertD16(Result source, Result target, D16Direction direction, bool d32,
                             const D16Layout& layout) {
	EXIT_IF(layout.width == 0 || layout.height == 0 || layout.layers == 0 ||
	        layout.height > MaxGroups[1] || layout.source_row_stride > UINT32_MAX ||
	        layout.target_row_stride > UINT32_MAX);
	auto& command = m_scheduler.Current();
	auto& kernels = command.GetContext().GetComputeKernels();
	const D3D12::SpecializationConstant constant[] = {{0, d32 ? 1u : 0u}};
	auto* pipeline = kernels.Get(direction == D16Direction::Promote ? TileShaders::PromoteD16()
	                                                                : TileShaders::DemoteD16(),
	                             constant);
	for (uint32_t layer = 0; layer < layout.layers; layer++) {
		const auto source_offset = source.offset + layout.source_slice_stride * layer;
		const auto target_offset = target.offset + layout.target_slice_stride * layer;
		TileShaderParams params {};
		params.src_base    = static_cast<uint32_t>(source_offset % RootAlignment);
		params.dst_base    = static_cast<uint32_t>(target_offset % RootAlignment);
		params.width       = layout.width;
		params.height      = layout.height;
		params.pitch_bytes = static_cast<uint32_t>(layout.source_row_stride);
		params.slice_bytes = static_cast<uint32_t>(layout.target_row_stride);
		ComputeKernels::Bindings bindings;
		bindings.input         = source.buffer;
		bindings.input_offset  = source_offset - params.src_base;
		bindings.output        = target.buffer;
		bindings.output_offset = target_offset - params.dst_base;
		bindings.constants = {reinterpret_cast<const uint32_t*>(&params), sizeof(params) / 4};
		kernels.Dispatch(command, pipeline, bindings, (layout.width + 63u) / 64u, layout.height, 1);
	}
}

void TileManager::SwapBgra16(Result input, Result output) {
	EXIT_NOT_IMPLEMENTED(input.size == 0 || input.size % 8u != 0 || input.size / 8u > UINT32_MAX ||
	                     output.size < input.size);
	const auto pixels = static_cast<uint32_t>(input.size / 8u);
	auto&      command = m_scheduler.Current();
	auto&      kernels = command.GetContext().GetComputeKernels();
	TileShaderParams params {};
	params.src_base = static_cast<uint32_t>(input.offset % RootAlignment);
	params.dst_base = static_cast<uint32_t>(output.offset % RootAlignment);
	params.width    = pixels;
	ComputeKernels::Bindings bindings;
	bindings.input         = input.buffer;
	bindings.input_offset  = input.offset - params.src_base;
	bindings.output        = output.buffer;
	bindings.output_offset = output.offset - params.dst_base;
	bindings.constants     = {reinterpret_cast<const uint32_t*>(&params), sizeof(params) / 4};
	kernels.Dispatch(command, kernels.Get(TileShaders::SwapBgra16()), bindings,
	                 (pixels + 63u) / 64u, 1, 1);
}

TileManager::Result TileManager::SwapBgra16(Result input) {
	auto output = GetScratchBuffer(input.size);
	SwapBgra16(input, output);
	return {output.buffer, 0, input.size};
}

} // namespace Libs::Graphics
