#include "graphics/host_gpu/d3d12/bdaPageTable.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <vector>

namespace Libs::Graphics {

namespace {

// fault_buffer_process.comp collects at most this many faulting pages per pass.
constexpr size_t MaxPageFaults     = 1024;
constexpr size_t PageFaultAreaSize = MaxPageFaults * sizeof(uint64_t);

// Root parameters of the fault kernel: its two storage buffers and spirv_to_dxil's runtime data.
enum FaultRootParameter : UINT { FaultBits, FaultList, FaultRuntimeData, FaultParameterCount };

} // namespace

BdaPageTable::BdaPageTable(GraphicContext& graphics, CommandScheduler& scheduler,
                           StreamBuffer& staging, BufferCache& buffer_cache, uint64_t page_count)
    : m_graphics(graphics), m_scheduler(scheduler), m_staging(staging),
      m_buffer_cache(buffer_cache),
      m_page_table(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                   BdaLayout::TABLE_ELEMENTS * sizeof(uint64_t)),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, page_count / 8),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0,
                        MaxPendingFaults * PageFaultAreaSize) {
	m_page_table.SetName("BDA Page Table Buffer");
	m_fault_buffer.SetName("Fault Buffer");
}

BdaPageTable::~BdaPageTable() {
	if (m_fault_pipeline != nullptr) {
		m_fault_pipeline->Release();
	}
	if (m_fault_root != nullptr) {
		m_fault_root->Release();
	}
}

void BdaPageTable::EnsureCleared() {
	if (m_cleared) {
		return;
	}
	m_page_table.Fill(0, m_page_table.Size(), 0);
	m_fault_buffer.Fill(0, m_fault_buffer.Size(), 0);
	m_cleared = true;
}

Buffer* BdaPageTable::PageTableBuffer() {
	EnsureCleared();
	return &m_page_table;
}

Buffer* BdaPageTable::FaultBuffer() {
	EnsureCleared();
	return &m_fault_buffer;
}

void BdaPageTable::Write(uint64_t offset, const void* data, uint64_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging.Size());
		const auto source = m_staging.Copy(bytes, chunk, 4);
		m_page_table.CopyFrom(m_scheduler.Current(), m_staging, source, offset, chunk);
		bytes += chunk;
		offset += chunk;
		size -= chunk;
	}
}

void BdaPageTable::Map(uint64_t first_page, uint64_t page_count, uint32_t page_bits,
                       const Buffer& buffer) {
	EnsureCleared();
	const uint64_t        index = buffer.BindlessIndex();
	std::vector<uint64_t> entries;
	for (uint64_t i = 0; i < page_count;) {
		const uint64_t page  = first_page + i;
		const uint64_t count = std::min(page_count - i, BdaLayout::PagesLeftInBlock(page));
		uint64_t       directory_value = 0;
		if (!m_blocks.Allocate(page, &directory_value)) {
			EXIT("BDA page table: all %" PRIu64 " slots in use\n", BdaLayout::SLOT_COUNT);
		}
		if (directory_value != 0) {
			Write((page >> BdaLayout::BLOCK_BITS) * sizeof(uint64_t), &directory_value,
			      sizeof(directory_value));
			if (std::has_single_bit(m_blocks.Used())) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "BDA page table: {} blocks of guest addresses in use ({} MB table)\n",
				    m_blocks.Used(), m_page_table.Size() >> 20u));
			}
		}
		entries.clear();
		for (uint64_t j = i; j < i + count; ++j) {
			const uint64_t offset = j << page_bits;
			EXIT_IF(offset > std::numeric_limits<uint32_t>::max());
			entries.push_back((index << 32u) | offset);
		}
		Write(m_blocks.EntryElement(page) * sizeof(uint64_t), entries.data(),
		      entries.size() * sizeof(uint64_t));
		i += count;
	}
}

void BdaPageTable::Unmap(uint64_t first_page, uint64_t page_count) {
	EnsureCleared();
	for (uint64_t i = 0; i < page_count;) {
		const uint64_t page  = first_page + i;
		const uint64_t count = std::min(page_count - i, BdaLayout::PagesLeftInBlock(page));
		// Blocks without a slot read the zero slot, which is never written.
		if (const auto element = m_blocks.EntryElement(page); element != 0) {
			m_page_table.Fill(element * sizeof(uint64_t), count * sizeof(uint64_t), 0);
		}
		i += count;
	}
}

