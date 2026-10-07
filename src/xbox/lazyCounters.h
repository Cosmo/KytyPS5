#ifndef KYTY_XBOX_LAZY_COUNTERS_H_
#define KYTY_XBOX_LAZY_COUNTERS_H_

// T3: how much guest memory is committed at the moment (lazily committed windows), and its peak. Read by the log lines of the guest memory code, so that a run
// shows how much of the mapped direct memory the game really touched.

#include <atomic>
#include <cstdint>

namespace Xbox::Lazy {

struct Counters {
	std::atomic<uint64_t> committed {0};
	std::atomic<uint64_t> peak {0};
	std::atomic<uint64_t> commits {0}; // number of commit calls since the start
};

inline Counters& GetCounters() {
	static Counters counters;
	return counters;
}

inline void AddCommitted(uint64_t bytes) {
	auto& c   = GetCounters();
	auto  now = c.committed.fetch_add(bytes, std::memory_order_relaxed) + bytes;
	c.commits.fetch_add(1, std::memory_order_relaxed);
	auto peak = c.peak.load(std::memory_order_relaxed);
	while (now > peak && !c.peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
	}
}

inline void SubCommitted(uint64_t bytes) {
	GetCounters().committed.fetch_sub(bytes, std::memory_order_relaxed);
}

} // namespace Xbox::Lazy

#endif
