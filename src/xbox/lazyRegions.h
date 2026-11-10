#ifndef KYTY_XBOX_LAZY_REGIONS_H_
#define KYTY_XBOX_LAZY_REGIONS_H_

// Local fork, T3: bookkeeping for guest memory that is reserved when it is mapped and committed in windows on the first touch.
// A region is a mapped range of guest memory. Its address range is cut into windows of 64 KiB (aligned in the address space, clipped to the
// region). A window is committed as a whole, read-write, the first time it is touched; the protection the guest (or a page watcher) asked for
// is remembered per 4 KiB page: it is applied when the window is committed, and again when the pager gives a window back that it had made
// no-access or read-only as a whole. Unmapping decommits.
// This class decides what to do; HostOps does it (VirtualAlloc and friends in the emulator, a mock in the test). No locking here.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <new>
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
// The pager (below, and GuestAddressSpace) adds the states of a window on its way out of memory:
//   kSwept:    the pager made the window no-access to see whether anybody touches it; a touch makes it read-write again and sets kTouched.
//   kEvicting: the window is read-only while the pager copies it to the store; a write (a touch) makes it read-write and aborts the eviction.
//   kTouched:  somebody touched the window since it was swept.
//   kEvicted:  the window is not committed, its contents are in a slot of the store (the window is not committed and `IsCommitted` is false).
enum WindowFlag : uint8_t { kFull = 1, kProt = 2, kNoAccess = 4, kSwept = 8, kEvicting = 16, kTouched = 32, kEvicted = 64 };

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
	virtual bool Commit(uint64_t address, uint64_t size) = 0; // commit read-write; the range is not committed
	// The range is committed. A `view` is one window that came back from the store (see CommitFromStore), and it is decommitted alone.
	virtual void Decommit(uint64_t address, uint64_t size, bool view) = 0;
	// The range is committed: some private windows (neighbours are one call), or a part of ONE view (a view is a mapping of its own: a call never spans two).
	virtual bool Protect(uint64_t address, uint64_t size, Mode mode) = 0;
	// The pager's store. A window comes back as a view of a section of its own: the contents of the slot are read into the section through a view that
	// nobody else knows, and the window becomes this view in one step. Another thread that accesses the window sees a fault until then and the
	// contents after (a window committed first and filled after would read as zeros for a thread that does not fault: it ended every run within minutes).
	virtual bool CommitFromStore(uint64_t /*window*/, uint64_t /*address*/, uint64_t /*size*/, uint32_t /*slot*/) { return false; }
	// A view becomes private memory with the same contents and protections (before an unmap cuts through the window and keeps a part of it).
	virtual bool MakePrivate(uint64_t /*address*/, uint64_t /*size*/) { return false; }
	virtual void FreeSlot(uint32_t /*slot*/) {}
	// Copies a window (read-only, nobody can write it) to a new slot of the store; false when that did not work. The slot is the window's if the
	// call returns true. For MakeRoom, which evicts without the pager thread.
	virtual bool Spill(uint64_t /*window*/, uint64_t /*address*/, uint64_t /*size*/, uint32_t& /*slot*/) { return false; }
	// The app is close to its limit of memory: a commit of `size` bytes now would leave less than a hard headroom. The commit makes room first, instead of
	// waiting for a failure (which the GPU and the heap, that cannot make room, may hit before the guest does).
	virtual bool Tight(uint64_t /*size*/) { return false; }
	// A view (one window, read-only now, its contents in `data`) becomes private memory with the same contents, read-write. The mapping of a view of a
	// section is a limited resource of the process (the Series X stops mapping at about 16800 of them), private memory is not.
	virtual bool Privatize(uint64_t /*address*/, uint64_t /*size*/, const void* /*data*/) { return false; }
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
		// Prepare both cut windows before removing any mappings. A page-in may evict other
		// windows to make room, so keep the first edge pinned while preparing the second.
		struct EdgePin {
			Regions& regions;
			uint64_t id = 0;
			~EdgePin() {
				if (id != 0) {
					regions.TempUnpin(id);
				}
			}
		} first_edge {*this};
		for (const uint64_t edge: {address, end}) {
			if ((edge & (kWindow - 1)) == 0) {
				continue;
			}
			const uint64_t w = edge >> kWindowBits;
			if (m_evicted.count(w) != 0 && CommitAt(w << kWindowBits, host) == CommitResult::Failed) {
				return false;
			}
			if (!Touch(w, host)) {
				return false;
			}
			// A view cannot be cut. Keep the surviving pages in private memory.
			Region* cut = FindMut(w << kWindowBits);
			if (cut != nullptr && cut->committed[w - (cut->start >> kWindowBits)] == 2) {
				if (!host.MakePrivate(w << kWindowBits, kWindow)) {
					return false;
				}
				cut->committed[w - (cut->start >> kWindowBits)] = 1;
			}
			if (edge == address && (end & (kWindow - 1)) != 0 && (end >> kWindowBits) != w) {
				first_edge.id = TempPin(address, 1);
			}
		}
		uint64_t cursor = address;
		while (cursor < end) {
			auto it = FindIt(cursor);
			Region old = std::move(it->second);
			m_regions.erase(it);
			const uint64_t a = std::max(old.start, cursor), b = std::min(old.end, end);
			// the windows lose their flags before anything is decommitted (a window the range cuts through is no longer committed as a whole);
			// the windows in the store are gone, their slots are free
			for (uint64_t w = a >> kWindowBits; w <= (b - 1) >> kWindowBits; ++w) {
				if ((m_flags.Get(w) & kSwept) != 0) {
					m_swept_at.erase(w);
				}
				m_flags.Store(w, 0);
				m_away.erase(w);
				if (const auto ev = m_evicted.find(w); ev != m_evicted.end()) {
					host.FreeSlot(ev->second);
					m_evicted.erase(ev);
				}
			}
			// decommit the committed windows inside [a, b), one host call per run of neighbouring private windows (a view is a call of its own)
			uint64_t run_start = 0, run_end = 0;
			for (uint64_t w = a >> kWindowBits; w <= (b - 1) >> kWindowBits; ++w) {
				const uint64_t lo = std::max(a, w << kWindowBits), hi = std::min(b, (w + 1) << kWindowBits);
				const uint8_t  kind = old.committed[w - (old.start >> kWindowBits)];
				if (kind == 1) {
					if (run_end == lo && run_end != run_start) {
						run_end = hi;
					} else {
						if (run_end > run_start) {
							host.Decommit(run_start, run_end - run_start, false);
						}
						run_start = lo;
						run_end   = hi;
					}
					continue;
				}
				if (run_end > run_start) {
					host.Decommit(run_start, run_end - run_start, false);
					run_start = run_end = 0;
				}
				if (kind == 2) {
					host.Decommit(lo, hi - lo, true);
				} else if (kind == 3) {
					host.Decommit(lo, hi - lo, false); // converted private window, in its own allocation
				}
			}
			if (run_end > run_start) {
				host.Decommit(run_start, run_end - run_start, false);
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
					// a window the pager swept (no-access) or is copying out (read-only) gets its pages' protections back first: the one asked for
					// applies to pages that are as the guest and the page watchers left them
					if (!Touch(w, host)) {
						return false;
					}
					NoteProtect(w, lo, hi, mode, clears);
					SetModes(r, w, lo, hi, mode);
					if (r.committed[w - (r.start >> kWindowBits)] >= 2) {
						// Views and converted private windows are separate allocations.
						if (!flush() || !host.Protect(lo, hi - lo, mode)) {
							return false;
						}
					} else if (run_end == lo && run_end != run_start) {
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
					SetModes(r, w, lo, hi, mode);
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
		bool           committed = false;
		if (host.Tight(hi - lo)) {
			// close to the limit: one window out for the one that comes in, no more (a batch would throw out what the game just brought back, and the
			// game would do nothing but page in; a failed commit makes room in a batch, below)
			MakeRoom(2, host);
		}
		for (int attempt = 0; attempt < 2 && !committed; ++attempt) {
			// the second attempt follows a failure (the process is at its limit of memory): some windows leave first
			// (A window from the store comes back as a view, and what failed may be the console's limit of mappings: only a view that leaves helps that.)
			if (attempt == 1) {
				const bool from_store = m_evicted.find(w) != m_evicted.end();
				if ((!from_store || MakeRoom(kRoomWindows, host, true) == 0) && MakeRoom(kRoomWindows, host) == 0) {
					break;
				}
			}
			const auto ev = m_evicted.find(w); // (found again: MakeRoom adds to the map)
			if (ev != m_evicted.end()) {
				// the window comes back from the store, as a view (committed value 2: it has to be protected and decommitted on its own)
				if (host.CommitFromStore(w, lo, hi - lo, ev->second)) {
					host.FreeSlot(ev->second);
					m_evicted.erase(ev);
					flag      = 2;
					committed = true;
					NoteReturn(w);
				}
			} else if (host.Commit(lo, hi - lo)) {
				flag      = 1;
				committed = true;
			}
		}
		if (!committed) {
			return CommitResult::Failed;
		}
		{
			// the flags of the window are set before the remembered protections are applied: a fault they cause must find them
			uint8_t bits = 0;
			if (lo == (w << kWindowBits) && hi == ((w + 1) << kWindowBits)) {
				bits = kFull | kTouched; // a window that was just committed (or came back from the store) is in use: the pager leaves it alone for a cycle
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
		ApplyModes(*r, w, lo, hi, Mode::ReadWrite, host);
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
				const auto result = CommitAt(std::max(cursor, w << kWindowBits), host);
				if (result == CommitResult::Failed) {
					return false;
				}
				if (result == CommitResult::AlreadyCommitted && !Touch(w, host)) {
					return false;
				}
				if ((m_flags.Get(w) & kFull) != 0) {
					m_flags.Or(w, kTouched); // and the system may write into it after this call returns: the pager keeps its hands off it for a cycle
				}
			}
			cursor = piece_end;
		}
		return true;
	}

	// ---- The pager (GuestAddressSpace has the thread and the store; this is its bookkeeping). Called under the caller's exclusive lock.
	// A window goes: swept (no-access, kSwept) -> if nobody touches it for a while: read-only while it is copied out (kEvicting) -> out of memory
	// (kEvicted, its contents in a slot). A touch at any point before the last step brings it back to read-write and aborts the way out.

	// Windows the pager never sweeps: the stacks of threads and fibers. A no-access page under the stack pointer cannot be recovered: Windows
	// pushes the exception frame onto that very stack, fails, and ends the process (a thread that comes back from a wait with its stack swept dies
	// on its first `ret`). Ranges are in windows and merge when they touch. Under the caller's exclusive lock, like everything that changes.
	void Pin(uint64_t address, uint64_t size) {
		if (size == 0) {
			return;
		}
		uint64_t first = address >> kWindowBits;
		uint64_t end   = ((address + size - 1) >> kWindowBits) + 1; // exclusive
		auto     it    = m_pins.upper_bound(first);
		if (it != m_pins.begin()) {
			const auto before = std::prev(it);
			if (before->second >= first) {
				first = before->first;
				end   = std::max(end, before->second);
				it    = m_pins.erase(before);
			}
		}
		while (it != m_pins.end() && it->first <= end) {
			end = std::max(end, it->second);
			it  = m_pins.erase(it);
		}
		m_pins[first] = end;
	}
	// The same for the length of a host call that reads or writes a guest buffer through the system (a file read into guest memory: the system cannot
	// fault a window back in, so the window must not be swept or evicted until the call returns). Returns an id for TempUnpin.
	uint64_t TempPin(uint64_t address, uint64_t size) {
		if (size == 0) {
			return 0;
		}
		const uint64_t id = ++m_temp_id;
		m_temps.push_back({id, address >> kWindowBits, ((address + size - 1) >> kWindowBits) + 1});
		return id;
	}
	void TempUnpin(uint64_t id) {
		for (auto it = m_temps.begin(); it != m_temps.end(); ++it) {
			if (it->id == id) {
				m_temps.erase(it);
				return;
			}
		}
	}
	[[nodiscard]] bool IsPinned(uint64_t window) const {
		for (const auto& temp: m_temps) {
			if (window >= temp.first && window < temp.end) {
				return true;
			}
		}
		auto it = m_pins.upper_bound(window);
		if (it == m_pins.begin()) {
			return false;
		}
		return window < std::prev(it)->second;
	}
	// Gives back what the pager took from the windows of the range (swept or evicting windows are touched, evicted ones are paged in): for a
	// range that was just pinned and may be in use as a stack already.
	bool Reclaim(uint64_t address, uint64_t size, HostOps& host) {
		if (size == 0) {
			return true;
		}
		bool restored = true;
		for (uint64_t w = address >> kWindowBits; w <= (address + size - 1) >> kWindowBits; ++w) {
			if (!Touch(w, host)) {
				restored = false;
			}
			if (m_evicted.count(w) != 0 && CommitAt(w << kWindowBits, host) == CommitResult::Failed) {
				restored = false;
			}
		}
		return restored;
	}
	// A window the pager may sweep: committed as a whole, no page no-access, not swept or on its way out, and not a stack. Read-only pages do
	// not keep it: a page watcher made them so to see writes (the GPU caches hold a copy of the game's textures and buffers), and that
	// memory is what the game touches least. While the pager holds such a window, a write faults as before; the window gets its pages'
	// protections back (Touch), and the write faults again, for the watcher. A no-access page keeps the window in memory: the pager copies
	// a window out through a read-only state, and that page must not be readable.
	[[nodiscard]] bool CanSweep(uint64_t window) const {
		const uint8_t f = m_flags.Get(window);
		return (f & kFull) != 0 && (f & Held()) == 0 && (f & kSwept) == 0 && !IsPinned(window);
	}
	// (Off: a window with any protected page stays, as before.)
	void SetTakeWatched(bool on) { m_take_watched = on; }
	// Sweeps the window unless it was touched since the last time the pager looked (then it only forgets the touch: the window gets its chance
	// in the next cycle).
	// A shielded window (it came back soon after it left, see NoteReturn) is not swept to leave either, unless `shielded_too`: the shield is
	// a preference, and gives way when the other windows are not enough. `waits`: see Sweep.
	bool SweepIfCold(uint64_t window, HostOps& host, bool waits = true, bool shielded_too = false) {
		if (!CanSweep(window)) {
			return false;
		}
		if ((m_flags.Get(window) & kTouched) != 0) {
			m_flags.And(window, static_cast<uint8_t>(~kTouched));
			return false;
		}
		if (waits && !shielded_too && Shielded(window)) {
			return false;
		}
		return Sweep(window, host, waits);
	}
	// Makes the window no-access and waits to see whether it is touched (a fault, or a host access, calls Touch). With `waits` the window joins
	// the swept windows that wait to leave (Coldest); without, the caller keeps track of it.
	bool Sweep(uint64_t window, HostOps& host, bool waits = true) {
		if (!CanSweep(window)) {
			return false;
		}
		if (waits) {
			const uint64_t now = m_now_ms.load(std::memory_order_relaxed);
			if (m_waiting.size() > 4 * m_swept_at.size() + 4096) {
				DropStaleWaiting();
			}
			m_swept_at[window] = now;
			m_waiting.push_back({now, window});
		}
		m_flags.And(window, static_cast<uint8_t>(~kTouched));
		m_flags.Or(window, kSwept); // before the protection: a fault it causes must find the flag
		if (!host.Protect(window << kWindowBits, kWindow, Mode::NoAccess)) {
			m_flags.And(window, static_cast<uint8_t>(~kSwept));
			m_swept_at.erase(window);
			return false;
		}
		return true;
	}
	// The swept windows wait in the order they were swept: one that is still swept was not touched since. Gives the one that waits longest,
	// if it waited for `min_idle_ms`; it stays swept, and the caller takes it out (BeginEvict) or gives it back (Touch).
	bool Coldest(uint64_t min_idle_ms, uint64_t& window) {
		const uint64_t now = m_now_ms.load(std::memory_order_relaxed);
		while (!m_waiting.empty()) {
			const Waiting first = m_waiting.front();
			const auto    found = m_swept_at.find(first.window);
			if (found == m_swept_at.end() || found->second != first.at_ms) {
				m_waiting.pop_front(); // touched meanwhile (and maybe swept again later: that is another entry)
				continue;
			}
			if (now - first.at_ms < min_idle_ms) {
				return false;
			}
			window = first.window;
			return true;
		}
		return false;
	}
	// How many swept windows wait.
	[[nodiscard]] uint64_t WaitingCount() const { return m_swept_at.size(); }
	// The swept window is read-write again and waits no longer, also when the host could not change its protection (then it stays swept
	// until a touch tries again).
	void GiveBack(uint64_t window, HostOps& host) {
		if (!Touch(window, host)) {
			m_swept_at.erase(window);
		}
	}
	// Gives a window held by the pager its pages' protections back. Other windows need no change.
	bool Touch(uint64_t window, HostOps& host) {
		const uint8_t flags = m_flags.Get(window);
		if ((flags & (kSwept | kEvicting)) == 0) {
			return true;
		}
		const Region* r = Find(window << kWindowBits);
		if (r == nullptr ||
		    !ApplyModes(*r, window, window << kWindowBits, (window + 1) << kWindowBits, (flags & kSwept) != 0 ? Mode::NoAccess : Mode::Read, host)) {
			return false;
		}
		if ((flags & kSwept) != 0) {
			m_swept_at.erase(window);
		}
		m_flags.Store(window, static_cast<uint8_t>((flags | kTouched) & ~(kSwept | kEvicting)));
		return true;
	}
	// A swept window that nobody touched: read-only, so that it can be copied out and a write is noticed. Returns false if no change was made.
	bool BeginEvict(uint64_t window, HostOps& host) {
		const uint8_t f = m_flags.Get(window);
		if ((f & kSwept) == 0 || (f & kTouched) != 0) {
			return false;
		}
		if (!host.Protect(window << kWindowBits, kWindow, Mode::Read)) {
			return false;
		}
		m_swept_at.erase(window);
		m_flags.Store(window, static_cast<uint8_t>((f & ~kSwept) | kEvicting));
		return true;
	}
	// The copy is in `slot`: the window leaves memory unless it was written meanwhile (Touch made it read-write; the slot is then not used).
	// Returns whether the window left.
	bool CommitEvict(uint64_t window, uint32_t slot, HostOps& host) {
		const uint8_t f = m_flags.Get(window);
		if ((f & kEvicting) == 0) {
			return false;
		}
		Region* r = FindMut(window << kWindowBits);
		if (r == nullptr) {
			return false;
		}
		// The window is marked as in the store before it is decommitted: whoever finds it decommitted (a host copy that sees reserved pages) must
		// also see that it is not a window that was never used, which reads as zero. (The map entries first: under a full heap their insert is
		// what fails, and then nothing has changed yet.)
		Away& away  = m_away[window];
		away.left_ms = m_now_ms.load(std::memory_order_relaxed);
		away.store_reads.store(0, std::memory_order_relaxed);
		m_evicted[window] = slot;
		m_flags.Store(window, kEvicted);
		host.Decommit(window << kWindowBits, kWindow, r->committed[window - (r->start >> kWindowBits)] == 2);
		r->committed[window - (r->start >> kWindowBits)] = 0;
		return true;
	}
	// The window on its way out holds nothing but zeros: it leaves without a slot, as a window that was never used (it reads as zero, and a
	// touch commits it again, zero-filled). Nothing is written now and nothing is read later. Returns whether the window left.
	bool CommitEvictZero(uint64_t window, HostOps& host) {
		if ((m_flags.Get(window) & kEvicting) == 0) {
			return false;
		}
		Region* r = FindMut(window << kWindowBits);
		if (r == nullptr) {
			return false;
		}
		m_flags.Store(window, 0);
		host.Decommit(window << kWindowBits, kWindow, r->committed[window - (r->start >> kWindowBits)] == 2);
		r->committed[window - (r->start >> kWindowBits)] = 0;
		return true;
	}

	// ---- Choosing what leaves. (The rules follow the swap of the Jadyx emulator; the code is ours.)
	// The pager's clock in milliseconds, set by its thread once per round: what "how long ago" means here.
	void SetNow(uint64_t ms) { m_now_ms.store(ms, std::memory_order_relaxed); }
	// A window that comes back within kQuickReturnMs of leaving was in use: the pager took it too early. It is shielded, that is not swept,
	// for kShieldMs, and for twice as long with every further quick return (up to kShieldMaxMs); a return after a long time halves that again.
	// MakeRoom (a failed commit) does not ask.
	static constexpr uint64_t kQuickReturnMs = 30000;
	static constexpr uint64_t kShieldMs      = 15000;
	static constexpr uint64_t kShieldMaxMs   = 240000;
	void                      SetShield(bool on) { m_shield = on; }
	[[nodiscard]] bool        Shielded(uint64_t window) const {
		if (!m_shield) {
			return false;
		}
		const auto found = m_away.find(window);
		return found != m_away.end() && found->second.shield_until_ms > m_now_ms.load(std::memory_order_relaxed);
	}
	// Windows that came back quickly, since the start.
	[[nodiscard]] uint64_t QuickReturns() const { return m_quick_returns.load(std::memory_order_relaxed); }
	// The emulator read a window in the store without bringing it back (a host copy: the GPU's reads of the game's buffers and textures).
	// Reads cause no fault, so a window that only the emulator reads looks cold and leaves, and then every read of it goes to the store.
	// True from the kStoreReads-th read since the window left: it is in use, and the caller brings it back. Under the caller's shared lock
	// (the count is atomic; windows enter and leave the map under the exclusive lock only).
	static constexpr uint8_t kStoreReads = 4;
	[[nodiscard]] bool       NoteStoreRead(uint64_t window) const {
		const auto found = m_away.find(window);
		if (found == m_away.end()) {
			return false;
		}
		const uint8_t reads = found->second.store_reads.load(std::memory_order_relaxed);
		if (reads < UINT8_MAX) {
			found->second.store_reads.store(static_cast<uint8_t>(reads + 1), std::memory_order_relaxed);
		}
		return reads + 1 >= kStoreReads;
	}
	// A commit failed: the process is at its limit of memory (the guest shares it with the GPU and the heap, and those grow without a budget).
	// Evicts up to `count` windows on the spot, the coldest it finds (swept ones first in effect: they were not touched for a while; then the ones
	// not touched since the last look; then any), so that the commit can be tried again. Under the caller's exclusive lock, which also holds off
	// every thread that writes to a window while it is copied: that one waits for the lock at its fault. Returns how many windows left.
	uint64_t MakeRoom(uint64_t count, HostOps& host, bool views_only = false) {
		uint64_t done     = 0;
		int      failures = 0; // in a row: when the host cannot spill (no store), trying 128 windows each time would only cost
		bool     give_up  = false;
		// The swept windows first, the one that waits longest before the others: nobody touched them since.
		for (uint64_t w = 0; !views_only && done < count && !give_up && Coldest(0, w);) {
			if (EvictNow(w, host)) {
				++done;
				failures = 0;
			} else {
				GiveBack(w, host);
				give_up = ++failures >= 16;
			}
		}
		for (int pass = 0; pass < 2 && done < count && !give_up; ++pass) {
			const uint64_t start   = m_room_hand;
			uint64_t       from    = start;
			bool           wrapped = false;
			for (;;) {
				bool end_of_lap = false;
				ForEachFullWindow(from, [&](uint64_t w) {
					if (wrapped && w >= start) {
						end_of_lap = true;
						return true;
					}
					m_room_hand = w + 1;
					const uint8_t f = m_flags.Get(w);
					if ((f & kFull) == 0 || (f & Held()) != 0 || IsPinned(w) || (views_only && !IsView(w))) {
						return false;
					}
					if (pass == 0 && (f & (kSwept | kTouched)) == kTouched) {
						m_flags.And(w, static_cast<uint8_t>(~kTouched)); // used lately: it gets its chance on the second pass
						return false;
					}
					if (EvictNow(w, host)) {
						++done;
						failures = 0;
					} else if (++failures >= 16) {
						give_up = true;
						return true;
					}
					return done >= count;
				});
				if (end_of_lap || done >= count || wrapped || give_up) {
					break;
				}
				wrapped = true;
				from    = 0;
			}
		}
		m_room_evicted += done;
		return done;
	}
	[[nodiscard]] uint64_t RoomEvicted() const { return m_room_evicted.load(std::memory_order_relaxed); }
	// Why on-the-spot evictions (MakeRoom) failed so far: the protection, the copy to the store, the heap (the map entry).
	[[nodiscard]] std::array<uint64_t, 3> RoomFailures() const { return m_room_failures; }

	// Is the window a view of a section of its own (it came back from the store and is still there)? Under the caller's lock.
	[[nodiscard]] bool IsView(uint64_t window) const {
		const Region* r = Find(window << kWindowBits);
		return r != nullptr && r->committed[window - (r->start >> kWindowBits)] == 2;
	}
	// A view on its way out (BeginEvict made it read-only, `data` is a copy of it that equals it) becomes private memory instead of leaving: the pager
	// does this when too many views exist. False: it is not a view on its way out, or the host could not do it (Touch gives it back then).
	bool CommitPrivatize(uint64_t window, const void* data, HostOps& host) {
		if ((m_flags.Get(window) & kEvicting) == 0) {
			return false;
		}
		Region* r = FindMut(window << kWindowBits);
		if (r == nullptr) {
			return false;
		}
		auto& flag = r->committed[window - (r->start >> kWindowBits)];
		if (flag != 2 || !host.Privatize(window << kWindowBits, kWindow, data)) {
			return false;
		}
		flag = 3; // private memory that is an allocation of its own (neighbours are not one call with it)
		// Read-write now, and in use for a cycle; its pages' protections come back.
		m_flags.Store(window, static_cast<uint8_t>(kFull | kTouched | (m_flags.Get(window) & (kProt | kNoAccess))));
		ApplyModes(*r, window, window << kWindowBits, (window + 1) << kWindowBits, Mode::ReadWrite, host);
		return true;
	}

	// Lock-free: is the window of the address in the store?
	[[nodiscard]] bool IsEvicted(uint64_t address) const { return (m_flags.Get(address >> kWindowBits) & kEvicted) != 0; }
	// The slot of a window in the store (under the lock).
	[[nodiscard]] bool EvictedSlot(uint64_t window, uint32_t& slot) const {
		const auto it = m_evicted.find(window);
		if (it == m_evicted.end()) {
			return false;
		}
		slot = it->second;
		return true;
	}
	[[nodiscard]] size_t EvictedCount() const { return m_evicted.size(); }
	// The committed windows by what the pager may do with them, for its report (under the caller's lock). A page watcher's protection keeps a
	// window in memory: `write_watched` has read-only pages, `read_watched` has no-access pages too.
	struct Census {
		uint64_t free = 0, write_watched = 0, read_watched = 0, pinned = 0, partial = 0;
	};
	[[nodiscard]] Census Count() const {
		Census census;
		for (const auto& [start, r]: m_regions) {
			for (uint64_t w = r.start >> kWindowBits; w <= (r.end - 1) >> kWindowBits; ++w) {
				if (r.committed[w - (r.start >> kWindowBits)] == 0) {
					continue;
				}
				const uint8_t f = m_flags.Get(w);
				if ((f & kFull) == 0) {
					++census.partial;
				} else if (IsPinned(w)) {
					++census.pinned;
				} else if ((f & kNoAccess) != 0) {
					++census.read_watched;
				} else if ((f & kProt) != 0) {
					++census.write_watched;
				} else {
					++census.free;
				}
			}
		}
		return census;
	}
	// Calls visit(window) for the windows committed as a whole, in address order, from the window `from` on, until visit returns true.
	template <class Visit>
	void ForEachFullWindow(uint64_t from, Visit&& visit) const {
		for (const auto& [start, r]: m_regions) {
			if ((r.end >> kWindowBits) < from) {
				continue;
			}
			for (uint64_t w = std::max(from, r.start >> kWindowBits); w <= (r.end - 1) >> kWindowBits; ++w) {
				if (r.committed[w - (r.start >> kWindowBits)] != 0 && (m_flags.Get(w) & kFull) != 0 && visit(w)) {
					return;
				}
			}
		}
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
	// Every window is fully committed with no guest or watcher no-access pages. Pager faults can still occur.
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
	// Every window is fully committed with no guest or watcher protection. The caller pins it before writing.
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
	// Remembers the mode of the pages [lo, hi) of the window. Only windows with a page that is not read-write have an entry.
	static void SetModes(Region& r, uint64_t w, uint64_t lo, uint64_t hi, Mode mode) {
		auto found = r.modes.find(w);
		if (found == r.modes.end()) {
			if (mode == Mode::ReadWrite) {
				return;
			}
			std::array<uint8_t, kPagesPerWindow> fresh;
			fresh.fill(static_cast<uint8_t>(Mode::ReadWrite));
			found = r.modes.emplace(w, fresh).first;
		}
		auto& modes = found->second;
		for (uint64_t p = lo; p < hi; p += kPage) {
			modes[(p - (w << kWindowBits)) / kPage] = static_cast<uint8_t>(mode);
		}
		if (std::all_of(modes.begin(), modes.end(), [](uint8_t m) { return m == static_cast<uint8_t>(Mode::ReadWrite); })) {
			r.modes.erase(found);
		}
	}
	// Gives the pages [lo, hi) of a committed window the modes remembered for them, one host call per run of pages with the same mode. All
	// of them are `from` now, and the runs that want just that are left alone. No page is more open than it should be at any time.
	static bool ApplyModes(const Region& r, uint64_t w, uint64_t lo, uint64_t hi, Mode from, HostOps& host) {
		const auto found = r.modes.find(w);
		if (found == r.modes.end()) {
			return from == Mode::ReadWrite || host.Protect(lo, hi - lo, Mode::ReadWrite);
		}
		const auto& modes     = found->second;
		bool        ok        = true;
		uint64_t    run_start = lo;
		Mode        run_mode  = ModeOf(modes, w, lo);
		for (uint64_t p = lo + kPage; p <= hi; p += kPage) {
			if (p == hi || ModeOf(modes, w, p) != run_mode) {
				if (run_mode != from) {
					ok = host.Protect(run_start, p - run_start, run_mode) && ok;
				}
				if (p < hi) {
					run_start = p;
					run_mode  = ModeOf(modes, w, p);
				}
			}
		}
		return ok;
	}
	// The flags of a window the pager leaves in memory.
	[[nodiscard]] uint8_t Held() const { return static_cast<uint8_t>(kNoAccess | kEvicting | (m_take_watched ? 0 : kProt)); }

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

	// A window came back from the store: was it away for a short time only?
	void NoteReturn(uint64_t window) {
		const auto found = m_away.find(window);
		if (found == m_away.end() || found->second.left_ms == 0) {
			return;
		}
		Away&          away = found->second;
		const uint64_t now  = m_now_ms.load(std::memory_order_relaxed);
		const uint64_t gone = now - away.left_ms;
		away.left_ms        = 0;
		if (gone < kQuickReturnMs) {
			away.quick           = static_cast<uint8_t>(std::min<int>(away.quick + 1, 8));
			away.shield_until_ms = now + std::min(kShieldMs << (away.quick - 1), kShieldMaxMs);
			m_quick_returns.fetch_add(1, std::memory_order_relaxed);
		} else if (gone > 4 * kQuickReturnMs) {
			away.quick >>= 1;
		}
	}
	// Entries of windows that no longer wait pile up in front of one that does: they are taken out when they are most of the queue.
	void DropStaleWaiting() {
		std::deque<Waiting> kept;
		for (const Waiting& entry: m_waiting) {
			const auto found = m_swept_at.find(entry.window);
			if (found != m_swept_at.end() && found->second == entry.at_ms) {
				kept.push_back(entry);
			}
		}
		m_waiting.swap(kept);
	}

	std::map<uint64_t, Region>              m_regions;
	WindowFlags                             m_flags;
	std::unordered_map<uint64_t, uint32_t>  m_evicted; // window -> slot of the store
	struct Waiting {
		uint64_t at_ms, window;
	};
	std::deque<Waiting>                    m_waiting;  // the swept windows in the order they were swept (entries of windows touched since stay until they are in front)
	std::unordered_map<uint64_t, uint64_t> m_swept_at; // a swept window that waits -> when it was swept
	struct Away {
		uint64_t left_ms = 0, shield_until_ms = 0; // when it left (0: it is back); until when the pager leaves it alone
		uint8_t  quick = 0;                        // quick returns in a row
		mutable std::atomic<uint8_t> store_reads {0}; // reads of it by the emulator while it is in the store (NoteStoreRead)
	};
	std::unordered_map<uint64_t, Away> m_away; // windows that left memory at some time
	std::atomic<uint64_t>              m_now_ms {0};
	std::atomic<uint64_t>              m_quick_returns {0};
	bool                               m_shield       = true;
	bool                               m_take_watched = true;
	std::map<uint64_t, uint64_t>            m_pins;    // first window -> one past the last window of a range the pager leaves alone (stacks)
	struct TempRange {
		uint64_t id, first, end;
	};
	std::vector<TempRange> m_temps; // buffers of host calls in progress
	uint64_t               m_temp_id = 0;

	// MakeRoom: a window leaves memory right now (the pager thread does this in steps, outside the lock).
	bool EvictNow(uint64_t window, HostOps& host) {
		if ((m_flags.Get(window) & kSwept) != 0) {
			if (!BeginEvict(window, host)) {
				m_room_failures[0]++;
				return false;
			}
		} else {
			m_flags.Or(window, kEvicting); // before the protection: a fault it causes must find the flag
			if (!host.Protect(window << kWindowBits, kWindow, Mode::Read)) {
				m_flags.And(window, static_cast<uint8_t>(~kEvicting));
				m_room_failures[0]++;
				return false;
			}
		}
		uint32_t slot = 0;
		if (!host.Spill(window, window << kWindowBits, kWindow, slot)) {
			Touch(window, host); // read-write again
			m_room_failures[1]++;
			return false;
		}
		bool evicted = false;
#if defined(__cpp_exceptions)
		try {
			evicted = CommitEvict(window, slot, host);
		} catch (const std::bad_alloc&) {
			// the heap is full too (the map entry): the window stays, read-write again
			Touch(window, host);
			m_room_failures[2]++;
		}
#else
		evicted = CommitEvict(window, slot, host);
#endif
		if (!evicted) {
			host.FreeSlot(slot);
			return false;
		}
		return true;
	}

	static constexpr uint64_t kRoomWindows = 128; // 8 MiB: what one failed commit makes room for
	uint64_t                  m_room_hand    = 0;
	std::atomic<uint64_t>     m_room_evicted {0};
	std::array<uint64_t, 3>   m_room_failures {}; // (RoomFailures; under the caller's lock)
};

} // namespace Xbox::Lazy

#endif
