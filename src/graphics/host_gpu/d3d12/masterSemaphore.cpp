#include "graphics/host_gpu/d3d12/masterSemaphore.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include <cinttypes>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics) {
	D3D12::Check(graphics.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)),
	             "CreateFence");
}

MasterSemaphore::~MasterSemaphore() {
	if (m_fence != nullptr) {
		m_fence->Release();
	}
}

void MasterSemaphore::Refresh() {
	const auto counter = m_fence->GetCompletedValue();
	if (counter == UINT64_MAX) {
		EXIT("D3D12 device removed while reading the GPU timeline\n%s",
		     D3D12::DeviceRemovedReport().c_str());
	}
	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}
	KYTY_PROFILER_BLOCK("MasterSemaphore::Wait");
	// A null event blocks until the fence reaches the value.
	D3D12::Check(m_fence->SetEventOnCompletion(tick, nullptr), "wait for GPU timeline");
	Refresh();
	EXIT_IF(!IsFree(tick));
}

} // namespace Libs::Graphics
