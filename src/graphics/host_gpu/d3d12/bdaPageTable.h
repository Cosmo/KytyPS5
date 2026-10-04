#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BDAPAGETABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BDAPAGETABLE_H_

#include "common/common.h"
#include "graphics/host_gpu/d3d12/buffer.h"

#include <array>
#include <cstdint>

struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace Libs::Graphics {

class BufferCache;
class CommandScheduler;
class StreamBuffer;
struct GraphicContext;

// Lets shaders read guest memory by address. DXIL has no pointers: spirv_to_dxil reads a 64-bit
// "address" as a descriptor heap index (bits 32-55) and a byte offset (bits 0-31) into that raw
// buffer view. Each cached guest page therefore maps to its buffer's bindless descriptor and the
// page's offset in the buffer (0: not cached). Accesses to other pages are recorded in a fault
// buffer and resolved by caching those pages for later work.
class BdaPageTable {
public:
	BdaPageTable(GraphicContext& graphics, CommandScheduler& scheduler, StreamBuffer& staging,
	             BufferCache& buffer_cache, uint64_t page_count);
	~BdaPageTable();
	KYTY_CLASS_NO_COPY(BdaPageTable);

	void Map(uint64_t first_page, uint64_t page_count, uint32_t page_bits, const Buffer& buffer);
	void Unmap(uint64_t first_page, uint64_t page_count);
	void ProcessFaults();

	[[nodiscard]] Buffer* PageTableBuffer();
	[[nodiscard]] Buffer* FaultBuffer();

private:
	static constexpr size_t MaxPendingFaults = 8;

	// New memory is not zeroed; both tables start out empty before their first use.
	void EnsureCleared();

	GraphicContext&                        m_graphics;
	CommandScheduler&                      m_scheduler;
	StreamBuffer&                          m_staging;
	BufferCache&                           m_buffer_cache;
	Buffer                                 m_page_table;
	Buffer                                 m_fault_buffer;
	Buffer                                 m_download_buffer;
	std::array<uint64_t, MaxPendingFaults> m_fault_areas {};
	uint32_t                               m_current_area   = 0;
	bool                                   m_cleared        = false;
	ID3D12RootSignature*                   m_fault_root     = nullptr;
	ID3D12PipelineState*                   m_fault_pipeline = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BDAPAGETABLE_H_
