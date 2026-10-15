// D3D12 implementation of the Image backend operations (see renderer/image/image.h).

#include "graphics/host_gpu/renderer/image/image.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/gpu_tiler_copy_rows_spv.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/d3d12/tiler.h"
#include "graphics/host_gpu/gpuBackend.h"
#include "graphics/host_gpu/renderer/image/imageView.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
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

// Views that D3D12 has no shader resource view for, which shaders read from a copy (ImageViewDescriptors::copy): 2D and 2D array
// views of a 3D image (upstream's e2DArrayCompatible views), and 2D and cube views that start at a later layer of an array (a
// Texture2D or TextureCube view always starts at the first).
[[nodiscard]] bool CopiedView(const ImageBacking& backing, const ImageViewInfo& view) {
	if (backing.image_type == vk::ImageType::e3D) {
		return view.type != vk::ImageViewType::e3D;
	}
	return (view.type == vk::ImageViewType::e2D || view.type == vk::ImageViewType::eCube) && view.base_layer != 0;
}

// Whether a resource is on the primary resource's memory: the primary one and those that share it (ImageResource::shared).
[[nodiscard]] bool OnPrimaryMemory(const ImageBacking& backing, uint32_t resource) {
	return resource == 0 || backing.resources[resource].shared;
}

// A 96-bit format: its textures are 128-bit resources (D3D12::GetFormatInfo), and copies between them and buffers widen or narrow
// each texel (RowCopy::texel_mode). The value the fourth channel gets: 1, as the PS5 reads it.
[[nodiscard]] bool Expanded96(vk::Format format) {
	return format == vk::Format::eR32G32B32Uint || format == vk::Format::eR32G32B32Sint || format == vk::Format::eR32G32B32Sfloat;
}
[[nodiscard]] uint32_t Expanded96One(vk::Format format) {
	return format == vk::Format::eR32G32B32Sfloat ? 0x3f800000u : 1u;
}

// A color in the shared-exponent format E5B9G9R9 (the conversion the Vulkan and D3D specifications give).
[[nodiscard]] uint32_t PackE5B9G9R9(const float* rgb) {
	constexpr int   Mantissa = 9;
	constexpr int   Bias     = 15;
	constexpr float Largest  = 65408.0f; // (2^9 - 1) / 2^9 * 2^(31 - 15)
	float           c[3];
	for (int i = 0; i < 3; i++) {
		c[i] = std::isnan(rgb[i]) ? 0.0f : std::clamp(rgb[i], 0.0f, Largest);
	}
	const float largest  = std::max({c[0], c[1], c[2]});
	int         exponent = largest > 0.0f ? std::max(-Bias - 1, static_cast<int>(std::floor(std::log2(largest)))) + 1 + Bias : 0;
	if (std::floor(largest / std::exp2(static_cast<float>(exponent - Bias - Mantissa)) + 0.5f) == static_cast<float>(1 << Mantissa)) {
		exponent++;
	}
	uint32_t packed = static_cast<uint32_t>(exponent) << 27u;
	for (int i = 0; i < 3; i++) {
		const auto value = static_cast<uint32_t>(std::floor(c[i] / std::exp2(static_cast<float>(exponent - Bias - Mantissa)) + 0.5f));
		packed |= std::min(value, 511u) << (9u * static_cast<uint32_t>(i));
	}
	return packed;
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
	uint32_t row_bytes       = 0; // of a destination row
	uint32_t rows            = 0;
	uint32_t slices          = 0;
	uint32_t texel_mode      = 0; // the kernel's TEXEL_MODE: 1 widens 12-byte texels to 16, 2 narrows them back
	uint32_t alpha           = 0; // the fourth dword a widened texel gets
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
	                           copy.slices,
	                           copy.alpha};
	auto&                               kernels    = command.GetContext().GetComputeKernels();
	const D3D12::SpecializationConstant constant[] = {{0, copy.texel_mode}};
	auto*                               pipeline   = kernels.Get(GPU_TILER_COPY_ROWS_SPV, constant);
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
		// Left without a host resource, like an image without a format: draws that use it are skipped.
		static std::atomic_bool reported = false;
		if (!reported.exchange(true)) {
			Log::WriteToConsoleAndLog(fmt::format("D3D12: image format {} is not supported; images of it are not created\n", static_cast<int>(info.pixel_format)));
		}
		return;
	}
	backing.format         = info.pixel_format;
	backing.image_type     = HostImageType(info.type);
	backing.mip_skip       = info.host_skip;
	backing.extent         = {std::max(info.extent.width >> info.host_skip, 1u), std::max(info.extent.height >> info.host_skip, 1u), info.extent.depth};
	backing.layers         = info.IsVolume() ? 1u : info.resources.layers;
	backing.mip_levels     = info.resources.levels - info.host_skip;
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

