#ifndef KYTY_XBOX_PAGER_RULES_H_
#define KYTY_XBOX_PAGER_RULES_H_

// Local fork: the numbers the pager decides with, apart from its thread (kernel/memoryAddressSpace.inc) and its bookkeeping (lazyRegions.h),
// so that a test can drive them (tests/pagerRulesTest.cpp). The rules follow the swap of the Jadyx emulator; the code is ours.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Xbox::Pager {

inline constexpr uint64_t kMiB = uint64_t {1} << 20u;

// How long a swept window must stay untouched before it leaves memory. Windows that come straight back say that the pager takes memory the
// game uses: the time grows with them, up to four times, and falls back when they stop (their count fades with a half-life of ten seconds).
// That choosiness is for the room below the budget. Above the budget the least time counts: the budget ends where the headroom begins, and
// the headroom is for what the GPU and the heap take in a burst, not for the pager to wait in. (It did wait there, and handed a part of the
// headroom to a game that waited for its memory: the app then ran into its limit on level loads.)
class IdleTime {
public:
	static constexpr uint64_t kBaseMs   = 8000;
	static constexpr uint64_t kLeastMs  = 1000;
	static constexpr double   kDoubling = 128.0; // quick returns (windows, 8 MiB) that double the time

	// `quick_returns`: windows that came back soon after they left, since the start. `above_budget`: the game holds more than its budget.
	uint64_t Update(uint64_t now_ms, uint64_t quick_returns, bool above_budget) {
		const double elapsed = static_cast<double>(now_ms - m_last_ms);
		m_churn        = m_churn * std::exp2(-elapsed / 10000.0) + static_cast<double>(quick_returns - m_last_returns);
		m_last_ms      = now_ms;
		m_last_returns = quick_returns;
		if (above_budget) {
			return kLeastMs;
		}
		return static_cast<uint64_t>(static_cast<double>(kBaseMs) * (1.0 + std::min(m_churn / kDoubling, 3.0)));
	}

private:
	double   m_churn        = 0.0;
	uint64_t m_last_ms      = 0;
	uint64_t m_last_returns = 0;
};

} // namespace Xbox::Pager

#endif
