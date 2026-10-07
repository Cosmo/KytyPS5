// Standalone CPU test of Xbox::Lazy::Regions (T3). Not part of the CMake build; compile and run with src/xbox/tests/run-lazy.cmd.
// A mock host keeps, per 4 KiB page, whether it is committed and its protection. Random Map, Unmap, Protect and commit operations run on the
// Regions object and on a plain per-page model; after every operation the two must agree on what is committed and what protection each page has.

#include "xbox/lazyRegions.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Xbox::Lazy::Mode;
using Xbox::Lazy::Regions;

constexpr uint64_t kBase  = 0x200000000ull; // arbitrary base of the test address space
constexpr uint64_t kPages = 8192;           // 32 MiB of 4 KiB pages
constexpr uint64_t kPage  = 0x1000;

struct Rng {
	uint64_t s = 0x2545F4914F6CDD1Dull;
	uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
	uint64_t Below(uint64_t n) { return Next() % n; }
};

int g_failures = 0;
#define CHECK(cond, ...)                                                                                                                             \
	do {                                                                                                                                             \
		if (!(cond)) {                                                                                                                               \
			++g_failures;                                                                                                                            \
			std::printf("FAIL line %d: ", __LINE__);                                                                                                 \
			std::printf(__VA_ARGS__);                                                                                                                \
			std::printf("\n");                                                                                                                        \
			if (g_failures > 20) {                                                                                                                   \
				std::exit(1);                                                                                                                        \
			}                                                                                                                                        \
		}                                                                                                                                            \
	} while (0)

struct Mock final: Xbox::Lazy::HostOps {
	std::vector<uint8_t> committed = std::vector<uint8_t>(kPages, 0);
	std::vector<Mode>    mode      = std::vector<Mode>(kPages, Mode::ReadWrite);
	long                 commits = 0, decommits = 0, protects = 0;
	static uint64_t      Page(uint64_t a) { return (a - kBase) / kPage; }
	bool Commit(uint64_t address, uint64_t size) override {
		for (uint64_t p = Page(address); p < Page(address + size); ++p) {
			CHECK(committed[p] == 0, "commit of page %llu that is already committed", (unsigned long long)p);
			committed[p] = 1;
			mode[p]      = Mode::ReadWrite;
		}
		++commits;
		return true;
	}
	void Decommit(uint64_t address, uint64_t size) override {
		for (uint64_t p = Page(address); p < Page(address + size); ++p) {
			CHECK(committed[p] == 1, "decommit of page %llu that is not committed", (unsigned long long)p);
			committed[p] = 0;
			mode[p]      = Mode::ReadWrite;
		}
		++decommits;
	}
	bool Protect(uint64_t address, uint64_t size, Mode m) override {
		for (uint64_t p = Page(address); p < Page(address + size); ++p) {
			CHECK(committed[p] == 1, "protect of page %llu that is not committed", (unsigned long long)p);
			mode[p] = m;
		}
		++protects;
		return true;
	}
};

// the plain model: per page, which region (0 = none), and the protection the guest wants
struct Model {
	std::vector<uint32_t> region = std::vector<uint32_t>(kPages, 0);
	std::vector<uint8_t>  committed = std::vector<uint8_t>(kPages, 0);
	std::vector<Mode>     wanted    = std::vector<Mode>(kPages, Mode::ReadWrite);
	uint32_t              next_region = 1;
};

uint64_t Addr(uint64_t page) { return kBase + page * kPage; }

void Verify(const Mock& host, const Model& model, const Regions& regions, const char* what) {
	for (uint64_t p = 0; p < kPages; ++p) {
		CHECK(host.committed[p] == model.committed[p], "%s: page %llu committed %d, model %d", what, (unsigned long long)p, host.committed[p], model.committed[p]);
		if (model.committed[p]) {
			CHECK(host.mode[p] == model.wanted[p], "%s: page %llu protection %d, wanted %d", what, (unsigned long long)p, (int)host.mode[p], (int)model.wanted[p]);
		}
		CHECK(regions.Contains(Addr(p), kPage) == (model.region[p] != 0), "%s: page %llu region membership", what, (unsigned long long)p);
		if (model.region[p] == 0) {
			CHECK(model.committed[p] == 0, "%s: unmapped page %llu is committed", what, (unsigned long long)p);
		}
	}
}

