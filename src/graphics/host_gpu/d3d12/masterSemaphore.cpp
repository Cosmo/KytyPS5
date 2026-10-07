#include "graphics/host_gpu/d3d12/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include <atomic>
#include <cinttypes>
#include <fmt/format.h>

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
	// Waits in steps: GPU work that doesn't finish for seconds is reported, with the pipeline it
	// is stuck in, while it still runs (the Xbox ends a game whose GPU hangs, without a device
	// removal to report).
	HANDLE event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	EXIT_IF(event == nullptr);
	D3D12::Check(m_fence->SetEventOnCompletion(tick, event), "wait for GPU timeline");
	uint32_t seconds = 0;
	while (WaitForSingleObject(event, 5000) == WAIT_TIMEOUT) {
		seconds += 5;
		static std::atomic_bool reported = false;
		if (!reported.exchange(true)) {
			Log::WriteToConsoleAndLog(fmt::format("GPU work not finished after {} s (tick {}, done {}):\n{}",
			                                      seconds, tick, m_fence->GetCompletedValue(),
			                                      D3D12::UnfinishedWorkReport()));
		}
	}
	CloseHandle(event);
	Refresh();
	EXIT_IF(!IsFree(tick));
}

} // namespace Libs::Graphics