void BdaPageTable::ProcessFaults() {
	KYTY_PROFILER_FUNCTION();
	EnsureCleared();
	auto* device = m_graphics.device;
	if (m_fault_pipeline == nullptr) {
		D3D12_ROOT_PARAMETER1 parameters[FaultParameterCount] {};
		parameters[FaultBits].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
		parameters[FaultBits].Descriptor.ShaderRegister = 0;
		parameters[FaultBits].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
		parameters[FaultList].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
		parameters[FaultList].Descriptor.ShaderRegister = 1;
		parameters[FaultList].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
		parameters[FaultRuntimeData].ParameterType      = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[FaultRuntimeData].Constants.Num32BitValues = D3D12::RuntimeDataDwords;
		parameters[FaultRuntimeData].Constants.RegisterSpace  = D3D12::RuntimeDataSpace;
		D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
		desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
		desc.Desc_1_1.NumParameters = FaultParameterCount;
		desc.Desc_1_1.pParameters   = parameters;
		D3D12::ComPtr<ID3DBlob> blob;
		D3D12::ComPtr<ID3DBlob> error;
		if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
			EXIT("D3D12: fault kernel root signature serialization failed: %s\n",
			     error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
		}
		D3D12::Check(device->CreateRootSignature(0, blob->GetBufferPointer(),
		                                         blob->GetBufferSize(),
		                                         IID_PPV_ARGS(&m_fault_root)),
		             "create fault kernel root signature");
		const auto shader = m_scheduler.Context().GetPipelineCache().GetCompiler().Compile(
		    FAULT_BUFFER_PROCESS_SPV, ShaderType::Compute, 0, false);
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
		pipeline.pRootSignature     = m_fault_root;
		pipeline.CS.pShaderBytecode = shader.bytecode.data();
		pipeline.CS.BytecodeLength  = shader.bytecode.size();
		D3D12::Check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_fault_pipeline)),
		             "create fault kernel pipeline");
	}

	if (const auto wait_tick = m_fault_areas[m_current_area]; wait_tick != 0) {
		m_scheduler.Wait(wait_tick);
		m_scheduler.PopPendingOperations();
	}
	const auto offset = m_current_area * PageFaultAreaSize;
	auto*      mapped = m_download_buffer.Mapped().data() + offset;
	std::memset(mapped, 0, PageFaultAreaSize);

	auto& command = m_scheduler.Current();
	auto* list    = command.Handle();
	m_fault_buffer.Use(command, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	m_download_buffer.Use(command, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	// Shader writes to the fault bits happen before this pass reads and clears them.
	command.GlobalMemoryBarrier();
	const auto     threads = BufferCache::CACHING_NUMPAGES / 32;
	const auto     groups  = static_cast<uint32_t>((threads + 63) / 64);
	const uint32_t runtime[D3D12::RuntimeDataDwords] = {groups, 1, 1};
	list->SetComputeRootSignature(m_fault_root);
	list->SetPipelineState(m_fault_pipeline);
	list->SetComputeRootUnorderedAccessView(FaultBits, m_fault_buffer.GpuAddress());
	list->SetComputeRootUnorderedAccessView(FaultList, m_download_buffer.GpuAddress() + offset);
	list->SetComputeRoot32BitConstants(FaultRuntimeData, D3D12::RuntimeDataDwords, runtime, 0);
	list->Dispatch(groups, 1, 1);
	command.GlobalMemoryBarrier();

	const auto area = m_current_area;
	m_scheduler.DeferOperation([this, mapped, area] {
		RangeSet    fault_ranges;
		const auto* faults = std::bit_cast<const uint64_t*>(mapped);
		const auto  count  = static_cast<uint32_t>(faults[0]);
		for (uint32_t index = 1; index <= count && index < MaxPageFaults; ++index) {
			fault_ranges.Add(faults[index], BufferCache::CACHING_PAGESIZE);
			LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 "\n", faults[index]);
		}
		fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
			EXIT_IF(end - start > std::numeric_limits<uint32_t>::max());
			(void)m_buffer_cache.FindBuffer(start, end - start);
		});
		m_fault_areas[area] = 0;
	});
	m_fault_areas[m_current_area++] = m_scheduler.CurrentTick();
	m_current_area %= MaxPendingFaults;
}

} // namespace Libs::Graphics
