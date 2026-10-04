#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/drawState.h"

#include <atomic>
#include <cinttypes>

namespace Libs::Graphics {

// Milestone 2: guest work is translated to DXIL and compute pipelines are created; nothing is
// recorded or executed yet.

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid());
	m_statistics.dispatches++;
	auto&       shaders = buffer.GetShaders();
	const auto& cs      = shaders.GetCs();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    cs.cs_regs.data_addr);
	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0 ||
	    cs.cs_regs.data_addr == 0) {
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	Common::LockGuard  lock(m_context.GetMutex());
	ShaderComputeInputInfo input_info {};
	input_info.dispatch_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	auto&       cache  = m_context.GetPipelineCache();
	const auto& shader = cache.GetComputeProgram(cs, buffer.GetRegisters().GetShaderRegisters(),
	                                             input_info);
	(void)cache.GetComputePipeline(input_info, shader);
}

static void PrepareGraphicsPrograms(RenderContext& context, CommandBuffer& buffer) {
	const auto& registers = buffer.GetRegisters();
	const auto& shaders   = buffer.GetShaders();
	if (!DrawHasValidVertexShader(shaders)) {
		return;
	}
	const bool pixel_active  = DrawHasActivePixelShader(registers, shaders);
	const auto export_mapping = RenderTargetExportMapping(registers);

	Common::LockGuard                    lock(context.GetMutex());
	std::array<ShaderVertexInputInfo, 3> vertex_info {};
	ShaderPixelInputInfo                 pixel_info {};
	(void)context.GetPipelineCache().GetGraphicsPrograms(
	    shaders.GetVs(), shaders.GetPs(), registers.GetShaderRegisters(), registers,
	    buffer.GetUserConfig(), export_mapping, pixel_active, vertex_info, pixel_info);
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr, uint32_t /*mode*/) {
	// The group counts are read from guest memory when a dispatch is recorded (a later milestone); until then the shader is not translated.
	m_statistics.dispatches++;
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id, 0, 0, 0, 0, args_addr);
}

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	m_statistics.draws++;
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, args.instance_count);
	PrepareGraphicsPrograms(m_context, buffer);
}

void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	m_statistics.draws++;
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, args.instance_count);
	PrepareGraphicsPrograms(m_context, buffer);
}

} // namespace Libs::Graphics
