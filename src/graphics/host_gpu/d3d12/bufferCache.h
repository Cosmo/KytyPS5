#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BUFFERCACHE_H_

#include "common/common.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

// Host storage for guest data that has no guest address, such as GDS.
class Buffer {
public:
	explicit Buffer(uint64_t size): m_data(size) {}
	KYTY_CLASS_NO_COPY(Buffer);

	[[nodiscard]] uint64_t           Size() const noexcept { return m_data.size(); }
	[[nodiscard]] std::span<uint8_t> Mapped() const noexcept { return m_data; }

private:
	mutable std::vector<uint8_t> m_data;
};

// Guest memory as the GPU sees it. This backend keeps no GPU copies of guest memory yet, so guest memory is always current and DMA works on it directly.
class BufferCache {
public:
	// The page size of the guest-address page table that shaders use to reach guest memory (see the shader emitter).
	static constexpr uint32_t CACHING_PAGEBITS = 14;
	static constexpr uint64_t CACHING_PAGESIZE = uint64_t {1} << CACHING_PAGEBITS;

	BufferCache() = default;
	KYTY_CLASS_NO_COPY(BufferCache);

	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }

	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds, bool src_gds);

	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t /*vaddr*/, uint64_t /*size*/) const noexcept { return false; }
	void               InvalidateMemory(uint64_t /*vaddr*/, uint64_t /*size*/) noexcept {}

private:
	static constexpr uint64_t GdsBufferSize = 64 * 1024;

	Buffer m_gds_buffer {GdsBufferSize};
};

class TextureCache {
public:
	TextureCache() = default;
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] bool IsRegionGpuModified(uint64_t /*address*/, uint64_t /*size*/) const noexcept { return false; }
	void               InvalidateMemory(uint64_t /*vaddr*/, uint64_t /*size*/) noexcept {}
	void               UnmapMemory(uint64_t /*vaddr*/, uint64_t /*size*/) noexcept {}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BUFFERCACHE_H_
