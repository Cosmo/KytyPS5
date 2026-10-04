#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_TEXTURECACHE_H_

#include "common/common.h"

#include <cstdint>

namespace Libs::Graphics {

// The texture cache the buffer cache works with, until the D3D12 renderer has textures: it holds none, so no guest range is ever an image and nothing is
// modified by the GPU behind the buffer cache's back.
class TextureCache {
public:
	TextureCache() = default;
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] bool ClearMeta(uint64_t /*address*/) noexcept { return true; }
	[[nodiscard]] bool FindImageFromRange(uint64_t /*address*/, uint64_t /*size*/) const noexcept { return false; }
	[[nodiscard]] bool IsRegionGpuModified(uint64_t /*address*/, uint64_t /*size*/) const noexcept { return false; }
	void               InvalidateMemoryFromGPU(uint64_t /*vaddr*/, uint64_t /*size*/) noexcept {}
	void               InvalidateMemory(uint64_t /*vaddr*/, uint64_t /*size*/) noexcept {}
	void               UnmapMemory(uint64_t /*vaddr*/, uint64_t /*size*/) noexcept {}
	void               ProcessDownloadImages() noexcept {}
	void               RunGarbageCollector() noexcept {}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_TEXTURECACHE_H_
