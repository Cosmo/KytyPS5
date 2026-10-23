#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_OCCLUSIONQUERIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_OCCLUSIONQUERIES_H_

#include "common/common.h"
#include "graphics/host_gpu/occlusionCounters.h"

#include <cstdint>
#include <memory>
#include <vector>

struct ID3D12GraphicsCommandList;
struct ID3D12QueryHeap;

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
struct GraphicContext;

// The guest's z-pass counters from D3D12 occlusion queries (Config::occlusion_queries_enabled), as
// the Vulkan renderer's: a draw counts its samples while a guest query is open, and the dumps'
// counters are written before the guest can see the GPU ran them. A dump resolves the queries
// ended since the last one into a readback buffer.
class OcclusionQueries {
public:
	OcclusionQueries(GraphicContext& graphics, CommandScheduler& scheduler);
	~OcclusionQueries();
	KYTY_CLASS_NO_COPY(OcclusionQueries);

	[[nodiscard]] bool Enabled() const { return m_heap != nullptr; }

	// Around the draw: the query that counts its samples, or -1.
	[[nodiscard]] int32_t Begin(ID3D12GraphicsCommandList* list) {
		return m_open == 0 ? -1 : BeginQuery(list);
	}
	void End(ID3D12GraphicsCommandList* list, int32_t query) const;

	// A PIXEL_PIPE_STAT_DUMP to address, after the draws recorded so far.
	void Dump(uint64_t address);
	// Waits for the GPU to run the dumps and writes their counters.
	void Publish() {
		if (!m_dumps.empty()) {
			PublishDumps();
		}
	}

private:
	struct PendingDump {
		uint64_t address;
		uint64_t tick;
		uint64_t queries;
		uint64_t uncounted;
	};

	int32_t BeginQuery(ID3D12GraphicsCommandList* list);
	void    PublishDumps();

	CommandScheduler&        m_scheduler;
	ID3D12QueryHeap*         m_heap = nullptr;
	std::unique_ptr<Buffer>  m_results; // One 64-bit count per query.
	std::vector<PendingDump> m_dumps;
	uint32_t                 m_open      = 0;
	uint64_t                 m_begun     = 0;
	uint64_t                 m_resolved  = 0;
	uint64_t                 m_read      = 0;
	uint64_t                 m_samples   = 0;
	uint64_t                 m_uncounted = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_OCCLUSIONQUERIES_H_
