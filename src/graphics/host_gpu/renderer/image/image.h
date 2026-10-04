#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/alignment.h"
#include "common/assert.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#if defined(KYTY_GPU_BACKEND_D3D12)
#include "graphics/host_gpu/d3d12/imageBacking.h"
#else
#include "graphics/host_gpu/graphicContext.h"
#endif

#include <compare>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class CommandScheduler;
struct ImageTestAccess;

#if !defined(KYTY_GPU_BACKEND_D3D12)
using ImageBacking    = VulkanImage;
using ImageViewHandle = vk::ImageView;
#endif

using ImageId = Common::SlotId;

struct CachedImageView {
	ImageViewInfo   info;
	ImageViewHandle view {};
};

struct ImageUsage {
	bool texture       = false;
	bool storage       = false;
	bool render_target = false;
	bool depth_target  = false;
	bool video_out     = false;
};

struct ImageBinding {
#if !defined(KYTY_GPU_BACKEND_D3D12)
	vk::ImageLayout  attachment_layout = vk::ImageLayout::eUndefined;
	vk::AccessFlags2 attachment_access;
#endif
	bool             is_bound      = false;
	bool             is_target     = false;
	bool             needs_rebind  = false;
	bool             force_general = false;
	bool             shader_write  = false;
};

// A host image holding the contents of a guest image. The cache state and view bookkeeping are
// shared; the host resource (`backing`) and the GPU operations are implemented per backend.
class Image final {
public:
	Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& info);
	~Image();
	KYTY_CLASS_NO_COPY(Image);

	[[nodiscard]] ImageViewHandle FindView(const ImageViewInfo& view_info);
	void Upload(std::span<const vk::BufferImageCopy> copies, BufferHandle buffer, uint64_t offset,
	            uint64_t size);
	void Download(std::span<const vk::BufferImageCopy> copies, BufferHandle buffer,
	              uint64_t offset, uint64_t size);
	void CopyImage(Image& source);
	void Resolve(Image& source, const ImageSubresourceRange& source_range,
	             const ImageSubresourceRange& destination_range);
	void CopyImageWithBuffer(Image& source, Buffer& buffer);
	void CopyMip(Image& source, uint32_t mip, uint32_t layer);
	// Copies `regions` of `source`, which lie within `range` of both images.
	void CopySubresources(Image& source, const ImageSubresourceRange& range,
	                      std::span<const vk::ImageCopy> regions);
	// Clears `range`; a `format` other than the image format clears through an aliasing view.
	void Clear(CommandBuffer& command, vk::Format format, const vk::ImageSubresourceRange& range,
	           const vk::ClearValue& clear, bool full_image);

#if defined(KYTY_GPU_BACKEND_D3D12)
	// Records the transition of `range` (all subresources when empty) to `state`, a
	// D3D12_RESOURCE_STATES value, on the resource `view` reads (the current contents when empty).
	// A resource of another format family is first brought up to date; a write state makes the
	// used resource the only current one.
	void Use(CommandBuffer& command, uint32_t state, std::optional<ImageSubresourceRange> range = {},
	         std::optional<ImageViewHandle> view = {});
	[[nodiscard]] ID3D12Resource*             Resource(ImageViewHandle view) const;
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE ShaderResourceView(ImageViewHandle view);
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE UnorderedAccessView(ImageViewHandle view);
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView(ImageViewHandle view);
	// A read-only view lets the image be sampled while it is the depth target.
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView(ImageViewHandle view,
	                                                           bool            read_only = false);
#else
	using Barriers = std::vector<vk::ImageMemoryBarrier2>;
	[[nodiscard]] Barriers GetBarriers(vk::ImageLayout                      destination_layout,
	                                   vk::AccessFlags2                     destination_access,
	                                   vk::PipelineStageFlags2              destination_stage,
	                                   std::optional<ImageSubresourceRange> range);
	void Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
	             std::optional<ImageSubresourceRange> range, vk::CommandBuffer command_buffer);
