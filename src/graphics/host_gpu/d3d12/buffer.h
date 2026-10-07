#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BUFFER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BUFFER_H_

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/memoryUsage.h"
#include "graphics/host_gpu/vulkanCommon.h" // vk::BufferCopy as a plain region description

#include <cstdint>
#include <fmt/format.h>
#include <span>
#include <string>
#include <utility>

struct ID3D12Resource;

namespace D3D12MA {
class Allocation;
} // namespace D3D12MA

namespace Libs::Graphics {

class Buffer;
class CommandBuffer;
class CommandScheduler;
struct GraphicContext;

// Identifies a buffer to the other renderer objects (images, tile manager), which track its
// state through it.
using BufferHandle = const Buffer*;

// A D3D12 buffer resource. DeviceLocal buffers live in GPU memory; the CPU-visible usages live in
// custom heaps that also allow unordered access, so shaders can bind them directly.
//
// Buffers decay to the COMMON state after every ExecuteCommandLists and are implicitly promoted
// from COMMON on first use, so state is tracked per command list (by scheduler tick).
class Buffer {
public:
	Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
	       uint64_t cpu_address, uint64_t size);
	// Shared code names the Vulkan usage of a transfer buffer; a D3D12 buffer has no such flags.
	Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage, uint64_t cpu_address, vk::BufferUsageFlags /*flags*/, uint64_t size)
	    : Buffer(graphics, scheduler, usage, cpu_address, size) {}
	~Buffer();
	KYTY_CLASS_NO_COPY(Buffer);

	[[nodiscard]] BufferHandle       Handle() const noexcept { return this; }
	[[nodiscard]] ID3D12Resource*    Resource() const noexcept { return m_resource; }
	[[nodiscard]] uint64_t           GpuAddress() const noexcept { return m_gpu_address; }
	[[nodiscard]] uint64_t           Size() const noexcept { return m_size; }
	[[nodiscard]] std::span<uint8_t> Mapped() const noexcept { return m_mapped; }
	[[nodiscard]] bool               IsCoherent() const noexcept { return true; }
	[[nodiscard]] MemoryUsage        Usage() const noexcept { return m_usage; }
	[[nodiscard]] uint64_t           CpuAddress() const noexcept { return m_cpu_address; }
	[[nodiscard]] uint64_t           Offset(uint64_t address) const noexcept {
		return address - m_cpu_address;
	}
	[[nodiscard]] bool     IsInBounds(uint64_t address, uint64_t size) const noexcept;
	[[nodiscard]] uint64_t FlushAtomSize() const noexcept { return 1; }
	void                   IncreaseStreamScore(int score) noexcept { stream_score += score; }
	[[nodiscard]] int      StreamScore() const noexcept { return stream_score; }
	// CPU-visible D3D12 heaps are coherent.
	void Flush(uint64_t /*offset*/, uint64_t /*size*/) {}
	void Invalidate(uint64_t /*offset*/, uint64_t /*size*/) {}

	void CopyFrom(CommandBuffer& command, const Buffer& source, uint64_t source_offset,
	              uint64_t destination_offset, uint64_t size);
	void UploadRegions(CommandBuffer& command, const Buffer& source,
	                   std::span<const vk::BufferCopy> copies);
	void DownloadRegions(CommandBuffer& command, const Buffer& destination,
	                     std::span<const vk::BufferCopy> copies, uint64_t destination_offset,
	                     uint64_t destination_size);
	void Fill(uint64_t offset, uint64_t size, uint32_t value) const;
	// Readback memory is coherent; the fence wait before reading suffices.
	void ReadbackBarrier(CommandBuffer& /*command*/, uint64_t /*offset*/, uint64_t /*size*/) const {}

	// Records a transition to `state` (a D3D12_RESOURCE_STATES value) in the current command list.
	void Use(CommandBuffer& command, uint32_t state) const;

	// The persistent descriptor heap slot of a raw view of the whole buffer, created on first
	// use, through which shaders read it by address (see BdaPageTable).
	[[nodiscard]] uint32_t BindlessIndex() const;

	template <typename... Args>
	void SetName(fmt::format_string<Args...> format, Args&&... args) const {
		if (Config::GraphicsDebugDumpEnabled()) {
			SetNameString(fmt::format(format, std::forward<Args>(args)...));
		}
	}

	// BufferCache state lives directly on the resource.
	bool   is_deleted   = false;
	int    stream_score = 0;
	size_t lru_id       = 0;

protected:
	[[nodiscard]] GraphicContext&   Graphics() const noexcept { return *m_graphics; }
	[[nodiscard]] CommandScheduler& Scheduler() const noexcept { return *m_scheduler; }

private:
	void SetNameString(const std::string& name) const;

	GraphicContext*    m_graphics    = nullptr;
	CommandScheduler*  m_scheduler   = nullptr;
	MemoryUsage        m_usage       = MemoryUsage::DeviceLocal;
	uint64_t           m_cpu_address = 0;
	uint64_t           m_size        = 0;
	D3D12MA::Allocation* m_allocation = nullptr;
	ID3D12Resource*    m_resource    = nullptr; // owned by m_allocation
	uint64_t           m_gpu_address = 0;
	std::span<uint8_t> m_mapped;
	mutable uint32_t   m_state      = 0;
	mutable uint64_t   m_state_tick = 0;
	mutable uint32_t   m_bindless_index = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BUFFER_H_
