#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_FORMATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_FORMATS_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics::D3D12 {

// How a renderer format (a vk::Format, the renderer's format identifier) is stored and viewed.
struct FormatInfo {
	DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN; // resource format; views may use any typed member
	DXGI_FORMAT view   = DXGI_FORMAT_UNKNOWN; // typed format of shader resource views
	// Shader resource view component mapping, for formats whose DXGI equivalent orders the
	// channels differently. Such formats cannot be written through render target or UAV views.
	UINT mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	// Depth/stencil formats.
	DXGI_FORMAT depth_view   = DXGI_FORMAT_UNKNOWN; // depth-stencil view
	DXGI_FORMAT stencil_view = DXGI_FORMAT_UNKNOWN; // shader resource view of the stencil plane

	[[nodiscard]] bool Supported() const noexcept { return view != DXGI_FORMAT_UNKNOWN; }
	[[nodiscard]] bool Writable() const noexcept {
		return mapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	}
	[[nodiscard]] bool IsDepth() const noexcept { return depth_view != DXGI_FORMAT_UNKNOWN; }
};

[[nodiscard]] FormatInfo GetFormatInfo(vk::Format format);
// The input assembler format of a vertex attribute; DXGI_FORMAT_UNKNOWN when unsupported.
[[nodiscard]] DXGI_FORMAT VertexFormat(vk::Format format);

// The host format of a guest colour format: the one the guest's encoding and channel order call for (renderer/image/textureCommon.cpp), as a
// format D3D12 can render as well as sample, with no component mapping. The bits stay where the guest keeps them; only the channel names
// change, and `host_to_storage` (each host channel -> the guest's storage channel) changes with them, so the pixel shader's export mapping
// and the views' component mappings put every channel where the guest has it.
struct HostColorFormat {
	vk::Format                      format = vk::Format::eUndefined;
	Prospero::ColorComponentMapping host_to_storage;
};
[[nodiscard]] HostColorFormat RenderableHostFormat(vk::Format format, Prospero::ColorComponentMapping host_to_storage);

// Combines a view's component mapping with the format's own channel order.
[[nodiscard]] UINT ComposeMapping(const vk::ComponentMapping& view, UINT format_mapping);

} // namespace Libs::Graphics::D3D12

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_FORMATS_H_
