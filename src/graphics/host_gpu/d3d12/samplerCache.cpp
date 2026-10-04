#include "graphics/host_gpu/d3d12/samplerCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/d3d12/descriptorHeap.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/samplerInfo.h"

#include <atomic>

namespace Libs::Graphics {

namespace {

[[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE AddressMode(vk::SamplerAddressMode mode) {
	switch (mode) {
		case vk::SamplerAddressMode::eRepeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		case vk::SamplerAddressMode::eMirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		case vk::SamplerAddressMode::eClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		case vk::SamplerAddressMode::eClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		case vk::SamplerAddressMode::eMirrorClampToEdge:
			return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
		default: EXIT("unsupported sampler address mode %d\n", static_cast<int>(mode));
	}
}

[[nodiscard]] D3D12_FILTER_TYPE FilterType(vk::Filter filter) {
	return filter == vk::Filter::eLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
}

} // namespace

SamplerHandle SamplerCache::GetSampler(const ShaderSamplerResource& r, bool integer_border) {
	Common::LockGuard lock(m_mutex);

	const SamplerKey key {r.fields[0], r.fields[1], r.fields[2], r.fields[3], integer_border ? 1u : 0u};
	if (auto found = m_samplers.find(key); found != m_samplers.end()) {
		return found->second;
	}

	// D3D12's mip LOD bias range is -16 to 15.99.
	const auto info = GuestSamplerInfo(r, integer_border, 15.99f);
	if (info.unnormalizedCoordinates != VK_FALSE) {
		static std::atomic_bool reported = false;
		if (!reported.exchange(true)) {
			Log::WriteToConsoleAndLog("D3D12: samplers with unnormalized coordinates are not supported yet; they sample with normalized coordinates\n");
		}
	}
	D3D12_SAMPLER_DESC desc {};
	const auto reduction = info.compareEnable != VK_FALSE ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
	                                                      : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
	if (info.anisotropyEnable != VK_FALSE) {
		desc.Filter        = D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
		desc.MaxAnisotropy = static_cast<UINT>(info.maxAnisotropy);
	} else {
		desc.Filter = D3D12_ENCODE_BASIC_FILTER(
		    FilterType(info.minFilter), FilterType(info.magFilter),
		    info.mipmapMode == vk::SamplerMipmapMode::eLinear ? D3D12_FILTER_TYPE_LINEAR
		                                                      : D3D12_FILTER_TYPE_POINT,
		    reduction);
		desc.MaxAnisotropy = 1;
	}
	desc.AddressU       = AddressMode(info.addressModeU);
	desc.AddressV       = AddressMode(info.addressModeV);
	desc.AddressW       = AddressMode(info.addressModeW);
	desc.MipLODBias     = info.mipLodBias;
	// vk::CompareOp and D3D12_COMPARISON_FUNC list the same functions, offset by one.
	desc.ComparisonFunc = info.compareEnable != VK_FALSE
	                          ? static_cast<D3D12_COMPARISON_FUNC>(static_cast<int>(info.compareOp) + 1)
	                          : D3D12_COMPARISON_FUNC_NEVER;
	desc.MinLOD         = info.minLod;
	desc.MaxLOD         = info.maxLod;
	switch (info.borderColor) {
		case vk::BorderColor::eIntOpaqueBlack:
		case vk::BorderColor::eFloatOpaqueBlack: desc.BorderColor[3] = 1.0f; break;
		case vk::BorderColor::eIntOpaqueWhite:
		case vk::BorderColor::eFloatOpaqueWhite:
			desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = 1.0f;
			desc.BorderColor[3]                                           = 1.0f;
			break;
		default: break;
	}

	const auto handle = m_heap.AllocateCpu(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	m_graphics.device->CreateSampler(&desc, handle);
	m_samplers.emplace(key, handle);
	return handle;
}

} // namespace Libs::Graphics
