#include "graphics/host_gpu/d3d12/render.h"

namespace Libs::Graphics {

// Guest work is accepted and counted; nothing is rendered yet.

void RenderExecutor::DispatchDirect(uint64_t /*submit_id*/, CommandBuffer& /*buffer*/, uint32_t /*thread_group_x*/, uint32_t /*thread_group_y*/,
                                    uint32_t /*thread_group_z*/, uint32_t /*mode*/) {
	m_statistics.dispatches++;
}

void RenderExecutor::DispatchIndirect(uint64_t /*submit_id*/, CommandBuffer& /*buffer*/, uint64_t /*args_addr*/, uint32_t /*mode*/) {
	m_statistics.dispatches++;
}

void RenderExecutor::DrawIndex(uint64_t /*submit_id*/, CommandBuffer& /*buffer*/, const DrawIndexArgs& /*args*/) {
	m_statistics.draws++;
}

void RenderExecutor::DrawAuto(uint64_t /*submit_id*/, CommandBuffer& /*buffer*/, const DrawAutoArgs& /*args*/) {
	m_statistics.draws++;
}

} // namespace Libs::Graphics
