// Internal memory address-space helpers included by memory.cpp.
// Kept in the same translation unit because they depend on memory.cpp-local helpers.

static constexpr uint64_t HOST_SYSTEM_MANAGED_MIN  = 0x0000040000ull;
static constexpr uint64_t HOST_SYSTEM_MANAGED_MAX  = 0x07ffffbfffull;
#if defined(__APPLE__) && (defined(__x86_64__) || defined(_M_X64))
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fbfffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x7000000000ull;
#else
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fffffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x1000000000ull;
#endif
static constexpr uint64_t HOST_USER_MAX = 0xfbffffffffull;

static bool GuestFreeRangeContains(uint64_t base, uint64_t range_size, uint64_t candidate,
                                   uint64_t size) {
	if (UINT64_MAX - base < range_size || candidate < base) {
		return false;
	}
	const auto range_end = base + range_size;
	return candidate <= range_end && size <= range_end - candidate;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static bool CoalescePlaceholders(uint64_t vaddr, uint64_t size) {
	return size != 0 && VirtualFree(reinterpret_cast<void*>(vaddr), size,
	                                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0;
}
#endif

template <typename Regions, typename Region, typename Gap>
static void ForEachSparseRange(const Regions& regions, uint64_t vaddr, uint64_t size,
                               Region&& region, Gap&& gap) {
	const auto end     = vaddr + size;
	auto       mapping = regions.lower_bound(vaddr);
	if (mapping != regions.begin()) {
		auto previous = std::prev(mapping);
		if (previous->second.vaddr + previous->second.size > vaddr) {
			mapping = previous;
		}
	}

	auto cursor = vaddr;
	for (; mapping != regions.end() && mapping->second.vaddr < end; ++mapping) {
		const auto& entry = mapping->second;
		const auto  start = std::max(vaddr, entry.vaddr);
		const auto  bytes = std::min(end, entry.vaddr + entry.size) - start;
		if (cursor < start) {
			gap(cursor, start - cursor);
		}
		region(entry, start, bytes);
		cursor = start + bytes;
	}
	if (cursor < end) {
		gap(cursor, end - cursor);
	}
}

template <typename Regions, typename Source, typename Gap>
static void CopySparseRange(const Regions& regions, uint64_t vaddr, void* data, uint64_t size,
                            Source&& source, Gap&& gap) {
	auto* destination = static_cast<uint8_t*>(data);
	ForEachSparseRange(
	    regions, vaddr, size,
	    [&](const auto& entry, uint64_t start, uint64_t bytes) {
		    std::memcpy(destination + (start - vaddr), source(entry, start), bytes);
	    },
	    [&](uint64_t start, uint64_t bytes) { gap(start, destination + (start - vaddr), bytes); });
}

class GuestBackingStore {
public:
	enum class FailureReason {
		None,
		BackingUnavailable,
		PhysicalRangeOutOfBounds,
		FixedMapViewFailed,
		PlaceholderMapFailed,
		ProtectFailed,
		WrongHostAddress,
		AddressRangeUnavailable,
	};

	explicit GuestBackingStore(uint64_t size): m_size(size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_handle = VirtualMemory::Windows::CreateGuestSection(m_size);
		if (m_handle == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: CreateFileMapping failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			return;
		}
		m_backing_base = static_cast<uint8_t*>(VirtualMemory::Windows::MapSectionAlias(m_handle, m_size));
		if (m_backing_base == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: writable alias failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			CloseHandle(m_handle);
			m_handle = nullptr;
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#if defined(__APPLE__)
		{
			char shm_name[64];
			snprintf(shm_name, sizeof(shm_name), "/kyty-dm-%d-%llx", static_cast<int>(getpid()),
			         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this)));
			m_fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (m_fd >= 0) {
				shm_unlink(shm_name);
			}
		}
#else
		m_fd = static_cast<int>(syscall(SYS_memfd_create, "KytyDirectMemory", 0));
#endif
		if (m_fd < 0) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: shared fd creation failed: %d\n",
			           errno);
			return;
		}
		if (ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: ftruncate failed: %d\n", errno);
			close(m_fd);
			m_fd = -1;
			return;
		}
		auto* backing = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
		if (backing == MAP_FAILED) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: writable alias failed: %d\n",
			           errno);
			close(m_fd);
			m_fd = -1;
			return;
		}
		m_backing_base = static_cast<uint8_t*>(backing);
#endif
	}

	~GuestBackingStore() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [_, entry]: m_maps) {
			UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size);
		}
		m_maps.clear();
		if (m_backing_base != nullptr) {
			UnmapViewOfFile(m_backing_base);
		}
		if (m_handle != nullptr) {
			CloseHandle(m_handle);
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		for (const auto& [_, entry]: m_maps) {
			EXIT_IF(
			    !UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size));
		}
		m_maps.clear();
		if (m_backing_base != nullptr) {
			munmap(m_backing_base, m_size);
		}
		if (m_fd >= 0) {
			close(m_fd);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestBackingStore);

	static const char* GetFailureReasonName(FailureReason reason) {
		switch (reason) {
			case FailureReason::None: return "n/a";
			case FailureReason::BackingUnavailable: return "backing-unavailable";
			case FailureReason::PhysicalRangeOutOfBounds: return "physical-range-out-of-bounds";
			case FailureReason::FixedMapViewFailed: return "fixed-map-view-failed";
			case FailureReason::PlaceholderMapFailed: return "placeholder-map-failed";
			case FailureReason::ProtectFailed: return "protect-failed";
			case FailureReason::WrongHostAddress: return "wrong-host-address";
			case FailureReason::AddressRangeUnavailable: return "address-range-unavailable";
			default: return "unknown";
		}
	}

	[[nodiscard]] bool IsAvailable() const {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return m_handle != nullptr && m_backing_base != nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		return m_fd >= 0 && m_backing_base != nullptr;
#else
		return false;
#endif
	}

	[[nodiscard]] uint64_t GetBackingBase() const {
		return reinterpret_cast<uint64_t>(m_backing_base);
	}

	[[nodiscard]] uint64_t GetBackingSize() const { return m_size; }

	bool ZeroRange(uint64_t backing_offset, uint64_t size) {
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		std::memset(m_backing_base + backing_offset, 0, size);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		g_test_backing_bytes_zeroed += size;
#endif
		return true;
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		// Skip unbacked gaps, mirroring sparse reads that zero-fill them.
		std::lock_guard lock(m_mutex);
		const auto*     source = static_cast<const uint8_t*>(data);
		ForEachSparseRange(
		    m_maps, vaddr, size,
		    [&](const auto& entry, uint64_t start, uint64_t bytes) {
			    std::memcpy(m_backing_base + (entry.backing_offset + start - entry.vaddr),
			                source + (start - vaddr), bytes);
		    },
		    [](uint64_t, uint64_t) {});
		return true;
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		const auto end   = vaddr + size;
		auto       first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			const auto& entry = mapping->second;
			const auto  bytes = std::min(end, entry.vaddr + entry.size) - current;
			if (!RangeOk(entry.backing_offset + current - entry.vaddr, bytes)) {
				return false;
			}
			current += bytes;
		}

		auto* destination = static_cast<uint8_t*>(data);
		current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - current;
			const auto  offset = entry.backing_offset + current - entry.vaddr;
			std::memcpy(destination + (current - vaddr), m_backing_base + offset, bytes);
			current += bytes;
		}
		return true;
	}

	template <typename Gap>
	bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size, Gap&& gap) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		CopySparseRange(m_maps, vaddr, data, size,
		                [this](const auto& entry, uint64_t start) {
			                return m_backing_base + (entry.backing_offset + start - entry.vaddr);
		                },
		                gap);
		return true;
	}

	bool ProtectViews(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		std::shared_lock lock(m_mutex);
		bool             ok = true;
		ForEachSparseRange(
		    m_maps, vaddr, size,
		    [&](const auto&, uint64_t start, uint64_t bytes) {
			    ok = ok && VirtualMemory::Protect(start, bytes, mode);
		    },
		    [&](uint64_t, uint64_t) { ok = false; });
		return ok;
	}

	bool MapFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	              VirtualMemory::Mode mode, FailureReason* failure_reason = nullptr) {
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			SetFailure(failure_reason, !IsAvailable() ? FailureReason::BackingUnavailable
			                                          : FailureReason::PhysicalRangeOutOfBounds);
			return false;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		void* mapped = ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason);
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			if (mapped != nullptr) {
				UnmapViewPreservePlaceholder(mapped, size);
				SetFailure(failure_reason, FailureReason::WrongHostAddress);
			}
			return false;
		}

		{
			std::lock_guard lock(m_mutex);
			EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		}
		SetFailure(failure_reason, FailureReason::None);
		return true;
#else
		std::lock_guard lock(m_mutex);
		void* mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			return false;
		}
		RemoveMappingsUnlocked(vaddr, size);
		EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		SetFailure(failure_reason, FailureReason::None);
		return true;
#endif
	}

	bool Unmap(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		const auto            end = vaddr + size;
		std::vector<MapEntry> targets;
		{
			std::lock_guard lock(m_mutex);
			auto            current = vaddr;
			while (current < end) {
				MapEntry entry {};
				if (!FindContainingUnlocked(current, 1, &entry)) {
					return false;
				}
				const auto part_size = std::min(end, entry.vaddr + entry.size) - current;
				targets.push_back(
				    {current, part_size, entry.backing_offset + current - entry.vaddr, entry.mode});
				current += part_size;
			}
		}

		size_t removed  = 0;
		auto   rollback = [&] {
			while (removed != 0) {
				const auto& previous = targets[--removed];
				EXIT_IF(!MapFixed(previous.vaddr, previous.size, previous.backing_offset,
				                  previous.mode));
			}
		};
		for (const auto& entry: targets) {
			if (!UnmapOne(entry.vaddr, entry.size)) {
				rollback();
				return false;
			}
			++removed;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Each unmapped file view becomes a separate Windows placeholder. The address-space owner
		// records this contiguous guest span as one free range, so normalize fragmented backing
		// views into one placeholder before publishing it there.
		if (removed > 1 && !CoalescePlaceholders(vaddr, size)) {
			rollback();
			return false;
		}
#endif

		return true;
	}

	bool UnmapOne(uint64_t vaddr, uint64_t size) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_store_unmaps_before_failure == 0) {
			g_test_backing_store_unmaps_before_failure = UINT32_MAX;
			return false;
		}
		if (g_test_backing_store_unmaps_before_failure != UINT32_MAX) {
			g_test_backing_store_unmaps_before_failure--;
		}
