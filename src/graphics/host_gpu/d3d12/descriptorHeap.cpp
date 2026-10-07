#include "graphics/host_gpu/d3d12/descriptorHeap.h"

#include "common/assert.h"
#include "graphics/host_gpu/d3d12/commandScheduler.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include <algorithm>

namespace Libs::Graphics {

namespace {

constexpr uint32_t ViewCapacity    = 1u << 19;
constexpr uint32_t SamplerCapacity = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
constexpr uint32_t CpuPageSize     = 1024;

} // namespace

DescriptorHeap::Ring::Ring(GraphicContext& graphics, D3D12_DESCRIPTOR_HEAP_TYPE type,
                           uint32_t capacity, uint32_t reserved)
    : m_capacity(capacity), m_first(reserved), m_head(reserved) {
	D3D12_DESCRIPTOR_HEAP_DESC desc {};
	desc.Type           = type;
	desc.NumDescriptors = capacity;
	desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	D3D12::Check(graphics.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)),
	             "create shader-visible descriptor heap");
	m_base.cpu       = m_heap->GetCPUDescriptorHandleForHeapStart();
	m_base.gpu       = m_heap->GetGPUDescriptorHandleForHeapStart();
	m_base.increment = graphics.device->GetDescriptorHandleIncrementSize(type);
}

DescriptorHeap::Ring::~Ring() {
	if (m_heap != nullptr) {
		m_heap->Release();
	}
}

DescriptorRange DescriptorHeap::Ring::Allocate(CommandScheduler& scheduler, uint32_t count) {
	EXIT_IF(count == 0 || count > m_capacity - m_first);
	const auto tick = scheduler.CurrentTick();
	for (;;) {
		while (!m_uses.empty() && m_uses.front().tick != tick &&
		       scheduler.IsFree(m_uses.front().tick)) {
			m_uses.pop_front();
		}
		const uint32_t begin    = m_head + count <= m_capacity ? m_head : m_first;
		const bool     overlaps = std::any_of(m_uses.begin(), m_uses.end(), [&](const Use& use) {
            return begin < use.end && use.begin < begin + count;
        });
		if (!overlaps) {
			if (!m_uses.empty() && m_uses.back().tick == tick && m_uses.back().end == begin) {
				m_uses.back().end = begin + count;
			} else {
				m_uses.push_back({tick, begin, begin + count});
			}
			m_head = begin + count;
			return {m_base.Cpu(begin), m_base.Gpu(begin), m_base.increment};
		}
		if (m_uses.front().tick == tick) {
			EXIT("D3D12: one command list needs more than %u descriptors\n", m_capacity - m_first);
		}
		scheduler.Wait(m_uses.front().tick);
	}
}

DescriptorHeap::CpuPool::~CpuPool() {
	for (auto* heap: m_heaps) {
		heap->Release();
	}
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CpuPool::Allocate(ID3D12Device*              device,
                                                              D3D12_DESCRIPTOR_HEAP_TYPE type) {
	if (m_free.empty()) {
		D3D12_DESCRIPTOR_HEAP_DESC desc {};
		desc.Type           = type;
		desc.NumDescriptors = CpuPageSize;
		ID3D12DescriptorHeap* heap = nullptr;
		D3D12::Check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)),
		             "create CPU descriptor heap");
		m_heaps.push_back(heap);
		const auto start     = heap->GetCPUDescriptorHandleForHeapStart();
		const auto increment = device->GetDescriptorHandleIncrementSize(type);
		for (uint32_t i = CpuPageSize; i > 0; i--) {
			m_free.push_back({start.ptr + static_cast<SIZE_T>(i - 1) * increment});
		}
	}
	const auto handle = m_free.back();
	m_free.pop_back();
	return handle;
}

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_views(graphics, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ViewCapacity, BindlessViews),
      m_samplers(graphics, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SamplerCapacity) {
	D3D12_DESCRIPTOR_HEAP_DESC desc {};
	desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = 1;
	D3D12::Check(graphics.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_scratch)),
	             "create scratch descriptor heap");

	D3D12_UNORDERED_ACCESS_VIEW_DESC null_view {};
	null_view.Format             = DXGI_FORMAT_R32_TYPELESS;
	null_view.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
	null_view.Buffer.NumElements = 1;
	null_view.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
	graphics.device->CreateUnorderedAccessView(nullptr, nullptr, &null_view, BindlessView(0));
}

uint32_t DescriptorHeap::AllocateBindlessView() {
	Common::LockGuard lock(m_bindless_mutex);
	if (!m_bindless_freed.empty() && m_scheduler.IsFree(m_bindless_freed.front().tick)) {
		const auto index = m_bindless_freed.front().index;
		m_bindless_freed.pop_front();
		return index;
	}
	if (m_bindless_next == BindlessViews) {
		EXIT("D3D12: more than %u buffers are addressable by shaders\n", BindlessViews - 1);
	}
	return m_bindless_next++;
}

void DescriptorHeap::FreeBindlessView(uint32_t index) {
	EXIT_IF(index == 0 || index >= BindlessViews);
	Common::LockGuard lock(m_bindless_mutex);
	// Work recorded until now may still read the slot.
	m_bindless_freed.push_back({index, m_scheduler.CurrentTick()});
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::BindlessView(uint32_t index) const noexcept {
	return m_views.Base().Cpu(index);
}

DescriptorHeap::~DescriptorHeap() {
	if (m_scratch != nullptr) {
		m_scratch->Release();
	}
}

void DescriptorHeap::Bind(CommandBuffer& command) {
	const auto tick = m_scheduler.CurrentTick();
	if (m_bound_tick == tick) {
		return;
	}
	ID3D12DescriptorHeap* heaps[] = {m_views.Heap(), m_samplers.Heap()};
	command.Handle()->SetDescriptorHeaps(2, heaps);
	m_bound_tick = tick;
}

DescriptorRange DescriptorHeap::AllocateViews(CommandBuffer& command, uint32_t count) {
	Bind(command);
	return m_views.Allocate(m_scheduler, count);
}

DescriptorRange DescriptorHeap::AllocateSamplers(CommandBuffer& command, uint32_t count) {
	Bind(command);
	return m_samplers.Allocate(m_scheduler, count);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::ScratchView() const noexcept {
	return m_scratch->GetCPUDescriptorHandleForHeapStart();
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::AllocateCpu(D3D12_DESCRIPTOR_HEAP_TYPE type) {
	Common::LockGuard lock(m_cpu_mutex);
	return m_cpu_pools[type].Allocate(m_graphics.device, type);
}

void DescriptorHeap::FreeCpu(D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
	Common::LockGuard lock(m_cpu_mutex);
	m_cpu_pools[type].Free(handle);
}

} // namespace Libs::Graphics
