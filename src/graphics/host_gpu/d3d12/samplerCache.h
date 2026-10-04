#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SAMPLERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SAMPLERCACHE_H_

#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/shader/shaderBindings.h"

#include <array>
#include <cstddef>
#include <unordered_map>

namespace Libs::Graphics {

class DescriptorHeap;
struct GraphicContext;

// A sampler: a descriptor in a non-shader-visible sampler heap.
using SamplerHandle = D3D12_CPU_DESCRIPTOR_HANDLE;

// One sampler descriptor per distinct guest sampler.
class SamplerCache {
public:
	SamplerCache(GraphicContext& graphics, DescriptorHeap& heap)
	    : m_graphics(graphics), m_heap(heap) {}
	KYTY_CLASS_NO_COPY(SamplerCache);

	[[nodiscard]] SamplerHandle GetSampler(const ShaderSamplerResource& r, bool integer_border);

private:
	using SamplerKey = std::array<uint32_t, 5>;

	struct SamplerKeyHash {
		std::size_t operator()(const SamplerKey& key) const {
			std::size_t hash = 0;
			for (auto value: key) {
				hash ^= static_cast<std::size_t>(value) +
				        static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
				        (hash >> 2u);
			}
			return hash;
		}
	};

	GraphicContext&                                               m_graphics;
	DescriptorHeap&                                               m_heap;
	Common::Mutex                                                 m_mutex;
	std::unordered_map<SamplerKey, SamplerHandle, SamplerKeyHash> m_samplers;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SAMPLERCACHE_H_