#endif

		MapEntry old {};
		{
			std::lock_guard lock(m_mutex);
			auto            next = m_maps.upper_bound(vaddr);
			if (next != m_maps.begin()) {
				auto entry = std::prev(next);
				if (vaddr + size <= entry->second.vaddr + entry->second.size) {
					old = entry->second;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					m_maps.erase(entry);
#endif
				}
			}
		}

		if (old.size == 0) {
			return false;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const uint64_t left_size  = vaddr - old.vaddr;
		const uint64_t right_addr = vaddr + size;
		const uint64_t right_size = old.vaddr + old.size - right_addr;
		const bool exact_unmap = (vaddr == old.vaddr && size == old.size);
		if (exact_unmap) {
			const bool unmapped =
			    UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size);
			if (!unmapped) {
				std::lock_guard lock(m_mutex);
				m_maps.emplace(old.vaddr, old);
				return false;
			}

			return true;
		}

		if (UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size)) {
			bool ok = true;
			if (left_size != 0 && left_size != old.size) {
				ok = SplitPlaceholder(old.vaddr, left_size) && ok;
			}
			if (right_size != 0) {
				ok = SplitPlaceholder(vaddr, size) && ok;
			}
			if (left_size != 0) {
				ok = MapFixed(old.vaddr, left_size, old.backing_offset, old.mode) && ok;
			}
			if (right_size != 0) {
				ok = MapFixed(right_addr, right_size,
				              old.backing_offset + (right_addr - old.vaddr), old.mode) &&
				     ok;
			}
			if (ok) {
				return true;
			}

			if (left_size != 0 && Contains(old.vaddr, left_size)) {
				EXIT_IF(!UnmapOne(old.vaddr, left_size));
			}
			if (right_size != 0 && Contains(right_addr, right_size)) {
				EXIT_IF(!UnmapOne(right_addr, right_size));
			}
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(old.vaddr), old.size,
			                    MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) == 0);
			EXIT_IF(!MapFixed(old.vaddr, old.size, old.backing_offset, old.mode));
			return false;
		}
		std::lock_guard lock(m_mutex);
		m_maps.emplace(old.vaddr, old);
		return false;
#else
		// Replace only the requested range so concurrent accesses and GPU watch
		// protections on the surviving mappings remain intact.
		if (!UnmapViewPreservePlaceholder(reinterpret_cast<void*>(vaddr), size)) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		RemoveMappingsUnlocked(vaddr, size);

		return true;
#endif
	}

	bool Contains(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
		const auto      end     = vaddr + size;
		auto            current = vaddr;

		while (current < end) {
			MapEntry entry {};
			if (!FindContainingUnlocked(current, 1, &entry)) {
				return false;
			}
			const auto entry_end = entry.vaddr + entry.size;
			if (entry_end <= current) {
				return false;
			}
			current = std::min(end, entry_end);
		}

		return true;
	}

private:
	struct MapEntry {
		uint64_t            vaddr          = 0;
		uint64_t            size           = 0;
		uint64_t            backing_offset = 0;
		VirtualMemory::Mode mode           = VirtualMemory::Mode::NoAccess;
	};

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	void RemoveMappingsUnlocked(uint64_t vaddr, uint64_t size) {
		const auto end = vaddr + size;
		auto       it = m_maps.upper_bound(vaddr);
		if (it != m_maps.begin()) {
			--it;
		}
		while (it != m_maps.end() && it->first < end) {
			const auto old     = it->second;
			const auto old_end = old.vaddr + old.size;
			if (old_end <= vaddr) {
				++it;
				continue;
			}
			it = m_maps.erase(it);
			if (old.vaddr < vaddr) {
				m_maps.emplace(old.vaddr,
				               MapEntry {old.vaddr, vaddr - old.vaddr, old.backing_offset, old.mode});
			}
			if (end < old_end) {
				m_maps.emplace(end, MapEntry {end, old_end - end,
				                             old.backing_offset + end - old.vaddr, old.mode});
			}
		}
	}
#endif

	bool FindContainingUnlocked(uint64_t vaddr, uint64_t size, MapEntry* out) const {
		EXIT_IF(out == nullptr);

		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		auto next = m_maps.upper_bound(vaddr);
		if (next == m_maps.begin()) {
			return false;
		}
		const auto& entry = std::prev(next)->second;
		if (vaddr + size <= entry.vaddr + entry.size) {
			*out = entry;
			return true;
		}

		return false;
	}

	[[nodiscard]] bool RangeOk(uint64_t backing_offset, uint64_t size) const {
		return size != 0 && backing_offset < m_size && size <= m_size - backing_offset;
	}

	static void SetFailure(FailureReason* out, FailureReason reason) {
		if (out != nullptr) {
			*out = reason;
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static bool ApplyProtection(void* ptr, uint64_t size, VirtualMemory::Mode mode) {
		return VirtualMemory::Protect(reinterpret_cast<uint64_t>(ptr), size, mode);
	}

	static bool SplitPlaceholder(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return true;
		}
		return VirtualFree(reinterpret_cast<void*>(vaddr), size,
		                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	void* ReplacePlaceholderView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                             VirtualMemory::Mode mode, FailureReason* failure_reason) {
		void* ptr = VirtualMemory::Windows::MapPlaceholderView(m_handle, vaddr, backing_offset, size, mode);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			}
			SetFailure(failure_reason, FailureReason::PlaceholderMapFailed);
			return nullptr;
		}

		if (mode == VirtualMemory::Mode::NoAccess && !ApplyProtection(ptr, size, mode)) {
			UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			SetFailure(failure_reason, FailureReason::ProtectFailed);
			return nullptr;
		}
		return ptr;
	}

	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t /*size*/) {
		return UnmapViewOfFile2(GetCurrentProcess(), vaddr, MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	HANDLE m_handle = nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	static int GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PROT_READ;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::Execute: return PROT_EXEC;
			case VirtualMemory::Mode::ExecuteRead: return PROT_EXEC | PROT_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PROT_EXEC | PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PROT_NONE;
		}
	}

	void* MapView(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	              FailureReason* failure_reason) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (const auto callback = g_test_before_backing_map) {
			g_test_before_backing_map = nullptr;
			callback(vaddr, size);
		}
#endif
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, GetProtect(mode),
		                 MAP_SHARED | MAP_FIXED, m_fd, static_cast<off_t>(backing_offset));
		if (ptr == MAP_FAILED) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			return nullptr;
		}
		return ptr;
	}

	// Replace the file view atomically instead of exposing an unmapped hole in the
	// process-lifetime guest reservation.
	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t size) {
		void* ptr = mmap(vaddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		return ptr != MAP_FAILED && ptr == vaddr;
	}

	int m_fd = -1;
#endif

	uint8_t*                     m_backing_base = nullptr;
	uint64_t                     m_size         = 0;
	std::shared_mutex            m_mutex;
	std::map<uint64_t, MapEntry> m_maps;
};

// The pager's store, for the host ops of the lazy regions (there is one address space per process).
static std::atomic<Xbox::PagerStore*> g_pager_store {nullptr};
// Debugging the pager: what happened to a window (printed for the windows the registers point into when the game crashes).
struct PagerWindowHistory {
	uint32_t evicts = 0, page_ins = 0;
	int64_t  last_evict_ms = 0, last_page_in_ms = 0;
	uint32_t last_page_in_thread = 0;
};
static std::mutex                                      g_history_mutex;
static std::unordered_map<uint64_t, PagerWindowHistory> g_history;
static int64_t                                         HistoryNowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void HistoryEvict(uint64_t window) {
	std::lock_guard lock(g_history_mutex);
	auto&           h = g_history[window];
	h.evicts++;
	h.last_evict_ms = HistoryNowMs();
}
static void HistoryPageIn(uint64_t window) {
	std::lock_guard lock(g_history_mutex);
	auto&           h = g_history[window];
	h.page_ins++;
	h.last_page_in_ms     = HistoryNowMs();
	h.last_page_in_thread = GetCurrentThreadId();
}

static thread_local bool     t_fault_thread = false; // the helper thread that serves faults on windows in the store

class GuestAddressSpace {
public:
	explicit GuestAddressSpace(uint64_t backing_size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		m_granularity = info.dwAllocationGranularity;
#endif
		ReserveGuestRegions();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Xbox fork (T3): lazily committed regions instead of the committed section and its host alias
		m_lazy_enabled = Xbox::Tunables::LazyMemory();
		if (m_lazy_enabled) {
			LOGF_COLOR(Log::Color::Green, "\t lazy guest memory: direct and flexible memory are committed on first touch, no backing section\n");
			// Xbox fork, experiment: the pager (the environment variable wins over the setting)
			auto budget_mb = static_cast<uint64_t>(Xbox::Tunables::GuestRamBudgetMbFromEnvironment());
			if (budget_mb == 0) {
				budget_mb = Config::GuestRamBudgetMb();
			}
			if (budget_mb == Config::kGuestRamBudgetAuto) {
				// not set: where the OS gives the app little memory (the Series X: 5 GiB) the pager is on, with that limit as its cap; elsewhere it is off
				const uint64_t limit = Config::AppMemoryLimit();
				budget_mb            = limit != 0 && limit <= (uint64_t {8} << 30u) ? limit >> 20u : 0;
			}
			if (budget_mb != 0) {
				StartPager(budget_mb << 20u);
			}
			return;
		}
#endif
		m_backing = new GuestBackingStore(backing_size);
		if (!m_backing->IsAvailable()) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory. Windows commits\n"
			     "this up front, so the paging file is usually what needs to be larger.\n",
			     backing_size / (1024U * 1024U));
#else
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory\n",
			     backing_size / (1024U * 1024U));
#endif
		}
	}

	~GuestAddressSpace() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		StopPager();
#endif
		delete m_backing;
		m_backing = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [start, size]: m_owned) {
			const auto end     = start + size;
			auto       current = start;
			while (current < end) {
				MEMORY_BASIC_INFORMATION info {};
				EXIT_IF(VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ==
				        0);
				const auto region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end   = region_start + static_cast<uint64_t>(info.RegionSize);
				if (info.State == MEM_COMMIT) {
					if (info.Type == MEM_MAPPED) {
						// a view of the pager (a window that came back from its store)
						EXIT_IF(UnmapViewOfFile2(GetCurrentProcess(), info.AllocationBase, MEM_PRESERVE_PLACEHOLDER) == 0);
						continue;
					}
					EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_start), info.RegionSize,
					                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
					continue;
				}
				if (info.State == MEM_RESERVE) {
					EXIT_IF(VirtualFree(info.AllocationBase, 0, MEM_RELEASE) == 0);
				}
				current = region_end;
			}
		}
