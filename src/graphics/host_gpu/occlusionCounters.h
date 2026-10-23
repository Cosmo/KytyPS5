#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_OCCLUSIONCOUNTERS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_OCCLUSIONCOUNTERS_H_

#include <cstdint>

namespace Libs::Graphics {

// The guest's query results (16-byte aligned): a begin/end pair of running z-pass counters per DB,
// 16 bytes apart; bit 63 marks a written counter. A query's samples are the sum of end - begin.
constexpr uint32_t OCCLUSION_QUERY_DBS   = 16;
constexpr uint64_t OCCLUSION_QUERY_READY = 1ull << 63u;

// A dump of the running count, all of it in DB 0. A begin also clears its end counters.
inline void WriteZPassCounters(volatile uint64_t* results, uint64_t samples, bool begin) {
	for (uint32_t db = 0; db < OCCLUSION_QUERY_DBS; db++) {
		results[db * 2] = OCCLUSION_QUERY_READY | (db == 0 ? samples : 0);
		if (begin) {
			results[db * 2 + 1] = 0;
		}
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_OCCLUSIONCOUNTERS_H_
