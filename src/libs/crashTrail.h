#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_CRASH_TRAIL_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_CRASH_TRAIL_H_

#include <cstdint>
#include <string_view>

// What a crash report shows of the time before the crash, whatever the log is set to: the faulting thread's last library calls, the last
// text the game printed (where a game's own assert message goes), and the last files it looked for and did not find.
namespace Libs::CrashTrail {

constexpr uint32_t CALLS = 32;

// A thread's last library calls, as the names PRINT_NAME has (static strings): two stores a call.
struct Calls {
	const char* library[CALLS] {};
	const char* function[CALLS] {};
	uint32_t    next = 0;
};
inline thread_local Calls g_calls;

inline void Call(const char* library, const char* function) {
	auto&      calls     = g_calls;
	const auto slot      = calls.next++ % CALLS;
	calls.library[slot]  = library;
	calls.function[slot] = function;
}

// Text the game printed (its printf, its writes to stdout and stderr).
void Output(std::string_view text);

// A file the game looked for and did not find (the host path).
void Missing(std::string_view path);

// The crash handler, on the faulting thread: prints this thread's last calls and the game's last text.
void Dump();

} // namespace Libs::CrashTrail

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_CRASH_TRAIL_H_
