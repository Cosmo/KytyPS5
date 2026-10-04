// D3D12 implementation of the Image backend operations (see renderer/image/image.h).

#include "graphics/host_gpu/renderer/image/image.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_rows_spv.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/gpuBackend.h"
#include "graphics/host_gpu/renderer/image/imageView.h"

#include <algorithm>
#include <atomic>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>
#include <D3D12MemAlloc.h>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

[[nodiscard]] uint64_t DivCeil(uint64_t value, uint64_t divisor) {
	return (value + divisor - 1) / divisor;
}

[[nodiscard]] bool IsWriteState(D3D12_RESOURCE_STATES state) {
	return (state & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
	                 D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST |
	                 D3D12_RESOURCE_STATE_RESOLVE_DEST)) != 0;
}

[[nodiscard]] vk::ImageType HostImageType(Prospero::ImageType type) {
	switch (type) {
		case Prospero::ImageType::kColor1D: return vk::ImageType::e1D;
		case Prospero::ImageType::kColor3D: return vk::ImageType::e3D;
		case Prospero::ImageType::kColor2D: return vk::ImageType::e2D;
		default: EXIT("non-base image type: %u\n", static_cast<uint32_t>(type));
	}
}

// SRGB formats cannot be written; their UNORM family members tell whether the family can.
[[nodiscard]] DXGI_FORMAT WritableEquivalent(DXGI_FORMAT format) {
	switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		default: return format;
	}
}

[[nodiscard]] D3D12_RESOURCE_FLAGS ResourceFlags(ID3D12Device* device,
                                                 const D3D12::FormatInfo& format, uint32_t samples,
                                                 bool block) {
	if (format.IsDepth()) {
		return D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	}
	if (block) {
		return D3D12_RESOURCE_FLAG_NONE;
	}
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support {WritableEquivalent(format.view)};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
	                                       sizeof(support)))) {
		return D3D12_RESOURCE_FLAG_NONE;
	}
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	if ((support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0) {
		flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	}
	if (samples == 1 && (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0) {
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	return flags;
}

// The format the placed footprint of a subresource must use.
[[nodiscard]] DXGI_FORMAT CopyFormat(ID3D12Device* device, ID3D12Resource* resource,
                                     UINT subresource) {
	const auto                         desc = resource->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout {};
	device->GetCopyableFootprints(&desc, subresource, 1, 0, &layout, nullptr, nullptr, nullptr);
	return layout.Footprint.Format;
}

[[nodiscard]] uint32_t Plane(vk::ImageAspectFlags aspect) {
	return aspect == vk::ImageAspectFlagBits::eStencil ? 1u : 0u;
}

// Bytes and texel extent of one copy block of an image aspect.
struct CopyBlock {
	uint32_t bytes  = 0;
	uint32_t width  = 1;
	uint32_t height = 1;
};

[[nodiscard]] CopyBlock GetCopyBlock(vk::Format format, vk::ImageAspectFlags aspect) {
	if (aspect == vk::ImageAspectFlagBits::eStencil) {
		return {1};
	}
	if (aspect == vk::ImageAspectFlagBits::eDepth) {
		return {DepthAspectTransferBytes(format)};
	}
	const auto extent = vk::blockExtent(format);
	return {vk::blockSize(format), extent[0], extent[1]};
}

void SetDebugName(ID3D12Resource* resource, const std::string& name) {
	const std::wstring wide(name.begin(), name.end());
	resource->SetName(wide.c_str());
}

// A device-local buffer that lives until the GPU completed the current tick.
Buffer& ScratchBuffer(GraphicContext& graphics, CommandScheduler& scheduler, uint64_t size) {
	auto  scratch = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
	                                         std::max<uint64_t>(size, 4));
	auto& result  = *scratch;
	scheduler.DeferOperation([owner = std::move(scratch)]() mutable { owner.reset(); });
	return result;
}

// Records the row copy kernel between two buffer layouts.
struct RowCopy {
	uint64_t src_offset      = 0;
	uint64_t src_row_pitch   = 0;
	uint64_t src_slice_pitch = 0;
	uint64_t dst_offset      = 0;
	uint64_t dst_row_pitch   = 0;
	uint64_t dst_slice_pitch = 0;
	uint32_t row_bytes       = 0;
	uint32_t rows            = 0;
	uint32_t slices          = 0;
};

void RecordRowCopy(CommandBuffer& command, const Buffer& source, const Buffer& destination,
                   const RowCopy& copy) {
	constexpr uint64_t Alignment    = 16;
	const uint64_t     src_root     = Common::AlignDown(copy.src_offset, Alignment);
	const uint64_t     dst_root     = Common::AlignDown(copy.dst_offset, Alignment);
	const uint64_t     src_extent   = source.Size() - src_root;
	const uint64_t     src_relative = copy.src_offset - src_root;
	const uint64_t     dst_relative = copy.dst_offset - dst_root;
	EXIT_NOT_IMPLEMENTED(src_extent > UINT32_MAX ||
	                     src_relative + copy.src_slice_pitch * copy.slices > UINT32_MAX ||
	                     dst_relative + copy.dst_slice_pitch * copy.slices > UINT32_MAX ||
	                     copy.src_row_pitch > UINT32_MAX || copy.dst_row_pitch > UINT32_MAX);
	const uint32_t params[] = {static_cast<uint32_t>(src_relative),
	                           static_cast<uint32_t>(copy.src_row_pitch),
	                           static_cast<uint32_t>(copy.src_slice_pitch),
	                           static_cast<uint32_t>(src_extent),
	                           static_cast<uint32_t>(dst_relative),
	                           static_cast<uint32_t>(copy.dst_row_pitch),
	                           static_cast<uint32_t>(copy.dst_slice_pitch),
	                           copy.row_bytes,
	                           copy.rows,
	                           copy.slices};
	auto& kernels  = command.GetContext().GetComputeKernels();
	auto* pipeline = kernels.Get(GPU_TILER_COPY_ROWS_SPV);
	ComputeKernels::Bindings bindings;
	bindings.input         = &source;
	bindings.input_offset  = src_root;
	bindings.output        = &destination;
	bindings.output_offset = dst_root;
	bindings.constants     = params;
	const auto dwords      = DivCeil(copy.row_bytes, 4);
	kernels.Dispatch(command, pipeline, bindings, static_cast<uint32_t>(DivCeil(dwords, 64)),
	                 copy.rows, copy.slices);
}

} // namespace