uint64_t Image::GpuBytes() const {
	uint64_t bytes = 0;
	for (const auto& resource: backing.resources) {
		bytes += resource.allocation != nullptr ? resource.allocation->GetSize() : 0; // (a shared one has none of its own)
	}
	for (const auto& view: backing.views) {
		bytes += view.copy.allocation != nullptr ? view.copy.allocation->GetSize() : 0;
	}
	return bytes;
}

Image::~Image() {
	KYTY_PROFILER_FUNCTION();
	if (!backing.views.empty()) {
		auto& heap = m_scheduler.Context().GetDescriptorHeap();
		for (const auto& view: backing.views) {
			const std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE> descriptors[] {
			    {view.srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
			    {view.copy_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
			    {view.uav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
			    {view.rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV},
			    {view.dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV},
			    {view.dsv_read_only, D3D12_DESCRIPTOR_HEAP_TYPE_DSV}};
			for (const auto& [handle, type]: descriptors) {
				if (handle.ptr != 0) {
					heap.FreeCpu(type, handle);
				}
			}
			if (view.copy.allocation != nullptr) {
				view.copy.allocation->Release();
			}
		}
	}
	// (The shared resources first: they are on the primary one's memory.)
	for (auto resource = backing.resources.rbegin(); resource != backing.resources.rend(); ++resource) {
		if (resource->shared) {
			resource->resource->Release();
		} else {
			resource->allocation->Release();
		}
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
	// A BC array on a device whose copies place its small layers wrongly lives in CPU-visible memory; Upload writes it with the CPU.
	resource.cpu_written = m_graphics.cpu_written_block_arrays && backing.image_type == vk::ImageType::e2D && backing.layers > 1 &&
	                       backing.samples == 1 && desc.Flags == D3D12_RESOURCE_FLAG_NONE && vk::blockSize(view_format) != 0 &&
	                       vk::blockExtent(view_format)[0] > 1;
	if (resource.cpu_written) {
		allocation.CustomPool = m_graphics.cpu_texture_pool;
	}
	// Config::SharedFormatAliases: the resource of another family is placed on the primary one's memory, with its description but for the
	// format, where the texel size is the same, the flags this family needs are among the primary one's, and D3D12 takes it (size and
	// alignment); otherwise it has memory of its own and gets the contents copied over. D3D12 leaves what one format's resource sees of
	// another's bytes undefined: the same description but for the format gives the same layout where that works (the Xbox).
	const bool plain_texels = vk::blockSize(view_format) != 0 && vk::blockExtent(view_format)[0] == 1 &&
	                          vk::blockExtent(view_format)[1] == 1;
	if (backing.resources.empty()) {
		if (Config::SharedFormatAliases() && !resource.cpu_written && plain_texels && backing.planes == 1) {
			allocation.Flags = D3D12MA::ALLOCATION_FLAG_CAN_ALIAS; // (never a committed resource: aliases need its heap)
		}
	} else if (auto& primary = backing.resources[0];
	           Config::SharedFormatAliases() && !resource.cpu_written && !primary.cpu_written && plain_texels && backing.planes == 1 &&
	           vk::blockSize(view_format) == vk::blockSize(backing.format) && vk::blockExtent(backing.format)[0] == 1 &&
	           (desc.Flags & ~backing.resource_flags) == 0 && primary.allocation != nullptr && primary.allocation->GetHeap() != nullptr) {
		auto shared  = desc;
		shared.Flags = backing.resource_flags;
		ID3D12Resource* aliased = nullptr;
		if (SUCCEEDED(m_graphics.allocator->CreateAliasingResource(primary.allocation, 0, &shared, D3D12_RESOURCE_STATE_COMMON, nullptr,
		                                                           IID_PPV_ARGS(&aliased)))) {
			resource.resource = aliased;
			resource.shared   = true;
			resource.states.assign(backing.SubresourceCount(), D3D12_RESOURCE_STATE_COMMON);
			backing.resources.push_back(std::move(resource));
			return static_cast<uint32_t>(backing.resources.size() - 1);
		}
	}
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
	if (OnPrimaryMemory(backing, resource) && backing.active != resource) {
		// Another resource on the same memory was used last: the GPU finishes with it and flushes its caches first. The primary
		// resource's first use (its discard) initializes the memory, so it comes before any other one's, which it would throw away.
		auto& primary = backing.resources[0];
		if (primary.needs_discard) {
			command.Handle()->DiscardResource(primary.resource, nullptr);
			primary.needs_discard = false;
		}
		D3D12_RESOURCE_BARRIER barrier {};
		barrier.Type                     = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
		barrier.Aliasing.pResourceBefore = backing.resources[backing.active].resource;
		barrier.Aliasing.pResourceAfter  = target.resource;
		command.Handle()->ResourceBarrier(1, &barrier);
		backing.active = resource;
	}
	if (target.needs_discard) {
		// Still in its creation state; the first use initializes the placed resource.
		command.Handle()->DiscardResource(target.resource, nullptr);
		target.needs_discard = false;
	}
	if (range && backing.mip_skip != 0) {
		// the range is in the guest's levels (low quality: the host image has fewer)
		const uint32_t skip = backing.mip_skip;
		const uint32_t end  = range->base_level + range->level_count;
		if (end <= skip) {
			return;
		}
		range->base_level  = range->base_level > skip ? range->base_level - skip : 0;
		range->level_count = end - skip - range->base_level;
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
	// On the primary resource's memory with one that holds the contents: nothing to copy (Transition's aliasing barrier hands the
	// memory over).
	if (OnPrimaryMemory(backing, resource)) {
		for (uint32_t holder = 0; holder < backing.resources.size(); holder++) {
			if (holder != resource && OnPrimaryMemory(backing, holder) && backing.resources[holder].version == backing.version) {
				backing.resources[resource].version = backing.version;
				return;
			}
		}
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
	// Shaders read a CopiedView from its copy; copies and targets use the image itself.
	constexpr auto shader_read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	if (view && (state & shader_read) != 0 && !IsWriteState(static_cast<D3D12_RESOURCE_STATES>(state)) &&
	    CopiedView(backing, backing.views[view->index].info)) {
		UseCopy(command, backing.views[view->index], state);
		return;
	}
	Transition(command, resource, state, range);
	if (IsWriteState(static_cast<D3D12_RESOURCE_STATES>(state))) {
		backing.version++;
		backing.resources[resource].version = backing.version;
		backing.current                     = resource;
	}
}

void Image::UseCopy(CommandBuffer& command, ImageViewDescriptors& view, uint32_t state) {
	const auto& info   = view.info;
	auto&       copy   = view.copy;
	auto&       from   = backing.resources[view.resource];
	const bool  volume = backing.image_type == vk::ImageType::e3D;
	const auto  width  = std::max(backing.extent.width >> info.base_level, 1u);
	const auto  height = std::max(backing.extent.height >> info.base_level, 1u);
	// The copy's subresources: its levels are the view's, its layers the view's layers (or slices); the planes as the image's.
	const auto subresource = [&](uint32_t level, uint32_t layer, uint32_t plane) {
		return level + (layer + plane * info.layer_count) * info.level_count;
	};
	if (copy.resource == nullptr) {
		// A block-compressed texture's size is whole blocks (a level of the image can be smaller).
		const auto block = GetCopyBlock(backing.format, vk::ImageAspectFlagBits::eColor);
		EXIT_NOT_IMPLEMENTED(width % block.width != 0 || height % block.height != 0);
		EXIT_NOT_IMPLEMENTED(volume && info.level_count != 1);
		D3D12_RESOURCE_DESC desc {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width            = width;
		desc.Height           = height;
		desc.DepthOrArraySize = static_cast<UINT16>(info.layer_count);
		desc.MipLevels        = static_cast<UINT16>(info.level_count);
		desc.Format           = from.family;
		desc.SampleDesc.Count = backing.samples;
		// Multisampled and depth textures must be targets.
		desc.Flags = from.resource->GetDesc().Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		D3D12MA::ALLOCATION_DESC allocation {};
		allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
		// A BC array the CPU writes (GraphicContext::cpu_written_block_arrays): the device's copies would place the copy's small
		// levels wrongly too, so the CPU copies it.
		copy.cpu_written = from.cpu_written;
		if (copy.cpu_written) {
			allocation.CustomPool = m_graphics.cpu_texture_pool;
		}
		const auto initial = copy.cpu_written ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST;
		const auto result  = m_graphics.allocator->CreateResource(&allocation, &desc, initial, nullptr, &copy.allocation, IID_NULL, nullptr);
		if (FAILED(result)) {
			m_graphics.LogMemoryBudget();
			EXIT("D3D12: failed to create the copy of an image view: %ux%u, %u levels, %u layers, format %d, HRESULT=0x%08x\n", width,
			     height, info.level_count, info.layer_count, static_cast<int>(desc.Format), static_cast<uint32_t>(result));
		}
		copy.resource = copy.allocation->GetResource();
		copy.family   = desc.Format;
		copy.states.assign(static_cast<size_t>(info.level_count) * info.layer_count * backing.planes, initial);
	}
	std::vector<D3D12_RESOURCE_BARRIER> barriers;
	const auto                          transition = [&](D3D12_RESOURCE_STATES after) {
		barriers.clear();
		for (uint32_t index = 0; index < copy.states.size(); index++) {
			if (copy.states[index] != after) {
				D3D12_RESOURCE_BARRIER barrier {};
				barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition.pResource   = copy.resource;
				barrier.Transition.Subresource = index;
				barrier.Transition.StateBefore = copy.states[index];
				barrier.Transition.StateAfter  = after;
				barriers.push_back(barrier);
				copy.states[index] = after;
			}
		}
		if (!barriers.empty()) {
			command.Handle()->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}
	};
	if (copy.version != backing.version && copy.cpu_written) {
		// Such an image only changes by the CPU's uploads (UploadWithCpu), which wait for the GPU.
		const auto           block = GetCopyBlock(backing.format, vk::ImageAspectFlagBits::eColor);
		std::vector<uint8_t> data;
		for (uint32_t level = 0; level < info.level_count; level++) {
			const uint64_t row_pitch   = DivCeil(std::max(width >> level, 1u), block.width) * block.bytes;
			const uint64_t slice_pitch = DivCeil(std::max(height >> level, 1u), block.height) * row_pitch;
			data.resize(slice_pitch);
			for (uint32_t layer = 0; layer < info.layer_count; layer++) {
				const auto source      = backing.Subresource(info.base_level + level, info.base_layer + layer, 0);
				const auto destination = subresource(level, layer, 0);
				D3D12::Check(from.resource->Map(source, nullptr, nullptr), "image view copy: map image");
				const HRESULT read = from.resource->ReadFromSubresource(data.data(), static_cast<UINT>(row_pitch), static_cast<UINT>(slice_pitch),
				                                                        source, nullptr);
				from.resource->Unmap(source, nullptr);
				D3D12::Check(read, "image view copy: read image");
				D3D12::Check(copy.resource->Map(destination, nullptr, nullptr), "image view copy: map copy");
				const HRESULT written = copy.resource->WriteToSubresource(destination, nullptr, data.data(), static_cast<UINT>(row_pitch),
				                                                          static_cast<UINT>(slice_pitch));
				copy.resource->Unmap(destination, nullptr);
				D3D12::Check(written, "image view copy: write copy");
			}
		}
		copy.version = backing.version;
	} else if (copy.version != backing.version) {
		Transition(command, view.resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
		           ImageSubresourceRange {info.base_level, info.level_count, info.base_layer, info.layer_count});
		transition(D3D12_RESOURCE_STATE_COPY_DEST);
		for (uint32_t plane = 0; plane < backing.planes; plane++) {
			for (uint32_t level = 0; level < info.level_count; level++) {
				for (uint32_t layer = 0; layer < info.layer_count; layer++) {
					D3D12_TEXTURE_COPY_LOCATION src {};
					src.pResource        = from.resource;
					src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					src.SubresourceIndex = backing.Subresource(info.base_level + level, volume ? 0 : info.base_layer + layer, plane);
					D3D12_TEXTURE_COPY_LOCATION dst {};
					dst.pResource        = copy.resource;
					dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dst.SubresourceIndex = subresource(level, layer, plane);
					if (volume) {
						// A 3D level is one subresource: each slice goes to its layer (Dawn copies between 3D and 2D array textures the same
						// way). A layer of an array is copied whole, as depth and multisampled textures require.
						const UINT      slice = info.base_layer + layer;
						const D3D12_BOX box {0, 0, slice, width, height, slice + 1};
						command.Handle()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
					} else {
						command.Handle()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					}
				}
			}
		}
		copy.version = backing.version;
	}
	transition(static_cast<D3D12_RESOURCE_STATES>(state));
}

ImageViewHandle Image::CreateView(const ImageViewInfo& view_info) {
	ImageViewDescriptors view;
	view.info     = view_info;
	view.resource = ResourceIndex(view_info.format);
	backing.views.push_back(view);
	return {static_cast<uint32_t>(backing.views.size() - 1)};
}

ID3D12Resource* Image::Resource(ImageViewHandle view) const {
	return backing.resources[backing.views[view.index].resource].resource;
}

void Image::UseSnapshot(CommandBuffer& command, ImageViewHandle handle, uint32_t state) {
	auto& view = backing.views[handle.index];
	Synchronize(command, view.resource);
	UseCopy(command, view, state);
}

D3D12_CPU_DESCRIPTOR_HANDLE Image::ShaderResourceView(ImageViewHandle handle, bool snapshot) {
	auto&      view      = backing.views[handle.index];
	const bool from_copy = snapshot || CopiedView(backing, view.info);
	if (const auto cached = from_copy ? view.copy_srv : view.srv; cached.ptr != 0) {
		return cached;
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
	if (from_copy) {
		// The view's copy (UseCopy made it before the first draw that reads it): its first level and layer are the view's.
		EXIT_IF(view.copy.resource == nullptr);
		const float copy_min_lod = static_cast<float>(info.min_lod) / 256.0f;
		switch (info.type) {
			case vk::ImageViewType::e2D:
				if (multisample) {
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
				} else {
					desc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipLevels           = info.level_count;
					desc.Texture2D.PlaneSlice          = plane;
					desc.Texture2D.ResourceMinLODClamp = copy_min_lod;
				}
				break;
			case vk::ImageViewType::eCube:
				desc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
				desc.TextureCube.MipLevels           = info.level_count;
				desc.TextureCube.ResourceMinLODClamp = copy_min_lod;
				break;
			default:
				desc.ViewDimension                      = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MipLevels           = info.level_count;
				desc.Texture2DArray.ArraySize           = info.layer_count;
				desc.Texture2DArray.ResourceMinLODClamp = copy_min_lod;
				break;
		}
		view.copy_srv = m_scheduler.Context().GetDescriptorHeap().AllocateCpu(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		m_graphics.device->CreateShaderResourceView(view.copy.resource, &desc, view.copy_srv);
		return view.copy_srv;
	}
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
			// resource's first array slice (later ones are CopiedView).
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
		// A format this device cannot render to (D3D12::RenderableHostFormat leaves only those): no view, the draw is skipped.
		static std::atomic_bool warned = false;
		if (!warned.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog(fmt::format("D3D12: image format {} cannot be rendered to on this device; draws into it are skipped\n",
			                                      static_cast<int>(info.format)));
		}
		return {};
	}
	D3D12_RENDER_TARGET_VIEW_DESC desc {};
	desc.Format = format.view;
	if (backing.image_type == vk::ImageType::e3D) {
		desc.ViewDimension         = D3D12_RTV_DIMENSION_TEXTURE3D;
		desc.Texture3D.MipSlice    = info.base_level;
		desc.Texture3D.FirstWSlice = info.type == vk::ImageViewType::e3D ? 0 : info.base_layer;
		desc.Texture3D.WSize       = info.type == vk::ImageViewType::e3D ? UINT(-1) : info.layer_count;
	} else if (backing.image_type == vk::ImageType::e1D) {
		desc.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
		desc.Texture1DArray.MipSlice        = info.base_level;
		desc.Texture1DArray.FirstArraySlice = info.base_layer;
		desc.Texture1DArray.ArraySize       = info.layer_count;
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
	const bool       expanded     = Expanded96(backing.format);
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
		const auto     host_row_bytes  = expanded ? static_cast<uint32_t>(width_blocks * 16u) : row_bytes;
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
			job.direct = !expanded && buffer_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0 &&
			             src_row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0 &&
			             (slices == 1 || src_slice_pitch == src_row_pitch * rows);
			if (job.direct) {
				job.footprint.Offset             = buffer_offset;
				job.footprint.Footprint.RowPitch = static_cast<UINT>(src_row_pitch);
			} else {
				const auto pitch  = Common::AlignUp<uint64_t>(host_row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
				scratch_size      = Common::AlignUp<uint64_t>(scratch_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
				job.footprint.Offset             = scratch_size;
				job.footprint.Footprint.RowPitch = static_cast<UINT>(pitch);
				scratch_size += pitch * rows * slices;
				job.rows = {buffer_offset, src_row_pitch, src_slice_pitch, job.footprint.Offset,
				            pitch,         pitch * rows,  host_row_bytes,  rows,
				            slices};
				if (expanded) {
					job.rows.texel_mode = 1;
					job.rows.alpha      = Expanded96One(backing.format);
				}
				if (!upload) {
					std::swap(job.rows.src_offset, job.rows.dst_offset);
					std::swap(job.rows.src_row_pitch, job.rows.dst_row_pitch);
					std::swap(job.rows.src_slice_pitch, job.rows.dst_slice_pitch);
					if (expanded) {
						job.rows.row_bytes  = row_bytes;
						job.rows.texel_mode = 2;
					}
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

// The copies' linear data (in buffer from offset, size bytes) is read back and written into the image's CPU-visible resource by the CPU: this
// device's copies would place the small layers of the BC array where the texture unit does not read them. Waiting for the GPU is allowed
// here: images are uploaded while a draw's resources are prepared, before anything is recorded for it.
void Image::UploadWithCpu(CommandBuffer& command, std::span<const vk::BufferImageCopy> copies, BufferHandle buffer, uint64_t offset,
                          uint64_t size) {
	auto& target = backing.resources[backing.current];
	D3D12_HEAP_PROPERTIES readback_heap {D3D12_HEAP_TYPE_READBACK};
	D3D12_RESOURCE_DESC   buffer_desc {};
	buffer_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer_desc.Width            = size;
	buffer_desc.Height           = 1;
	buffer_desc.DepthOrArraySize = 1;
	buffer_desc.MipLevels        = 1;
	buffer_desc.SampleDesc.Count = 1;
	buffer_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	Microsoft::WRL::ComPtr<ID3D12Resource> readback;
	D3D12::Check(m_graphics.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                                IID_PPV_ARGS(&readback)),
	      "image upload: readback buffer");
	buffer->Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	command.Handle()->CopyBufferRegion(readback.Get(), 0, buffer->Resource(), offset, size);
	m_scheduler.FlushAndWait();

	const uint8_t* data = nullptr;
	D3D12_RANGE    read {0, static_cast<SIZE_T>(size)};
	D3D12::Check(readback->Map(0, &read, reinterpret_cast<void**>(const_cast<uint8_t**>(&data))), "image upload: map readback buffer");
	for (const auto& copy: copies) {
		const auto& sub        = copy.imageSubresource;
		const auto  block      = GetCopyBlock(backing.format, sub.aspectMask);
		const auto  row_texels = copy.bufferRowLength != 0 ? copy.bufferRowLength : copy.imageExtent.width;
		const auto  image_rows = copy.bufferImageHeight != 0 ? copy.bufferImageHeight : copy.imageExtent.height;
		const uint64_t row_pitch   = DivCeil(row_texels, block.width) * block.bytes;
		const uint64_t slice_pitch = DivCeil(image_rows, block.height) * row_pitch;
		const auto     slices      = copy.imageExtent.depth;
		// Whole blocks, but not past the level's edge (a BC level of 1x1 texels is one block; its box is 1x1).
		const uint64_t  level_width  = std::max(backing.extent.width >> sub.mipLevel, 1u);
		const uint64_t  level_height = std::max(backing.extent.height >> sub.mipLevel, 1u);
		const D3D12_BOX box {static_cast<UINT>(copy.imageOffset.x), static_cast<UINT>(copy.imageOffset.y), static_cast<UINT>(copy.imageOffset.z),
		                    static_cast<UINT>(std::min<uint64_t>(copy.imageOffset.x + DivCeil(copy.imageExtent.width, block.width) * block.width, level_width)),
		                    static_cast<UINT>(std::min<uint64_t>(copy.imageOffset.y + DivCeil(copy.imageExtent.height, block.height) * block.height, level_height)),
		                    static_cast<UINT>(copy.imageOffset.z + slices)};
		for (uint32_t layer = 0; layer < sub.layerCount; layer++) {
			const uint64_t start = copy.bufferOffset + slice_pitch * slices * layer;
			EXIT_IF(start < offset || start - offset + slice_pitch * slices > size);
			const auto subresource = backing.Subresource(sub.mipLevel, sub.baseArrayLayer + layer, Plane(sub.aspectMask));
			// A whole level is written without a box (as the self test does).
			const bool whole = box.left == 0 && box.top == 0 && box.front == 0 && box.right == level_width && box.bottom == level_height;
			D3D12::Check(target.resource->Map(subresource, nullptr, nullptr), "image upload: map image");
			const HRESULT result = target.resource->WriteToSubresource(subresource, whole ? nullptr : &box, data + (start - offset),
			                                                           static_cast<UINT>(row_pitch), static_cast<UINT>(slice_pitch));
			target.resource->Unmap(subresource, nullptr);
			if (FAILED(result)) {
				EXIT("D3D12: image upload: WriteToSubresource failed (0x%08x): subresource %u (level %u, layer %u), box %u,%u,%u-%u,%u,%u%s, row "
				     "pitch %" PRIu64 ", slice pitch %" PRIu64 ", image %ux%u, %u levels, %u layers, format %d\n",
				     static_cast<uint32_t>(result), subresource, sub.mipLevel, sub.baseArrayLayer + layer, box.left, box.top, box.front, box.right,
				     box.bottom, box.back, whole ? " (whole level, no box)" : "", row_pitch, slice_pitch, backing.extent.width, backing.extent.height,
				     backing.mip_levels, backing.layers, static_cast<int>(target.family));
			}
		}
	}
	D3D12_RANGE none {0, 0};
	readback->Unmap(0, &none);
	// As Use does for a write: this resource now holds the image's contents.
	backing.version++;
	target.version = backing.version;
	static std::atomic_int logged = 0;
	if (logged.fetch_add(1) < 8) {
		Log::WriteToConsoleAndLog(fmt::format("D3D12: BC array written by the CPU: {}x{}, {} layers, {} mips, {} regions\n", backing.extent.width,
		                                      backing.extent.height, backing.layers, backing.mip_levels, copies.size()));
	}
}

void Image::Upload(std::span<const vk::BufferImageCopy> copies, BufferHandle buffer,
                   uint64_t offset, uint64_t size) {
	EXIT_IF(copies.empty() || buffer == nullptr || size == 0);
	auto& command = m_scheduler.Current();
	if (backing.resources[backing.current].cpu_written) {
		UploadWithCpu(command, copies, buffer, offset, size);
		return;
	}
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

bool Image::ReadbackColor(std::vector<uint8_t>& data, uint32_t& width, uint32_t& height, uint32_t& row_bytes) {
	if (backing.resources.empty() || backing.samples != 1 || backing.planes != 1 || backing.image_type != vk::ImageType::e2D) {
		return false;
	}
	auto* resource = backing.resources[backing.current].resource;
	if (resource == nullptr) {
		return false;
	}
	const auto                         desc = resource->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
	UINT                               rows       = 0;
	UINT64                             row_size   = 0;
	UINT64                             total_size = 0;
	m_graphics.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &total_size);
	if (total_size == 0 || rows == 0) {
		return false;
	}
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC buffer_desc {};
	buffer_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer_desc.Width            = total_size;
	buffer_desc.Height           = 1;
	buffer_desc.DepthOrArraySize = 1;
	buffer_desc.MipLevels        = 1;
	buffer_desc.SampleDesc.Count = 1;
	buffer_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	Microsoft::WRL::ComPtr<ID3D12Resource> readback;
	if (FAILED(m_graphics.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
		return false;
	}
	auto& command = m_scheduler.Current();
	Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE, ImageSubresourceRange {0, 1, 0, 1});
	D3D12_TEXTURE_COPY_LOCATION destination {};
	destination.pResource       = readback.Get();
	destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint = footprint;
	D3D12_TEXTURE_COPY_LOCATION source {};
	source.pResource        = resource;
	source.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	source.SubresourceIndex = 0;
	command.Handle()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	m_scheduler.FlushAndWait();
	void*         mapped = nullptr;
	D3D12_RANGE   range {0, static_cast<SIZE_T>(total_size)};
	if (FAILED(readback->Map(0, &range, &mapped)) || mapped == nullptr) {
		return false;
	}
	width     = static_cast<uint32_t>(desc.Width);
	height    = desc.Height;
	row_bytes = static_cast<uint32_t>(row_size);
	data.resize(static_cast<size_t>(row_bytes) * rows);
	for (UINT row = 0; row < rows; ++row) {
		std::memcpy(data.data() + static_cast<size_t>(row) * row_bytes, static_cast<const uint8_t*>(mapped) + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
		            row_bytes);
	}
	D3D12_RANGE none {0, 0};
	readback->Unmap(0, &none);
	return true;
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

void Image::CopyDepthToColor(CommandBuffer& command, Image& depth, const ImageSubresourceRange& depth_range,
                             const ImageSubresourceRange& range, const D3D12_RECT& rect) {
	EXIT_IF(rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top);
	depth.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE, ImageSubresourceRange {depth_range.base_level, 1, depth_range.base_layer, 1});
	Use(command, D3D12_RESOURCE_STATE_COPY_DEST, ImageSubresourceRange {range.base_level, 1, range.base_layer, 1});
	auto*      from              = depth.backing.resources[depth.backing.current].resource;
	auto*      to                = backing.resources[backing.current].resource;
	const auto depth_subresource = depth.backing.Subresource(depth_range.base_level, depth_range.base_layer, 0);
	const auto subresource       = backing.Subresource(range.base_level, range.base_layer, 0);
	const auto width             = std::max(depth.backing.extent.width >> depth_range.base_level, 1u);
	const auto height            = std::max(depth.backing.extent.height >> depth_range.base_level, 1u);
	EXIT_IF(static_cast<uint32_t>(rect.right) > width || static_cast<uint32_t>(rect.bottom) > height);

	// A depth plane can only be copied whole: all of it goes into a buffer, and the rectangle goes on from there.
	const auto pitch   = static_cast<uint32_t>(Common::AlignUp<uint64_t>(uint64_t {width} * sizeof(float), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
	auto&      scratch = ScratchBuffer(m_graphics, m_scheduler, uint64_t {pitch} * height);
	D3D12_TEXTURE_COPY_LOCATION linear {};
	linear.pResource                          = scratch.Resource();
	linear.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	linear.PlacedFootprint.Footprint.Format   = CopyFormat(m_graphics.device, from, depth_subresource);
	linear.PlacedFootprint.Footprint.Width    = width;
	linear.PlacedFootprint.Footprint.Height   = height;
	linear.PlacedFootprint.Footprint.Depth    = 1;
	linear.PlacedFootprint.Footprint.RowPitch = pitch;
	D3D12_TEXTURE_COPY_LOCATION plane {};
	plane.pResource        = from;
	plane.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	plane.SubresourceIndex = depth_subresource;
	auto* list             = command.Handle();
	scratch.Use(command, D3D12_RESOURCE_STATE_COPY_DEST);
	list->CopyTextureRegion(&linear, 0, 0, 0, &plane, nullptr);

	scratch.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
	linear.PlacedFootprint.Footprint.Format = CopyFormat(m_graphics.device, to, subresource);
	D3D12_TEXTURE_COPY_LOCATION target {};
	target.pResource        = to;
	target.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	target.SubresourceIndex = subresource;
	const D3D12_BOX box {static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, static_cast<UINT>(rect.right),
	                     static_cast<UINT>(rect.bottom), 1};
	list->CopyTextureRegion(&target, box.left, box.top, 0, &linear, &box);
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

void Image::CopyImageWithBuffer(Image& source, Buffer& /*buffer*/, TileManager& tiler) {
	EXIT_IF(source.backing.samples != 1 || backing.samples != 1);
	const auto source_transform = source.info.GetColorTransform();
	const auto target_transform = info.GetColorTransform();
	if (source_transform == target_transform) {
		// Copies between format families already go through a buffer (CopyContents).
		CopyImage(source);
		return;
	}
	// As upstream's Vulkan copy: through a buffer, in which the source's color transform (the bgra16 swap, the 10-11-11 order) is
	// undone and the target's applied. (A D3D12 dispatch cannot read and write one buffer: each transform writes a new one.)
	const auto     bytes = source.info.bytes_per_block;
	const uint32_t block = source.info.IsBlock() ? 4u : 1u;
	EXIT_IF(source.info.IsDepth() || info.IsDepth() || bytes == 0 || bytes != info.bytes_per_block ||
	        block != (info.IsBlock() ? 4u : 1u));
	const bool     source_volume = source.backing.image_type == vk::ImageType::e3D;
	const bool     target_volume = backing.image_type == vk::ImageType::e3D;
	const uint32_t levels        = std::min(source.backing.mip_levels, backing.mip_levels);
	for (uint32_t level = 0; level < levels; level++) {
		const auto width  = std::max(source.backing.extent.width >> level, 1u);
		const auto height = std::max(source.backing.extent.height >> level, 1u);
		const auto slices = std::min(source_volume ? std::max(source.backing.extent.depth >> level, 1u) : source.backing.layers,
		                             target_volume ? std::max(backing.extent.depth >> level, 1u) : backing.layers);
		const auto size   = static_cast<uint64_t>(DivCeil(width, block)) * bytes * DivCeil(height, block);
		EXIT_IF(slices == 0 || size == 0);
		for (uint32_t slice = 0; slice < slices; slice++) {
			vk::BufferImageCopy from {};
			from.imageSubresource = {vk::ImageAspectFlagBits::eColor, level, source_volume ? 0u : slice, 1};
			from.imageOffset      = {0, 0, source_volume ? static_cast<int32_t>(slice) : 0};
			from.imageExtent      = {width, height, 1};
			auto to               = from;
			to.imageSubresource   = {vk::ImageAspectFlagBits::eColor, level, target_volume ? 0u : slice, 1};
			to.imageOffset        = {0, 0, target_volume ? static_cast<int32_t>(slice) : 0};
			auto data             = tiler.GetScratchBuffer(size);
			from.bufferOffset     = data.offset;
			source.Download({&from, 1}, data.buffer, data.offset, size);
			data.size = size;
			if (source_transform != ColorTransform::None) {
				data = tiler.TransformColor(data, source_transform, false);
			}
			if (target_transform != ColorTransform::None) {
				data = tiler.TransformColor(data, target_transform, true);
			}
			to.bufferOffset = data.offset;
			Upload({&to, 1}, data.buffer, data.offset, size);
		}
	}
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
		// The view the clear goes through must fit the image (Vulkan clears without one): 1D images take a 1D-array view.
		view.type        = backing.image_type == vk::ImageType::e1D ? vk::ImageViewType::e1DArray
		                                                          : vk::ImageViewType::e2DArray;
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
		} else if (format == vk::Format::eE5B9G9R9UfloatPack32) {
			// D3D12 neither renders to nor writes shared-exponent images: the packed color is copied in from a buffer it fills.
			const bool     volume = backing.image_type == vk::ImageType::e3D;
			const auto     width  = std::max(backing.extent.width >> level, 1u);
			const auto     height = std::max(backing.extent.height >> level, 1u);
			const auto     depth  = volume ? std::max(backing.extent.depth >> level, 1u) : 1u;
			const auto     pitch  = static_cast<UINT>(Common::AlignUp<uint64_t>(uint64_t {width} * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
			const uint64_t size   = uint64_t {pitch} * height * depth;
			auto&          scratch = ScratchBuffer(m_graphics, m_scheduler, size);
			scratch.Fill(0, size, PackE5B9G9R9(clear.color.float32.data()));
			scratch.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Use(command, D3D12_RESOURCE_STATE_COPY_DEST, subresources, handle);
			auto* target = backing.resources[resource].resource;
			for (uint32_t layer = 0; layer < (volume ? 1u : range.layerCount); layer++) {
				D3D12_TEXTURE_COPY_LOCATION dst {};
				dst.pResource        = target;
				dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.SubresourceIndex = backing.Subresource(level, volume ? 0 : range.baseArrayLayer + layer, 0);
				D3D12_TEXTURE_COPY_LOCATION src {};
				src.pResource                          = scratch.Resource();
				src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint.Footprint.Format   = CopyFormat(m_graphics.device, target, dst.SubresourceIndex);
				src.PlacedFootprint.Footprint.Width    = width;
				src.PlacedFootprint.Footprint.Height   = height;
				src.PlacedFootprint.Footprint.Depth    = depth;
				src.PlacedFootprint.Footprint.RowPitch = pitch;
				list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			}
		} else {
			EXIT("D3D12: image format %d cannot be cleared\n", static_cast<int>(format));
		}
	}
}

} // namespace Libs::Graphics
