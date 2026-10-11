#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_WORKCOUNTERS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_WORKCOUNTERS_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Libs::Graphics {

// What the UWP app's overlay shows of the renderer's work: totals since the start, written by the renderer and read by the app.
struct WorkCounters {
	std::atomic<uint64_t> draws {0};
	// Draws that would draw something but are not supported here, and the reason of the last one.
	std::atomic<uint64_t>    skipped_draws {0};
	std::atomic<const char*> skip_reason {nullptr};
	// The GPU's time for the guest's command lists (D3D12: timestamps at their start and end).
	std::atomic<uint64_t> gpu_ns {0};
	// Time spent compiling the game's shaders and making pipelines, which the GPU work waits for.
	std::atomic<uint64_t> compile_ns {0};
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

// Adds its lifetime to WorkCounters::compile_ns; one inside another (on the same thread) adds nothing.
class CompileTimer {
public:
	CompileTimer() { m_outer = Depth()++ == 0; }
	~CompileTimer() {
		Depth()--;
		if (m_outer) {
			const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
			GetWorkCounters().compile_ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
		}
	}
	CompileTimer(const CompileTimer&)            = delete;
	CompileTimer& operator=(const CompileTimer&) = delete;

private:
	static int& Depth() {
		static thread_local int depth = 0;
		return depth;
	}

	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
	bool                                  m_outer = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_WORKCOUNTERS_H_
