// Standalone CPU test of the pager bookkeeping in Xbox::Lazy::Regions (sweep, touch, evict, page in). Not part of the CMake build; run with
// run-lazy.cmd. A mock host keeps per 4 KiB page: committed, protection and a content number (what a guest write changes); a mock store keeps the
// slots. Random mapping, guest accesses, protection, sweeps, evictions (with a write sometimes arriving while the window is copied out) and unmaps
// run on the Regions object and on a plain per-page model. After each step the two must agree on what is committed, on the protection of every
// committed page, on the contents (a window that went to the store must come back unchanged) and on what is in the store.

#include "xbox/lazyRegions.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace {

using Xbox::Lazy::Mode;
using Xbox::Lazy::Regions;

constexpr uint64_t kBase    = 0x300000000ull;
constexpr uint64_t kPages   = 1024; // 4 MiB: 64 windows
constexpr uint64_t kPage    = 0x1000;
constexpr uint64_t kWindowP = 16; // pages per window

struct Rng {
	uint64_t s = 0x9E3779B97F4A7C15ull;
	uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
	uint64_t Below(uint64_t n) { return Next() % n; }
};

int g_failures = 0;
long g_privatized = 0;
long g_sweeps = 0, g_evicted = 0, g_aborted = 0, g_page_ins = 0, g_cut_page_ins = 0; // how much the runs did of what they test
#define CHECK(cond, ...)                                                                                                                            \
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

uint64_t Addr(uint64_t page) { return kBase + page * kPage; }
uint64_t Page(uint64_t address) { return (address - kBase) / kPage; }