#else
		for (const auto& [start, size]: m_owned) {
			munmap(reinterpret_cast<void*>(start), size);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestAddressSpace);

	[[nodiscard]] uint64_t GetGranularity() const { return m_granularity; }
	[[nodiscard]] uint64_t GetBackingBase() const { return m_backing != nullptr ? m_backing->GetBackingBase() : 0; }
	[[nodiscard]] uint64_t GetBackingSize() const { return m_backing != nullptr ? m_backing->GetBackingSize() : 0; }
	[[nodiscard]] bool     LazyEnabled() const { return m_lazy_enabled; }

	bool ZeroBacking(uint64_t backing_offset, uint64_t size) {
		if (m_lazy_enabled) {
			return true; // a lazily committed page starts as zero, and unmapping decommits it
		}
		return m_backing->ZeroRange(backing_offset, size);
	}

	[[nodiscard]] bool Owns(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return vaddr >= range.first && vaddr + size <= range.first + range.second;
		});
	}

	[[nodiscard]] bool OverlapsOwned(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return VirtualRangesOverlap(vaddr, size, range.first, range.second);
		});
	}

	// Writes the backed bytes of a range that holds no private mappings.
	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		if (m_lazy_enabled) {
			// Lazy memory has no backing section: the regions' parts of the range are written with LazyCopyIn, which skips the gaps.
			{
				std::shared_lock lock(m_mutex);
				if (OverlapsPrivateUnlocked(vaddr, size)) {
					return false;
				}
			}
			return LazyCopyIn(vaddr, data, size);
		}
		std::shared_lock lock(m_mutex);
		return !OverlapsPrivateUnlocked(vaddr, size) &&
		       m_backing->TryWriteBacking(vaddr, data, size);
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (m_lazy_enabled) {
			return LazyCopyOut(vaddr, data, size, false);
		}
		return m_backing->TryReadBacking(vaddr, data, size);
	}

	template <bool copy_private>
	bool CopySparseMemory(uint64_t vaddr, void* data, uint64_t size) {
		if (data == nullptr || !Owns(vaddr, size)) {
			return false;
		}
		if (m_lazy_enabled) {
			// Lazy memory has no backing section: its regions are read with LazyCopyOut (gaps as zeros), private mappings from the guest address.
			std::vector<std::pair<uint64_t, uint64_t>> privates;
			{
				std::lock_guard lock(m_mutex);
				auto            mapping = m_mapped.upper_bound(vaddr);
				if (mapping != m_mapped.begin()) {
					--mapping;
				}
				for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
					const auto& region = mapping->second;
					if (region.type == MappingType::Private && region.vaddr + region.size > vaddr) {
						if constexpr (!copy_private) {
							return false;
						}
						const auto start = std::max(vaddr, region.vaddr);
						privates.emplace_back(start, std::min(vaddr + size, region.vaddr + region.size) - start);
					}
				}
			}
			if (!LazyCopyOut(vaddr, data, size, true)) {
				return false;
			}
			for (const auto& [start, bytes]: privates) {
				std::memcpy(static_cast<uint8_t*>(data) + (start - vaddr), reinterpret_cast<const void*>(start), bytes);
			}
			return true;
		}
		std::shared_lock lock(m_mutex);
		if constexpr (!copy_private) {
			if (OverlapsPrivateUnlocked(vaddr, size)) {
				return false;
			}
		}
		return m_backing->TryReadSparseBacking(
		    vaddr, data, size, [&](uint64_t start, uint8_t* destination, uint64_t bytes) {
			    const auto zero = [](uint64_t, uint8_t* output, uint64_t length) {
				    std::memset(output, 0, length);
			    };
			    if constexpr (copy_private) {
				    // Backing gaps can contain private mappings or reservations only.
				    CopySparseRange(m_mapped, start, destination, bytes,
				                    [](const auto& region, uint64_t address) {
					                    EXIT_IF(region.type != MappingType::Private);
					                    return reinterpret_cast<const void*>(address);
				                    },
				                    zero);
			    } else {
				    zero(start, destination, bytes);
			    }
		    });
	}

	bool BackingContains(uint64_t vaddr, uint64_t size) {
		if (m_lazy_enabled) {
			std::lock_guard lock(m_mutex);
			return ContainsMappedUnlocked(vaddr, size, MappingType::Backing);
		}
		return m_backing->Contains(vaddr, size);
	}

	// ---- Xbox fork (T3): lazily committed guest memory. The bookkeeping is Xbox::Lazy::Regions (tested on the CPU); this is the Windows side.
	// Locks, in this order: m_copy_lock (shared for host copies, exclusive for unmap), m_mutex (the address space), m_lazy_mutex. The commit fault
	// handler takes only m_lazy_mutex and nothing under m_lazy_mutex touches guest memory, so a fault can never wait for a lock its own
	// thread holds. m_copy_lock keeps host copies and unmapping apart (a copy never runs on decommitted pages).

	// Access violation at an address: commit its window when it is a page of a lazy region that is not committed yet.
	// Returns false when the fault is not ours (a page watcher, a bad pointer), so the next handler sees it.
	bool HandleCommitFault(uint64_t address, bool is_write) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!m_lazy_enabled) {
			return false;
		}
		// Fastest path, no system call and no lock: the window flags (xbox/lazyRegions.h). A window committed as a whole has no commit fault. If some
		// page of it is protected, the fault is a page watcher's (or the guest's own protection): the next handler decides. Otherwise the fault raced
		// with the commit and the access can be retried. (VirtualQuery below takes time in proportion to the run of equal pages around the address:
		// hundreds of microseconds in guest memory, for each of thousands of page watcher faults per second.)
		{
			const auto flags = m_lazy.FlagsOf(address);
			if ((flags & Xbox::Lazy::kFull) != 0) {
				if ((flags & (Xbox::Lazy::kSwept | Xbox::Lazy::kEvicting)) == 0) {
					return (flags & Xbox::Lazy::kProt) == 0;
				}
				// The pager's: a window it swept (no-access) or is copying out (read-only) is touched. It is read-write again and not cold; an
				// eviction in progress notices and gives up. (If another thread was first, there is nothing to do and the access can be retried.)
				std::lock_guard lock(m_lazy_mutex);
				LazyHost        host;
				m_lazy.Touch(address >> Xbox::Lazy::Regions::kWindowBits, host);
				return true;
			}
		}
		// Fast path without any lock: a fault on a page that is committed cannot be a commit fault. It is a page watcher or a bad access, and
		// those are frequent; they must not queue up behind the lock below. (Unless another thread committed this window an instant ago.)
		// Not for a window in the pager's store: another thread may just be bringing it back, and its pages are committed (zero) before they are
		// filled. This fault waits for the lock, and then finds the window ready.
		if ((m_lazy.FlagsOf(address) & Xbox::Lazy::kEvicted) == 0) {
			MEMORY_BASIC_INFORMATION info {};
			if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != 0) {
				if (info.State == MEM_COMMIT) {
					const DWORD protect = info.Protect & 0xFF;
					return protect == PAGE_READWRITE || (protect == PAGE_READONLY && !is_write);
				}
				if (info.State != MEM_RESERVE) {
					return false;
				}
			}
		}
		// Bringing a window back from the store reads a file and fills 16 guard pages (each of them a nested exception): too deep for the stack of the
		// thread that faulted. Exception frames and the frames below them stay on that stack as junk, and the game reads stack it never wrote (a float
		// from an exception frame as a pointer, crashes minutes in). A helper thread does the work and this thread only waits.
		if (m_fault_thread.joinable() && !t_fault_thread && (m_lazy.FlagsOf(address) & Xbox::Lazy::kEvicted) != 0) {
			return RunOnFaultThread(address, is_write);
		}
		return CommitFaultLocked(address, is_write);
#else
		(void)address;
		(void)is_write;
#endif
		return false;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// The part of HandleCommitFault that commits (and pages in) the window.
	bool CommitFaultLocked(uint64_t address, bool is_write) {
		std::lock_guard lock(m_lazy_mutex);
		LazyHost        host;
		switch (m_lazy.CommitAt(address, host)) {
			case Xbox::Lazy::Regions::CommitResult::Committed: return true;
			case Xbox::Lazy::Regions::CommitResult::NotLazy: return false;
			case Xbox::Lazy::Regions::CommitResult::AlreadyCommitted: {
				// committed by another thread an instant ago: retry when the page allows the access now, else it is a watcher fault
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
					return false;
				}
				const DWORD protect = info.Protect & 0xFF;
				return protect == PAGE_READWRITE || (protect == PAGE_READONLY && !is_write);
			}
			case Xbox::Lazy::Regions::CommitResult::Failed:
				EXIT("Memory: out of commit while committing guest memory at 0x%016" PRIx64 " (guest resident %" PRIu64 " MB, error %u, %" PRIu64 " windows evicted on a failed commit, window flags %u)\n", address,
				     Xbox::Lazy::GetCounters().committed.load() >> 20u, static_cast<uint32_t>(GetLastError()), m_lazy.RoomEvicted(), static_cast<uint32_t>(m_lazy.FlagsOf(address)));
		}
#else
		(void)address;
		(void)is_write;
#endif
		return false;
	}


	// The guest memory totals the overlay shows (GuestMemoryTotals in memory.h).
	GuestMemoryTotals GetTotals() {
		GuestMemoryTotals totals;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!m_lazy_enabled) {
			return totals;
		}
		constexpr uint64_t kWindow = Xbox::Lazy::Regions::kWindow;
		totals.resident            = Xbox::Lazy::GetCounters().committed.load(std::memory_order_relaxed);
		if (m_pager_store != nullptr) {
			{
				std::shared_lock lock(m_lazy_mutex);
				totals.swapped = m_lazy.EvictedCount() * kWindow;
			}
			totals.swapped_out = m_pager_store->counters.evicted.load(std::memory_order_relaxed) * kWindow;
			totals.swapped_in  = m_pager_store->counters.page_ins.load(std::memory_order_relaxed) * kWindow;
		}
#endif
		return totals;
	}

	// Debugging the pager: for each value (the registers of a crash) that points into guest memory, what happened to its window.
	void DumpPagerContext(const uint64_t* values, size_t count) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!m_lazy_enabled || m_pager_store == nullptr) {
			return;
		}
		const auto now = HistoryNowMs();
		std::printf("--- Pager context: windows the registers point into ---\n");
		for (size_t i = 0; i < count; ++i) {
			const uint64_t value = values[i];
			if (!m_lazy.Contains(value, 1)) {
				continue;
			}
			const uint64_t      window = value >> Xbox::Lazy::Regions::kWindowBits;
			PagerWindowHistory  h {};
			bool                known = false;
			{
				std::lock_guard lock(g_history_mutex);
				if (const auto it = g_history.find(window); it != g_history.end()) {
					h     = it->second;
					known = true;
				}
			}
			std::printf("  value %zu = 0x%016" PRIx64 " window 0x%" PRIx64 " flags 0x%02x %s: evicts %u, page ins %u, last evict %" PRId64
			            " ms ago, last page in %" PRId64 " ms ago (thread %u)\n",
			            i, value, window, static_cast<unsigned>(m_lazy.FlagsOf(value)), known ? "history" : "never evicted", h.evicts, h.page_ins,
			            h.evicts != 0 ? now - h.last_evict_ms : -1, h.page_ins != 0 ? now - h.last_page_in_ms : -1, h.last_page_in_thread);
		}
		std::fflush(stdout);
#else
		(void)values;
		(void)count;
#endif
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Gives a range of lazy memory (all of it reserved, nothing committed) back as placeholders. A window that came back from the pager's store and
	// went out again is a private reservation of its own, so the range can be several allocations: one call for each. Returns how many (0: failed).
	static size_t ReleaseLazyRange(uint64_t vaddr, uint64_t size) {
		size_t     count  = 0;
		auto       cursor = vaddr;
		const auto end    = vaddr + size;
		while (cursor < end) {
			MEMORY_BASIC_INFORMATION info {};
			if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) {
				return 0;
			}
			const auto region_end = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(info.BaseAddress) + static_cast<uint64_t>(info.RegionSize));
			if (VirtualFree(reinterpret_cast<void*>(cursor), region_end - cursor, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				return 0;
			}
			++count;
			cursor = region_end;
		}
		return count;
	}