#endif

	void InvalidateCpuWrite(uint64_t vaddr, uint64_t size) {
		if (ImageRangeOverlaps(info.data.address, info.data.size, vaddr, size)) {
			m_cpu_dirty        = true;
			m_maybe_cpu_dirty  = false;
			m_maybe_hash_valid = false;
		} else if (ImagePageRangesOverlap(info.data.address, info.data.size, vaddr, size)) {
			m_maybe_cpu_dirty = true;
		}
	}

	[[nodiscard]] bool IsCpuDirty() const { return m_cpu_dirty || m_maybe_cpu_dirty; }
	[[nodiscard]] bool IsDefinitelyCpuDirty() const { return m_cpu_dirty; }
	[[nodiscard]] bool IsMaybeCpuDirty() const { return m_maybe_cpu_dirty; }
	void               MarkMaybeCpuDirty() {
		if (!m_cpu_dirty) {
			m_maybe_cpu_dirty = true;
		}
	}
	[[nodiscard]] bool NeedsMaybeCpuHash() const {
		return m_maybe_cpu_dirty && !m_maybe_hash_valid;
	}
	void SetMaybeCpuHash(uint64_t hash) {
		if (!NeedsMaybeCpuHash()) {
			EXIT("image cannot initialize maybe-dirty hash\n");
		}
		m_maybe_cpu_hash   = hash;
		m_maybe_hash_valid = true;
	}
	[[nodiscard]] bool ResolveMaybeCpuHash(uint64_t hash) {
		if (!m_maybe_cpu_dirty || !m_maybe_hash_valid || m_cpu_dirty) {
			EXIT("image cannot resolve maybe-dirty hash\n");
		}
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
		m_cpu_dirty |= hash != m_maybe_cpu_hash;
		return m_cpu_dirty;
	}

	void RefreshComplete() {
		if (!IsCpuDirty()) {
			EXIT("clean image cannot complete a refresh\n");
		}
		m_cpu_dirty        = false;
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
	}

	[[nodiscard]] bool IsGpuModified() const noexcept { return m_gpu_modified; }
	void               MarkGpuModified() noexcept { m_gpu_modified = true; }
	void               ClearGpuModified() noexcept { m_gpu_modified = false; }

	[[nodiscard]] bool IsBufferModified() const noexcept { return m_buffer_modified; }
	void               MarkBufferModified() noexcept { m_buffer_modified = true; }
	void               ClearBufferModified() noexcept { m_buffer_modified = false; }

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size,
	                            bool pages = false) const noexcept {
		return pages ? ImagePageRangesOverlap(info.data.address, info.data.size, address, size)
		             : ImageRangeOverlaps(info.data.address, info.data.size, address, size);
	}
	[[nodiscard]] bool SafeToDownload() const noexcept {
		return IsGpuModified() && !IsBufferModified() && !IsCpuDirty();
	}
	[[nodiscard]] bool IsTracked() const noexcept { return track_addr != 0 && track_addr_end != 0; }
	[[nodiscard]] uint64_t AccountedSize() const noexcept {
		return backing.image == nullptr ? 0 : Common::AlignUp(info.data.size, 1024);
	}
	[[nodiscard]] uint64_t HashGuestEdges() const;

	ImageInfo        info;
	ImageBacking     backing;
	std::vector<CachedImageView> views;
	ImageUsage       usage;
	ImageBinding     binding;
	bool             registered     = false;
	mutable uint32_t query_epoch    = 0;
	uint64_t         track_addr     = 0;
	uint64_t         track_addr_end = 0;
	ImageId          depth_id {};
	uint64_t         tick_accessed_last = 0;
	size_t           lru_id             = 0;
	// CommandScheduler tick and guest DCC metadata bytes MaterializeDccClear last read back from
	// the GPU for this image. UINT64_MAX means never. Lets repeated FindImage lookups of the same
	// metadata within the same still-unsubmitted recording (i.e. many draws to the same bound
	// render target) skip the synchronous GPU readback after the first one; the guest bytes are
	// still inspected, so CPU writes and other slices are always seen.
	uint64_t         dcc_readback_tick    = UINT64_MAX;
	uint64_t         dcc_readback_address = 0;
	uint64_t         dcc_readback_size    = 0;

private:
	friend struct ImageTestAccess;

	// Creates the host view for a validated, normalized description.
	[[nodiscard]] ImageViewHandle CreateView(const ImageViewInfo& view_info);

#if defined(KYTY_GPU_BACKEND_D3D12)
	struct BufferRegion;
	[[nodiscard]] uint32_t ResourceIndex(vk::Format view_format);
	void                   Transition(CommandBuffer& command, uint32_t resource, uint32_t state,
	                                  std::optional<ImageSubresourceRange> range);
	void                   Synchronize(CommandBuffer& command, uint32_t resource);
	void CopyRegions(CommandBuffer& command, std::span<const vk::BufferImageCopy> copies,
	                 BufferHandle buffer, bool upload);
	void CopyContents(CommandBuffer& command, Image& source, uint32_t source_resource,
	                  uint32_t destination_resource, std::span<const vk::ImageCopy> copies);
#endif

	[[nodiscard]] static vk::ImageAspectFlags FullAspectMask(vk::Format format) noexcept;
	[[nodiscard]] static uint32_t             CopyRows(uint64_t row_size, uint32_t rows,
	                                                   uint64_t capacity) noexcept;
	[[nodiscard]] static std::pair<uint32_t, uint32_t>
	SanitizeCopyLayers(const Image& source, const Image& destination, uint32_t depth);

	GraphicContext&   m_graphics;
	CommandScheduler& m_scheduler;
	uint64_t          m_maybe_cpu_hash   = 0;
	bool              m_cpu_dirty        = false;
	bool              m_maybe_cpu_dirty  = false;
	bool              m_maybe_hash_valid = false;
	bool              m_gpu_modified     = false;
	bool              m_buffer_modified  = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
