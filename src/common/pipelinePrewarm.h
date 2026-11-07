#ifndef EMULATOR_SRC_COMMON_PIPELINEPREWARM_H_
#define EMULATOR_SRC_COMMON_PIPELINEPREWARM_H_

#include <atomic>
#include <cstdint>
#include <string>

// The game menu's "Compile pipelines" and the library's "Play and build known pipelines". The Xbox's driver keeps no compiled pipelines across runs, so a game compiles each one again
// when it first needs it, which stalls the game. This asks the D3D12 pipeline cache to build the pipelines earlier runs described, in the background
// (PipelineCache::StartPrewarm); the menu follows it through GetStatus.
namespace Common::PipelinePrewarm {

enum class State : uint32_t { Idle, Running, Done, NothingKnown, AllReady };

namespace Detail {
inline std::atomic_bool      requested {false};
inline std::atomic<uint32_t> state {0};
inline std::atomic<uint32_t> total {0};
inline std::atomic<uint32_t> built {0};
inline std::atomic<uint32_t> failed {0};
} // namespace Detail

inline void Request() {
	Detail::requested = true;
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