struct Mock final: Xbox::Lazy::HostOps {
	std::vector<uint8_t>  committed = std::vector<uint8_t>(kPages, 0);
	std::vector<Mode>     mode      = std::vector<Mode>(kPages, Mode::ReadWrite);
	std::vector<uint32_t> content   = std::vector<uint32_t>(kPages, 0);
	std::vector<std::array<uint32_t, kWindowP>> slots;
	std::vector<uint8_t>                        live;
	uint64_t                                    limit_pages = ~uint64_t {0}; // the commit limit of the process, in pages
	uint64_t Resident() const {
		uint64_t n = 0;
		for (const auto c: committed) {
			n += c != 0;
		}
		return n;
	}
	bool Commit(uint64_t address, uint64_t size) override {
		if (Resident() + size / kPage > limit_pages) {
			return false;
		}
		for (uint64_t p = Page(address); p < Page(address + size); ++p) {
			CHECK(committed[p] == 0, "commit of page %llu that is committed", (unsigned long long)p);
			committed[p] = 1;
			mode[p]      = Mode::ReadWrite;
			content[p]   = 0; // fresh pages are zero
		}
		return true;
	}
	// per window: committed as a view (it came back from the store). A view is a mapping of its own: host calls on it never span two windows.
	std::vector<uint8_t> view = std::vector<uint8_t>(kPages / kWindowP, 0);
	// per window: private memory that is an allocation of its own (a view that was turned into private memory): a host call never spans it with a neighbour
	std::vector<uint8_t> lone = std::vector<uint8_t>(kPages / kWindowP, 0);
	void                 CheckNoLone(uint64_t first_page, uint64_t end_page, const char* what) const {
		if ((end_page - 1) / kWindowP == first_page / kWindowP) {
			return; // one window
		}
		for (uint64_t w = first_page / kWindowP; w <= (end_page - 1) / kWindowP; ++w) {
			CHECK(lone[w] == 0, "%s spans windows and window %llu is an allocation of its own (pages %llu..%llu)", what, (unsigned long long)w, (unsigned long long)first_page,
			      (unsigned long long)end_page);
		}
	}
	bool                 AnyView(uint64_t first_page, uint64_t end_page) const {
		for (uint64_t w = first_page / kWindowP; w <= (end_page - 1) / kWindowP; ++w) {
			if (view[w] != 0) {
				return true;
			}
		}
		return false;
	}
	void Decommit(uint64_t address, uint64_t size, bool is_view) override {
		const uint64_t first = Page(address), end = Page(address + size);
		CheckNoLone(first, end, "decommit");
		CHECK(is_view == AnyView(first, end), "decommit says view %d, the windows say %d", is_view, AnyView(first, end));
		CHECK(!is_view || (first % kWindowP == 0 && end - first == kWindowP), "decommit of a view that is not one window (pages %llu..%llu)", (unsigned long long)first,
		      (unsigned long long)end);
		for (uint64_t p = first; p < end; ++p) {
			CHECK(committed[p] == 1, "decommit of page %llu that is not committed", (unsigned long long)p);
			committed[p] = 0;
			mode[p]      = Mode::ReadWrite;
			content[p]   = 0;
		}
		if (is_view) {
			view[first / kWindowP] = 0;
		}
		for (uint64_t w = first / kWindowP; w <= (end - 1) / kWindowP; ++w) {
			lone[w] = 0; // decommitted: nothing is left of what was private memory of its own that this call is about
		}
	}
	bool Protect(uint64_t address, uint64_t size, Mode m) override {
		const uint64_t first = Page(address), end = Page(address + size);
		CheckNoLone(first, end, "protect");
		CHECK(!AnyView(first, end) || (first / kWindowP == (end - 1) / kWindowP), "protect call spans two windows and one of them is a view (pages %llu..%llu)",
		      (unsigned long long)first, (unsigned long long)end);
		for (uint64_t p = first; p < end; ++p) {
			CHECK(committed[p] == 1, "protect of page %llu that is not committed", (unsigned long long)p);
			mode[p] = m;
		}
		return true;
	}
	bool CommitFromStore(uint64_t, uint64_t address, uint64_t size, uint32_t slot) override {
		if (Resident() + size / kPage > limit_pages) {
			return false;
		}
		++g_page_ins;
		CHECK(slot < slots.size() && live[slot], "page in from slot %u that is not live", slot);
		const uint64_t first = Page(address);
		CHECK(first % kWindowP == 0 && size == kWindowP * kPage, "a view that is not a whole window (page %llu, size %llu)", (unsigned long long)first, (unsigned long long)size);
		for (uint64_t i = 0; i < size / kPage; ++i) {
			CHECK(committed[first + i] == 0, "commit from the store of page %llu that is committed", (unsigned long long)(first + i));
			committed[first + i] = 1;
			mode[first + i]      = Mode::ReadWrite;
			content[first + i]   = slots[slot][(first + i) % kWindowP];
		}
		view[first / kWindowP] = 1;
		return true;
	}
	bool MakePrivate(uint64_t address, uint64_t size) override {
		const uint64_t first = Page(address);
		CHECK(size == kWindowP * kPage && view[first / kWindowP] != 0, "make private of something that is not a view (page %llu)", (unsigned long long)first);
		view[first / kWindowP] = 0; // contents and protections stay
		return true;
	}
	bool Privatize(uint64_t address, uint64_t size, const void* data) override {
		const uint64_t first = Page(address);
		CHECK(size == kWindowP * kPage && first % kWindowP == 0 && view[first / kWindowP] != 0, "privatize of something that is not a view (page %llu)", (unsigned long long)first);
		CHECK(mode[first] == Mode::Read, "privatize of a view that is not read-only (page %llu, mode %d)", (unsigned long long)first, (int)mode[first]);
		const auto* words = static_cast<const uint32_t*>(data);
		for (uint64_t i = 0; i < kWindowP; ++i) {
			content[first + i] = words[i];
			mode[first + i]    = Mode::ReadWrite;
		}
		view[first / kWindowP] = 0;
		lone[first / kWindowP] = 1;
		++g_privatized;
		return true;
	}
	bool Spill(uint64_t window, uint64_t address, uint64_t size, uint32_t& slot) override {
		const uint64_t first = Page(address);
		CHECK(size == kWindowP * kPage && first % kWindowP == 0 && window == (address >> 16), "spill of something that is not a window (page %llu)", (unsigned long long)first);
		CHECK(mode[first] == Mode::Read, "spill of a window that is not read-only (page %llu, mode %d)", (unsigned long long)first, (int)mode[first]);
		slot = AllocSlot();
		for (uint64_t i = 0; i < kWindowP; ++i) {
			slots[slot][i] = content[first + i];
		}
		return true;
	}
	void FreeSlot(uint32_t slot) override {
		CHECK(slot < slots.size() && live[slot], "free of slot %u that is not live", slot);
		live[slot] = 0;
	}
	uint32_t AllocSlot() {
		for (uint32_t i = 0; i < live.size(); ++i) {
			if (!live[i]) {
				live[i] = 1;
				return i;
			}
		}
		slots.emplace_back();
		live.push_back(1);
		return static_cast<uint32_t>(slots.size() - 1);
	}
};

