#ifndef KYTY_XBOX_LAZY_REGIONS_H_
#define KYTY_XBOX_LAZY_REGIONS_H_

// Local fork, T3: bookkeeping for guest memory that is reserved when it is mapped and committed in windows on the first touch.
// A region is a mapped range of guest memory. Its address range is cut into windows of 64 KiB (aligned in the address space, clipped to the
// region). A window is committed as a whole, read-write, the first time it is touched; the protection the guest (or a page watcher) asked for
// is remembered per 4 KiB page for windows that are not committed yet and applied when the window is committed. Unmapping decommits.
// This class decides what to do; HostOps does it (VirtualAlloc and friends in the emulator, a mock in the test). No locking here.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Xbox::Lazy {

enum class Mode : uint8_t { NoAccess = 0, Read = 1, ReadWrite = 2 };

// What the fault handler and the host copies may know about a 64 KiB window without asking the system. VirtualQuery takes time in proportion to the
// length of the run of equal pages around the address (hundreds of microseconds in guest memory), and the fault handler used to call it for every
// page watcher fault. Only windows that are committed as a whole carry flags.
//   kFull:     the whole window is committed (by one region).
//   kProt:     some page of it may be read-only or no-access (cleared only by a protection call that covers the window with read-write).
//   kNoAccess: some page of it may be no-access (cleared by a call that covers the window with read or read-write).
// A flag that is set although it no longer holds is harmless (the slow path runs); a flag that is clear although it holds would not be.
enum WindowFlag : uint8_t { kFull = 1, kProt = 2, kNoAccess = 4 };

// The flags of every window of a 48 bit address space: readable from any thread without a lock, written by one thread at a time (the caller's lock).
class WindowFlags {
public:
	WindowFlags(): m_top(std::make_unique<std::atomic<Leaf*>[]>(kTop)) {
		for (size_t i = 0; i < kTop; ++i) {
			m_top[i].store(nullptr, std::memory_order_relaxed);
		}
	}
	~WindowFlags() {
		for (size_t i = 0; i < kTop; ++i) {
			delete m_top[i].load(std::memory_order_relaxed);
		}
	}
	WindowFlags(const WindowFlags&)            = delete;
	WindowFlags& operator=(const WindowFlags&) = delete;

	[[nodiscard]] uint8_t Get(uint64_t window) const noexcept {
		const auto* slot = SlotIfAny(window);
		return slot == nullptr ? 0 : slot->load(std::memory_order_acquire);
	}
	void Store(uint64_t window, uint8_t value) {
		if (auto* slot = value != 0 ? &Create(window) : SlotIfAny(window)) {
			slot->store(value, std::memory_order_release);
		}
	}
	void Or(uint64_t window, uint8_t bits) {
		if (auto* slot = &Create(window); slot != nullptr) {
			slot->fetch_or(bits, std::memory_order_acq_rel);
		}
	}
	void And(uint64_t window, uint8_t mask) {
		if (auto* slot = SlotIfAny(window)) {
			slot->fetch_and(mask, std::memory_order_acq_rel);
		}
	}

private:
	static constexpr uint64_t kLeafBits = 16;
	static constexpr uint64_t kLeaf     = uint64_t {1} << kLeafBits;
	static constexpr size_t   kTop      = size_t {1} << 16;
	struct Leaf {
		std::array<std::atomic<uint8_t>, kLeaf> flags;
		Leaf() {
			for (auto& f: flags) {
				f.store(0, std::memory_order_relaxed);
			}
		}
	};