Image::Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& image_info)
    : info(image_info), m_graphics(graphics), m_scheduler(scheduler) {
	KYTY_PROFILER_FUNCTION();
	ImageOps::Validate(info);
	m_cpu_dirty =
	    !info.data.Empty() && info.metadata.compression == VideoOutCompression::Uncompressed;
	if (info.pixel_format == vk::Format::eUndefined) {
		return;
	}
	const auto format = D3D12::GetFormatInfo(info.pixel_format);
	if (!format.Supported()) {
		// Left without a host resource, like an image without a format: draws that use it are skipped (guard G1 for 96-bit formats).
		static std::atomic_bool reported = false;
		if (!reported.exchange(true)) {
			Log::WriteToConsoleAndLog(fmt::format("D3D12: image format {} is not supported; images of it are not created\n", static_cast<int>(info.pixel_format)));
		}
		return;
	}
	backing.format         = info.pixel_format;
	backing.image_type     = HostImageType(info.type);
	backing.extent         = info.extent;
	backing.layers         = info.IsVolume() ? 1u : info.resources.layers;
	backing.mip_levels     = info.resources.levels;
	backing.samples        = info.samples;
	backing.flags          = info.IsVolume() ? vk::ImageCreateFlagBits::e2DArrayCompatible
	                                         : vk::ImageCreateFlags {};
	backing.planes         = format.stencil_view != DXGI_FORMAT_UNKNOWN ? 2u : 1u;
	backing.resource_flags = ResourceFlags(graphics.device, format, info.samples, info.IsBlock());
	(void)ResourceIndex(info.pixel_format);
	backing.image                = backing.resources[0].resource;
	backing.resources[0].version = backing.version;
	if (Config::GraphicsDebugDumpEnabled()) {
		SetDebugName(backing.image,
		             fmt::format("Kyty.Image[guest=0x{:016x} size=0x{:x} extent={}x{}x{} format={} "
		                         "mips={} layers={} samples={}]",
		                         info.data.address, info.data.size, info.extent.width,
		                         info.extent.height, info.extent.depth,
		                         static_cast<uint32_t>(info.pixel_format), info.resources.levels,
		                         info.resources.layers, info.samples));
	}
}