#endif

	// A host call is about to read or write this buffer through the system (file and network I/O): committed, brought back from the pager's store, and
	// kept in memory until UnpinBuffer. The pin comes first, so that the pager cannot take a window between the two.
	uint64_t PinBuffer(uint64_t address, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!m_lazy_enabled || size == 0) {
			return 0;
		}
		uint64_t id = 0;
		{
			std::lock_guard lock(m_lazy_mutex);
			id = m_lazy.TempPin(address, size);
		}
		EnsureCommitted(address, size);
		return id;
#else
		(void)address;
		(void)size;
		return 0;
#endif
	}
	void UnpinBuffer(uint64_t id) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (id != 0) {
			std::lock_guard lock(m_lazy_mutex);
			m_lazy.TempUnpin(id);
		}
#else
		(void)id;
#endif
	}

	// A thread or a fiber runs on this memory: the pager must leave it alone (Xbox::Lazy::Regions::Pin), and gives back what it took already.
	void PinRange(uint64_t address, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!m_lazy_enabled || size == 0) {
			return;
		}
		std::lock_guard lock(m_lazy_mutex);
		LazyHost        host;
		m_lazy.Pin(address, size);
		m_lazy.Reclaim(address, size, host);
#else
		(void)address;
		(void)size;
#endif
	}

	// Commits the windows of a range before a host call (file read, network receive) writes into it. Parts that are not lazy are ignored.
	bool EnsureCommitted(uint64_t address, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!m_lazy_enabled || size == 0) {
			return true;
		}
		std::lock_guard lock(m_lazy_mutex);
		LazyHost        host;
		auto            cursor = address;
		const auto      end    = address + size;
		while (cursor < end) {
			const auto length = m_lazy.ContainedLength(cursor, end - cursor);
			if (length == 0) {
				cursor = std::min(end, m_lazy.NextStart(cursor));
				continue;
			}
			if (!m_lazy.EnsureCommitted(cursor, length, host)) {
				return false;
			}
			cursor += length;
		}
#else
		(void)address;
		(void)size;
