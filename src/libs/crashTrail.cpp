#include "libs/crashTrail.h"

#include "common/threads.h"

#include <array>
#include <cstdio>
#include <mutex>
#include <string>

namespace Libs::CrashTrail {

namespace {

// The game's last pieces of text, each with the thread that printed it, cut to a line's length.
constexpr size_t OUTPUT_LINES  = 24;
constexpr size_t OUTPUT_LENGTH = 300;

struct Line {
	int         thread = 0;
	std::string text;
};

std::mutex                         g_output_mutex;
std::array<Line, OUTPUT_LINES>     g_output;
size_t                             g_output_next = 0;

// The last files looked for and not found (guarded by g_output_mutex).
constexpr size_t                   MISSING_FILES = 8;
std::array<Line, MISSING_FILES>    g_missing;
size_t                             g_missing_next = 0;

} // namespace

void Missing(std::string_view path) {
	const int       thread = Common::Thread::GetThreadIdUnique();
	std::lock_guard lock(g_output_mutex);
	auto&           line = g_missing[g_missing_next++ % MISSING_FILES];
	line.thread          = thread;
	line.text.assign(path.substr(0, OUTPUT_LENGTH));
}

void Output(std::string_view text) {
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '\0')) {
		text.remove_suffix(1);
	}
	if (text.empty()) {
		return;
	}
	const int       thread = Common::Thread::GetThreadIdUnique();
	std::lock_guard lock(g_output_mutex);
	auto&           line = g_output[g_output_next++ % OUTPUT_LINES];
	line.thread          = thread;
	line.text.assign(text.substr(0, OUTPUT_LENGTH));
}

void Dump() {
	const auto& calls = g_calls;
	std::printf("--- Last library calls of this thread (oldest first) ---\n");
	const uint32_t count = calls.next < CALLS ? calls.next : CALLS;
	for (uint32_t i = calls.next - count; i != calls.next; i++) {
		const auto slot = i % CALLS;
		std::printf("  %s::%s\n", calls.library[slot] != nullptr ? calls.library[slot] : "?",
		            calls.function[slot] != nullptr ? calls.function[slot] : "?");
	}
	// Another thread may hold the lock forever now; then the lines are read as they are.
	const bool locked = g_output_mutex.try_lock();
	std::printf("--- Last text the game printed (oldest first)%s ---\n", locked ? "" : ", read without the lock");
	for (size_t i = 0; i < OUTPUT_LINES; i++) {
		const auto& line = g_output[(g_output_next + i) % OUTPUT_LINES];
		if (!line.text.empty()) {
			std::printf("  [%d] %s\n", line.thread, line.text.c_str());
		}
	}
	std::printf("--- Last files the game looked for and did not find (oldest first) ---\n");
	for (size_t i = 0; i < MISSING_FILES; i++) {
		const auto& line = g_missing[(g_missing_next + i) % MISSING_FILES];
		if (!line.text.empty()) {
			std::printf("  [%d] %s\n", line.thread, line.text.c_str());
		}
	}
	if (locked) {
		g_output_mutex.unlock();
	}
}

} // namespace Libs::CrashTrail