Image::~Image() {
	KYTY_PROFILER_FUNCTION();
	if (!backing.views.empty()) {
		auto& heap = m_scheduler.Context().GetDescriptorHeap();
		for (const auto& view: backing.views) {
			const std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE> descriptors[] {
			    {view.srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
			    {view.uav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
			    {view.rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV},
			    {view.dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV},
			    {view.dsv_read_only, D3D12_DESCRIPTOR_HEAP_TYPE_DSV}};
			for (const auto& [handle, type]: descriptors) {
				if (handle.ptr != 0) {
					heap.FreeCpu(type, handle);
				}
			}
		}
	}
	for (auto& resource: backing.resources) {
		resource.allocation->Release();
	}
}

uint32_t Image::ResourceIndex(vk::Format view_format) {
	const auto format = D3D12::GetFormatInfo(view_format);
	EXIT_IF(!format.Supported());
	for (uint32_t i = 0; i < backing.resources.size(); i++) {
		if (backing.resources[i].family == format.family) {
			return i;
		}
	}

	D3D12_RESOURCE_DESC desc {};
	switch (backing.image_type) {
		case vk::ImageType::e1D: desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; break;
		case vk::ImageType::e2D: desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; break;
		default: desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; break;
	}
	desc.Width            = backing.extent.width;
	desc.Height           = backing.extent.height;
	desc.DepthOrArraySize = static_cast<UINT16>(
	    backing.image_type == vk::ImageType::e3D ? backing.extent.depth : backing.layers);
	desc.MipLevels        = static_cast<UINT16>(backing.mip_levels);
	desc.Format           = format.family;
	desc.SampleDesc.Count = backing.samples;
	desc.Flags            = backing.resources.empty()
	                            ? backing.resource_flags
	                            : ResourceFlags(m_graphics.device, format, backing.samples,
	                                            vk::blockSize(view_format) != 0 &&
	                                                vk::blockExtent(view_format)[0] > 1);

	// Render and depth targets start in their target state, the one a discard requires.
	const auto initial =
	    (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 ? D3D12_RESOURCE_STATE_DEPTH_WRITE
	    : (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0
	        ? D3D12_RESOURCE_STATE_RENDER_TARGET
	        : D3D12_RESOURCE_STATE_COMMON;
	D3D12MA::ALLOCATION_DESC allocation {};
	allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
	ImageResource resource;
	resource.family   = format.family;
	const auto result = m_graphics.allocator->CreateResource(&allocation, &desc, initial, nullptr,
	                                                         &resource.allocation, IID_NULL,
	                                                         nullptr);
	if (FAILED(result)) {
		m_graphics.LogMemoryBudget();
		EXIT("D3D12: failed to create image: extent=%ux%ux%u format=%d family=%d layers=%u "
		     "levels=%u samples=%u flags=0x%x, HRESULT=0x%08x\n",
		     backing.extent.width, backing.extent.height, backing.extent.depth,
		     static_cast<int>(view_format), static_cast<int>(format.family), backing.layers,
		     backing.mip_levels, backing.samples, static_cast<uint32_t>(desc.Flags),
		     static_cast<uint32_t>(result));
	}
	resource.resource      = resource.allocation->GetResource();
	resource.needs_discard =
	    initial != D3D12_RESOURCE_STATE_COMMON && resource.allocation->GetHeap() != nullptr;
	resource.states.assign(backing.SubresourceCount(), initial);
	backing.resources.push_back(std::move(resource));
	return static_cast<uint32_t>(backing.resources.size() - 1);
}

void Image::Transition(CommandBuffer& command, uint32_t resource, uint32_t state,
                       std::optional<ImageSubresourceRange> range) {
	auto&      target = backing.resources[resource];
	const auto after  = static_cast<D3D12_RESOURCE_STATES>(state);
	if (target.needs_discard) {
		// Still in its creation state; the first use initializes the placed resource.
		command.Handle()->DiscardResource(target.resource, nullptr);
		target.needs_discard = false;
	}
	const bool volume = backing.image_type == vk::ImageType::e3D;
	const auto levels = range ? ImageSubresourceRange {range->base_level, range->level_count,
	                                                   volume ? 0 : range->base_layer,
	                                                   volume ? 1 : range->layer_count}
	                          : ImageSubresourceRange {0, backing.mip_levels, 0, backing.layers};

	std::vector<D3D12_RESOURCE_BARRIER> barriers;
	for (uint32_t plane = 0; plane < backing.planes; plane++) {
		for (uint32_t level = levels.base_level; level < levels.base_level + levels.level_count;
		     level++) {
			for (uint32_t layer = levels.base_layer;
			     layer < levels.base_layer + levels.layer_count; layer++) {
				const auto index  = backing.Subresource(level, layer, plane);
				auto&      before = target.states[index];
				if (before == after) {
					continue;
				}
				D3D12_RESOURCE_BARRIER barrier {};
				barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition.pResource   = target.resource;
				barrier.Transition.Subresource = index;
				barrier.Transition.StateBefore = before;
				barrier.Transition.StateAfter  = after;
				barriers.push_back(barrier);
				before = after;
			}
		}
	}
	if (barriers.empty()) {
		return;
	}
	if (barriers.size() == target.states.size() &&
	    std::all_of(barriers.begin(), barriers.end(), [&](const auto& barrier) {
		    return barrier.Transition.StateBefore == barriers[0].Transition.StateBefore;
	    })) {
		barriers.resize(1);
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	}
	command.Handle()->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

void Image::Synchronize(CommandBuffer& command, uint32_t resource) {
	if (backing.resources[resource].version == backing.version) {
		return;
	}
	const auto source = backing.current;
	EXIT_IF(source == resource);
	std::vector<vk::ImageCopy> copies;
	const auto                 aspect = FullAspectMask(backing.format) & ~vk::ImageAspectFlagBits::eStencil;
	for (uint32_t level = 0; level < backing.mip_levels; level++) {
		vk::ImageCopy copy {};
		copy.srcSubresource = {aspect, level, 0, backing.layers};
		copy.dstSubresource = copy.srcSubresource;
		copy.extent         = {std::max(backing.extent.width >> level, 1u),
		                       std::max(backing.extent.height >> level, 1u),
		                       std::max(backing.extent.depth >> level, 1u)};
		copies.push_back(copy);
	}
	CopyContents(command, *this, source, resource, copies);
	backing.resources[resource].version = backing.version;
}

void Image::Use(CommandBuffer& command, uint32_t state, std::optional<ImageSubresourceRange> range,
                std::optional<ImageViewHandle> view) {
	const auto resource = view ? backing.views[view->index].resource : backing.current;
	Synchronize(command, resource);
	Transition(command, resource, state, range);
	if (IsWriteState(static_cast<D3D12_RESOURCE_STATES>(state))) {
		backing.version++;
		backing.resources[resource].version = backing.version;
		backing.current                     = resource;
	}
}

ImageViewHandle Image::CreateView(const ImageViewInfo& view_info) {
	if (backing.image_type == vk::ImageType::e3D && view_info.type != vk::ImageViewType::e3D &&
	    !(view_info.usage & vk::ImageUsageFlagBits::eStorage)) {
		EXIT_NOT_IMPLEMENTED("D3D12: sampled 2D views of 3D images are not supported\n");
	}
	ImageViewDescriptors view;
	view.info     = view_info;
	view.resource = ResourceIndex(view_info.format);
	backing.views.push_back(view);
	return {static_cast<uint32_t>(backing.views.size() - 1)};
}

ID3D12Resource* Image::Resource(ImageViewHandle view) const {
	return backing.resources[backing.views[view.index].resource].resource;
}

D3D12_CPU_DESCRIPTOR_HANDLE Image::ShaderResourceView(ImageViewHandle handle) {
	auto& view = backing.views[handle.index];
	if (view.srv.ptr != 0) {
		return view.srv;
	}
	const auto& info       = view.info;
	const auto  image      = D3D12::GetFormatInfo(backing.format);
	const auto  format     = D3D12::GetFormatInfo(info.format);
	const bool  stencil    = info.aspect == vk::ImageAspectFlagBits::eStencil;
	const bool  depth      = info.aspect == vk::ImageAspectFlagBits::eDepth;
	const bool  multisample = backing.samples > 1;

	D3D12_SHADER_RESOURCE_VIEW_DESC desc {};
	desc.Format = stencil ? image.stencil_view : depth ? image.view : format.view;
	desc.Shader4ComponentMapping =
	    D3D12::ComposeMapping(info.mapping, stencil || depth ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
	                                                         : format.mapping);
	const float min_lod = static_cast<float>(info.base_level) + static_cast<float>(info.min_lod) / 256.0f;
	const UINT  plane   = stencil ? 1 : 0;
	switch (info.type) {
		case vk::ImageViewType::e1D:
			desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE1D;
			desc.Texture1D.MostDetailedMip     = info.base_level;
			desc.Texture1D.MipLevels           = info.level_count;
			desc.Texture1D.ResourceMinLODClamp = min_lod;
			break;
		case vk::ImageViewType::e1DArray:
			desc.ViewDimension                      = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
			desc.Texture1DArray.MostDetailedMip     = info.base_level;
			desc.Texture1DArray.MipLevels           = info.level_count;
			desc.Texture1DArray.FirstArraySlice     = info.base_layer;
			desc.Texture1DArray.ArraySize           = info.layer_count;
			desc.Texture1DArray.ResourceMinLODClamp = min_lod;
			break;
		case vk::ImageViewType::e2D:
			// The view dimension must match the shader's Texture2D, which always reads the
			// resource's first array slice.
			EXIT_NOT_IMPLEMENTED(info.base_layer != 0);
			if (multisample) {
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			} else {
				desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MostDetailedMip     = info.base_level;
				desc.Texture2D.MipLevels           = info.level_count;
				desc.Texture2D.PlaneSlice          = plane;
				desc.Texture2D.ResourceMinLODClamp = min_lod;
			}
			break;
		case vk::ImageViewType::e2DArray:
			if (multisample) {
				desc.ViewDimension                    = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
				desc.Texture2DMSArray.FirstArraySlice = info.base_layer;
				desc.Texture2DMSArray.ArraySize       = info.layer_count;
			} else {
				desc.ViewDimension                      = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MostDetailedMip     = info.base_level;
				desc.Texture2DArray.MipLevels           = info.level_count;
				desc.Texture2DArray.FirstArraySlice     = info.base_layer;
				desc.Texture2DArray.ArraySize           = info.layer_count;
				desc.Texture2DArray.PlaneSlice          = plane;
				desc.Texture2DArray.ResourceMinLODClamp = min_lod;
			}
			break;
		case vk::ImageViewType::e3D:
			desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE3D;
			desc.Texture3D.MostDetailedMip     = info.base_level;
			desc.Texture3D.MipLevels           = info.level_count;
			desc.Texture3D.ResourceMinLODClamp = min_lod;
			break;
		case vk::ImageViewType::eCube:
			desc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
			desc.TextureCube.MostDetailedMip     = info.base_level;
			desc.TextureCube.MipLevels           = info.level_count;
			desc.TextureCube.ResourceMinLODClamp = min_lod;
			EXIT_NOT_IMPLEMENTED(info.base_layer != 0);
			break;
		case vk::ImageViewType::eCubeArray:
			desc.ViewDimension                        = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
			desc.TextureCubeArray.MostDetailedMip     = info.base_level;
			desc.TextureCubeArray.MipLevels           = info.level_count;
			desc.TextureCubeArray.First2DArrayFace    = info.base_layer;
			desc.TextureCubeArray.NumCubes            = info.layer_count / 6;
			desc.TextureCubeArray.ResourceMinLODClamp = min_lod;
			break;
		default: EXIT("D3D12: invalid image view type %d\n", static_cast<int>(info.type));
	}
	view.srv = m_scheduler.Context().GetDescriptorHeap().AllocateCpu(
	    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_graphics.device->CreateShaderResourceView(backing.resources[view.resource].resource, &desc,
	                                            view.srv);
	return view.srv;
}

D3D12_CPU_DESCRIPTOR_HANDLE Image::UnorderedAccessView(ImageViewHandle handle) {
	auto& view = backing.views[handle.index];
	if (view.uav.ptr != 0) {
		return view.uav;
	}
	const auto& info   = view.info;
	const auto  format = D3D12::GetFormatInfo(info.format);
	if (!format.Writable() ||
	    (backing.resources[view.resource].resource->GetDesc().Flags &
	     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
		EXIT("D3D12: image format %d cannot be written by shaders\n",
		     static_cast<int>(info.format));
	}
	// The view dimension must match the shader's declared image type.
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc {};
	desc.Format = format.view;
	if (backing.image_type == vk::ImageType::e3D) {
		desc.ViewDimension         = D3D12_UAV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MipSlice    = info.base_level;
		desc.Texture3D.FirstWSlice = info.type == vk::ImageViewType::e3D ? 0 : info.base_layer;
		desc.Texture3D.WSize = info.type == vk::ImageViewType::e3D ? UINT(-1) : info.layer_count;
	} else {
		switch (info.type) {
			case vk::ImageViewType::e1D:
				EXIT_NOT_IMPLEMENTED(info.base_layer != 0);
				desc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE1D;
				desc.Texture1D.MipSlice = info.base_level;
				break;
			case vk::ImageViewType::e1DArray:
				desc.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
				desc.Texture1DArray.MipSlice        = info.base_level;
				desc.Texture1DArray.FirstArraySlice = info.base_layer;
				desc.Texture1DArray.ArraySize       = info.layer_count;
				break;
			case vk::ImageViewType::e2D:
				EXIT_NOT_IMPLEMENTED(info.base_layer != 0);
				desc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipSlice = info.base_level;
				break;
			default:
				desc.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MipSlice        = info.base_level;
				desc.Texture2DArray.FirstArraySlice = info.base_layer;
				desc.Texture2DArray.ArraySize       = info.layer_count;
				break;
		}
	}
	view.uav = m_scheduler.Context().GetDescriptorHeap().AllocateCpu(
	    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_graphics.device->CreateUnorderedAccessView(backing.resources[view.resource].resource, nullptr,
	                                             &desc, view.uav);
	return view.uav;
}

D3D12_CPU_DESCRIPTOR_HANDLE Image::RenderTargetView(ImageViewHandle handle) {
	auto& view = backing.views[handle.index];
	if (view.rtv.ptr != 0) {
		return view.rtv;
	}
	const auto& info   = view.info;
	const auto  format = D3D12::GetFormatInfo(info.format);
	if (!format.Writable() || (backing.resources[view.resource].resource->GetDesc().Flags &
	                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
		EXIT("D3D12: image format %d cannot be rendered to\n", static_cast<int>(info.format));
	}
	D3D12_RENDER_TARGET_VIEW_DESC desc {};
	desc.Format = format.view;
	if (backing.image_type == vk::ImageType::e3D) {
		desc.ViewDimension         = D3D12_RTV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MipSlice    = info.base_level;
		desc.Texture3D.FirstWSlice = info.type == vk::ImageViewType::e3D ? 0 : info.base_layer;
		desc.Texture3D.WSize       = info.type == vk::ImageViewType::e3D ? UINT(-1) : info.layer_count;
	} else if (backing.samples > 1) {
		desc.ViewDimension                    = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
		desc.Texture2DMSArray.FirstArraySlice = info.base_layer;
		desc.Texture2DMSArray.ArraySize       = info.layer_count;
	} else {
		desc.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice        = info.base_level;
		desc.Texture2DArray.FirstArraySlice = info.base_layer;
		desc.Texture2DArray.ArraySize       = info.layer_count;
	}
	view.rtv = m_scheduler.Context().GetDescriptorHeap().AllocateCpu(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_graphics.device->CreateRenderTargetView(backing.resources[view.resource].resource, &desc,
	                                          view.rtv);
	return view.rtv;
}

D3D12_CPU_DESCRIPTOR_HANDLE Image::DepthStencilView(ImageViewHandle handle, bool read_only) {
	auto& view   = backing.views[handle.index];
	auto& target = read_only ? view.dsv_read_only : view.dsv;
	if (target.ptr != 0) {
		return target;
	}
	const auto& info   = view.info;
	const auto  format = D3D12::GetFormatInfo(backing.format);
	EXIT_IF(!format.IsDepth());
	D3D12_DEPTH_STENCIL_VIEW_DESC desc {};
	desc.Format = format.depth_view;
	if (read_only) {
		desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
		if (format.stencil_view != DXGI_FORMAT_UNKNOWN) {
			desc.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
		}
	}
	if (backing.samples > 1) {
		desc.ViewDimension                    = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
		desc.Texture2DMSArray.FirstArraySlice = info.base_layer;
		desc.Texture2DMSArray.ArraySize       = info.layer_count;
	} else {
		desc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		desc.Texture2DArray.MipSlice        = info.base_level;
		desc.Texture2DArray.FirstArraySlice = info.base_layer;
		desc.Texture2DArray.ArraySize       = info.layer_count;
	}
	target = m_scheduler.Context().GetDescriptorHeap().AllocateCpu(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_graphics.device->CreateDepthStencilView(backing.resources[view.resource].resource, &desc,
	                                          target);
	return target;
}

void Image::CopyRegions(CommandBuffer& command, std::span<const vk::BufferImageCopy> copies,
                        BufferHandle buffer, bool upload) {
	EXIT_IF(copies.empty() || buffer == nullptr);
	auto* device   = m_graphics.device;
	auto* resource = backing.resources[backing.current].resource;

	// Buffer-to-texture copies need 256-byte row pitches and 512-byte footprint offsets. Regions
	// in other layouts go through a scratch buffer, repacked by the row copy kernel.
	struct Job {
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
		UINT                               subresource = 0;
		D3D12_BOX                          box {};
		RowCopy                            rows;
		bool                               direct = false;
	};
	std::vector<Job> jobs;
	uint64_t         scratch_size = 0;
	for (const auto& copy: copies) {
		const auto& sub    = copy.imageSubresource;
		const auto  block  = GetCopyBlock(backing.format, sub.aspectMask);
		EXIT_IF(block.bytes == 0);
		const auto row_texels = copy.bufferRowLength != 0 ? copy.bufferRowLength : copy.imageExtent.width;
		const auto image_rows =
		    copy.bufferImageHeight != 0 ? copy.bufferImageHeight : copy.imageExtent.height;
		const uint64_t src_row_pitch   = DivCeil(row_texels, block.width) * block.bytes;
		const uint64_t src_slice_pitch = DivCeil(image_rows, block.height) * src_row_pitch;
		const auto     width_blocks    = DivCeil(copy.imageExtent.width, block.width);
		const auto     rows            = static_cast<uint32_t>(DivCeil(copy.imageExtent.height, block.height));
		const auto     slices          = copy.imageExtent.depth;
		const auto     row_bytes       = static_cast<uint32_t>(width_blocks * block.bytes);
		const bool     volume          = backing.image_type == vk::ImageType::e3D;
		const auto     layers          = volume ? 1u : sub.layerCount;
		for (uint32_t layer = 0; layer < layers; layer++) {
			Job job;
			job.subresource =
			    backing.Subresource(sub.mipLevel, volume ? 0 : sub.baseArrayLayer + layer,
			                        Plane(sub.aspectMask));
			job.footprint.Footprint.Format = CopyFormat(device, resource, job.subresource);
			job.footprint.Footprint.Width  = static_cast<UINT>(width_blocks * block.width);
			job.footprint.Footprint.Height = rows * block.height;
			job.footprint.Footprint.Depth  = slices;
			job.box = {static_cast<UINT>(copy.imageOffset.x), static_cast<UINT>(copy.imageOffset.y),
			           static_cast<UINT>(copy.imageOffset.z),
			           static_cast<UINT>(copy.imageOffset.x) + job.footprint.Footprint.Width,
			           static_cast<UINT>(copy.imageOffset.y) + job.footprint.Footprint.Height,
			           static_cast<UINT>(copy.imageOffset.z) + slices};
			const uint64_t buffer_offset =
			    copy.bufferOffset + src_slice_pitch * slices * layer;
			job.direct = buffer_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0 &&
			             src_row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0 &&
			             (slices == 1 || src_slice_pitch == src_row_pitch * rows);
			if (job.direct) {
				job.footprint.Offset             = buffer_offset;
				job.footprint.Footprint.RowPitch = static_cast<UINT>(src_row_pitch);
			} else {
				const auto pitch  = Common::AlignUp<uint64_t>(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
				scratch_size      = Common::AlignUp<uint64_t>(scratch_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
				job.footprint.Offset             = scratch_size;
				job.footprint.Footprint.RowPitch = static_cast<UINT>(pitch);
				scratch_size += pitch * rows * slices;
				job.rows = {buffer_offset, src_row_pitch, src_slice_pitch, job.footprint.Offset,
				            pitch,         pitch * rows,  row_bytes,       rows,
				            slices};
				if (!upload) {
					std::swap(job.rows.src_offset, job.rows.dst_offset);
					std::swap(job.rows.src_row_pitch, job.rows.dst_row_pitch);
					std::swap(job.rows.src_slice_pitch, job.rows.dst_slice_pitch);
				}
			}
			jobs.push_back(job);
		}
	}

	Buffer* scratch = scratch_size != 0 ? &ScratchBuffer(m_graphics, m_scheduler, scratch_size)
	                                    : nullptr;
	const auto copy_texture = [&](const Job& job, const Buffer& linear) {
		D3D12_TEXTURE_COPY_LOCATION texture {};
		texture.pResource        = resource;
		texture.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		texture.SubresourceIndex = job.subresource;
		D3D12_TEXTURE_COPY_LOCATION footprint {};
		footprint.pResource       = linear.Resource();
		footprint.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		footprint.PlacedFootprint = job.footprint;
		if (upload) {
			command.Handle()->CopyTextureRegion(&texture, job.box.left, job.box.top, job.box.front,
			                                    &footprint, nullptr);
		} else {
			command.Handle()->CopyTextureRegion(&footprint, 0, 0, 0, &texture, &job.box);
		}
	};

	if (upload) {
		for (const auto& job: jobs) {
			if (!job.direct) {
				RecordRowCopy(command, *buffer, *scratch, job.rows);
			}
		}
		buffer->Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
		if (scratch != nullptr) {
			scratch->Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
		}
		for (const auto& job: jobs) {
			copy_texture(job, job.direct ? *buffer : *scratch);
		}
		return;
	}

	buffer->Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	if (scratch != nullptr) {
		scratch->Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	}
	for (const auto& job: jobs) {
		copy_texture(job, job.direct ? *buffer : *scratch);
	}
	for (const auto& job: jobs) {
		if (!job.direct) {
			RecordRowCopy(command, *scratch, *buffer, job.rows);
		}
	}
}

void Image::Upload(std::span<const vk::BufferImageCopy> copies, BufferHandle buffer,
                   uint64_t /*offset*/, uint64_t size) {
	EXIT_IF(copies.empty() || buffer == nullptr || size == 0);
	auto& command = m_scheduler.Current();
	Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	CopyRegions(command, copies, buffer, true);
}

void Image::Download(std::span<const vk::BufferImageCopy> copies, BufferHandle buffer,
                     uint64_t /*offset*/, uint64_t size) {
	EXIT_IF(copies.empty() || buffer == nullptr || size == 0);
	auto& command = m_scheduler.Current();
	Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	CopyRegions(command, copies, buffer, false);
}

void Image::CopyContents(CommandBuffer& command, Image& source, uint32_t source_resource,
                         uint32_t destination_resource, std::span<const vk::ImageCopy> copies) {
	auto& from = source.backing.resources[source_resource];
	auto& to   = backing.resources[destination_resource];
	source.Transition(command, source_resource, D3D12_RESOURCE_STATE_COPY_SOURCE, {});
	Transition(command, destination_resource, D3D12_RESOURCE_STATE_COPY_DEST, {});

	const bool source_volume      = source.backing.image_type == vk::ImageType::e3D;
	const bool destination_volume = backing.image_type == vk::ImageType::e3D;
	const bool direct             = from.family == to.family;

	struct Region {
		UINT      source_subresource      = 0;
		UINT      destination_subresource = 0;
		D3D12_BOX box {};
		UINT      x = 0, y = 0, z = 0;
		CopyBlock block;
		uint64_t  offset = 0;
		uint32_t  pitch  = 0;
	};
	std::vector<Region> regions;
	uint64_t            scratch_size = 0;
	for (const auto& copy: copies) {
		const auto count = source_volume && destination_volume
		                       ? 1u
		                       : std::max(copy.srcSubresource.layerCount, copy.dstSubresource.layerCount);
		const auto depth = source_volume && destination_volume ? copy.extent.depth : 1u;
		const auto block = GetCopyBlock(source.backing.format, copy.srcSubresource.aspectMask);
		const auto destination_block = GetCopyBlock(backing.format, copy.dstSubresource.aspectMask);
		EXIT_NOT_IMPLEMENTED(block.bytes != destination_block.bytes ||
		                     block.width != destination_block.width ||
		                     block.height != destination_block.height);
		for (uint32_t i = 0; i < count; i++) {
			Region region;
			region.block              = block;
			region.source_subresource = source.backing.Subresource(
			    copy.srcSubresource.mipLevel,
			    source_volume ? 0 : copy.srcSubresource.baseArrayLayer + i,
			    Plane(copy.srcSubresource.aspectMask));
			region.destination_subresource =
			    backing.Subresource(copy.dstSubresource.mipLevel,
			                        destination_volume ? 0 : copy.dstSubresource.baseArrayLayer + i,
			                        Plane(copy.dstSubresource.aspectMask));
			const auto source_z =
			    static_cast<UINT>(copy.srcOffset.z) + (source_volume && !destination_volume ? i : 0);
			const auto width  = static_cast<UINT>(DivCeil(copy.extent.width, block.width) * block.width);
			const auto height = static_cast<UINT>(DivCeil(copy.extent.height, block.height) * block.height);
			region.box = {static_cast<UINT>(copy.srcOffset.x), static_cast<UINT>(copy.srcOffset.y),
			              source_z, static_cast<UINT>(copy.srcOffset.x) + width,
			              static_cast<UINT>(copy.srcOffset.y) + height, source_z + depth};
			region.x = static_cast<UINT>(copy.dstOffset.x);
			region.y = static_cast<UINT>(copy.dstOffset.y);
			region.z = static_cast<UINT>(copy.dstOffset.z) + (destination_volume && !source_volume ? i : 0);
			if (!direct) {
				region.pitch  = static_cast<uint32_t>(Common::AlignUp<uint64_t>(
                    DivCeil(width, block.width) * block.bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
				scratch_size  = Common::AlignUp<uint64_t>(scratch_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
				region.offset = scratch_size;
				scratch_size += static_cast<uint64_t>(region.pitch) * (height / block.height) * depth;
			}
			regions.push_back(region);
		}
	}

	auto* list = command.Handle();
	if (direct) {
		for (const auto& region: regions) {
			D3D12_TEXTURE_COPY_LOCATION src {};
			src.pResource        = from.resource;
			src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = region.source_subresource;
			D3D12_TEXTURE_COPY_LOCATION dst {};
			dst.pResource        = to.resource;
			dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = region.destination_subresource;
			list->CopyTextureRegion(&dst, region.x, region.y, region.z, &src, &region.box);
		}
		return;
	}

	// Different format families cannot be copied directly; the bytes go through a buffer.
	auto& scratch = ScratchBuffer(m_graphics, m_scheduler, scratch_size);
	scratch.Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	const auto footprint = [&](const Region& region, ID3D12Resource* resource, UINT subresource) {
		D3D12_TEXTURE_COPY_LOCATION location {};
		location.pResource                          = scratch.Resource();
		location.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		location.PlacedFootprint.Offset             = region.offset;
		location.PlacedFootprint.Footprint.Format   = CopyFormat(m_graphics.device, resource, subresource);
		location.PlacedFootprint.Footprint.Width    = region.box.right - region.box.left;
		location.PlacedFootprint.Footprint.Height   = region.box.bottom - region.box.top;
		location.PlacedFootprint.Footprint.Depth    = region.box.back - region.box.front;
		location.PlacedFootprint.Footprint.RowPitch = region.pitch;
		return location;
	};
	for (const auto& region: regions) {
		D3D12_TEXTURE_COPY_LOCATION src {};
		src.pResource        = from.resource;
		src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = region.source_subresource;
		const auto linear    = footprint(region, from.resource, region.source_subresource);
		list->CopyTextureRegion(&linear, 0, 0, 0, &src, &region.box);
	}
	scratch.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	for (const auto& region: regions) {
		D3D12_TEXTURE_COPY_LOCATION dst {};
		dst.pResource        = to.resource;
		dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = region.destination_subresource;
		const auto linear    = footprint(region, to.resource, region.destination_subresource);
		list->CopyTextureRegion(&dst, region.x, region.y, region.z, &linear, nullptr);
	}
}

void Image::CopyImage(Image& source) {
	EXIT_IF(source.backing.samples != backing.samples);
	const uint32_t levels     = std::min(source.backing.mip_levels, backing.mip_levels);
	const uint32_t base_depth = backing.image_type == vk::ImageType::e3D
	                                ? backing.extent.depth
	                                : source.backing.extent.depth;
	const auto source_aspect =
	    FullAspectMask(source.backing.format) & ~vk::ImageAspectFlagBits::eStencil;
	const auto destination_aspect =
	    FullAspectMask(backing.format) & ~vk::ImageAspectFlagBits::eStencil;
	std::vector<vk::ImageCopy> copies;
	for (uint32_t level = 0; level < levels; level++) {
		const auto width  = std::max(source.backing.extent.width >> level, 1u);
		const auto height = std::max(source.backing.extent.height >> level, 1u);
		const auto depth  = std::max(base_depth >> level, 1u);
		const auto [source_layers, destination_layers] = SanitizeCopyLayers(source, *this, depth);
		vk::ImageCopy copy {};
		copy.srcSubresource = {source_aspect, level, 0, source_layers};
		copy.dstSubresource = {destination_aspect, level, 0, destination_layers};
		copy.extent         = {width, height,
		                       source.backing.image_type == vk::ImageType::e3D &&
		                               backing.image_type == vk::ImageType::e3D
		                                   ? depth
		                                   : 1u};
		copies.push_back(copy);
	}
	if (copies.empty()) {
		return;
	}
	auto& command = m_scheduler.Current();
	source.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	CopyContents(command, source, source.backing.current, backing.current, copies);
}

void Image::Resolve(Image& source, const ImageSubresourceRange& source_range,
                    const ImageSubresourceRange& destination_range) {
	EXIT_IF(backing.samples != 1 || source.backing.image_type != vk::ImageType::e2D ||
	        backing.image_type != vk::ImageType::e2D || source_range.level_count != 1 ||
	        destination_range.level_count != 1);
	const auto layers = std::min({source_range.layer_count, destination_range.layer_count,
	                              source.backing.layers - source_range.base_layer,
	                              backing.layers - destination_range.base_layer});
	auto&      command = m_scheduler.Current();
	if (source.backing.samples == 1) {
		vk::ImageCopy copy {};
		copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, source_range.base_level,
		                       source_range.base_layer, layers};
		copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, destination_range.base_level,
		                       destination_range.base_layer, layers};
		copy.extent         = {info.extent.width, info.extent.height, 1};
		source.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
		CopyContents(command, source, source.backing.current, backing.current, {&copy, 1});
		return;
	}
	source.Use(command, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
	Use(command, D3D12_RESOURCE_STATE_RESOLVE_DEST);
	const auto format = D3D12::GetFormatInfo(backing.format).view;
	for (uint32_t layer = 0; layer < layers; layer++) {
		command.Handle()->ResolveSubresource(
		    backing.resources[backing.current].resource,
		    backing.Subresource(destination_range.base_level, destination_range.base_layer + layer, 0),
		    source.backing.resources[source.backing.current].resource,
		    source.backing.Subresource(source_range.base_level, source_range.base_layer + layer, 0),
		    format);
	}
}

void Image::CopyImageWithBuffer(Image& source, Buffer& /*buffer*/) {
	// Copies between format families already go through a buffer (CopyContents).
	EXIT_IF(source.backing.samples != 1 || backing.samples != 1);
	CopyImage(source);
}

void Image::CopyMip(Image& source, uint32_t mip, uint32_t layer) {
	EXIT_IF(source.backing.samples != backing.samples || mip >= backing.mip_levels ||
	        layer >= backing.layers);
	const auto width  = std::max(backing.extent.width >> mip, 1u);
	const auto height = std::max(backing.extent.height >> mip, 1u);
	const auto depth  = std::max(backing.extent.depth >> mip, 1u);
	EXIT_IF(width != source.backing.extent.width || height != source.backing.extent.height);
	const auto [source_layers, destination_layers] = SanitizeCopyLayers(source, *this, depth);
	const auto aspects = FullAspectMask(source.backing.format);
	EXIT_IF(aspects != FullAspectMask(backing.format));
	std::vector<vk::ImageCopy> copies;
	for (const auto aspect: {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
	                         vk::ImageAspectFlagBits::eStencil}) {
		if (!(aspects & aspect)) {
			continue;
		}
		vk::ImageCopy copy {};
		copy.srcSubresource = {aspect, 0, 0, source_layers};
		copy.dstSubresource = {aspect, mip, layer, destination_layers};
		copy.extent         = {width, height,
		                       source.backing.image_type == vk::ImageType::e3D &&
		                               backing.image_type == vk::ImageType::e3D
		                                   ? depth
		                                   : 1u};
		copies.push_back(copy);
	}
	auto& command = m_scheduler.Current();
	source.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	CopyContents(command, source, source.backing.current, backing.current, copies);
}

void Image::CopySubresources(Image& source, const ImageSubresourceRange& range,
                             std::span<const vk::ImageCopy> regions) {
	auto& command = m_scheduler.Current();
	source.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE, range);
	Use(command, D3D12_RESOURCE_STATE_COPY_DEST, range);
	CopyContents(command, source, source.backing.current, backing.current, regions);
}

void Image::Clear(CommandBuffer& command, vk::Format format, const vk::ImageSubresourceRange& range,
                  const vk::ClearValue& clear, bool /*full_image*/) {
	auto* list = command.Handle();
	for (uint32_t level = range.baseMipLevel; level < range.baseMipLevel + range.levelCount;
	     level++) {
		const ImageSubresourceRange subresources {level, 1, range.baseArrayLayer, range.layerCount};
		ImageViewInfo               view {};
		view.format      = range.aspectMask & vk::ImageAspectFlagBits::eColor ? format : backing.format;
		view.type        = vk::ImageViewType::e2DArray;
		view.aspect      = range.aspectMask & vk::ImageAspectFlagBits::eColor
		                       ? vk::ImageAspectFlags {vk::ImageAspectFlagBits::eColor}
		                       : vk::ImageAspectFlags {vk::ImageAspectFlagBits::eDepth};
		view.base_level  = level;
		view.base_layer  = range.baseArrayLayer;
		view.layer_count = range.layerCount;

		if (!(range.aspectMask & vk::ImageAspectFlagBits::eColor)) {
			const auto handle = FindView(view);
			Use(command, D3D12_RESOURCE_STATE_DEPTH_WRITE, subresources, handle);
			D3D12_CLEAR_FLAGS flags {};
			if (range.aspectMask & vk::ImageAspectFlagBits::eDepth) {
				flags |= D3D12_CLEAR_FLAG_DEPTH;
			}
			if (range.aspectMask & vk::ImageAspectFlagBits::eStencil) {
				flags |= D3D12_CLEAR_FLAG_STENCIL;
			}
			list->ClearDepthStencilView(DepthStencilView(handle), flags, clear.depthStencil.depth,
			                            static_cast<UINT8>(clear.depthStencil.stencil), 0, nullptr);
			continue;
		}

		const auto handle   = FindView(view);
		const auto resource = backing.views[handle.index].resource;
		const auto flags    = backing.resources[resource].resource->GetDesc().Flags;
		const auto numeric  = vk::componentNumericFormat(format, 0);
		const bool integer  = std::string_view(numeric) == "UINT" || std::string_view(numeric) == "SINT";
		float      color[4] {};
		for (int i = 0; i < 4; i++) {
			color[i] = !integer ? clear.color.float32[i]
			           : std::string_view(numeric) == "UINT"
			               ? static_cast<float>(clear.color.uint32[i])
			               : static_cast<float>(clear.color.int32[i]);
		}
		if ((flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
			Use(command, D3D12_RESOURCE_STATE_RENDER_TARGET, subresources, handle);
			list->ClearRenderTargetView(RenderTargetView(handle), color, 0, nullptr);
		} else if ((flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
			Use(command, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, subresources, handle);
			auto&      heap    = m_scheduler.Context().GetDescriptorHeap();
			const auto cpu     = UnorderedAccessView(handle);
			const auto visible = heap.AllocateViews(command, 1);
			m_graphics.device->CopyDescriptorsSimple(1, visible.cpu, cpu,
			                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto* target = backing.resources[resource].resource;
			if (integer) {
				list->ClearUnorderedAccessViewUint(visible.gpu, cpu, target, clear.color.uint32.data(),
				                                   0, nullptr);
			} else {
				list->ClearUnorderedAccessViewFloat(visible.gpu, cpu, target, color, 0, nullptr);
			}
		} else {
			EXIT("D3D12: image format %d cannot be cleared\n", static_cast<int>(format));
		}
	}
}

} // namespace Libs::Graphics