#endif
		return true;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// A line for the pager's failures and decisions in the emulator log (LOGF_COLOR only reaches the console, which nobody sees on the Series X).
	static void PagerLog(const char* format, ...) {
		char    buffer[512];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		Log::WriteToConsoleAndLog(buffer);
	}

	struct LazyHost final: Xbox::Lazy::HostOps {
		static VirtualMemory::Mode Native(Xbox::Lazy::Mode mode) {
			switch (mode) {
				case Xbox::Lazy::Mode::NoAccess: return VirtualMemory::Mode::NoAccess;
				case Xbox::Lazy::Mode::Read: return VirtualMemory::Mode::Read;
				case Xbox::Lazy::Mode::ReadWrite: break;
			}
			return VirtualMemory::Mode::ReadWrite;
		}
		// Debugging the pager (guest_commit_limit_mb): a wall for the guest's commits, like the console's limit of memory that the PC does not have.
		static bool WallReached(uint64_t size) {
			static const uint64_t limit = static_cast<uint64_t>(Config::GuestCommitLimitMb()) << 20u;
			return limit != 0 && Xbox::Lazy::GetCounters().committed.load(std::memory_order_relaxed) + size > limit;
		}
		bool Commit(uint64_t address, uint64_t size) override {
			if (WallReached(size)) {
				return false;
			}
			if (!VirtualMemory::Windows::CommitLazy(address, size)) {
				PagerLog("\t lazy guest memory: commit of 0x%016" PRIx64 " bytes at 0x%016" PRIx64 " failed, error %u\n", size, address,
				           static_cast<uint32_t>(GetLastError()));
				return false;
			}
			Xbox::Lazy::AddCommitted(size);
			return true;
		}
		// A `view` is one window that came back from the store (CommitFromStore): it is unmapped, and the window is reserved private memory again.
		void Decommit(uint64_t address, uint64_t size, bool view) override {
			if (view) {
				EXIT_IF(UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(address), MEM_PRESERVE_PLACEHOLDER) == 0);
				EXIT_IF(!VirtualMemory::Windows::ReserveLazy(address, size));
				Xbox::Lazy::GetCounters().views.fetch_sub(1, std::memory_order_relaxed);
			} else {
				EXIT_IF(!VirtualMemory::Windows::DecommitLazy(address, size));
			}
			Xbox::Lazy::SubCommitted(size);
		}
		bool Protect(uint64_t address, uint64_t size, Xbox::Lazy::Mode mode) override {
			return VirtualMemory::Windows::ProtectLazy(address, size, Native(mode));
		}
		// The pager's store: a window comes back from its slot as a view of a section of its own. The contents are read into the section through a
		// view that only this thread knows (the window is inaccessible all that time: a thread that touches it faults and waits for the lock the caller
		// holds), and the window becomes the section's view in one step. No thread ever sees the window empty, which a window committed first and
		// filled after would be for one that reads it without a fault.
		bool CommitFromStore(uint64_t window, uint64_t address, uint64_t size, uint32_t slot) override {
			if (WallReached(size)) {
				return false;
			}
			auto* store = g_pager_store.load(std::memory_order_acquire);
			EXIT_IF(store == nullptr);
			const auto start   = std::chrono::steady_clock::now();
			HANDLE     section = VirtualMemory::Windows::CreateGuestSection(size);
			if (section == nullptr) {
				PagerLog("\t pager: no section for a window at 0x%016" PRIx64 ", error %u\n", address, static_cast<uint32_t>(GetLastError()));
				return false;
			}
			void* scratch = VirtualMemory::Windows::MapSectionAlias(section, size);
			if (scratch == nullptr) {
				const auto error = static_cast<uint32_t>(GetLastError()); // before anything else can change it
				PagerLog("\t pager: no view of a section for a window at 0x%016" PRIx64 ", error %u (%lld views, guest %" PRIu64 " MB, app memory %" PRIu64 " MB)\n", address,
				         error, static_cast<long long>(Xbox::Lazy::GetCounters().views.load()), Xbox::Lazy::GetCounters().committed.load() >> 20u,
				         Config::AppMemoryUsage() >> 20u);
				CloseHandle(section);
				return false;
			}
			if (!store->Read(slot, 0, scratch, size)) {
				EXIT("Memory: the pager could not read a window of guest memory back (slot %u)\n", slot);
			}
			if (!store->CheckChecksum(slot, Xbox::PagerStore::Checksum(scratch, size) ^ (window * 0x9E3779B97F4A7C15ull))) {
				store->counters.corrupt++;
				PagerLog("\t pager: slot %u came back different from what was written\n", slot);
			}
			UnmapViewOfFile(scratch);
			// the reserved window becomes a placeholder, and the placeholder the view
			if (VirtualFree(reinterpret_cast<void*>(address), size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				PagerLog("\t pager: the window at 0x%016" PRIx64 " did not become a placeholder, error %u\n", address, static_cast<uint32_t>(GetLastError()));
				CloseHandle(section);
				return false;
			}
			void* view = VirtualMemory::Windows::MapPlaceholderView(section, address, 0, size, VirtualMemory::Mode::ReadWrite);
			CloseHandle(section); // the view keeps the section
			if (view == nullptr || reinterpret_cast<uint64_t>(view) != address) {
				PagerLog("\t pager: the view of a window at 0x%016" PRIx64 " failed, error %u\n", address, static_cast<uint32_t>(GetLastError()));
				EXIT_IF(!VirtualMemory::Windows::ReserveLazy(address, size));
				return false;
			}
			Xbox::Lazy::GetCounters().views.fetch_add(1, std::memory_order_relaxed);
			Xbox::Lazy::AddCommitted(size);
			HistoryPageIn(window);
			store->counters.page_ins++;
			store->counters.page_in_ns += static_cast<uint64_t>(
			    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
			return true;
		}
		// A view becomes private memory (the contents and the protections of its pages stay): an unmap cuts through the window and keeps a part of it.
		bool MakePrivate(uint64_t address, uint64_t size) override {
			constexpr uint64_t   kPage = 0x1000;
			std::vector<DWORD>   protections(size / kPage);
			for (uint64_t i = 0; i < protections.size(); ++i) {
				MEMORY_BASIC_INFORMATION info {};
				EXIT_IF(VirtualQuery(reinterpret_cast<const void*>(address + i * kPage), &info, sizeof(info)) == 0);
				protections[i] = info.Protect & 0xFF;
			}
			EXIT_IF(!VirtualMemory::Windows::ProtectLazy(address, size, VirtualMemory::Mode::ReadWrite));
			std::vector<uint8_t> copy(size);
			std::memcpy(copy.data(), reinterpret_cast<const void*>(address), size);
			EXIT_IF(UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(address), MEM_PRESERVE_PLACEHOLDER) == 0);
			EXIT_IF(!VirtualMemory::Windows::ReserveLazy(address, size));
			EXIT_IF(!VirtualMemory::Windows::CommitLazy(address, size)); // the window stays committed: the counter does not change
			Xbox::Lazy::GetCounters().views.fetch_sub(1, std::memory_order_relaxed);
			std::memcpy(reinterpret_cast<void*>(address), copy.data(), size);
			for (uint64_t i = 0; i < protections.size(); ++i) {
				if (protections[i] == PAGE_READONLY || protections[i] == PAGE_NOACCESS) {
					EXIT_IF(!VirtualMemory::Windows::ProtectLazy(address + i * kPage, kPage,
					                                             protections[i] == PAGE_READONLY ? VirtualMemory::Mode::Read : VirtualMemory::Mode::NoAccess));
				}
			}
			return true;
		}
		// Close to the limit of the app's memory (the OS ends the app there; the margin is for what the GPU and the heap take in one step).
		bool Tight(uint64_t size) override {
			const uint64_t limit = Config::AppMemoryLimit();
			if (limit == 0 || g_pager_store.load(std::memory_order_acquire) == nullptr) { // (without the pager there is nothing to make room with)
				return false;
			}
			constexpr uint64_t kHardHeadroom = uint64_t {384} << 20u;
			const uint64_t     used          = Config::AppMemoryUsage();
			return used != 0 && used + size + kHardHeadroom > limit;
		}
		// A view (read-only: a thread that touches the window waits for the lock at its fault) becomes private memory with the same contents. The mapping
		// of a view of a section is a limited resource (this fails at about 16800 of them on the Series X); private memory is not.
		bool Privatize(uint64_t address, uint64_t size, const void* data) override {
			EXIT_IF(UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void*>(address), MEM_PRESERVE_PLACEHOLDER) == 0);
			EXIT_IF(!VirtualMemory::Windows::ReserveLazy(address, size));
			// the commit of the view was given back an instant ago: a thread outside the lock may have taken it
			bool committed = false;
			for (int attempt = 0; attempt < 200 && !committed; ++attempt) {
				committed = VirtualMemory::Windows::CommitLazy(address, size);
				if (!committed) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			if (!committed) {
				EXIT("Memory: out of commit while turning a view of guest memory into private memory at 0x%016" PRIx64 "\n", address);
			}
			std::memcpy(reinterpret_cast<void*>(address), data, size);
			Xbox::Lazy::GetCounters().views.fetch_sub(1, std::memory_order_relaxed);
			return true;
		}
		// MakeRoom: the window is read-only and the caller holds the lock (a writer waits for it at its fault), so what is copied is what is stored.
		bool Spill(uint64_t window, uint64_t address, uint64_t size, uint32_t& slot) override {
			auto* store = g_pager_store.load(std::memory_order_acquire);
			if (store == nullptr) {
				return false;
			}
			const auto*    memory = reinterpret_cast<const void*>(address); // written from the window itself: no buffer to allocate in a fault handler
			const uint64_t sum    = Xbox::PagerStore::Checksum(memory, size);
			const uint32_t s      = store->Alloc();
			if (!store->Write(s, memory, size)) {
				store->Free(s);
				return false;
			}
			if (Xbox::PagerStore::Checksum(memory, size) != sum) { // a write that did not fault
				store->counters.torn++;
				store->Free(s);
				return false;
			}
			store->SetChecksum(s, sum ^ (window * 0x9E3779B97F4A7C15ull));
			store->counters.evicted++;
			store->counters.emergency++;
			HistoryEvict(window);
			slot = s;
			return true;
		}
		void FreeSlot(uint32_t slot) override {
			if (auto* store = g_pager_store.load(std::memory_order_acquire)) {
				store->Free(slot);
			}
		}
	};

	// ---- Xbox fork: the pager. While the lazily committed guest memory is above the budget (guest_ram_budget_mb in kyty-uwp.json), the pager takes
	// the coldest windows out of memory: it sweeps candidates (no-access), and a window that nobody touched for a second is copied to the store (a
	// file) and decommitted. A touch brings it back: a fault, or a host call that is about to access it. The states of a window are in
	// xbox/lazyRegions.h; this is the thread and the store.
	void StartPager(uint64_t budget_bytes) {
		auto store = std::make_unique<Xbox::PagerStore>();
		// the working directory is the app's LocalState (the UWP app sets it); the file is deleted when the process ends
		if (!store->Open(L"guest-store.tmp")) {
			LOGF_COLOR(Log::Color::Red, "\t pager: could not create its store file, error %u: guest memory stays in memory\n", static_cast<uint32_t>(GetLastError()));
			return;
		}
		m_pager_store = std::move(store);
		g_pager_store.store(m_pager_store.get(), std::memory_order_release);
		m_budget = budget_bytes;
		m_pager_mode = Config::GuestRamPagerMode();
			m_fault_thread = std::jthread([this](std::stop_token stop) { FaultLoop(stop); });
			m_pager = std::jthread([this](std::stop_token stop) { PagerLoop(stop); });
			if (m_pager_mode == 1) {
				LOGF_COLOR(Log::Color::Green, "\t pager: sweep only, nothing leaves memory\n");
			}
		LOGF_COLOR(Log::Color::Green, "\t pager: guest memory above %" PRIu64 " MB is taken out of memory and kept in a file\n", budget_bytes >> 20u);
	}

	// A fault on a window in the store is served by a helper thread (see HandleCommitFault); the faulting thread waits here.
	struct FaultRequest {
		uint64_t address;
		bool     is_write;
		bool     done   = false;
		bool     result = false;
	};
	bool RunOnFaultThread(uint64_t address, bool is_write) {
		FaultRequest     request {address, is_write};
		std::unique_lock lock(m_fault_mutex);
		m_fault_queue.push_back(&request);
		m_fault_wake.notify_one();
		m_fault_done.wait(lock, [&request] { return request.done; });
		return request.result;
	}
	void FaultLoop(std::stop_token stop) {
		t_fault_thread = true;
		std::unique_lock lock(m_fault_mutex);
		while (!stop.stop_requested()) {
			m_fault_wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return !m_fault_queue.empty(); });
			while (!m_fault_queue.empty()) {
				auto* request = m_fault_queue.front();
				m_fault_queue.pop_front();
				lock.unlock();
				const bool result = CommitFaultLocked(request->address, request->is_write);
				lock.lock();
				request->result = result;
				request->done   = true;
				m_fault_done.notify_all();
			}
		}
	}

	void StopPager() {
		if (m_fault_thread.joinable()) {
			m_fault_thread.request_stop();
			m_fault_thread.join();
		}
		if (m_pager.joinable()) {
			m_pager.request_stop();
			m_pager.join();
		}
		g_pager_store.store(nullptr, std::memory_order_release);
	}

	void PagerLoop(std::stop_token stop) {
		using Clock                    = std::chrono::steady_clock;
		constexpr uint64_t kWindow     = Xbox::Lazy::Regions::kWindow;
		constexpr uint64_t kWindowBits = Xbox::Lazy::Regions::kWindowBits;
		constexpr auto     kDwell      = std::chrono::milliseconds(1000);
		constexpr uint64_t kSweepChunk = 2048; // windows swept per hold of the lock
		auto&              counters    = m_pager_store->counters;
		const auto         resident    = [] { return Xbox::Lazy::GetCounters().committed.load(std::memory_order_relaxed); };
		struct Swept {
			uint64_t         window;
			Clock::time_point at;
		};
		std::deque<Swept>    swept;
		uint64_t             hand = 0;
		std::vector<uint8_t> copy(kWindow);
		auto                 last_report = Clock::now();
		uint64_t             previous[6] = {}; // swept, evicted, aborted, page ins, page in ns, written
		uint64_t             previous_problems[4] = {}; // errors, torn, corrupt, evicted on a failed commit
		std::string          last_game_memory;          // the game memory line last written
		// The budget in force. A commit that failed (Regions::MakeRoom evicted on the spot) is the wall of the process: the GPU and the heap share its
		// memory and grow, and the guest was at the wall with no room for them. The budget drops to what the guest held then minus a margin, and creeps
		// back up (8 MB a second) once the wall was not hit for half a minute.
		constexpr uint64_t kWallMargin = uint64_t {256} << 20u;
		constexpr uint64_t kHeadroom   = uint64_t {768} << 20u;
		constexpr uint64_t kFloor      = uint64_t {512} << 20u; // the guest holds this much at least, whatever the others take
		const uint64_t     app_limit   = Config::AppMemoryLimit();
		// Views of sections (the pager's page ins) are a limited resource: the Series X stops mapping at about 16800 of them. Above the cap cold views
		// become private memory. (Mode 3, for testing on a PC: a cap of 300.)
		const int64_t view_cap = m_pager_mode == 3 ? 300 : 6000;
		uint64_t           budget      = m_budget;
		uint64_t           last_room   = 0;
		auto               last_wall   = Clock::now();
		auto               last_creep  = Clock::now();
		while (!stop.stop_requested()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			const auto     now = Clock::now();
			if (const uint64_t room = m_lazy.RoomEvicted(); room != last_room) {
				last_room = room;
				last_wall = now;
				const uint64_t held   = resident();
				const uint64_t target = held > kWallMargin ? held - kWallMargin : 0;
				const uint64_t lowered = std::max(m_budget / 2, std::min(budget, target));
				if (lowered < budget) {
					PagerLog("\t pager: a commit failed with %" PRIu64 " MB of guest memory: budget %" PRIu64 " MB -> %" PRIu64 " MB\n", held >> 20u, budget >> 20u, lowered >> 20u);
					budget = lowered;
				}
			} else if (budget < m_budget && now - last_wall >= std::chrono::seconds(30) && now - last_creep >= std::chrono::seconds(1)) {
				budget     = std::min(m_budget, budget + (uint64_t {8} << 20u));
				last_creep = now;
			}
			// The OS ends the app at its limit of memory, and the GPU and the heap are in it too, growing in steps of up to 128 MB: the guest may hold what the
			// limit leaves after them and a headroom. (Without a probe, or a limit: the budget as it is.)
			uint64_t effective = budget;
			if (app_limit != 0) {
				if (const uint64_t used = Config::AppMemoryUsage(); used != 0) {
					const uint64_t held  = resident();
					const uint64_t other = used > held ? used - held : 0;
					const uint64_t room  = app_limit > other + kHeadroom ? app_limit - other - kHeadroom : 0;
					effective            = std::min(budget, std::max(kFloor, room));
				}
			}
			const uint64_t low = effective - effective / 16;
			// 1. Windows swept a second ago: the ones nobody touched leave memory while the guest is above the low mark; the others are let go.
			while (!swept.empty() && now - swept.front().at >= kDwell && !stop.stop_requested()) {
				const uint64_t window = swept.front().window;
				swept.pop_front();
				std::shared_lock copy_lock(m_copy_lock); // an unmap waits for us: the window is not freed while it is copied
				LazyHost         host;
				// Enough is out (or sweep only, for debugging: nothing leaves memory): the window stays, read-write. A view stays as private memory when
				// there are too many views: this is the way back from the limit of mappings (see Privatize).
				const bool enough    = m_pager_mode == 1 || resident() <= low;
				bool       privatize = false;
				bool       begun     = false;
				{
					std::lock_guard lock(m_lazy_mutex);
					if (enough) {
						privatize = m_pager_mode != 1 && Xbox::Lazy::GetCounters().views.load(std::memory_order_relaxed) > view_cap && m_lazy.IsView(window);
						if (!privatize) {
							m_lazy.Touch(window, host);
							continue;
						}
					}
					begun = m_lazy.BeginEvict(window, host);
				}
				if (!begun) {
					continue; // touched meanwhile
				}
				if (privatize) {
					// the window is read-only now: what is copied here is what it holds when it becomes private memory (a write is a fault that waits for the lock)
					std::memcpy(copy.data(), reinterpret_cast<const void*>(window << kWindowBits), kWindow);
					std::lock_guard lock(m_lazy_mutex);
					if ((m_lazy.FlagsOf(window << kWindowBits) & Xbox::Lazy::kEvicting) != 0 &&
					    std::memcmp(copy.data(), reinterpret_cast<const void*>(window << kWindowBits), kWindow) != 0) {
						counters.torn++;
						m_lazy.Touch(window, host);
					} else if (m_lazy.CommitPrivatize(window, copy.data(), host)) {
						counters.privatized++;
					} else {
						m_lazy.Touch(window, host); // touched meanwhile, or the host could not
					}
					continue;
				}
				const uint32_t slot = m_pager_store->Alloc();
				// the window is read-only now: a write is a fault that aborts the eviction below, so what is copied here is what is stored
				std::memcpy(copy.data(), reinterpret_cast<const void*>(window << kWindowBits), kWindow);
				const uint64_t sum     = Xbox::PagerStore::Checksum(copy.data(), kWindow);
				const bool     written = m_pager_store->Write(slot, copy.data(), kWindow);
				bool           left    = false;
				{
					std::lock_guard lock(m_lazy_mutex);
					if (written) {
						// Still read-only (nobody touched it through a fault): it must still equal the copy. A difference is a write that did not
						// fault (a host call that changes the protection itself), and the window stays.
						if ((m_lazy.FlagsOf(window << kWindowBits) & Xbox::Lazy::kEvicting) != 0 &&
						    std::memcmp(copy.data(), reinterpret_cast<const void*>(window << kWindowBits), kWindow) != 0) {
							counters.torn++;
							m_lazy.Touch(window, host);
						} else {
							m_pager_store->SetChecksum(slot, sum ^ (window * 0x9E3779B97F4A7C15ull));
							left = m_lazy.CommitEvict(window, slot, host);
							if (left && m_pager_mode == 2) {
								m_lazy.CommitAt(window << kWindowBits, host); // debugging: straight back in (a decommit and a commit, and the contents through the store)
							}
						}
					} else {
						m_lazy.Touch(window, host); // the store failed: the window stays
					}
				}
				if (left) {
					counters.evicted++;
					HistoryEvict(window);
				} else {
					m_pager_store->Free(slot);
					counters.aborted++;
				}
			}
			// 2. Above the budget and nothing waiting: sweep the next windows, from where the last sweep ended.
			const bool over_budget = resident() > effective;
			const auto views_now   = Xbox::Lazy::GetCounters().views.load(std::memory_order_relaxed);
			if (swept.empty() && (over_budget || views_now > view_cap)) {
				// above the budget any window will do; with too many views only views are swept (and become private memory)
				const uint64_t need   = over_budget ? (resident() - low) / kWindow : static_cast<uint64_t>(views_now - view_cap);
				const uint64_t target = std::min<uint64_t>(need + need / 2 + 16, 65536);
				uint64_t       count  = 0;
				bool           wrapped = false;
				while (count < target && !stop.stop_requested()) {
					std::lock_guard lock(m_lazy_mutex);
					LazyHost        host;
					uint64_t        in_chunk = 0;
					bool            any      = false;
					m_lazy.ForEachFullWindow(hand, [&](uint64_t window) {
						any  = true;
						hand = window + 1;
						if (!over_budget && !m_lazy.IsView(window)) {
							return false;
						}
						if (m_lazy.SweepIfCold(window, host)) {
							swept.push_back({window, now});
							++count;
							++in_chunk;
						}
						return in_chunk >= kSweepChunk || count >= target;
					});
					if (!any) { // the end of the address space: start again at the top, once
						if (wrapped) {
							break;
						}
						wrapped = true;
						hand    = 0;
					}
				}
				counters.swept += count;
			}
			if (now - last_report >= std::chrono::seconds(10)) {
				size_t evicted_now = 0;
				{
					std::shared_lock lock(m_lazy_mutex);
					evicted_now = m_lazy.EvictedCount();
				}
				const uint64_t current[6] = {counters.swept.load(), counters.evicted.load(), counters.aborted.load(), counters.page_ins.load(), counters.page_in_ns.load(),
				                             counters.written.load()};
				const auto     seconds    = std::chrono::duration<double>(now - last_report).count();
				const uint64_t page_ins   = current[3] - previous[3];
				const auto     gigabytes  = [](uint64_t bytes) { return static_cast<double>(bytes) / static_cast<double>(uint64_t {1} << 30u); };
				// What a person reads: the swap file and its traffic.
				Log::WriteToConsoleAndLog(fmt::format(
				    "Swap: {:.1f} GB in RAM (limit {:.1f} GB), {:.1f} GB in the file; last {:.0f} s: {} MB out, {} MB in{}\n", gigabytes(resident()),
				    gigabytes(effective), gigabytes(evicted_now * kWindow), seconds, (current[5] - previous[5]) >> 20u, (page_ins * kWindow) >> 20u,
				    page_ins != 0 ? fmt::format(" ({:.1f} ms each)", static_cast<double>(current[4] - previous[4]) / 1e6 / static_cast<double>(page_ins)) : std::string()));
				// Problems, only when there were some.
				const uint64_t problems[4] = {counters.errors.load(), counters.torn.load(), counters.corrupt.load(), counters.emergency.load()};
				std::string    problem_text;
				const char*    problem_names[4] = {"errors", "torn windows", "corrupt windows", "windows evicted on a failed commit"};
				for (int i = 0; i < 4; i++) {
					if (problems[i] != previous_problems[i]) {
						problem_text += fmt::format("{}{} {}", problem_text.empty() ? "" : ", ", problems[i] - previous_problems[i], problem_names[i]);
					}
				}
				if (!problem_text.empty()) {
					Log::WriteToConsoleAndLog(fmt::format("Swap problems in the last {:.0f} s: {}\n", seconds, problem_text));
				}
				std::copy(std::begin(problems), std::end(problems), std::begin(previous_problems));
				// Everything, with the developer setting's memory report.
				if (Config::GpuMemoryLog()) {
					Log::WriteToConsoleAndLog(fmt::format(
					    "pager: resident {} MB of {} MB, {} windows in the store ({} MB); last {:.0f} s: swept {}, evicted {} ({:.0f} MB/s written), aborted {}, paged in {} (avg {:.0f} us), "
					    "store reads {}, errors {}, torn {}, corrupt {}, evicted on a failed commit {}, app memory {} of {} MB, views {} (turned private {})\n",
					    resident() >> 20u, effective >> 20u, evicted_now, (evicted_now * kWindow) >> 20u, seconds, current[0] - previous[0], current[1] - previous[1],
					    static_cast<double>(current[5] - previous[5]) / 1048576.0 / seconds, current[2] - previous[2], page_ins,
					    page_ins != 0 ? static_cast<double>(current[4] - previous[4]) / 1000.0 / static_cast<double>(page_ins) : 0.0, counters.read_hits.load(), counters.errors.load(), counters.torn.load(), counters.corrupt.load(), counters.emergency.load(), Config::AppMemoryUsage() >> 20u, app_limit >> 20u, Xbox::Lazy::GetCounters().views.load(), counters.privatized.load()));
					Log::WriteToConsoleAndLog(GuestPrivateMemoryReport());
				}
				std::copy(std::begin(current), std::end(current), std::begin(previous));
				last_report = now;
				// The game's code and stacks, when they changed.
				if (auto game_memory = GuestPrivateMemorySummary(); game_memory != last_game_memory) {
					Log::WriteToConsoleAndLog(game_memory);
					last_game_memory = std::move(game_memory);
				}
			}
		}
	}

	// A guest protection for lazy memory: reads and writes are all the host distinguishes (the guest never gets execute rights here).
	bool LazyProtect(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		Xbox::Lazy::Mode lazy_mode = Xbox::Lazy::Mode::ReadWrite;
		switch (mode) {
			case VirtualMemory::Mode::NoAccess:
			case VirtualMemory::Mode::Execute: lazy_mode = Xbox::Lazy::Mode::NoAccess; break;
			case VirtualMemory::Mode::Read:
			case VirtualMemory::Mode::ExecuteRead: lazy_mode = Xbox::Lazy::Mode::Read; break;
			default: break;
		}
		std::lock_guard lock(m_lazy_mutex);
		LazyHost        host;
		return m_lazy.Protect(vaddr, size, lazy_mode, host);
	}

	// Host copy of committed guest pages that may be read-only or no-access (page watchers): make them readable for the copy and restore.
	// A guest thread that touches such a page during the copy does not fault; see the T3 design note, "the known race".
	void HostCopyFromGuest(uint64_t address, uint8_t* destination, uint64_t size) {
		auto       cursor = address;
		const auto end    = address + size;
		while (cursor < end) {
			MEMORY_BASIC_INFORMATION info {};
			if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) {
				std::memset(destination + (cursor - address), 0, end - cursor);
				return;
			}
			const auto run_end = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize);
			const auto bytes   = run_end - cursor;
			const DWORD protect = info.Protect & 0xFF;
			if (info.State != MEM_COMMIT) {
				// Zero, except for windows that the pager moved to its store (a window that was committed when the range was looked at may be
				// there by now): reading those goes through the fault that brings them back.
				for (auto a = cursor; a < run_end;) {
					const auto next = std::min<uint64_t>(run_end, ((a >> Xbox::Lazy::Regions::kWindowBits) + 1) << Xbox::Lazy::Regions::kWindowBits);
					if (m_lazy.IsEvicted(a)) {
						std::memcpy(destination + (a - address), reinterpret_cast<const void*>(a), next - a);
					} else {
						std::memset(destination + (a - address), 0, next - a);
					}
					a = next;
				}
			} else if (protect == PAGE_NOACCESS) {
				EXIT_IF(!VirtualMemory::Windows::ProtectLazy(cursor, bytes, VirtualMemory::Mode::Read));
				std::memcpy(destination + (cursor - address), reinterpret_cast<const void*>(cursor), bytes);
				// not "back to what it was": the page manager may have changed these pages meanwhile
				RestoreHostProtection(cursor, bytes, VirtualMemory::Mode::NoAccess);
			} else {
				std::memcpy(destination + (cursor - address), reinterpret_cast<const void*>(cursor), bytes);
			}
			cursor = run_end;
		}
	}

	static void HostCopyToGuest(uint64_t address, const uint8_t* source, uint64_t size) {
		auto       cursor = address;
		const auto end    = address + size;
		while (cursor < end) {
			MEMORY_BASIC_INFORMATION info {};
			EXIT_IF(VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT);
			const auto  run_end = std::min<uint64_t>(end, reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize);
			const auto  bytes   = run_end - cursor;
			const DWORD protect = info.Protect & 0xFF;
			if (protect == PAGE_READWRITE) {
				std::memcpy(reinterpret_cast<void*>(cursor), source + (cursor - address), bytes);
			} else {
				EXIT_IF(!VirtualMemory::Windows::ProtectLazy(cursor, bytes, VirtualMemory::Mode::ReadWrite));
				std::memcpy(reinterpret_cast<void*>(cursor), source + (cursor - address), bytes);
				// not "back to what it was": the page manager may have changed these pages meanwhile
				RestoreHostProtection(cursor, bytes, protect == PAGE_READONLY ? VirtualMemory::Mode::Read : VirtualMemory::Mode::NoAccess);
			}
			cursor = run_end;
		}
	}
