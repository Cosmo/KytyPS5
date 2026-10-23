#include "graphics/host_gpu/d3d12/occlusionQueries.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/d3d12/buffer.h"
#include "graphics/host_gpu/d3d12/commandScheduler.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include <algorithm>
#include <cstring>

namespace Libs::Graphics {

// Queries no dump has read yet; past them a draw counts one sample.
constexpr uint32_t QUERY_COUNT = 16384;

OcclusionQueries::OcclusionQueries(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler) {
	if (!Config::OcclusionQueriesEnabled()) {
		return;
	}
	D3D12_QUERY_HEAP_DESC desc {};
	desc.Type  = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
	desc.Count = QUERY_COUNT;
	D3D12::Check(graphics.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&m_heap)), "create occlusion query heap");
	m_results = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
	                                     uint64_t {QUERY_COUNT} * sizeof(uint64_t));
	m_results->SetName("occlusion query results");
	LOGF("D3D12 occlusion queries: true\n");
}

OcclusionQueries::~OcclusionQueries() {
	if (m_heap != nullptr) {
		m_heap->Release();
	}
}

int32_t OcclusionQueries::BeginQuery(ID3D12GraphicsCommandList* list) {
	if (m_begun - m_read == QUERY_COUNT) {
		m_uncounted++;
		return -1;
	}
	const auto query = static_cast<uint32_t>(m_begun++ % QUERY_COUNT);
	// D3D12_QUERY_TYPE_OCCLUSION counts the samples exactly.
	list->BeginQuery(m_heap, D3D12_QUERY_TYPE_OCCLUSION, query);
	return static_cast<int32_t>(query);
}

void OcclusionQueries::End(ID3D12GraphicsCommandList* list, int32_t query) const {
	if (query >= 0) {
		list->EndQuery(m_heap, D3D12_QUERY_TYPE_OCCLUSION, static_cast<uint32_t>(query));
	}
}

void OcclusionQueries::Dump(uint64_t address) {
	// Begins dump to the results, ends 8 bytes into them.
	if ((address & 8u) == 0) {
		m_open++;
	} else if (m_open != 0) {
		m_open--;
	}
	if (m_resolved != m_begun) {
		// The queries ended so far go to their slots of the results; they wrap around the heap at
		// most once.
		auto& command = m_scheduler.Current();
		m_results->Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
		for (uint64_t resolved = m_resolved; resolved < m_begun;) {
			const auto first = static_cast<uint32_t>(resolved % QUERY_COUNT);
			const auto count =
			    static_cast<uint32_t>(std::min<uint64_t>(m_begun - resolved, QUERY_COUNT - first));
			command.Handle()->ResolveQueryData(m_heap, D3D12_QUERY_TYPE_OCCLUSION, first, count,
			                                   m_results->Resource(), uint64_t {first} * sizeof(uint64_t));
			resolved += count;
		}
		m_resolved = m_begun;
	}
	m_dumps.push_back({address, m_scheduler.CurrentTick(), m_begun, m_uncounted});
}

void OcclusionQueries::PublishDumps() {
	if (m_dumps.back().queries != m_read) {
		m_scheduler.Wait(m_dumps.back().tick);
	}
	const auto results = m_results->Mapped();
	for (const auto& dump: m_dumps) {
		for (; m_read < dump.queries; m_read++) {
			uint64_t samples = 0;
			std::memcpy(&samples, results.data() + (m_read % QUERY_COUNT) * sizeof(uint64_t), sizeof(samples));
			m_samples += samples;
		}
		WriteZPassCounters(reinterpret_cast<volatile uint64_t*>(dump.address),
		                   m_samples + dump.uncounted, (dump.address & 8u) == 0);
	}
	m_dumps.clear();
}

} // namespace Libs::Graphics
