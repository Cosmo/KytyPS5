#ifndef KYTY_XBOX_PAGER_STORE_H_
#define KYTY_XBOX_PAGER_STORE_H_

// The pager's store (Windows): a temporary file of slots of one 64 KiB window each, where windows of guest memory that are not needed in memory
// are kept. Slots are written by the pager thread and read by whoever touches the window again; both use explicit offsets, so the calls need no
// lock of ours.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // windows.h's min and max macros break the standard algorithms in the headers after this one
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace Xbox {

struct PagerCounters {
	std::atomic<uint64_t> swept {0}, evicted {0}, aborted {0}, page_ins {0}, page_in_ns {0}, read_hits {0}, written {0}, read {0}, errors {0};
	// a window that was written while it was copied out (caught, the eviction is dropped); a slot that came back different from what was written
	std::atomic<uint64_t> torn {0}, corrupt {0};
	// windows evicted on the spot because a commit failed (Regions::MakeRoom); they are in `evicted` too
	std::atomic<uint64_t> emergency {0};
	// views turned into private memory (too many views of sections: Regions::CommitPrivatize)
	std::atomic<uint64_t> privatized {0};
};

class PagerStore {
public:
	static constexpr uint64_t kSlotSize = uint64_t {64} * 1024;

	~PagerStore() { Close(); }

#if defined(_WIN32)
	// Creates the file (deleted when the process ends). False when that fails.
	bool Open(const wchar_t* path) {
		CREATEFILE2_EXTENDED_PARAMETERS parameters {};
		parameters.dwSize           = sizeof(parameters);
		parameters.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
		parameters.dwFileFlags      = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_RANDOM_ACCESS;
		m_file = CreateFile2(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, &parameters);
		return m_file != INVALID_HANDLE_VALUE;
	}
	void Close() {
		if (m_file != INVALID_HANDLE_VALUE) {
			CloseHandle(m_file);
			m_file = INVALID_HANDLE_VALUE;
		}
	}
	[[nodiscard]] bool IsOpen() const { return m_file != INVALID_HANDLE_VALUE; }

	uint32_t Alloc() {
		std::lock_guard lock(m_mutex);
		++m_in_use;
		if (!m_free.empty()) {
			const auto slot = m_free.back();
			m_free.pop_back();
			return slot;
		}
		return m_next++;
	}
	void Free(uint32_t slot) {
		std::lock_guard lock(m_mutex);
		--m_in_use;
		m_free.push_back(slot);
	}
	// A checksum of what was written to a slot, checked when the slot is read back.
	static uint64_t Checksum(const void* data, uint64_t size) {
		const auto* words = static_cast<const uint64_t*>(data);
		uint64_t    h     = 0x9E3779B97F4A7C15ull;
		for (uint64_t i = 0; i < size / 8; ++i) {
			h = (h ^ words[i]) * 0xFF51AFD7ED558CCDull;
			h ^= h >> 29u;
		}
		return h;
	}
	void SetChecksum(uint32_t slot, uint64_t sum) {
		std::lock_guard lock(m_mutex);
		if (m_sums.size() <= slot) {
			m_sums.resize(slot + 1, 0);
		}
		m_sums[slot] = sum;
	}
	[[nodiscard]] bool CheckChecksum(uint32_t slot, uint64_t sum) const {
		std::lock_guard lock(m_mutex);
		return slot < m_sums.size() && m_sums[slot] == sum;
	}
	[[nodiscard]] uint64_t SlotsInUse() const {
		std::lock_guard lock(m_mutex);
		return m_in_use;
	}

	bool Write(uint32_t slot, const void* data, uint64_t size) {
		return Transfer(slot, 0, const_cast<void*>(data), size, true);
	}
	bool Read(uint32_t slot, uint64_t offset, void* data, uint64_t size) { return Transfer(slot, offset, data, size, false); }
#endif

	PagerCounters counters;

private:
#if defined(_WIN32)
	bool Transfer(uint32_t slot, uint64_t offset, void* data, uint64_t size, bool write) {
		uint64_t done = 0;
		while (done < size) {
			OVERLAPPED overlapped {};
			const uint64_t position = static_cast<uint64_t>(slot) * kSlotSize + offset + done;
			overlapped.Offset       = static_cast<DWORD>(position);
			overlapped.OffsetHigh   = static_cast<DWORD>(position >> 32u);
			DWORD       transferred = 0;
			const DWORD chunk       = static_cast<DWORD>(std::min<uint64_t>(size - done, 1u << 20u));
			const BOOL  ok          = write ? WriteFile(m_file, static_cast<const uint8_t*>(data) + done, chunk, &transferred, &overlapped)
			                                : ReadFile(m_file, static_cast<uint8_t*>(data) + done, chunk, &transferred, &overlapped);
			if (!ok || transferred == 0) {
				counters.errors++;
				return false;
			}
			done += transferred;
		}
		(write ? counters.written : counters.read) += size;
		return true;
	}

	HANDLE                m_file = INVALID_HANDLE_VALUE;
	mutable std::mutex    m_mutex;
	std::vector<uint32_t> m_free;
	std::vector<uint64_t> m_sums;
	uint32_t              m_next   = 0;
	uint64_t              m_in_use = 0;
#endif
};

} // namespace Xbox

#endif
