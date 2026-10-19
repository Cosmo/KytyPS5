#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_WORKCOUNTERS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_WORKCOUNTERS_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {

// What the UWP app's overlay shows of the renderer's work: totals since the start, written by the renderer and read by the app (the draws made are
// upstream's counter, common/debugCounters.h).
struct WorkCounters {
	// Draws that would draw something but are not supported here, and the reason of the last one.
	std::atomic<uint64_t>    skipped_draws {0};
	std::atomic<const char*> skip_reason {nullptr};
	// The GPU's time for the guest's command lists (D3D12: timestamps at their start and end).
	std::atomic<uint64_t> gpu_ns {0};
};

inline WorkCounters& GetWorkCounters() {
	static WorkCounters counters;
	return counters;
}

inline void CountSkippedDraw(const char* reason) {
	auto& counters = GetWorkCounters();
	counters.skipped_draws.fetch_add(1, std::memory_order_relaxed);
	counters.skip_reason.store(reason, std::memory_order_relaxed);
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_WORKCOUNTERS_H_