struct Model {
	std::vector<uint32_t> region    = std::vector<uint32_t>(kPages, 0);
	std::vector<uint8_t>  committed = std::vector<uint8_t>(kPages, 0);
	std::vector<Mode>     wanted    = std::vector<Mode>(kPages, Mode::ReadWrite);
	std::vector<uint32_t> content   = std::vector<uint32_t>(kPages, 0);
	std::map<uint64_t, std::array<uint32_t, kWindowP>> evicted; // window (page / 16) -> saved contents
	std::set<uint64_t> pinned;                                  // windows the pager must leave alone (stacks)
	uint32_t next_region = 1;
};

uint64_t WindowOf(uint64_t page) { return page / kWindowP; }
// the Regions calls take the window number of the address space (address >> 16)
uint64_t Abs(uint64_t window) { return (kBase >> 16) + window; }

// Pages of the window of `page` that belong to the same region as `page`.
void ModelCommit(Model& model, uint64_t page) {
	const uint64_t w = WindowOf(page);
	for (uint64_t p = w * kWindowP; p < (w + 1) * kWindowP; ++p) {
		if (model.region[p] == model.region[page]) {
			model.committed[p] = 1;
		}
	}
	// a window that comes back from the store has its contents again
	if (const auto it = model.evicted.find(w); it != model.evicted.end()) {
		for (uint64_t i = 0; i < kWindowP; ++i) {
			model.content[w * kWindowP + i] = it->second[i];
		}
		model.evicted.erase(it);
	}
}

void Verify(const Mock& host, const Model& model, const Regions& regions, const char* what) {
	std::set<uint32_t> expected_slots;
	for (uint64_t w = 0; w < kPages / kWindowP; ++w) {
		const uint64_t first = w * kWindowP;
		const uint8_t  flags = regions.FlagsOf(Addr(first));
		const bool     ev    = model.evicted.count(w) != 0;
		if (model.pinned.count(w) != 0) {
			CHECK((flags & (Xbox::Lazy::kSwept | Xbox::Lazy::kEvicting | Xbox::Lazy::kEvicted)) == 0 && !regions.CanSweep(Abs(w)), "%s: pinned window %llu is in the pager's hands (flags %d)", what,
			      (unsigned long long)w, flags);
		}
		CHECK(regions.IsEvicted(Addr(first)) == ev, "%s: window %llu evicted: regions %d, model %d", what, (unsigned long long)w, regions.IsEvicted(Addr(first)), ev);
		if (ev) {
			uint32_t slot = 0;
			CHECK(regions.EvictedSlot(Abs(w), slot), "%s: window %llu has no slot", what, (unsigned long long)w);
			CHECK(slot < host.slots.size() && host.live[slot], "%s: slot %u of window %llu is not live", what, slot, (unsigned long long)w);
			expected_slots.insert(slot);
			for (uint64_t i = 0; i < kWindowP; ++i) {
				CHECK(host.slots[slot][i] == model.evicted.at(w)[i], "%s: slot %u page %llu holds %u, expected %u", what, slot, (unsigned long long)i, host.slots[slot][i],
				      model.evicted.at(w)[i]);
				CHECK(host.committed[first + i] == 0 && model.committed[first + i] == 0, "%s: page %llu of an evicted window is committed", what, (unsigned long long)(first + i));
			}
		}
		for (uint64_t p = first; p < first + kWindowP; ++p) {
			CHECK(host.committed[p] == model.committed[p], "%s: page %llu committed %d, model %d", what, (unsigned long long)p, host.committed[p], model.committed[p]);
			if (model.region[p] == 0) {
				CHECK(host.committed[p] == 0, "%s: unmapped page %llu is committed", what, (unsigned long long)p);
			}
			if (model.committed[p]) {
				CHECK(host.content[p] == model.content[p], "%s: page %llu holds %u, expected %u", what, (unsigned long long)p, host.content[p], model.content[p]);
				Mode expected = model.wanted[p];
				if (flags & Xbox::Lazy::kSwept) {
					expected = Mode::NoAccess;
				} else if (flags & Xbox::Lazy::kEvicting) {
					expected = Mode::Read;
				}
				CHECK(host.mode[p] == expected, "%s: page %llu protection %d, expected %d (flags %d)", what, (unsigned long long)p, (int)host.mode[p], (int)expected, flags);
			}
		}
	}
	size_t live = 0;
	for (const auto l: host.live) {
		live += l;
	}
	CHECK(live == expected_slots.size() && regions.EvictedCount() == expected_slots.size(), "%s: %zu live slots, %zu windows in the store, regions say %zu", what, live,
	      expected_slots.size(), regions.EvictedCount());
}

