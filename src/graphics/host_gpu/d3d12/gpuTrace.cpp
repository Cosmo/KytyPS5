#include "graphics/host_gpu/d3d12/gpuTrace.h"

#include "common/assert.h"

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

} // namespace Libs::Graphics::D3D12
