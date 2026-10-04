#include "graphics/host_gpu/d3d12/formats.h"

#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics::D3D12 {

namespace {

constexpr UINT Map(UINT r, UINT g, UINT b, UINT a) {
	return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a);
}

constexpr UINT SwapRB = Map(2, 1, 0, 3);
constexpr UINT One    = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;

constexpr FormatInfo Color(DXGI_FORMAT family, DXGI_FORMAT view,
                           UINT mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) {
	return {family, view, mapping};
}

constexpr FormatInfo Depth(DXGI_FORMAT family, DXGI_FORMAT view, DXGI_FORMAT depth_view,
                           DXGI_FORMAT stencil_view = DXGI_FORMAT_UNKNOWN) {
	return {family, view, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, depth_view, stencil_view};
}

} // namespace

FormatInfo GetFormatInfo(vk::Format format) {
	using F = vk::Format;
	switch (format) {
		case F::eR8Unorm: return Color(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM);
		case F::eR8Snorm: return Color(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SNORM);
		case F::eR8Uint: return Color(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UINT);
		case F::eR8Sint: return Color(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_SINT);

		case F::eR8G8Unorm: return Color(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM);
		case F::eR8G8Snorm: return Color(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SNORM);
		case F::eR8G8Uint: return Color(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UINT);
		case F::eR8G8Sint: return Color(DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_SINT);

		case F::eR8G8B8A8Unorm:
		case F::eA8B8G8R8UnormPack32:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM);
		case F::eR8G8B8A8Srgb:
		case F::eA8B8G8R8SrgbPack32:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
		case F::eR8G8B8A8Snorm:
		case F::eA8B8G8R8SnormPack32:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SNORM);
		case F::eR8G8B8A8Uint:
		case F::eA8B8G8R8UintPack32:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT);
		case F::eR8G8B8A8Sint:
		case F::eA8B8G8R8SintPack32:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SINT);

		case F::eB8G8R8A8Unorm:
			return Color(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM);
		case F::eB8G8R8A8Srgb:
			return Color(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
		case F::eB8G8R8A8Snorm:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SNORM, SwapRB);
		case F::eB8G8R8A8Uint:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UINT, SwapRB);
		case F::eB8G8R8A8Sint:
			return Color(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_SINT, SwapRB);

		case F::eR16Unorm: return Color(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM);
		case F::eR16Snorm: return Color(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SNORM);
		case F::eR16Uint: return Color(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UINT);
		case F::eR16Sint: return Color(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_SINT);
		case F::eR16Sfloat: return Color(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_FLOAT);

		case F::eR16G16Unorm: return Color(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM);
		case F::eR16G16Snorm: return Color(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SNORM);
		case F::eR16G16Uint: return Color(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UINT);
		case F::eR16G16Sint: return Color(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_SINT);
		case F::eR16G16Sfloat: return Color(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT);

		case F::eR16G16B16A16Unorm:
			return Color(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM);
		case F::eR16G16B16A16Snorm:
			return Color(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SNORM);
		case F::eR16G16B16A16Uint:
			return Color(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UINT);
		case F::eR16G16B16A16Sint:
			return Color(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_SINT);
		case F::eR16G16B16A16Sfloat:
			return Color(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT);

		case F::eR32Uint: return Color(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT);
		case F::eR32Sint: return Color(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_SINT);
		case F::eR32Sfloat: return Color(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT);

		case F::eR32G32Uint: return Color(DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_UINT);
		case F::eR32G32Sint: return Color(DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_SINT);
		case F::eR32G32Sfloat: return Color(DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT);

		case F::eR32G32B32Uint:
			return Color(DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_UINT);
		case F::eR32G32B32Sint:
			return Color(DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_SINT);
		case F::eR32G32B32Sfloat:
			return Color(DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_FLOAT);

		case F::eR32G32B32A32Uint:
			return Color(DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_UINT);
		case F::eR32G32B32A32Sint:
			return Color(DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_SINT);
		case F::eR32G32B32A32Sfloat:
			return Color(DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT);

		// Packed formats name channels from the most significant bits, DXGI from the least.
		case F::eA2B10G10R10UnormPack32:
			return Color(DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM);
		case F::eA2B10G10R10UintPack32:
			return Color(DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT);
		case F::eA2R10G10B10UnormPack32:
			return Color(DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, SwapRB);
		case F::eA2R10G10B10UintPack32:
			return Color(DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UINT, SwapRB);
		case F::eB10G11R11UfloatPack32:
			return Color(DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT);
		case F::eE5B9G9R9UfloatPack32:
			return Color(DXGI_FORMAT_R9G9B9E5_SHAREDEXP, DXGI_FORMAT_R9G9B9E5_SHAREDEXP);
		case F::eR5G6B5UnormPack16:
			return Color(DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM);
		case F::eB5G6R5UnormPack16:
			return Color(DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, SwapRB);
		case F::eA1R5G5B5UnormPack16:
			return Color(DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM);
		case F::eA4R4G4B4UnormPack16:
			return Color(DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM);
		case F::eR4G4B4A4UnormPack16:
			return Color(DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, Map(3, 0, 1, 2));

		case F::eBc1RgbaUnormBlock: return Color(DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM);
		case F::eBc1RgbaSrgbBlock:
			return Color(DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB);
		case F::eBc1RgbUnormBlock:
			return Color(DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, Map(0, 1, 2, One));
		case F::eBc1RgbSrgbBlock:
			return Color(DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB, Map(0, 1, 2, One));
		case F::eBc2UnormBlock: return Color(DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM);
		case F::eBc2SrgbBlock: return Color(DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB);
		case F::eBc3UnormBlock: return Color(DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM);
		case F::eBc3SrgbBlock: return Color(DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB);
		case F::eBc4UnormBlock: return Color(DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM);
		case F::eBc4SnormBlock: return Color(DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_SNORM);
		case F::eBc5UnormBlock: return Color(DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM);
		case F::eBc5SnormBlock: return Color(DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_SNORM);
		case F::eBc6HUfloatBlock: return Color(DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_UF16);
		case F::eBc6HSfloatBlock: return Color(DXGI_FORMAT_BC6H_TYPELESS, DXGI_FORMAT_BC6H_SF16);
		case F::eBc7UnormBlock: return Color(DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM);
		case F::eBc7SrgbBlock: return Color(DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB);

		case F::eD16Unorm:
			return Depth(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM);
		case F::eD32Sfloat:
			return Depth(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT);
		case F::eX8D24UnormPack32:
		case F::eD24UnormS8Uint:
			return Depth(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
			             DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_X24_TYPELESS_G8_UINT);
		case F::eD32SfloatS8Uint:
			return Depth(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
			             DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);

		default: return {};
	}
}