// A guest (or host) access to a page, as the fault handler and the host calls treat it.
void Access(Mock& host, Model& model, Regions& regions, uint64_t page, bool write) {
	if (model.region[page] == 0) {
		return;
	}
	const uint64_t w = WindowOf(page);
	if (!model.committed[page]) {
		const auto r = regions.CommitAt(Addr(page), host); // a commit fault, or a page in
		CHECK(r == Regions::CommitResult::Committed, "access to an uncommitted page gave %d", (int)r);
		ModelCommit(model, page);
	}
	const uint8_t flags = regions.FlagsOf(Addr(page));
	if ((flags & Xbox::Lazy::kSwept) != 0 || ((flags & Xbox::Lazy::kEvicting) != 0 && write)) {
		CHECK(regions.Touch(Abs(w), host), "touch of a swept window found nothing to do");
	}
	if (write && host.mode[page] == Mode::ReadWrite) {
		host.content[page]++;
		model.content[page]++;
	}
}

void RandomRun(uint64_t seed, int steps) {
	Rng rng;
	rng.s ^= seed * 0xD1B54A32D192ED03ull;
	Regions regions;
	Mock    host;
	Model   model;
	for (int step = 0; step < steps; ++step) {
		const uint64_t op = rng.Below(100);
		if (op < 12) {
			// map: mostly whole windows, sometimes ragged
			const bool     aligned = rng.Below(4) != 0;
			const uint64_t len     = aligned ? kWindowP * (1 + rng.Below(6)) : 1 + rng.Below(60);
			const uint64_t first   = aligned ? kWindowP * rng.Below((kPages - len) / kWindowP) : rng.Below(kPages - len);
			bool           free    = true;
			for (uint64_t p = first; p < first + len; ++p) {
				free = free && model.region[p] == 0;
			}
			if (!free) {
				continue;
			}
			CHECK(regions.Map(Addr(first), len * kPage), "map refused");
			const uint32_t id = model.next_region++;
			for (uint64_t p = first; p < first + len; ++p) {
				model.region[p]    = id;
				model.committed[p] = 0;
				model.wanted[p]    = Mode::ReadWrite;
				model.content[p]   = 0;
			}
		} else if (op < 18) {
			// unmap (may cut through windows that are in the store: those come back first)
			const uint64_t len   = 1 + rng.Below(kWindowP * 3);
			const uint64_t first = rng.Below(kPages - len);
			bool           all   = true;
			for (uint64_t p = first; p < first + len; ++p) {
				all = all && model.region[p] != 0;
			}
			if (!all) {
				continue;
			}
			// the model: a window in the store that the range cuts through is committed (and its contents are back) before the cut
			for (uint64_t w = WindowOf(first); w <= WindowOf(first + len - 1); ++w) {
				const bool inside = first <= w * kWindowP && (w + 1) * kWindowP <= first + len;
				if (model.evicted.count(w) != 0 && !inside) {
					const auto saved = model.evicted.at(w);
					++g_cut_page_ins;
					uint64_t   rp    = w * kWindowP;
					while (model.region[rp] == 0) {
						++rp;
					}
					ModelCommit(model, rp);
					for (uint64_t i = 0; i < kWindowP; ++i) {
						model.content[w * kWindowP + i] = saved[i];
					}
				}
			}
			CHECK(regions.Unmap(Addr(first), len * kPage, host), "unmap refused");
			for (uint64_t p = first; p < first + len; ++p) {
				model.region[p]    = 0;
				model.committed[p] = 0;
				model.wanted[p]    = Mode::ReadWrite;
				model.content[p]   = 0;
			}
			for (uint64_t w = WindowOf(first); w <= WindowOf(first + len - 1); ++w) {
				model.evicted.erase(w); // a window that was in the store and is inside the range is gone
			}
			// regions that were cut in two are two regions now
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
		} else if (op < 21) {
			// protection for a range
			const uint64_t len   = 1 + rng.Below(40);
			const uint64_t first = rng.Below(kPages - len);
			const Mode     m     = static_cast<Mode>(rng.Below(3));
			bool           all   = true;
			for (uint64_t p = first; p < first + len; ++p) {
				all = all && model.region[p] != 0;
			}
			if (!all) {
				continue;
			}
			CHECK(regions.Protect(Addr(first), len * kPage, m, host), "protect refused");
			for (uint64_t p = first; p < first + len; ++p) {
				model.wanted[p] = m;
			}
		} else if (op < 55) {
			Access(host, model, regions, rng.Below(kPages), rng.Below(2) == 0);
		} else if (op < 68) {
			// the pager sweeps a window
			const uint64_t w     = rng.Below(kPages / kWindowP);
			const bool     can   = regions.CanSweep(Abs(w));
			const bool     swept = regions.Sweep(Abs(w), host);
			CHECK(can == swept, "sweep result %d, CanSweep said %d", swept, can);
			g_sweeps += swept ? 1 : 0;
		} else if (op < 71) {
			// a thread or a fiber starts on some windows: they are pinned, and what the pager took from them comes back
			const uint64_t w     = rng.Below(kPages / kWindowP - 3);
			const uint64_t count = 1 + rng.Below(3);
			regions.Pin(Addr(w * kWindowP), count * kWindowP * kPage);
			regions.Reclaim(Addr(w * kWindowP), count * kWindowP * kPage, host);
			for (uint64_t i = w; i < w + count; ++i) {
				model.pinned.insert(i);
				if (model.evicted.count(i) != 0) {
					ModelCommit(model, i * kWindowP);
				}
			}
		} else if (op < 77) {
			// the pager turns a swept view into private memory (too many views): the copy is taken, a write may arrive, and the window must be as it was
			const uint64_t w = rng.Below(kPages / kWindowP);
			if (host.view[w] == 0 || !regions.IsView(Abs(w))) {
				continue;
			}
			regions.Sweep(Abs(w), host); // (it may be swept already)
			if (!regions.BeginEvict(Abs(w), host)) {
				continue;
			}
			std::array<uint32_t, kWindowP> data {};
			for (uint64_t i = 0; i < kWindowP; ++i) {
				data[i] = host.content[w * kWindowP + i];
			}
			if (rng.Below(3) == 0) {
				Access(host, model, regions, w * kWindowP + rng.Below(kWindowP), true);
			}
			const bool flagged = (regions.FlagsOf(Addr(w * kWindowP)) & Xbox::Lazy::kEvicting) != 0;
			const bool done    = regions.CommitPrivatize(Abs(w), data.data(), host);
			CHECK(done == flagged, "privatize gave %d, the window was %s", done, flagged ? "still read-only" : "touched");
			if (!done && flagged) {
				regions.Touch(Abs(w), host);
			}
		} else if (op < 92) {
			// the pager takes a swept window out of memory; a write may arrive while it is copied
			const uint64_t w = rng.Below(kPages / kWindowP);
			if (!regions.BeginEvict(Abs(w), host)) {
				continue;
			}
			const uint32_t slot = host.AllocSlot();
			for (uint64_t i = 0; i < kWindowP; ++i) {
				host.slots[slot][i] = host.content[w * kWindowP + i];
			}
			const bool interfere = rng.Below(3) == 0;
			if (interfere) {
				Access(host, model, regions, w * kWindowP + rng.Below(kWindowP), true);
			}
			const bool left = regions.CommitEvict(Abs(w), slot, host);
			if (left) {
				++g_evicted;
				CHECK(!interfere, "the window left although a write arrived while it was copied");
				std::array<uint32_t, kWindowP> saved {};
				for (uint64_t i = 0; i < kWindowP; ++i) {
					saved[i] = model.content[w * kWindowP + i];
					model.committed[w * kWindowP + i] = 0;
					model.content[w * kWindowP + i]   = 0;
				}
				model.evicted[w] = saved;
			} else {
				++g_aborted;
				host.FreeSlot(slot); // what the pager does with a copy it did not need
			}
		} else {
			// a host call writes into a range: committed, brought back from the store, read-write
			const uint64_t len   = 1 + rng.Below(60);
			const uint64_t first = rng.Below(kPages - len);
			bool           all   = true;
			for (uint64_t p = first; p < first + len; ++p) {
				all = all && model.region[p] != 0;
			}
			const bool ok = regions.EnsureCommitted(Addr(first), len * kPage, host);
			CHECK(ok == all, "ensure result %d, expected %d", ok, all);
			if (ok) {
				for (uint64_t p = first; p < first + len; ++p) {
					if (!model.committed[p]) {
						ModelCommit(model, p);
					}
				}
			}
		}
		if (step % 5 == 0) {
			Verify(host, model, regions, "step");
		}
	}
	Verify(host, model, regions, "end");
}