#endif

	// Copy guest memory to the host: uncommitted pages read as zero without being committed. With `sparse`, gaps between regions read as zero too.
	bool LazyCopyOut(uint64_t vaddr, void* data, uint64_t size, bool sparse) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock copy_lock(m_copy_lock);
		struct Segment {
			uint64_t address, bytes;
			int      kind; // 0 gap, 1 not committed, 2 committed
		};
		// Fast path: every window of the range is committed as a whole and has no no-access page (the window flags, no lock, no system call): a plain
		// copy cannot fault, and the copy lock keeps unmapping away. Most host reads of guest memory (shader resource descriptors, buffer contents)
		// are small and end here.
		if (m_lazy.AllReadable(vaddr, size)) {
			std::memcpy(data, reinterpret_cast<const void*>(vaddr), size);
			return true;
		}
		std::vector<Segment> segments;
		{
			std::shared_lock lock(m_lazy_mutex);
			auto            cursor = vaddr;
			const auto      end    = vaddr + size;
			while (cursor < end) {
				const auto length = m_lazy.ContainedLength(cursor, end - cursor);
				if (length == 0) {
					if (!sparse) {
						return false;
					}
					const auto next = std::min(end, m_lazy.NextStart(cursor));
					segments.push_back({cursor, next - cursor, 0});
					cursor = next;
					continue;
				}
				m_lazy.ForEachSegment(cursor, length, [&](uint64_t address, uint64_t bytes, bool committed) {
					if (committed) {
						segments.push_back({address, bytes, 2});
						return;
					}
					// Not committed: zero, except the windows that are in the pager's store. Their contents are read from the store right here, under
					// the lock that keeps their slots (a page in takes the lock exclusively), without bringing the window back into memory.
					auto* piece = static_cast<uint8_t*>(data) + (address - vaddr);
					std::memset(piece, 0, bytes);
					auto* store = g_pager_store.load(std::memory_order_acquire);
					for (uint64_t a = address; store != nullptr && m_lazy.EvictedCount() != 0 && a < address + bytes;) {
						const uint64_t window = a >> Xbox::Lazy::Regions::kWindowBits;
						const uint64_t next   = std::min(address + bytes, (window + 1) << Xbox::Lazy::Regions::kWindowBits);
						uint32_t       slot   = 0;
						if (m_lazy.IsEvicted(a) && m_lazy.EvictedSlot(window, slot) &&
						    store->Read(slot, a - (window << Xbox::Lazy::Regions::kWindowBits), piece + (a - address), next - a)) {
							store->counters.read_hits++;
						}
						a = next;
					}
				});
				cursor += length;
			}
		}
		auto* destination = static_cast<uint8_t*>(data);
		for (const auto& segment: segments) {
			if (segment.kind != 2) {
				std::memset(destination + (segment.address - vaddr), 0, segment.bytes);
			} else if (m_lazy.AllReadable(segment.address, segment.bytes)) {
				std::memcpy(destination + (segment.address - vaddr), reinterpret_cast<const void*>(segment.address), segment.bytes);
			} else {
				HostCopyFromGuest(segment.address, destination + (segment.address - vaddr), segment.bytes);
			}
		}
		return true;
