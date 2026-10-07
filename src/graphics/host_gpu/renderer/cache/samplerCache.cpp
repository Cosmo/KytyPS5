#include "graphics/host_gpu/renderer/cache/samplerCache.h"

#include "graphics/host_gpu/renderer/cache/samplerInfo.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/renderContext.h"

namespace Libs::Graphics {

SamplerCache::~SamplerCache() {
	for (const auto& [key, sampler]: m_samplers) {
		(void)key;
		m_graphics.device.destroySampler(sampler, nullptr);
	}
}

vk::Sampler SamplerCache::GetSampler(const ShaderSamplerResource& r, bool integer_border) {
	Common::LockGuard lock(m_mutex);

	const SamplerKey key {r.fields[0], r.fields[1], r.fields[2], r.fields[3], integer_border};
	if (auto iter = m_samplers.find(key); iter != m_samplers.end()) {
		return iter->second;
	}

	const auto sampler_info = GuestSamplerInfo(r, integer_border, m_graphics.GetPhysicalDeviceProperties().limits.maxSamplerLodBias);

	vk::Sampler vk_sampler = nullptr;
	const auto  result     = m_graphics.device.createSampler(&sampler_info, nullptr, &vk_sampler);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || vk_sampler == nullptr);

	m_samplers.emplace(key, vk_sampler);
	return vk_sampler;
}

} // namespace Libs::Graphics