DXGI_FORMAT VertexFormat(vk::Format format) {
	const auto info = GetFormatInfo(format);
	// Swizzled formats rely on view component mappings, which vertex fetch lacks.
	return info.Supported() && info.Writable() && !info.IsDepth() && vk::blockExtent(format)[0] == 1
	           ? info.view
	           : DXGI_FORMAT_UNKNOWN;
}

UINT ComposeMapping(const vk::ComponentMapping& view, UINT format_mapping) {
	const vk::ComponentSwizzle swizzles[] {view.r, view.g, view.b, view.a};
	UINT                       sources[4] {};
	for (UINT channel = 0; channel < 4; channel++) {
		switch (swizzles[channel]) {
			case vk::ComponentSwizzle::eZero:
				sources[channel] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
				break;
			case vk::ComponentSwizzle::eOne:
				sources[channel] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
				break;
			case vk::ComponentSwizzle::eR:
			case vk::ComponentSwizzle::eG:
			case vk::ComponentSwizzle::eB:
			case vk::ComponentSwizzle::eA: {
				const auto component = static_cast<UINT>(swizzles[channel]) -
				                       static_cast<UINT>(vk::ComponentSwizzle::eR);
				sources[channel] = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(component, format_mapping);
				break;
			}
			default:
				sources[channel] = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(channel, format_mapping);
				break;
		}
	}
	return Map(sources[0], sources[1], sources[2], sources[3]);
}

} // namespace Libs::Graphics::D3D12
