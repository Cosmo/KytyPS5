#include "graphics/host_gpu/d3d12/gpuTrace.h"

#include "common/assert.h"
#include "common/logging/log.h"

#include <chrono>
#include <fmt/format.h>

namespace Libs::Graphics::D3D12 {

GpuTrace& GetGpuTrace() {
	static GpuTrace trace;
	return trace;
}

void GpuTrace::Create(ID3D12Device* device) {
	EXIT_IF(m_buffer != nullptr);
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = SLOTS * sizeof(uint32_t);
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.SampleDesc       = {1, 0};
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
	                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                      IID_PPV_ARGS(&m_buffer)),
	      "create GPU trace buffer");
	// Mapped for good: after a device removal, Map fails but the memory stays readable.
	void* values = nullptr;
	Check(m_buffer->Map(0, nullptr, &values), "map GPU trace buffer");
	m_values = static_cast<const volatile uint32_t*>(values);
	m_pipelines = std::make_unique<std::atomic<const TracedPipeline*>[]>(SLOTS);
}

void GpuTrace::Destroy() {
	StopWatchdog();
	std::lock_guard lock(m_lists_mutex);
	m_lists.clear();
	if (m_buffer != nullptr) {
		m_buffer->Release();
		m_buffer = nullptr;
		m_values = nullptr;
	}
}

uint32_t GpuTrace::Begin(ID3D12GraphicsCommandList* list, const TracedPipeline* pipeline) {
	const uint32_t slot = m_next.fetch_add(1, std::memory_order_relaxed) % SLOTS;
	m_pipelines[slot].store(pipeline, std::memory_order_relaxed);
	Write(list, slot, STARTED, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN);
	return slot;
}

void GpuTrace::End(ID3D12GraphicsCommandList* list, uint32_t slot) {
	Write(list, slot, FINISHED, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT);
}

void GpuTrace::Write(ID3D12GraphicsCommandList* list, uint32_t slot, uint32_t value,
                     D3D12_WRITEBUFFERIMMEDIATE_MODE mode) {
	if (m_buffer == nullptr) {
		return;
	}
	ID3D12GraphicsCommandList2* list2 = nullptr;
	{
		std::lock_guard lock(m_lists_mutex);
		auto& found = m_lists[list];
		if (found == nullptr) {
			Check(list->QueryInterface(IID_PPV_ARGS(&found)), "query ID3D12GraphicsCommandList2");
		}
		list2 = found.Get();
	}
	const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter {
	    m_buffer->GetGPUVirtualAddress() + slot * sizeof(uint32_t), value};
	list2->WriteBufferImmediate(1, &parameter, &mode);
}

std::vector<const TracedPipeline*> GpuTrace::Unfinished(size_t max_count) const {
	std::vector<const TracedPipeline*> result;
	if (m_values == nullptr) {
		return result;
	}
	const uint32_t next = m_next.load(std::memory_order_relaxed);
	for (uint32_t age = SLOTS; age > 0 && result.size() < max_count; age--) {
		const uint32_t slot = (next - age) % SLOTS;
		const auto*    pipeline = m_pipelines[slot].load(std::memory_order_relaxed);
		if (m_values[slot] == STARTED && pipeline != nullptr) {
			result.push_back(pipeline);
		}
	}
	return result;
}

bool GpuTrace::OldestUnfinished(uint32_t& serial, const TracedPipeline*& pipeline) const {
	if (m_values == nullptr) {
		return false;
	}
	const uint32_t next = m_next.load(std::memory_order_relaxed);
	for (uint32_t age = SLOTS; age > 0; age--) {
		const uint32_t slot   = (next - age) % SLOTS;
		const auto*    traced = m_pipelines[slot].load(std::memory_order_relaxed);
		if (m_values[slot] == STARTED && traced != nullptr) {
			serial   = next - age;
			pipeline = traced;
			return true;
		}
	}
	return false;
}

void GpuTrace::Watch(const TracedPipeline* pipeline) {
	std::lock_guard lock(m_watched_mutex);
	m_watched.push_back(pipeline);
}

void GpuTrace::StartWatchdog(ID3D12Device* device, uint32_t hang_seconds) {
	StopWatchdog();
	m_watchdog_stop = false;
	m_watchdog      = std::thread([this, device, hang_seconds] { WatchdogLoop(device, hang_seconds); });
}

void GpuTrace::StopWatchdog() {
	m_watchdog_stop = true;
	if (m_watchdog.joinable()) {
		if (m_watchdog.get_id() == std::this_thread::get_id()) {
			m_watchdog.detach(); // ending the process from the watchdog itself
		} else {
			m_watchdog.join();
		}
	}
}

void GpuTrace::WatchdogLoop(ID3D12Device* device, uint32_t hang_seconds) {
	using Clock                 = std::chrono::steady_clock;
	const auto start            = Clock::now();
	const auto since_start      = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };
	bool       have_oldest      = false;
	uint32_t   oldest_serial    = 0;
	auto       oldest_since     = start;
	// Per watched pipeline: the highest of 1, 10, 100, ... finished draws reported so far.
	std::unordered_map<const TracedPipeline*, uint32_t> reported;
	Log::WriteToConsoleAndLog(fmt::format("GPU watchdog: running (a GPU hang ends the emulator after {} s, the device being removed at once)\n", hang_seconds));
	while (!m_watchdog_stop) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (m_values == nullptr) {
			continue;
		}
		if (FAILED(device->GetDeviceRemovedReason())) {
			EXIT("GPU watchdog: the D3D12 device was removed %.1f s after the watchdog started\n%s", since_start(), DeviceRemovedReport().c_str());
		}
		const TracedPipeline* oldest = nullptr;
		uint32_t              serial = 0;
		const bool            found  = OldestUnfinished(serial, oldest);
		if (!found || !have_oldest || serial != oldest_serial) {
			have_oldest   = found;
			oldest_serial = serial;
			oldest_since  = Clock::now();
		} else if (std::chrono::duration<double>(Clock::now() - oldest_since).count() >= hang_seconds) {
			EXIT("GPU watchdog: the GPU has been at the same work for %u s (%.1f s after the watchdog started)\n%s", hang_seconds, since_start(),
			     UnfinishedWorkReport().c_str());
		}
		std::vector<const TracedPipeline*> watched;
		{
			std::lock_guard lock(m_watched_mutex);
			watched = m_watched;
		}
		if (watched.empty()) {
			continue;
		}
		std::unordered_map<const TracedPipeline*, uint32_t> finished;
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			const auto* traced = m_pipelines[slot].load(std::memory_order_relaxed);
			if (traced != nullptr && m_values[slot] == FINISHED) {
				for (const auto* candidate: watched) {
					if (candidate == traced) {
						finished[traced]++;
					}
				}
			}
		}
		for (const auto* traced: watched) {
			const uint32_t count = finished[traced];
			uint32_t       level = 0;
			for (uint32_t step = 1; step <= count && step < 100000; step *= 10) {
				level = step;
			}
			if (level > reported[traced]) {
				reported[traced] = level;
				const std::string name(traced->name.begin(), traced->name.end());
				Log::WriteToConsoleAndLog(fmt::format("GPU watchdog: {} (overridden shaders): {} draws finished ({:.1f} s after the watchdog started)\n", name, count, since_start()));
			}
		}
	}
}

} // namespace Libs::Graphics::D3D12
