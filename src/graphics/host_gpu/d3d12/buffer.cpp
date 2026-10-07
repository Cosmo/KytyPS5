#include "graphics/host_gpu/d3d12/buffer.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/renderContext.h"

#include <D3D12MemAlloc.h>

#include <algorithm>
#include <cinttypes>
#include <string>

namespace Libs::Graphics {

namespace {

[[nodiscard]] D3D12MA::ALLOCATION_DESC AllocationDesc(const GraphicContext& graphics,
                                                     MemoryUsage           usage) {
	D3D12MA::ALLOCATION_DESC desc {};
	switch (usage) {
		case MemoryUsage::DeviceLocal: desc.HeapType = D3D12_HEAP_TYPE_DEFAULT; return desc;
		case MemoryUsage::Upload:
		case MemoryUsage::Stream: desc.CustomPool = graphics.upload_pool; return desc;
		case MemoryUsage::Download: desc.CustomPool = graphics.readback_pool; return desc;
	}
	EXIT("invalid buffer memory usage\n");
}

[[nodiscard]] bool IsReadState(D3D12_RESOURCE_STATES state) {
	return (state & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0 && state != 0;
}

} // namespace

Buffer::Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
               uint64_t cpu_address, uint64_t size)
    : m_graphics(&graphics), m_scheduler(&scheduler), m_usage(usage), m_cpu_address(cpu_address),
      m_size(size) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(size == 0);

	const auto allocation = AllocationDesc(graphics, usage);

	D3D12_RESOURCE_DESC desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width            = size;
	desc.Height           = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const auto result     = graphics.allocator->CreateResource(
        &allocation, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &m_allocation, IID_NULL, nullptr);
	if (FAILED(result)) {
		graphics.LogMemoryBudget();
		EXIT("D3D12: failed to create a %" PRIu64 "-byte buffer, HRESULT=0x%08" PRIx32 "\n",
		     size, static_cast<uint32_t>(result));
	}
	m_resource    = m_allocation->GetResource();
	m_gpu_address = m_resource->GetGPUVirtualAddress();

	if (usage != MemoryUsage::DeviceLocal) {
		void* data = nullptr;
		D3D12::Check(m_resource->Map(0, nullptr, &data), "map buffer");
		m_mapped = {static_cast<uint8_t*>(data), static_cast<size_t>(size)};
	}
}

Buffer::~Buffer() {
	if (m_bindless_index != 0) {
		Scheduler().Context().GetDescriptorHeap().FreeBindlessView(m_bindless_index);
	}
	if (m_allocation != nullptr) {
		m_allocation->Release();
	}
}

uint32_t Buffer::BindlessIndex() const {
	if (m_bindless_index == 0) {
		auto& heap       = Scheduler().Context().GetDescriptorHeap();
		m_bindless_index = heap.AllocateBindlessView();
		// Raw views reach at most 2^27 dwords; addresses past that read zeros.
		D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
		view.Format             = DXGI_FORMAT_R32_TYPELESS;
		view.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
		view.Buffer.NumElements = static_cast<UINT>(std::min<uint64_t>((m_size + 3) / 4, 1u << 27));
		view.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
		Graphics().device->CreateUnorderedAccessView(m_resource, nullptr, &view,
		                                             heap.BindlessView(m_bindless_index));
	}
	return m_bindless_index;
}

bool Buffer::IsInBounds(uint64_t address, uint64_t size) const noexcept {
	return address >= m_cpu_address && size <= Size() && address - m_cpu_address <= Size() - size;
}

void Buffer::SetNameString(const std::string& name) const {
	const std::wstring wide(name.begin(), name.end());
	m_resource->SetName(wide.c_str());
}

void Buffer::Use(CommandBuffer& command, uint32_t state) const {
	const auto target = static_cast<D3D12_RESOURCE_STATES>(state);
	const auto tick   = Scheduler().CurrentTick();
	if (m_state_tick != tick) {
		// The buffer decayed to COMMON after its last command list and is promoted implicitly.
		m_state_tick = tick;
		m_state      = target;
		return;
	}
	const auto current = static_cast<D3D12_RESOURCE_STATES>(m_state);
	if (current == target || (IsReadState(target) && (current & target) == target)) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource   = m_resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = current;
	barrier.Transition.StateAfter  = IsReadState(current) && IsReadState(target) ? current | target
	                                                                            : target;
	command.Handle()->ResourceBarrier(1, &barrier);
	m_state = barrier.Transition.StateAfter;
}

void Buffer::CopyFrom(CommandBuffer& command, const Buffer& source, uint64_t source_offset,
                      uint64_t destination_offset, uint64_t size) {
	if (size == 0 || source_offset > source.Size() || size > source.Size() - source_offset ||
	    destination_offset > Size() || size > Size() - destination_offset) {
		EXIT("Buffer: invalid copy range\n");
	}
	// A buffer cannot be copy source and destination at once.
	EXIT_NOT_IMPLEMENTED(&source == this);
	source.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	command.Handle()->CopyBufferRegion(m_resource, destination_offset, source.m_resource,
	                                   source_offset, size);
}

void Buffer::UploadRegions(CommandBuffer& command, const Buffer& source,
                           std::span<const vk::BufferCopy> copies) {
	source.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	for (const auto& copy: copies) {
		command.Handle()->CopyBufferRegion(m_resource, copy.dstOffset, source.m_resource,
		                                   copy.srcOffset, copy.size);
	}
}

void Buffer::DownloadRegions(CommandBuffer& command, const Buffer& destination,
                             std::span<const vk::BufferCopy> copies,
                             uint64_t /*destination_offset*/, uint64_t /*destination_size*/) {
	EXIT_IF(destination.Usage() != MemoryUsage::Download);
	Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	destination.Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	for (const auto& copy: copies) {
		command.Handle()->CopyBufferRegion(destination.m_resource, copy.dstOffset, m_resource,
		                                   copy.srcOffset, copy.size);
	}
}

void Buffer::Fill(uint64_t offset, uint64_t size, uint32_t value) const {
	if (((offset | size) & 3u) != 0 || offset > Size() || size > Size() - offset) {
		EXIT("Buffer: fill range must be dword aligned and in bounds\n");
	}
	auto& command = Scheduler().Current();
	Use(command, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
	view.Format              = DXGI_FORMAT_R32_UINT;
	view.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
	view.Buffer.FirstElement = offset / 4;
	view.Buffer.NumElements  = static_cast<UINT>(size / 4);

	auto&      heaps   = Scheduler().Context().GetDescriptorHeap();
	const auto visible = heaps.AllocateViews(command, 1);
	const auto scratch = heaps.ScratchView();
	Graphics().device->CreateUnorderedAccessView(m_resource, nullptr, &view, visible.cpu);
	Graphics().device->CreateUnorderedAccessView(m_resource, nullptr, &view, scratch);
	const UINT values[4] = {value, value, value, value};
	command.Handle()->ClearUnorderedAccessViewUint(visible.gpu, scratch, m_resource, values, 0,
	                                               nullptr);
}

} // namespace Libs::Graphics
