#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_

#include "graphics/host_gpu/renderArgs.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/shader/shader.h"

#if defined(KYTY_GPU_BACKEND_D3D12)
#include "graphics/host_gpu/d3d12/pipelineCache.h"
#else
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#endif

#include <array>
#include <cstdint>
#include <utility>

// Draw state shared by the host GPU backends: renderer/renderDraw.cpp prepares a draw, each
// backend's RenderExecutor::ExecutePreparedDraw records it.

namespace Libs::Graphics {

class CommandBuffer;

[[nodiscard]] std::pair<int32_t, uint32_t>
ResolveDrawOffsets(uint32_t index_offset, const ShaderVertexInputInfo& vs_input_info);

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
};

struct DrawCallInfo {
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;

	[[nodiscard]] bool IsIndexed() const { return debug_op == CommandBufferDebugOp::DrawIndex; }
	[[nodiscard]] const char* Name() const { return IsIndexed() ? "DrawIndex" : "DrawIndexAuto"; }
};

struct DrawEmitInfo {
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	BufferHandle  buffer {};
	uint64_t      offset = 0;
	vk::IndexType type   = vk::IndexType::eUint16;
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<BufferHandle, MaxBuffers> buffers {};
	std::array<uint64_t, MaxBuffers>     offsets {};
	std::array<uint64_t, MaxBuffers>     sizes {};
	uint32_t                             count = 0;
};

// Host buffers holding the guest vertex buffers of the draw's vertex shader.
[[nodiscard]] PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                         const ShaderVertexInputInfo& vs_input_info);
[[nodiscard]] PreparedIndexBuffer   PrepareIndexBuffer(CommandBuffer&               buffer,
                                                       const DrawIndexBufferSource& source);
void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                       uint32_t phase);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
