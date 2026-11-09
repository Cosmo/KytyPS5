#ifndef EMULATOR_SRC_COMMON_PIPELINEPREWARM_H_
#define EMULATOR_SRC_COMMON_PIPELINEPREWARM_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// The game menu's "Compile pipelines" and the library's "Play and build known pipelines". The Xbox's driver keeps no compiled pipelines across runs, so a game compiles each one again
// when it first needs it, which stalls the game. This asks the D3D12 pipeline cache to build the pipelines earlier runs described, in the background
// (PipelineCache::StartPrewarm); the menu follows it through GetStatus.
namespace Common::PipelinePrewarm {

enum class State : uint32_t { Idle, Running, Done, NothingKnown, AllReady };

namespace Detail {
inline std::atomic_bool before_start {false};
// Set by the D3D12 pipeline cache while it exists: builds the described pipelines `workers` at a time and returns when they are built.
inline std::function<void(uint32_t)> build_and_wait;
inline std::atomic_bool      requested {false};
inline std::atomic<uint32_t> state {0};
inline std::atomic<uint32_t> total {0};
inline std::atomic<uint32_t> built {0};
inline std::atomic<uint32_t> failed {0};
} // namespace Detail

inline void Request() {
	Detail::requested = true;
}

// The library's "Build known pipelines, then play": the game starts once the pipelines earlier runs described are built.
inline void RequestBeforeStart() {
	Detail::before_start = true;
}

// The game's thread, before the game's code runs: with the request, builds the described pipelines `workers` at a time and returns when
// they are built (nothing goes on in the game meanwhile, so more run at once than while it plays). Without the request, or a builder, nothing.
inline void BuildBeforeStart(uint32_t workers) {
	if (Detail::before_start.exchange(false) && Detail::build_and_wait) {
		Detail::build_and_wait(workers);
	}
}

[[nodiscard]] inline bool TakeRequest() {
	return Detail::requested.load(std::memory_order_relaxed) && Detail::requested.exchange(false);
}

inline void SetState(State state) {
	Detail::state = static_cast<uint32_t>(state);
}

// A line for the menu; it starts with "Compiling" while the work goes on.
[[nodiscard]] inline std::string GetStatus() {
	const auto built  = std::to_string(Detail::built.load());
	const auto total  = std::to_string(Detail::total.load());
	const auto failed = Detail::failed.load();
	switch (static_cast<State>(Detail::state.load())) {
		case State::Running: return "Compiling pipelines: " + built + " of " + total;
		case State::Done: return "Pipelines compiled: " + built + (failed != 0 ? " (" + std::to_string(failed) + " failed)" : "");
		case State::NothingKnown: return "No known pipelines yet: play this game once first";
		case State::AllReady: return "All known pipelines are ready";
		case State::Idle: break;
	}
	return "Compiling pipelines...";
}

} // namespace Common::PipelinePrewarm

#endif // EMULATOR_SRC_COMMON_PIPELINEPREWARM_H_