void RandomRun(uint64_t seed, int steps) {
	Rng rng;
	rng.s ^= seed * 0x9E3779B97F4A7C15ull;
	Regions regions;
	Mock    host;
	Model   model;
	for (int step = 0; step < steps; ++step) {
		const uint64_t op = rng.Below(100);
		if (op < 20) {
			// map a range of free pages (page aligned, any length, often not aligned to 64 KiB)
			const uint64_t len   = 1 + rng.Below(200);
			const uint64_t first = rng.Below(kPages - len);
			bool           free  = true;
			for (uint64_t p = first; p < first + len; ++p) {
				free = free && model.region[p] == 0;
			}
			if (!free) {
				continue;
			}
			CHECK(regions.Map(Addr(first), len * kPage), "map of a free range refused");
			const uint32_t id = model.next_region++;
			for (uint64_t p = first; p < first + len; ++p) {
				model.region[p] = id;
				model.committed[p] = 0;
				model.wanted[p] = Mode::ReadWrite;
			}
		} else if (op < 35) {
			// unmap a range that lies inside regions (maybe several)
			const uint64_t len   = 1 + rng.Below(120);
			const uint64_t first = rng.Below(kPages - len);
			bool           all   = true;
			for (uint64_t p = first; p < first + len; ++p) {
				all = all && model.region[p] != 0;
			}
			const bool ok = regions.Unmap(Addr(first), len * kPage, host);
			CHECK(ok == all, "unmap result %d, expected %d", ok, all);
			if (ok) {
				for (uint64_t p = first; p < first + len; ++p) {
					model.region[p] = 0;
					model.committed[p] = 0;
					model.wanted[p] = Mode::ReadWrite;
				}
				// a region that was cut in two is two regions now: give every run of pages its own id
				uint32_t previous = 0, current = 0;
				for (uint64_t p = 0; p < kPages; ++p) {
					const uint32_t old = model.region[p];
					if (old == 0) {
						previous = 0;
						continue;
					}
					if (old != previous) {
						current = model.next_region++;
					}
					previous        = old;
					model.region[p] = current;
				}
			}
		} else if (op < 55) {
			// protection for a range inside regions
			const uint64_t len   = 1 + rng.Below(100);
			const uint64_t first = rng.Below(kPages - len);
			const Mode     m     = static_cast<Mode>(rng.Below(3));
			bool           all   = true;
			for (uint64_t p = first; p < first + len; ++p) {
				all = all && model.region[p] != 0;
			}
			const bool ok = regions.Protect(Addr(first), len * kPage, m, host);
			CHECK(ok == all, "protect result %d, expected %d", ok, all);
			if (ok) {
				for (uint64_t p = first; p < first + len; ++p) {
					model.wanted[p] = m;
				}
			}
		} else if (op < 85) {
			// first touch of a page: commits the window of the page's region
			const uint64_t page = rng.Below(kPages);
			const auto     r    = regions.CommitAt(Addr(page), host);
			if (model.region[page] == 0) {
				CHECK(r == Regions::CommitResult::NotLazy, "touch of an unmapped page gave %d", (int)r);
			} else if (model.committed[page]) {
				CHECK(r == Regions::CommitResult::AlreadyCommitted, "touch of a committed page gave %d", (int)r);
			} else {
				CHECK(r == Regions::CommitResult::Committed, "touch of an uncommitted page gave %d", (int)r);
				// the window: pages of the same absolute 64 KiB window and the same region
				const uint64_t window = (Addr(page) >> 16);
				for (uint64_t p = 0; p < kPages; ++p) {
					if ((Addr(p) >> 16) == window && model.region[p] == model.region[page]) {
						model.committed[p] = 1;
					}
				}
			}
		} else {
			// make a range committed (a file read into a buffer)
			const uint64_t len   = 1 + rng.Below(150);
			const uint64_t first = rng.Below(kPages - len);
			bool           all   = true;
			for (uint64_t p = first; p < first + len; ++p) {
				all = all && model.region[p] != 0;
			}
			const bool ok = regions.EnsureCommitted(Addr(first), len * kPage, host);
			CHECK(ok == all, "ensure result %d, expected %d", ok, all);
			if (ok) {
				for (uint64_t p = first; p < first + len; ++p) {
					const uint64_t window = (Addr(p) >> 16);
					for (uint64_t q = 0; q < kPages; ++q) {
						if ((Addr(q) >> 16) == window && model.region[q] == model.region[p]) {
							model.committed[q] = 1;
						}
					}
				}
			}
		}
		if (step % 7 == 0) {
			Verify(host, model, regions, "step");
		}
	}
	Verify(host, model, regions, "end");
	// segments report the same committed state as the model
	for (int i = 0; i < 200; ++i) {
		const uint64_t len = 1 + rng.Below(300), first = rng.Below(kPages - len);
		bool           all = true;
		for (uint64_t p = first; p < first + len; ++p) {
			all = all && model.region[p] != 0;
		}
		uint64_t covered = 0;
		bool     ok      = regions.ForEachSegment(Addr(first), len * kPage, [&](uint64_t a, uint64_t s, bool committed) {
            // the window state is per window, the model per page: a committed segment must have committed pages only if the region allows
            for (uint64_t p = (a - kBase) / kPage; p < (a + s - kBase) / kPage; ++p) {
                CHECK(model.committed[p] == (committed ? 1 : 0), "segment page %llu: committed %d, model %d", (unsigned long long)p, committed, model.committed[p]);
            }
            covered += s;
        });
		CHECK(ok == all, "segments result %d, expected %d", ok, all);
		if (ok) {
			CHECK(covered == len * kPage, "segments cover %llu of %llu", (unsigned long long)covered, (unsigned long long)(len * kPage));
		}
	}
}

} // namespace