#else
		(void)vaddr;
		(void)data;
		(void)size;
		(void)sparse;
		return false;
#endif
	}

	// Copy host data into guest memory: the windows of the range are committed first (the data has to live somewhere). Gaps between regions are
	// skipped, as sparse reads zero them (upstream's backed pages of a GPU publish).
	bool LazyCopyIn(uint64_t vaddr, const void* data, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock                           copy_lock(m_copy_lock);
		std::vector<std::pair<uint64_t, uint64_t>> pieces;
		{
			std::lock_guard lock(m_lazy_mutex);
			LazyHost        host;
			const auto      end = vaddr + size;
			for (auto cursor = vaddr; cursor < end;) {
				const auto length = m_lazy.ContainedLength(cursor, end - cursor);
				if (length == 0) {
					cursor = std::min(end, m_lazy.NextStart(cursor));
					continue;
				}
				if (!m_lazy.EnsureCommitted(cursor, length, host)) {
					return false;
				}
				pieces.emplace_back(cursor, length);
				cursor += length;
			}
		}
		for (const auto& [address, bytes]: pieces) {
			const auto* source = static_cast<const uint8_t*>(data) + (address - vaddr);
			if (m_lazy.AllWritable(address, bytes)) {
				// every window is committed as a whole and read-write: a plain copy cannot fault (see LazyCopyOut)
				std::memcpy(reinterpret_cast<void*>(address), source, bytes);
			} else {
				HostCopyToGuest(address, source, bytes);
			}
		}
		return true;
#else
		(void)vaddr;
		(void)data;
		(void)size;
		return false;
#endif
	}

	bool Protect(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	// Tracker protections only read m_mapped; the page tracker orders them per page.
	bool ProtectTransient(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	bool MapBacking(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                VirtualMemory::Mode               mode,
	                GuestBackingStore::FailureReason* failure_reason = nullptr) {
		std::lock_guard lock(m_mutex);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		const bool replacing = ContainsMappedUnlocked(vaddr, size, MappingType::Backing);
#else
		constexpr bool replacing = false;
#endif
		if (!replacing && !ConsumeUnlocked(vaddr, size)) {
			if (failure_reason != nullptr) {
				*failure_reason = GuestBackingStore::FailureReason::AddressRangeUnavailable;
			}
			return false;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (m_lazy_enabled) {
			// Xbox fork (T3): reserve the range as private memory without committing it; windows are committed on first touch
			if (VirtualMemory::IsExecute(mode)) {
				LOGF_COLOR(Log::Color::Red, "\t lazy guest memory: an executable mapping is not supported (vaddr=0x%016" PRIx64 ")\n", vaddr);
				AddFreeUnlocked(vaddr, size);
				return false;
			}
			if (!VirtualMemory::Windows::ReserveLazy(vaddr, size)) {
				if (failure_reason != nullptr) {
					*failure_reason = GuestBackingStore::FailureReason::PlaceholderMapFailed;
				}
				AddFreeUnlocked(vaddr, size);
				return false;
			}
			{
				std::lock_guard lazy_lock(m_lazy_mutex);
				EXIT_IF(!m_lazy.Map(vaddr, size));
			}
			EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Backing));
			return true;
		}
#endif
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;
		if (m_backing->MapFixed(vaddr, size, backing_offset, host_mode, failure_reason)) {
			if (!replacing) {
				EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Backing));
			}
			return true;
		}
		if (!replacing) {
			AddFreeUnlocked(vaddr, size);
		}
		return false;
	}

	bool UnmapBacking(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// lock order: m_copy_lock before m_mutex (a host copy may be waiting for the page manager, which waits for m_mutex)
		std::unique_lock<std::shared_mutex> copy_lock;
		if (m_lazy_enabled) {
			copy_lock = std::unique_lock<std::shared_mutex>(m_copy_lock);
		}
#endif
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Backing)) {
			return false;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (m_lazy_enabled) {
			{
				std::lock_guard  lazy_lock(m_lazy_mutex);
				LazyHost         host;
				if (!m_lazy.Unmap(vaddr, size, host)) {
					return false;
				}
			}
			// give the range back as placeholders, one mapped region at a time (as ReleaseCommitted does)
			std::vector<std::pair<uint64_t, uint64_t>> pieces;
			const auto                                 end     = vaddr + size;
			auto                                       current = vaddr;
			while (current < end) {
				const auto it       = FindMappedContainingUnlocked(current);
				const auto part_end = std::min(end, it->second.vaddr + it->second.size);
				pieces.emplace_back(current, part_end - current);
				current = part_end;
			}
			size_t released = 0;
			for (const auto& [piece_vaddr, piece_size]: pieces) {
				released += ReleaseLazyRange(piece_vaddr, piece_size);
			}
			EXIT_IF(released == 0 || (released > 1 && !CoalescePlaceholders(vaddr, size)));
			EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Backing));
			AddFreeUnlocked(vaddr, size);
			return true;
		}
#endif
		if (!m_backing->Unmap(vaddr, size)) {
			return false;
		}
		EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Backing));
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	// Reservations live in VirtualRanges; these free-range checks only read the host placeholders.
	uint64_t ReserveAligned(uint64_t search_addr, uint64_t size, uint64_t alignment) {
		if (size == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment != 0 ? alignment : PageSize(), PageSize());
		std::shared_lock lock(m_mutex);
		if (search_addr == 0) {
			auto addr = FindFreeAlignedUnlocked(HOST_SYSTEM_MANAGED_MIN,
			                                    HOST_SYSTEM_MANAGED_MAX + 1u, size, alignment);
			return addr != 0 ? addr
			                 : FindFreeAlignedUnlocked(HOST_USER_MIN, HOST_USER_MAX + 1u, size,
			                                           alignment);
		}
		return FindFreeAlignedUnlocked(search_addr, HOST_USER_MAX + 1u, size, alignment);
	}

	uint64_t FindFreeAligned(uint64_t search_start, uint64_t search_end, uint64_t size,
	                         uint64_t alignment) {
		if (size == 0 || alignment == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment, PageSize());
		std::shared_lock lock(m_mutex);
		return FindFreeAlignedUnlocked(search_start, search_end, size, alignment);
	}

	bool ReserveFixed(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	bool TestContainsFree(uint64_t vaddr, uint64_t size) {
		std::shared_lock lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}
#endif

	bool ReleaseFree(uint64_t vaddr, uint64_t size) {
		std::shared_lock lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}

	bool Commit(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		std::lock_guard lock(m_mutex);
		if (!ConsumeUnlocked(vaddr, size)) {
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (!VirtualMemory::Windows::CommitPlaceholder(vaddr, size, host_mode)) {
			AddFreeUnlocked(vaddr, size);
			return false;
		}
		EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Private));
		return true;
#else
		if (VirtualMemory::Protect(vaddr, size, host_mode)) {
			EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Private));
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
#endif
	}

	bool ReleaseCommitted(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Private)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		std::vector<std::pair<uint64_t, uint64_t>> regions;
		while (current < end) {
			const auto it       = FindMappedContainingUnlocked(current);
			const auto part_end = std::min(end, it->second.vaddr + it->second.size);
			regions.emplace_back(current, part_end - current);
			current = part_end;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [region_vaddr, region_size]: regions) {
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_vaddr), region_size,
			                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
		}
		// ReleaseCommitted also publishes the aggregate span as one logical free range.
		EXIT_IF(regions.size() > 1 && !CoalescePlaceholders(vaddr, size));
#else
		for (const auto& [region_vaddr, region_size]: regions) {
			RestorePlaceholder(region_vaddr, region_size);
		}
#endif
		EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Private));
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	bool SelfTest() {
		const auto addr = ReserveAligned(0, PageSize(), PageSize());
		if (addr == 0) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: 16 KiB range unavailable\n");
			return false;
		}
		if (!Commit(addr, PageSize(), VirtualMemory::Mode::ReadWrite)) {
			LOGF_COLOR(Log::Color::Red, "\t guest address-space self-test: commit failed\n");
			return false;
		}

		auto* ptr = reinterpret_cast<uint64_t*>(addr);
		*ptr      = 0x4b59545950484f4cull; // "KYTYPHOL"

		if (!ReleaseCommitted(addr, PageSize())) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: release-committed failed\n");
			return false;
		}

		LOGF_COLOR(Log::Color::Green, "\t guest address-space self-test: ok\n");
		return true;
	}

private:
	enum class MappingType { Backing, Private };

	struct MappedRegion {
		uint64_t    vaddr = 0;
		uint64_t    size  = 0;
		MappingType type  = MappingType::Private;
	};

	static constexpr uint64_t PageSize() { return 0x4000; }

	static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
		if (alignment == 0) {
			return 0;
		}
		const auto remainder = value % alignment;
		const auto increment = (remainder != 0 ? alignment - remainder : 0);
		return increment <= UINT64_MAX - value ? value + increment : 0;
	}

	static uint64_t AlignDown(uint64_t value, uint64_t alignment) {
		return value & ~(alignment - 1u);
	}

	[[nodiscard]] static bool IsPageAligned(uint64_t value) {
		return (value & (PageSize() - 1u)) == 0;
	}

	bool ProtectMappedUnlocked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		const auto end = vaddr + size;
		auto       it  = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto& region      = it->second;
			const auto  range_start = std::max(vaddr, region.vaddr);
			const auto  range_end   = std::min(end, region.vaddr + region.size);
			if (range_start >= range_end) {
				continue;
			}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			// Lazy backing protects its own pages.
			if (m_lazy_enabled && region.type == MappingType::Backing) {
				if (!LazyProtect(range_start, range_end - range_start, mode)) {
					return false;
				}
				continue;
			}
			// Rollback can split a backing view without changing the owner interval.
			// Windows protection calls must stay within each actual view.
			if (region.type == MappingType::Backing) {
				if (!m_backing->ProtectViews(range_start, range_end - range_start, mode)) {
					return false;
				}
				continue;
			}
