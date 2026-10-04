#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_IMAGEBACKING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_IMAGEBACKING_H_

#include "common/common.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

} // namespace Libs::Graphics

namespace D3D12MA {
class Allocation;
} // namespace D3D12MA

namespace Libs::Graphics {

// A view of an image: an index into ImageBacking::views.
struct ImageViewHandle {
	uint32_t index = UINT32_MAX;
};

// The descriptors of one view, created when first needed. The shared view cache normalizes
// sampled and attachment views to one description, while D3D12 needs a descriptor per usage.
struct ImageViewDescriptors {
	ImageViewInfo               info;
	uint32_t                    resource = 0; // index into ImageBacking::resources
	D3D12_CPU_DESCRIPTOR_HANDLE srv {};
	D3D12_CPU_DESCRIPTOR_HANDLE uav {};
	D3D12_CPU_DESCRIPTOR_HANDLE rtv {};
	D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
	D3D12_CPU_DESCRIPTOR_HANDLE dsv_read_only {};
};

// One D3D12 resource holding an image, and the state of each of its subresources.
struct ImageResource {
	D3D12MA::Allocation*               allocation = nullptr;
	ID3D12Resource*                    resource   = nullptr; // owned by allocation
	// A placed render or depth target starts undefined; its first use discards it.
	bool                               needs_discard = false;
	DXGI_FORMAT                        family   = DXGI_FORMAT_UNKNOWN;
	std::vector<D3D12_RESOURCE_STATES> states;
	uint64_t                           version = 0; // contents are current when equal to the image's
};

// The host resources of an image. D3D12 views must share their resource's format family, so an
// image viewed in formats of other families keeps one resource per family and copies between
// them when a different family is used (resources[0] is the primary one).
struct ImageBacking {
	ImageBacking() = default;
	KYTY_CLASS_NO_COPY(ImageBacking);

	vk::Format           format     = vk::Format::eUndefined;
	vk::ImageType        image_type = vk::ImageType::e2D;
	vk::Extent3D         extent     = {1, 1, 1};
	uint32_t             layers     = 1;
	uint32_t             mip_levels = 1;
	uint32_t             samples    = 1;
	vk::ImageCreateFlags flags      = {};
	uint32_t             planes     = 1; // 2 for depth/stencil formats
	ID3D12Resource*      image      = nullptr; // resources[0].resource
	D3D12_RESOURCE_FLAGS resource_flags = D3D12_RESOURCE_FLAG_NONE;

	std::vector<ImageResource>        resources;
	std::vector<ImageViewDescriptors> views;
	uint64_t                   version = 1; // bumped by every write to the current resource
	uint32_t                   current = 0; // resource holding the latest contents

	[[nodiscard]] uint32_t Subresource(uint32_t level, uint32_t layer, uint32_t plane) const {
		const uint32_t array_size = image_type == vk::ImageType::e3D ? 1u : layers;
		return level + (layer + plane * array_size) * mip_levels;
	}
	[[nodiscard]] uint32_t SubresourceCount() const {
		return mip_levels * (image_type == vk::ImageType::e3D ? 1u : layers) * planes;
	}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_IMAGEBACKING_H_