	[[nodiscard]] std::atomic<uint8_t>* SlotIfAny(uint64_t window) const noexcept {
		if ((window >> kLeafBits) >= kTop) {
			return nullptr;
		}
		Leaf* leaf = m_top[window >> kLeafBits].load(std::memory_order_acquire);
		return leaf == nullptr ? nullptr : &leaf->flags[window & (kLeaf - 1)];
	}
	// A slot for windows beyond the address space too (a shared dummy that nobody reads back), so callers need no special case.
	std::atomic<uint8_t>& Create(uint64_t window) {
		static std::atomic<uint8_t> dummy {0};
		if ((window >> kLeafBits) >= kTop) {
			return dummy;
		}
		Leaf* leaf = m_top[window >> kLeafBits].load(std::memory_order_acquire);
		if (leaf == nullptr) {
			leaf = new Leaf();
			m_top[window >> kLeafBits].store(leaf, std::memory_order_release);
		}
		return leaf->flags[window & (kLeaf - 1)];
	}

	std::unique_ptr<std::atomic<Leaf*>[]> m_top;
};

class HostOps {
public:
	virtual ~HostOps()                                         = default;
	virtual bool Commit(uint64_t address, uint64_t size)       = 0; // commit read-write; the range is not committed
	virtual void Decommit(uint64_t address, uint64_t size)     = 0; // the range is committed
	virtual bool Protect(uint64_t address, uint64_t size, Mode mode) = 0; // the range is committed
};

class Regions {
public:
	static constexpr uint64_t kWindowBits = 16;
	static constexpr uint64_t kWindow     = uint64_t {1} << kWindowBits;
	static constexpr uint64_t kPage       = 0x1000;
	static constexpr uint64_t kPagesPerWindow = kWindow / kPage;

	enum class CommitResult { Committed, AlreadyCommitted, NotLazy, Failed };

	// Adds a region. The range must be page aligned and must not overlap a region.
	bool Map(uint64_t address, uint64_t size) {
		if (size == 0 || (address & (kPage - 1)) != 0 || (size & (kPage - 1)) != 0 || Overlaps(address, size)) {
			return false;
		}
		Region r;
		r.start     = address;
		r.end       = address + size;
		r.committed.assign(WindowCount(address, address + size), 0);
		m_regions.emplace(address, std::move(r));
		return true;
	}

