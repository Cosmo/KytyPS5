#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/memoryUsage.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>
#include <utility>

VK_DEFINE_HANDLE(VmaAllocation)

namespace Libs::Graphics {

class CommandBuffer;
class CommandScheduler;
struct GraphicContext;

// Identifies a buffer to the other renderer objects (images, tile manager).
using BufferHandle = vk::Buffer;

inline constexpr vk::BufferUsageFlags ReadFlags =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;

inline constexpr vk::BufferUsageFlags AllFlags =
    ReadFlags | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;

// A Vulkan buffer. The constructor without usage flags, the accessors and the copy/fill/name
// operations form the interface shared code uses with every backend.
class Buffer {
public:
	// Buffers that mirror guest memory (cpu_address != 0) are also shader-addressable.
	Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
	       uint64_t cpu_address, uint64_t size);
	Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
	       uint64_t cpu_address, vk::BufferUsageFlags flags, uint64_t size);
	~Buffer();
	KYTY_CLASS_NO_COPY(Buffer);

	[[nodiscard]] vk::Buffer         Handle() const noexcept { return m_buffer; }
	[[nodiscard]] uint64_t           Size() const noexcept { return m_size; }
	[[nodiscard]] std::span<uint8_t> Mapped() const noexcept { return m_mapped; }
	[[nodiscard]] bool               IsCoherent() const noexcept { return m_coherent; }
	[[nodiscard]] MemoryUsage        Usage() const noexcept { return m_usage; }
	[[nodiscard]] uint64_t           CpuAddress() const noexcept { return m_cpu_address; }
	[[nodiscard]] vk::DeviceAddress BufferDeviceAddress() const noexcept;
	[[nodiscard]] uint64_t           Offset(uint64_t address) const noexcept {
		return address - m_cpu_address;
	}
	[[nodiscard]] bool IsInBounds(uint64_t address, uint64_t size) const noexcept;
	// Granularity of Flush/Invalidate ranges on non-coherent memory.
	[[nodiscard]] uint64_t FlushAtomSize() const noexcept;
	void               IncreaseStreamScore(int score) noexcept { stream_score += score; }
	[[nodiscard]] int  StreamScore() const noexcept { return stream_score; }
	void               Flush(uint64_t offset, uint64_t size);
	void               Invalidate(uint64_t offset, uint64_t size);
	void CopyFrom(CommandBuffer& command, const Buffer& source, uint64_t source_offset,
	              uint64_t destination_offset, uint64_t size,
	              vk::AccessFlags source_before      = vk::AccessFlagBits::eMemoryWrite,
	              vk::AccessFlags destination_before = vk::AccessFlagBits::eMemoryRead |
	                                                   vk::AccessFlagBits::eMemoryWrite,
	              vk::AccessFlags source_after       = vk::AccessFlagBits::eMemoryRead |
	                                                   vk::AccessFlagBits::eMemoryWrite,
	              vk::AccessFlags destination_after  = vk::AccessFlagBits::eMemoryRead |
	                                                   vk::AccessFlagBits::eMemoryWrite);
	// Uploads staged regions into this buffer; the whole buffer is then visible to all commands.
	void UploadRegions(CommandBuffer& command, const Buffer& source,
	                   std::span<const vk::BufferCopy> copies);
	// Copies regions of this buffer into a Download buffer for the host to read.
	void DownloadRegions(CommandBuffer& command, const Buffer& destination,
	                     std::span<const vk::BufferCopy> copies, uint64_t destination_offset,
	                     uint64_t destination_size);
	void Fill(uint64_t offset, uint64_t size, uint32_t value);
	// Makes earlier GPU writes to a Download buffer range visible to host reads.
	void ReadbackBarrier(CommandBuffer& command, uint64_t offset, uint64_t size) const;

	template <typename... Args>
	void SetName(fmt::format_string<Args...> format, Args&&... args) const;

	// BufferCache state lives directly on the resource.
	bool   is_deleted   = false;
	int    stream_score = 0;
	size_t lru_id       = 0;

protected:
	[[nodiscard]] GraphicContext&   Graphics() const noexcept { return *m_graphics; }
	[[nodiscard]] CommandScheduler& Scheduler() const noexcept { return *m_scheduler; }

private:
	[[nodiscard]] vk::BufferMemoryBarrier Barrier(uint64_t offset, uint64_t size,
	                                              vk::AccessFlags source,
	                                              vk::AccessFlags destination) const;
	[[nodiscard]] vk::Device DeviceForNaming() const noexcept;

	GraphicContext*               m_graphics    = nullptr;
	CommandScheduler*             m_scheduler   = nullptr;
	MemoryUsage                   m_usage       = MemoryUsage::DeviceLocal;
	uint64_t                      m_cpu_address = 0;
	vk::DeviceAddress             m_device_address = 0;
	vk::Buffer                    m_buffer     = nullptr;
	VmaAllocation                 m_allocation = nullptr;
	uint64_t                      m_size;
	bool                          m_coherent = false;
	std::span<uint8_t>            m_mapped;
};

template <typename... Args>
void Buffer::SetName(fmt::format_string<Args...> format, Args&&... args) const {
	SetVulkanObjectNameF(DeviceForNaming(), m_buffer, format, std::forward<Args>(args)...);
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFER_H_