// The process is at its commit limit: a commit that fails makes room (Regions::MakeRoom) instead of failing. 40 windows are used through a limit of 12 resident
// windows (2 of them pinned); every window keeps what was written to it, no matter how often it went to the store and back.
long g_room = 0;
void RoomRun(uint64_t seed) {
	Rng rng;
	rng.s ^= seed * 0xA24BAED4963EE407ull;
	constexpr uint64_t kWindows = 40;
	Regions            regions;
	Mock               host;
	host.limit_pages = 12 * kWindowP;
	CHECK(regions.Map(Addr(0), kWindows * kWindowP * kPage), "map refused");
	regions.Pin(Addr(3 * kWindowP), 2 * kWindowP * kPage);
	std::vector<uint32_t> expected(kWindows, 0);
	for (int step = 0; step < 3000; ++step) {
		const uint64_t w    = rng.Below(kWindows);
		const uint64_t page = w * kWindowP + rng.Below(kWindowP);
		if (!host.committed[page]) {
			const auto r = regions.CommitAt(Addr(page), host);
			CHECK(r == Regions::CommitResult::Committed, "commit at the limit gave %d (step %d)", (int)r, step);
			if (r != Regions::CommitResult::Committed) {
				return;
			}
		}
		const uint8_t flags = regions.FlagsOf(Addr(page));
		if ((flags & Xbox::Lazy::kSwept) != 0) {
			CHECK(regions.Touch(Abs(w), host), "touch of a swept window found nothing to do");
		}
		for (uint64_t i = 0; i < kWindowP; ++i) {
			CHECK(host.content[w * kWindowP + i] == expected[w], "window %llu page %llu holds %u, expected %u (step %d)", (unsigned long long)w, (unsigned long long)i,
			      host.content[w * kWindowP + i], expected[w], step);
		}
		if (rng.Below(2) == 0) {
			++expected[w];
			for (uint64_t i = 0; i < kWindowP; ++i) {
				host.content[w * kWindowP + i] = expected[w];
			}
		}
		if (rng.Below(4) == 0) {
			regions.Sweep(Abs(rng.Below(kWindows)), host); // some windows wait swept: MakeRoom takes those first
		}
		CHECK(host.Resident() <= host.limit_pages, "%llu pages resident, the limit is %llu", (unsigned long long)host.Resident(), (unsigned long long)host.limit_pages);
		for (const uint64_t pinned: {3ull, 4ull}) {
			CHECK(!regions.IsEvicted(Addr(pinned * kWindowP)), "pinned window %llu was evicted", (unsigned long long)pinned);
		}
	}
	g_room += static_cast<long>(regions.RoomEvicted());
	CHECK(regions.RoomEvicted() > 0, "nothing was evicted to make room");
	// whatever is in the store is as written
	for (uint64_t w = 0; w < kWindows; ++w) {
		uint32_t slot = 0;
		if (regions.EvictedSlot(Abs(w), slot)) {
			for (uint64_t i = 0; i < kWindowP; ++i) {
				CHECK(host.slots[slot][i] == expected[w], "stored window %llu page %llu holds %u, expected %u", (unsigned long long)w, (unsigned long long)i, host.slots[slot][i],
				      expected[w]);
			}
		}
	}
}

} // namespace

int main() {
	for (uint64_t seed = 1; seed <= 80; ++seed) {
		RandomRun(seed, 1500);
	}
	for (uint64_t seed = 1; seed <= 20; ++seed) {
		RoomRun(seed);
	}
	std::printf("made room by eviction: %ld windows\n", g_room);
	std::printf("views turned into private memory: %ld\n", g_privatized);
	CHECK(g_privatized > 0, "no view was ever turned into private memory");
	std::printf("exercised: %ld sweeps, %ld windows evicted, %ld evictions aborted by a write, %ld page ins (%ld of the windows were cut by an unmap)\n", g_sweeps, g_evicted,
	            g_aborted, g_page_ins, g_cut_page_ins);
	std::printf(g_failures == 0 ? "lazy pager test: all passed\n" : "lazy pager test: %d failures\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