int main() {
	for (uint64_t seed = 1; seed <= 60; ++seed) {
		RandomRun(seed, 1200);
	}
	// a few directed cases
	{
		Regions regions;
		Mock    host;
		CHECK(regions.Map(Addr(3), 40 * kPage), "map");
		CHECK(!regions.Map(Addr(10), 5 * kPage), "overlapping map must be refused");
		CHECK(regions.Protect(Addr(5), 2 * kPage, Mode::NoAccess, host), "protect before commit");
		CHECK(host.protects == 0, "an uncommitted window must not be protected");
		CHECK(regions.CommitAt(Addr(5), host) == Regions::CommitResult::Committed, "commit");
		CHECK(host.mode[5] == Mode::NoAccess && host.mode[6] == Mode::NoAccess && host.mode[7] == Mode::ReadWrite, "remembered protection applied at commit");
		CHECK(regions.Unmap(Addr(3), 40 * kPage, host), "unmap");
		CHECK(regions.RegionCount() == 0, "all regions gone");
		for (uint64_t p = 0; p < kPages; ++p) {
			CHECK(host.committed[p] == 0, "page %llu left committed", (unsigned long long)p);
		}
		CHECK(regions.Map(Addr(3), 40 * kPage), "the same range can be mapped again");
		CHECK(!regions.IsCommitted(Addr(5)), "and starts uncommitted");
	}
	// neighbouring committed windows are protected and decommitted with one host call each, not one per window
	{
		Regions regions;
		Mock    host;
		CHECK(regions.Map(Addr(0), 1600 * kPage), "map 100 windows");
		CHECK(regions.EnsureCommitted(Addr(0), 1600 * kPage, host), "commit all");
		host.protects = 0;
		CHECK(regions.Protect(Addr(0), 1600 * kPage, Mode::Read, host), "protect all");
		CHECK(host.protects == 1, "protecting 100 committed windows took %ld host calls", host.protects);
		host.decommits = 0;
		CHECK(regions.Unmap(Addr(0), 1600 * kPage, host), "unmap all");
		CHECK(host.decommits == 1, "unmapping 100 committed windows took %ld host calls", host.decommits);
		// two committed runs with a hole between them: two calls
		CHECK(regions.Map(Addr(0), 1600 * kPage), "map again");
		CHECK(regions.EnsureCommitted(Addr(0), 160 * kPage, host), "commit the first 10 windows");
		CHECK(regions.EnsureCommitted(Addr(320 * 4), 160 * kPage, host), "commit 10 windows after a hole");
		host.protects = 0;
		CHECK(regions.Protect(Addr(0), 1600 * kPage, Mode::NoAccess, host), "protect everything");
		CHECK(host.protects == 2, "two committed runs took %ld host calls", host.protects);
	}
	std::printf(g_failures == 0 ? "lazy regions test: all passed\n" : "lazy regions test: %d failures\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
