#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STREAMBUFFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STREAMBUFFER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/memoryUsage.h"

#if defined(KYTY_GPU_BACKEND_D3D12)
#include "graphics/host_gpu/d3d12/buffer.h"
#else
#include "graphics/host_gpu/renderer/cache/buffer.h"
#endif

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
struct GraphicContext;

// A ring of CPU-visible memory. Each Map/Commit reservation is reusable once the GPU completed
// the tick that committed it.
class StreamBuffer final: public Buffer {
public:
	StreamBuffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
	             uint64_t size);

	[[nodiscard]] std::pair<uint8_t*, uint64_t> Map(uint64_t size, uint64_t alignment = 0,
	                                                bool allow_wait = true);
	void                                        Commit();
	[[nodiscard]] uint64_t Copy(const void* source, uint64_t size, uint64_t alignment = 0);

private:
	friend struct StreamBufferTestAccess;

	struct Watch {
		uint64_t tick        = 0;
		uint64_t upper_bound = 0;
	};

	[[nodiscard]] static bool NormalizeReservation(bool coherent, uint64_t atom, uint64_t& size,
	                                               uint64_t& alignment);
	[[nodiscard]] bool        WaitPendingOperations(const std::vector<Watch>& watches,
	                                                std::optional<size_t>     invalidation_mark,
	                                                uint64_t requested_upper_bound, bool allow_wait,
	                                                size_t& wait_cursor, uint64_t& wait_bound);

	uint64_t              m_offset      = 0;
	uint64_t              m_mapped_size = 0;
	std::vector<Watch>    m_current_watches;
	size_t                m_current_watch_cursor = 0;
	std::optional<size_t> m_invalidation_mark;
	std::vector<Watch>    m_previous_watches;
	size_t                m_wait_cursor = 0;
	uint64_t              m_wait_bound  = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STREAMBUFFER_H_
