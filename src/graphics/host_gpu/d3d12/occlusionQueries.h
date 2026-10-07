#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_OCCLUSIONQUERIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_OCCLUSIONQUERIES_H_

#include <cstdint>

namespace Libs::Graphics {

// The D3D12 renderer counts no samples of the guest's occlusion queries: they read as all visible
// (CommandProcessor::EventWrite), as in upstream's renderer without --occlusion-queries.
class OcclusionQueries {
public:
	[[nodiscard]] bool Enabled() const { return false; }
	void               Dump(uint64_t /*address*/) {}
	void               Publish() {}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_OCCLUSIONQUERIES_H_
