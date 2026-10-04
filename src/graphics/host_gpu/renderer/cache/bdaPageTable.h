#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDAPAGETABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDAPAGETABLE_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/bdaLayout.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <cstdint>

namespace Libs::Graphics {

class BufferCache;

// Lets shaders read guest memory by address (buffer device address). Each cached guest page maps
// to the device address of the buffer holding it; accesses to unmapped pages are recorded in a
// fault buffer and resolved by caching those pages for later work. The table is two-level
// (BdaLayout).
class BdaPageTable {
public:
	BdaPageTable(GraphicContext& graphics, CommandScheduler& scheduler, StreamBuffer& staging,
	             BufferCache& buffer_cache, uint64_t page_count);
	KYTY_CLASS_NO_COPY(BdaPageTable);

	void Map(uint64_t first_page, uint64_t page_count, uint32_t page_bits, const Buffer& buffer);
	void Unmap(uint64_t first_page, uint64_t page_count);
	void ProcessFaults() { m_fault_manager.ProcessFaultBuffer(); }

	[[nodiscard]] Buffer* PageTableBuffer() {
		EnsureCleared();
		return &m_page_table;
	}
	[[nodiscard]] Buffer* FaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }

private:
	// New memory is not zeroed; the directory and the zero slot must be before the first use.
	void EnsureCleared();
	// Copies `size` bytes into the table at byte `offset`.
	void Write(uint64_t offset, const void* data, uint64_t size);

	CommandScheduler& m_scheduler;
	StreamBuffer&     m_staging;
	Buffer            m_page_table;
	FaultManager      m_fault_manager;
	BdaBlocks         m_blocks;
	bool              m_cleared = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDAPAGETABLE_H_