	// True when the whole range lies inside lazy regions (possibly several, touching each other).
	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		uint64_t cursor = address;
		const uint64_t end = address + size;
		while (cursor < end) {
			const Region* r = Find(cursor);
			if (r == nullptr) {
				return false;
			}
			cursor = std::min(end, r->end);
		}
		return size != 0;
	}

	// Takes the range out of the regions: committed windows are decommitted (only the part inside the range), regions are split or removed.
	bool Unmap(uint64_t address, uint64_t size, HostOps& host) {
		if (!Contains(address, size)) {
			return false;
		}
		const uint64_t end = address + size;
		uint64_t cursor    = address;
		while (cursor < end) {
			auto it = FindIt(cursor);
			Region old = std::move(it->second);
			m_regions.erase(it);
			const uint64_t a = std::max(old.start, cursor), b = std::min(old.end, end);
			// the windows lose their flags before anything is decommitted (a window the range cuts through is no longer committed as a whole)
			for (uint64_t w = a >> kWindowBits; w <= (b - 1) >> kWindowBits; ++w) {
				m_flags.Store(w, 0);
			}
			// decommit the committed windows inside [a, b), one host call per run of neighbouring committed windows
			uint64_t run_start = 0, run_end = 0;
			for (uint64_t w = a >> kWindowBits; w <= (b - 1) >> kWindowBits; ++w) {
				const uint64_t lo = std::max(a, w << kWindowBits), hi = std::min(b, (w + 1) << kWindowBits);
				if (old.committed[w - (old.start >> kWindowBits)] != 0) {
					if (run_end == lo && run_end != run_start) {
						run_end = hi;
					} else {
						if (run_end > run_start) {
							host.Decommit(run_start, run_end - run_start);
						}
						run_start = lo;
						run_end   = hi;
					}
				} else if (run_end > run_start) {
					host.Decommit(run_start, run_end - run_start);
					run_start = run_end = 0;
				}
			}
			if (run_end > run_start) {
				host.Decommit(run_start, run_end - run_start);
			}
			if (old.start < a) {
				AddPiece(old, old.start, a);
			}
			if (b < old.end) {
				AddPiece(old, b, old.end);
			}
			cursor = b;
		}
		return true;
	}

	// The protection the guest or a page watcher wants for the range. Committed pages get it now, the others remember it.
	bool Protect(uint64_t address, uint64_t size, Mode mode, HostOps& host) {
		if (!Contains(address, size) || (address & (kPage - 1)) != 0 || (size & (kPage - 1)) != 0) {
			return false;
		}
		const uint64_t end = address + size;
		uint64_t cursor    = address;
		std::vector<std::pair<uint64_t, uint8_t>> clears; // window, mask of the flags to keep: applied after the host calls
		while (cursor < end) {
			Region& r = *FindMut(cursor);
			const uint64_t a = cursor, b = std::min(end, r.end);
			// One host call per run of neighbouring committed windows: a page watcher protects whole buffers, and a call per 64 KiB would turn
			// one system call into thousands (it made every watcher fault cost milliseconds).
			uint64_t run_start = 0, run_end = 0;
			const auto flush = [&]() {
				const bool ok = run_end <= run_start || host.Protect(run_start, run_end - run_start, mode);
				run_start = run_end = 0;
				return ok;
			};
			for (uint64_t w = a >> kWindowBits; w <= (b - 1) >> kWindowBits; ++w) {
				const uint64_t lo = std::max(a, w << kWindowBits), hi = std::min(b, (w + 1) << kWindowBits);
				if (r.committed[w - (r.start >> kWindowBits)] != 0) {
					NoteProtect(w, lo, hi, mode, clears);
					if (run_end == lo && run_end != run_start) {
						run_end = hi;
					} else {
						if (!flush()) {
							return false;
						}
						run_start = lo;
						run_end   = hi;
					}
				} else {
					if (!flush()) {
						return false;
					}
					auto found = r.modes.find(w);
					if (found == r.modes.end()) {
						std::array<uint8_t, kPagesPerWindow> fresh;
						fresh.fill(static_cast<uint8_t>(Mode::ReadWrite));
						found = r.modes.emplace(w, fresh).first;
					}
					auto& modes = found->second;
					for (uint64_t p = lo; p < hi; p += kPage) {
						modes[(p - (w << kWindowBits)) / kPage] = static_cast<uint8_t>(mode);
					}
					// all pages read-write again: nothing to remember
					if (std::all_of(modes.begin(), modes.end(), [](uint8_t m) { return m == static_cast<uint8_t>(Mode::ReadWrite); })) {
						r.modes.erase(w);
					}
				}
			}
			if (!flush()) {
				return false;
			}
			cursor = b;
		}
		for (const auto& [w, mask]: clears) {
			m_flags.And(w, mask);
		}
		return true;
	}

	// Commits the window that contains the address, if it is not committed yet.
	CommitResult CommitAt(uint64_t address, HostOps& host) {
		Region* r = FindMut(address);
		if (r == nullptr) {
			return CommitResult::NotLazy;
		}
		const uint64_t w = address >> kWindowBits;
		auto&          flag = r->committed[w - (r->start >> kWindowBits)];
		if (flag != 0) {
			return CommitResult::AlreadyCommitted;
		}
		const uint64_t lo = std::max(r->start, w << kWindowBits), hi = std::min(r->end, (w + 1) << kWindowBits);
		if (!host.Commit(lo, hi - lo)) {
			return CommitResult::Failed;
		}
		flag = 1;
		{
			// the flags of the window are set before the remembered protections are applied: a fault they cause must find them
			uint8_t bits = 0;
			if (lo == (w << kWindowBits) && hi == ((w + 1) << kWindowBits)) {
				bits = kFull;
				if (const auto found = r->modes.find(w); found != r->modes.end()) {
					for (const auto m: found->second) {
						if (m != static_cast<uint8_t>(Mode::ReadWrite)) {
							bits |= kProt;
						}
						if (m == static_cast<uint8_t>(Mode::NoAccess)) {
							bits |= kNoAccess;
						}
					}
				}
			}
			m_flags.Store(w, bits);
		}
		auto it = r->modes.find(w);
		if (it != r->modes.end()) {
			// apply the remembered protections page by page, merging runs
			const auto modes = it->second;
			r->modes.erase(it);
			uint64_t run_start = lo;
			Mode     run_mode  = ModeOf(modes, w, lo);
			for (uint64_t p = lo + kPage; p <= hi; p += kPage) {
				const Mode m = p < hi ? ModeOf(modes, w, p) : Mode::ReadWrite;
				if (p == hi || m != run_mode) {
					if (run_mode != Mode::ReadWrite) {
						host.Protect(run_start, p - run_start, run_mode);
					}
					run_start = p;
					run_mode  = m;
				}
			}
		}
		return CommitResult::Committed;
	}

	// Commits every window the range touches.
	bool EnsureCommitted(uint64_t address, uint64_t size, HostOps& host) {
		if (!Contains(address, size)) {
			return false;
		}
		const uint64_t end = address + size;
		uint64_t       cursor = address;
		while (cursor < end) {
			const Region* r = Find(cursor);
			const uint64_t piece_end = std::min(end, r->end);
			// every window of this region inside [cursor, piece_end); a window shared with the next region is committed there by the next round
			for (uint64_t w = cursor >> kWindowBits; w <= (piece_end - 1) >> kWindowBits; ++w) {
				if (CommitAt(std::max(cursor, w << kWindowBits), host) == CommitResult::Failed) {
					return false;
				}
			}
			cursor = piece_end;
		}
		return true;
	}

	// Calls visit(address, size, committed) for the parts of the range, split at window borders where the committed state changes.
	template <class Visit>
	bool ForEachSegment(uint64_t address, uint64_t size, Visit&& visit) const {
		if (!Contains(address, size)) {
			return false;
		}
		const uint64_t end = address + size;
		uint64_t seg_start = address;
		bool     seg_state = IsCommitted(address);
		for (uint64_t w = (address >> kWindowBits) + 1; (w << kWindowBits) < end; ++w) {
			const bool state = IsCommitted(w << kWindowBits);
			if (state != seg_state) {
				visit(seg_start, (w << kWindowBits) - seg_start, seg_state);
				seg_start = w << kWindowBits;
				seg_state = state;
			}
		}
		visit(seg_start, end - seg_start, seg_state);
		return true;
	}

	// Lock-free answers from the window flags (see WindowFlag); false means: ask the slow way.
	[[nodiscard]] uint8_t FlagsOf(uint64_t address) const { return m_flags.Get(address >> kWindowBits); }
	// Every window of the range is committed as a whole and has no no-access page: a host read cannot fault.
	[[nodiscard]] bool AllReadable(uint64_t address, uint64_t size) const {
		if (size == 0) {
			return false;
		}
		for (uint64_t w = address >> kWindowBits; w <= (address + size - 1) >> kWindowBits; ++w) {
			if ((m_flags.Get(w) & (kFull | kNoAccess)) != kFull) {
				return false;
			}
		}
		return true;
	}
	// Every window of the range is committed as a whole and read-write: a host write cannot fault.
	[[nodiscard]] bool AllWritable(uint64_t address, uint64_t size) const {
		if (size == 0) {
			return false;
		}
		for (uint64_t w = address >> kWindowBits; w <= (address + size - 1) >> kWindowBits; ++w) {
			if ((m_flags.Get(w) & (kFull | kProt)) != kFull) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool IsCommitted(uint64_t address) const {
		const Region* r = Find(address);
		return r != nullptr && r->committed[(address >> kWindowBits) - (r->start >> kWindowBits)] != 0;
	}

	// Number of bytes from the address (at most max) that lie inside regions without a gap.
	[[nodiscard]] uint64_t ContainedLength(uint64_t address, uint64_t max) const {
		uint64_t       cursor = address;
		const uint64_t end    = address + max;
		while (cursor < end) {
			const Region* r = Find(cursor);
			if (r == nullptr) {
				break;
			}
			cursor = std::min(end, r->end);
		}
		return cursor - address;
	}

	// Start of the first region that begins after the address, UINT64_MAX when there is none.
	[[nodiscard]] uint64_t NextStart(uint64_t address) const {
		auto it = m_regions.upper_bound(address);
		return it == m_regions.end() ? UINT64_MAX : it->first;
	}

	[[nodiscard]] size_t RegionCount() const { return m_regions.size(); }

private:

	// What a protection call does to the flags of a committed window: bits are set before the host call, cleared after the last one.
	void NoteProtect(uint64_t w, uint64_t lo, uint64_t hi, Mode mode, std::vector<std::pair<uint64_t, uint8_t>>& clears) {
		if ((m_flags.Get(w) & kFull) == 0) {
			return;
		}
		const bool covers = lo == (w << kWindowBits) && hi == ((w + 1) << kWindowBits);
		if (mode != Mode::ReadWrite) {
			m_flags.Or(w, static_cast<uint8_t>(kProt | (mode == Mode::NoAccess ? kNoAccess : 0)));
			if (mode == Mode::Read && covers) {
				clears.push_back({w, static_cast<uint8_t>(~kNoAccess)});
			}
		} else if (covers) {
			clears.push_back({w, static_cast<uint8_t>(~(kProt | kNoAccess))});
		}
	}

	struct Region {
		uint64_t                                                 start = 0, end = 0;
		std::vector<uint8_t>                                     committed; // per window
		std::unordered_map<uint64_t, std::array<uint8_t, kPagesPerWindow>> modes; // window (absolute index) -> mode per 4 KiB page, only if not all read-write
	};

	static uint64_t WindowCount(uint64_t start, uint64_t end) { return ((end - 1) >> kWindowBits) - (start >> kWindowBits) + 1; }

	static Mode ModeOf(const std::array<uint8_t, kPagesPerWindow>& modes, uint64_t window, uint64_t page_address) {
		return static_cast<Mode>(modes[(page_address - (window << kWindowBits)) / kPage]);
	}

	void AddPiece(const Region& old, uint64_t start, uint64_t end) {
		Region r;
		r.start = start;
		r.end   = end;
		r.committed.assign(WindowCount(start, end), 0);
		for (uint64_t w = start >> kWindowBits; w <= (end - 1) >> kWindowBits; ++w) {
			r.committed[w - (start >> kWindowBits)] = old.committed[w - (old.start >> kWindowBits)];
			if (auto it = old.modes.find(w); it != old.modes.end()) {
				r.modes[w] = it->second;
			}
		}
		m_regions.emplace(start, std::move(r));
	}

	bool Overlaps(uint64_t address, uint64_t size) const {
		auto it = m_regions.upper_bound(address);
		if (it != m_regions.begin()) {
			auto prev = std::prev(it);
			if (prev->second.end > address) {
				return true;
			}
		}
		return it != m_regions.end() && it->first < address + size;
	}

	std::map<uint64_t, Region>::iterator FindIt(uint64_t address) {
		auto it = m_regions.upper_bound(address);
		if (it == m_regions.begin()) {
			return m_regions.end();
		}
		--it;
		return address < it->second.end ? it : m_regions.end();
	}
	const Region* Find(uint64_t address) const {
		auto it = m_regions.upper_bound(address);
		if (it == m_regions.begin()) {
			return nullptr;
		}
		--it;
		return address < it->second.end ? &it->second : nullptr;
	}
	Region* FindMut(uint64_t address) {
		auto it = FindIt(address);
		return it == m_regions.end() ? nullptr : &it->second;
	}

	std::map<uint64_t, Region> m_regions;
	WindowFlags                m_flags;
};

} // namespace Xbox::Lazy

#endif