#endif
			if (!VirtualMemory::Protect(range_start, range_end - range_start, mode)) {
				return false;
			}
		}
		return true;
	}

	bool ConsumeUnlocked(uint64_t vaddr, uint64_t size) {
		auto it = FindContaining(vaddr, size);
		if (it == m_free.end() || !SplitUnlocked(it, vaddr, size)) {
			return false;
		}
		it = m_free.find(vaddr);
		if (it == m_free.end() || it->second != size) {
			return false;
		}
		m_free.erase(it);
		return true;
	}

	std::map<uint64_t, MappedRegion>::iterator FindMappedContainingUnlocked(uint64_t vaddr) {
		auto next = m_mapped.upper_bound(vaddr);
		if (next == m_mapped.begin()) {
			return m_mapped.end();
		}
		auto it = std::prev(next);
		return vaddr < it->second.vaddr + it->second.size ? it : m_mapped.end();
	}

	bool ContainsMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			const auto it = FindMappedContainingUnlocked(current);
			if (it == m_mapped.end() || it->second.type != type) {
				return false;
			}
			current = std::min(end, it->second.vaddr + it->second.size);
		}
		return true;
	}

	[[nodiscard]] bool OverlapsPrivateUnlocked(uint64_t vaddr, uint64_t size) const {
		auto mapping = m_mapped.upper_bound(vaddr);
		if (mapping != m_mapped.begin()) {
			--mapping;
		}
		for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
			const auto& region = mapping->second;
			if (region.type == MappingType::Private && region.vaddr + region.size > vaddr) {
				return true;
			}
		}
		return false;
	}

	bool AddMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		const auto end  = vaddr + size;
		auto       next = m_mapped.lower_bound(vaddr);
		if (next != m_mapped.end() && next->second.vaddr < end) {
			return false;
		}
		if (next != m_mapped.begin()) {
			const auto& previous = std::prev(next)->second;
			if (previous.vaddr + previous.size > vaddr) {
				return false;
			}
		}
		return m_mapped.emplace(vaddr, MappedRegion {vaddr, size, type}).second;
	}

	bool RemoveMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (!ContainsMappedUnlocked(vaddr, size, type)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			auto       it       = FindMappedContainingUnlocked(current);
			const auto original = it->second;
			const auto part_end = std::min(end, original.vaddr + original.size);
			m_mapped.erase(it);
			if (original.vaddr < current) {
				const auto left_size = current - original.vaddr;
				EXIT_IF(!AddMappedUnlocked(original.vaddr, left_size, original.type));
			}
			if (part_end < original.vaddr + original.size) {
				const auto right_size = original.vaddr + original.size - part_end;
				EXIT_IF(!AddMappedUnlocked(part_end, right_size, original.type));
			}
			current = part_end;
		}
		return true;
	}

	void ReserveGuestRegions() {
		const std::array<std::pair<uint64_t, uint64_t>, 3> regions {{
		    {HOST_SYSTEM_MANAGED_MIN, HOST_SYSTEM_RESERVED_MAX + 1u},
		    {HOST_USER_MIN, HOST_USER_MAX + 1u},
		    {kExtendedMemoryBase, kExtendedMemoryBase + kExtendedMemorySize},
		}};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		constexpr uint64_t MinimumRegionSize = 0x1000000;
		for (const auto& [guest_start, guest_end]: regions) {
			auto next_addr = guest_start;
			while (next_addr < guest_end) {
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(next_addr), &info, sizeof(info)) == 0) {
					EXIT("failed to inspect guest address space at 0x%016" PRIx64 "\n", next_addr);
				}

				const auto region_base = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end = std::min<uint64_t>(
				    guest_end, region_base + static_cast<uint64_t>(info.RegionSize));
				const auto reserve_start =
				    AlignUp(std::max(region_base, guest_start), m_granularity);
				const auto reserve_end = AlignDown(region_end, m_granularity);

				if (info.State == MEM_FREE && reserve_end > reserve_start &&
				    reserve_end - reserve_start > MinimumRegionSize) {
					if (!VirtualMemory::Windows::ReservePlaceholder(reserve_start, reserve_end - reserve_start)) {
						EXIT("failed to reserve guest address space at 0x%016" PRIx64
						     ", size 0x%016" PRIx64 "\n",
						     reserve_start, reserve_end - reserve_start);
					}
					AddFreeUnlocked(reserve_start, reserve_end - reserve_start);
					m_owned.emplace_back(reserve_start, reserve_end - reserve_start);
				}

				if (region_end <= next_addr) {
					EXIT("guest address-space scan did not advance at 0x%016" PRIx64 "\n", next_addr);
				}
				next_addr = region_end;
			}
		}
#else
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
		std::vector<std::pair<uint64_t, uint64_t>> occupied;
		FILE*                                      maps = fopen("/proc/self/maps", "r");
		EXIT_IF(maps == nullptr);

		char line[512];
		while (fgets(line, sizeof(line), maps) != nullptr) {
			unsigned long long mapping_start = 0;
			unsigned long long mapping_end   = 0;
			if (sscanf(line, "%llx-%llx", &mapping_start, &mapping_end) == 2) {
				occupied.emplace_back(static_cast<uint64_t>(mapping_start),
				                      static_cast<uint64_t>(mapping_end));
			}
		}
		fclose(maps);

		auto reserve_range = [this](uint64_t start, uint64_t end) {
			start = AlignUp(start, PageSize());
			end   = AlignDown(end, PageSize());
			if (start == 0 || end <= start) {
				return;
			}

			const auto size = end - start;
			int        flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif

			void* ptr = mmap(reinterpret_cast<void*>(start), size, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, size);
				}
				return;
			}

			AddFreeUnlocked(start, size);
			m_owned.emplace_back(start, size);
		};

		for (const auto& [region_start, region_end]: regions) {
			auto current = region_start;

			for (const auto& [mapping_start, mapping_end]: occupied) {
				if (mapping_end <= current) {
					continue;
				}
				if (mapping_start >= region_end) {
					break;
				}

				if (mapping_start > current) {
					reserve_range(current, std::min(mapping_start, region_end));
				}

				current = std::max(current, mapping_end);
				if (current >= region_end) {
					break;
				}
			}

			if (current < region_end) {
				reserve_range(current, region_end);
			}
		}
#else
		for (const auto& [start, end]: regions) {
			int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif
			void* ptr = mmap(reinterpret_cast<void*>(start), end - start, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, end - start);
				}
				EXIT("failed to reserve guest address space at 0x%016" PRIx64
				     ", size 0x%016" PRIx64 "\n",
				     start, end - start);
			}
			AddFreeUnlocked(start, end - start);
			m_owned.emplace_back(start, end - start);
		}
#endif
#endif
	}

	uint64_t FindFreeAlignedUnlocked(uint64_t search_start, uint64_t search_end, uint64_t size,
	                                 uint64_t alignment) const {
		if (search_start >= search_end || size > search_end - search_start) {
			return 0;
		}
		// Include the predecessor because search_start can lie inside a free range.
		auto it = m_free.upper_bound(search_start);
		if (it != m_free.begin()) {
			--it;
		}
		for (; it != m_free.end() && it->first < search_end; ++it) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			g_test_free_range_visits.fetch_add(1, std::memory_order_relaxed);
#endif
			const auto& [base, range_size] = *it;
			const auto candidate           = AlignUp(std::max(base, search_start), alignment);
			if (candidate < search_end && size <= search_end - candidate &&
			    GuestFreeRangeContains(base, range_size, candidate, size)) {
				return candidate;
			}
		}
		return 0;
	}

	std::map<uint64_t, uint64_t>::iterator FindContaining(uint64_t vaddr, uint64_t size) {
		auto next = m_free.upper_bound(vaddr);
		if (next == m_free.begin()) {
			return m_free.end();
		}
		auto it = std::prev(next);
		return GuestFreeRangeContains(it->first, it->second, vaddr, size) ? it : m_free.end();
	}

	bool SplitUnlocked(std::map<uint64_t, uint64_t>::iterator it, uint64_t vaddr, uint64_t size) {
		const auto base = it->first;
		const auto end  = it->first + it->second;
		if (base == vaddr && end == vaddr + size) {
			return true;
		}

		if (base != vaddr) {
			const auto left_size = vaddr - base;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(base), left_size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           base, end - base, vaddr, size, static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			it->second = left_size;
			it         = m_free.emplace(vaddr, end - vaddr).first;
		}

		if (it->second != size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(it->first), size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           it->first, it->second, vaddr, size,
				           static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			const auto right_start = it->first + size;
			const auto right_size  = it->second - size;
			it->second             = size;
			m_free.emplace(right_start, right_size);
		}

		return true;
	}

	void AddFreeUnlocked(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0) {
			return;
		}
		auto [it, inserted] = m_free.emplace(vaddr, size);
		if (!inserted) {
			it->second = size;
		}

		auto base = it->first;
		auto end  = it->first + it->second;

		auto first = it;
		while (first != m_free.begin()) {
			auto prev = std::prev(first);
			if (prev->first + prev->second != base) {
				break;
			}
			base  = prev->first;
			first = prev;
		}

		auto last = std::next(it);
		while (last != m_free.end() && end == last->first) {
			end  = last->first + last->second;
			last = std::next(last);
		}

		if (base != it->first || end != it->first + it->second) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (CoalescePlaceholders(base, end - base)) {
				m_free.erase(first, last);
				m_free.emplace(base, end - base);
			}
#else
			m_free.erase(first, last);
			m_free.emplace(base, end - base);
#endif
		}
	}

	void RestorePlaceholder(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
		// The address remains owned for the process lifetime; MAP_FIXED only replaces one of our
		// mappings and never acquires a new guest address.
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		EXIT_IF(ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != vaddr);
#else
		(void)vaddr;
		(void)size;
#endif
	}

	std::shared_mutex                          m_mutex;
	std::map<uint64_t, uint64_t>               m_free;
	std::map<uint64_t, MappedRegion>            m_mapped;
	std::vector<std::pair<uint64_t, uint64_t>> m_owned;
	uint64_t                                   m_granularity = 0x10000;
	GuestBackingStore*                         m_backing     = nullptr;
	bool                                       m_lazy_enabled = false;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Xbox::Lazy::Regions                        m_lazy;
	std::shared_mutex                          m_lazy_mutex; // shared: lookups for host copies; exclusive: every change and the commit itself
	std::shared_mutex                          m_copy_lock;
	std::unique_ptr<Xbox::PagerStore>          m_pager_store;
	uint64_t                                   m_budget = 0;
	uint32_t                                   m_pager_mode = 0; // Config::GuestRamPagerMode
	std::mutex                                 m_fault_mutex;
	std::condition_variable                    m_fault_wake, m_fault_done;
	std::deque<FaultRequest*>                  m_fault_queue;
	std::jthread                               m_fault_thread;
	std::jthread                               m_pager; // last: it stops first when the address space is destroyed
#endif
};
