// The compressed pool in front of the pager's swap file. A window is still written to the file as before, and a copy compressed with LZ4 stays in RAM: the
// pool is a read cache, and a window read back from it takes about 20 us instead of about 600 us. A copy can go at any time: the oldest go when the pool is
// full.
#include "xbox/pagerStore.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// The LZ4 Tracy brings. The UWP app builds Tracy without its zones, and then nothing else compiles this file.
#if defined(TRACY_ENABLE)
#include "../../3rdparty/tracy/public/common/tracy_lz4.hpp"
#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "../../3rdparty/tracy/public/common/tracy_lz4.cpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

namespace Xbox {

namespace {

constexpr uint64_t Window       = PagerStore::kSlotSize;
constexpr uint32_t Granule      = 4096; // the pool's unit: a window takes any free ones, so nothing fragments
constexpr uint32_t ChunkSize    = 1024; // granules the pool grows by (4 MB)
constexpr uint32_t MaxGranules  = 12;   // a window that stays over 75% of its size is not kept: it only goes to the file
constexpr uint32_t MaxSlots     = 262144; // 16 GB of swap file; slots above are not kept
constexpr uint32_t None         = UINT32_MAX;

struct Entry {
	uint32_t bytes = 0; // 0: not in the pool
	uint32_t newer = None;
	uint32_t older = None;
	uint32_t count = 0;
	uint32_t granules[MaxGranules] {};
};

// Everything is allocated when the pool starts or grows, never for a window: a window can leave on a guest thread's stack (Regions::MakeRoom), or with the
// heap full.
struct Pool {
	std::mutex            mutex;
	uint32_t              max_chunks = 0; // 0: off
	std::vector<char*>    chunks;
	std::vector<uint32_t> free;
	std::vector<Entry>    entries; // by slot
	uint32_t              newest = None;
	uint32_t              oldest = None;
	char                  gather[MaxGranules * Granule];
};

Pool g_pool;

char* GranuleAt(uint32_t index) {
	return g_pool.chunks[index / ChunkSize] + static_cast<uint64_t>(index % ChunkSize) * Granule;
}

void Remove(uint32_t slot, PagerCounters& counters) {
	auto& entry = g_pool.entries[slot];
	if (entry.bytes == 0) {
		return;
	}
	(entry.newer != None ? g_pool.entries[entry.newer].older : g_pool.newest) = entry.older;
	(entry.older != None ? g_pool.entries[entry.older].newer : g_pool.oldest) = entry.newer;
	for (uint32_t i = 0; i < entry.count; i++) {
		g_pool.free.push_back(entry.granules[i]);
	}
	counters.pool_bytes -= uint64_t {entry.count} * Granule;
	counters.pool_raw -= Window;
	entry.bytes = 0;
	entry.count = 0;
	entry.newer = None;
	entry.older = None;
}

// Room for `need` granules: the pool grows up to its size, then its oldest windows go.
bool Reserve(uint32_t need, PagerCounters& counters) {
	while (g_pool.free.size() < need) {
		if (g_pool.chunks.size() < g_pool.max_chunks) {
			if (auto* chunk = static_cast<char*>(std::malloc(uint64_t {ChunkSize} * Granule)); chunk != nullptr) {
				const auto first = static_cast<uint32_t>(g_pool.chunks.size()) * ChunkSize;
				g_pool.chunks.push_back(chunk);
				for (uint32_t i = 0; i < ChunkSize; i++) {
					g_pool.free.push_back(first + i);
				}
				continue;
			}
		}
		if (g_pool.oldest == None) {
			return false;
		}
		Remove(g_pool.oldest, counters);
		counters.pool_dropped++;
	}
	return true;
}

void Put(uint32_t slot, const char* packed, uint32_t bytes, PagerCounters& counters) {
	std::lock_guard lock(g_pool.mutex);
	if (g_pool.max_chunks == 0) {
		return;
	}
	const uint32_t need = (bytes + Granule - 1) / Granule;
	if (slot >= MaxSlots || need > MaxGranules) {
		counters.pool_skipped++;
		return;
	}
	Remove(slot, counters);
	if (!Reserve(need, counters)) {
		counters.pool_skipped++;
		return;
	}
	auto& entry = g_pool.entries[slot];
	for (uint32_t i = 0; i < need; i++) {
		const uint32_t granule = g_pool.free.back();
		g_pool.free.pop_back();
		entry.granules[i] = granule;
		std::memcpy(GranuleAt(granule), packed + uint64_t {i} * Granule, std::min<uint64_t>(Granule, bytes - uint64_t {i} * Granule));
	}
	entry.bytes = bytes;
	entry.count = need;
	entry.older = g_pool.newest;
	entry.newer = None;
	(g_pool.newest != None ? g_pool.entries[g_pool.newest].newer : g_pool.oldest) = slot;
	g_pool.newest = slot;
	counters.pool_bytes += uint64_t {need} * Granule;
	counters.pool_raw += Window;
}

} // namespace

void PagerPoolPut(uint32_t slot, const void* data, uint64_t size, PagerCounters& counters) {
	// Fixed buffers, as in the pool. A second thread at the same time leaves its window out of the pool.
	static std::mutex mutex;
	std::unique_lock  lock(mutex, std::try_to_lock);
	if (!lock.owns_lock() || size != Window) {
		counters.pool_skipped++;
		return;
	}
	static tracy::LZ4_stream_t state;
	static char                packed[LZ4_COMPRESSBOUND(Window)];
	const int bytes = tracy::LZ4_compress_fast_extState(&state, static_cast<const char*>(data), packed, static_cast<int>(size), static_cast<int>(sizeof(packed)), 1);
	if (bytes > 0) {
		Put(slot, packed, static_cast<uint32_t>(bytes), counters);
	}
}

bool StartPagerPool(uint64_t bytes) {
	std::lock_guard lock(g_pool.mutex);
	const auto      chunks = static_cast<uint32_t>(bytes / (uint64_t {ChunkSize} * Granule));
	if (chunks == 0) {
		return false;
	}
	g_pool.entries.resize(MaxSlots);
	g_pool.chunks.reserve(chunks);
	g_pool.free.reserve(uint64_t {chunks} * ChunkSize);
	g_pool.max_chunks = chunks;
	return true;
}

bool PagerPoolRead(uint32_t slot, void* data, PagerCounters& counters) {
	std::lock_guard lock(g_pool.mutex);
	if (slot >= g_pool.entries.size() || g_pool.entries[slot].bytes == 0) {
		return false;
	}
	const auto& entry = g_pool.entries[slot];
	for (uint32_t i = 0; i < entry.count; i++) {
		std::memcpy(g_pool.gather + uint64_t {i} * Granule, GranuleAt(entry.granules[i]), Granule);
	}
	if (tracy::LZ4_decompress_safe(g_pool.gather, static_cast<char*>(data), static_cast<int>(entry.bytes), static_cast<int>(Window)) != static_cast<int>(Window)) {
		counters.pool_bad++;
		Remove(slot, counters);
		return false; // the file has it
	}
	counters.pool_hits++;
	return true;
}

void PagerPoolDrop(uint32_t slot, PagerCounters& counters) {
	std::lock_guard lock(g_pool.mutex);
	if (slot < g_pool.entries.size()) {
		Remove(slot, counters);
	}
}

} // namespace Xbox
