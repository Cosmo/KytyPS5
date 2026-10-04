#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_MASTERSEMAPHORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_MASTERSEMAPHORE_H_

#include "common/common.h"

#include <atomic>
#include <cstdint>

struct ID3D12Fence;

namespace Libs::Graphics {

struct GraphicContext;

// GPU timeline: every queue submission signals the next tick on one fence.
class MasterSemaphore {
public:
	explicit MasterSemaphore(GraphicContext& graphics);
	~MasterSemaphore();
	KYTY_CLASS_NO_COPY(MasterSemaphore);

	[[nodiscard]] uint64_t CurrentTick() const noexcept {
		return m_current_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t KnownGpuTick() const noexcept {
		return m_gpu_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool     IsFree(uint64_t tick) const noexcept { return KnownGpuTick() >= tick; }
	[[nodiscard]] uint64_t NextTick() noexcept {
		return m_current_tick.fetch_add(1, std::memory_order_release);
	}
	[[nodiscard]] ID3D12Fence* Handle() const noexcept { return m_fence; }

	void Refresh();
	void Wait(uint64_t tick);

private:
	ID3D12Fence*          m_fence = nullptr;
	std::atomic<uint64_t> m_gpu_tick {0};
	std::atomic<uint64_t> m_current_tick {1};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_MASTERSEMAPHORE_H_
