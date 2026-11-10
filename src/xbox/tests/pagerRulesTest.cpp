// Standalone test of the pager's rules in xbox/pagerRules.h (how long a window must be idle). Not part of the CMake build; run with
// run-lazy.cmd.

#include "xbox/pagerRules.h"

#include <cstdio>

namespace {

using Xbox::Pager::IdleTime;

int g_failures = 0;
#define CHECK(cond, ...)                                                                                                                            \
	do {                                                                                                                                             \
		if (!(cond)) {                                                                                                                               \
			++g_failures;                                                                                                                            \
			std::printf("FAIL line %d: ", __LINE__);                                                                                                 \
			std::printf(__VA_ARGS__);                                                                                                                \
			std::printf("\n");                                                                                                                        \
		}                                                                                                                                            \
	} while (0)

void IdleTimes() {
	{
		IdleTime idle;
		CHECK(idle.Update(100, 0, false) == 8000, "calm: not 8 s");
		// 128 windows came straight back: twice the time; ten seconds later half of that is forgotten
		CHECK(idle.Update(200, 128, false) >= 15900, "after 128 quick returns: not doubled");
		const uint64_t later = idle.Update(10200, 128, false);
		CHECK(later > 11500 && later < 12500, "ten seconds later: %llu ms", (unsigned long long)later);
		// a storm of them: four times, not more
		CHECK(idle.Update(10300, 5000, false) == 32000, "a storm of quick returns: not four times");
		// above the budget there is no time to be choosy, whatever came back: the headroom is not the pager's
		CHECK(idle.Update(10400, 5000, true) == IdleTime::kLeastMs, "above the budget: not the least");
		// and below it again the quick returns still count
		CHECK(idle.Update(10500, 5000, false) > 30000, "below the budget again: the quick returns were forgotten");
	}
	{
		IdleTime idle;
		CHECK(idle.Update(100, 0, true) == IdleTime::kLeastMs, "calm and above the budget: not the least");
	}
}

} // namespace

int main() {
	IdleTimes();
	std::printf(g_failures == 0 ? "pager rules test: all passed\n" : "pager rules test: %d failures\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
