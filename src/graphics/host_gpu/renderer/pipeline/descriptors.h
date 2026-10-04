#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#if defined(KYTY_GPU_BACKEND_D3D12)
#include "graphics/host_gpu/d3d12/samplerCache.h"
#else
#include "graphics/host_gpu/renderer/cache/samplerCache.h"
#endif

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

// A host buffer range bound to a shader. A size of UINT64_MAX extends to the buffer end.
struct BufferBinding {
	BufferHandle buffer {};
	uint64_t     offset = 0;
	uint64_t     size   = 0;
};

struct TextureBinding {
	ImageId                      image_id;
	ImageViewHandle              image_view {};
	TextureCache::ImageDesc      desc;
#if !defined(KYTY_GPU_BACKEND_D3D12)
	vk::ImageLayout              layout = vk::ImageLayout::eUndefined;
#endif
	std::vector<ImageViewHandle> mip_views;
};

struct PreparedBindings {
	struct BufferSource {
		uint64_t address = 0;
		uint64_t size    = 0;
		BufferId id;
	};

	// The draw owns the immutable compiled-program/runtime-snapshot association through commit.
	const ShaderStageRuntime* runtime = nullptr;
	// Keep the resolved guest range through cache preparation; only the host buffer ID may
	// become stale and need resolving again when bindings are rebound.
	std::vector<BufferSource>   buffer_sources;
	std::vector<BufferBinding>  buffers;
	std::vector<TextureBinding> images;
	std::vector<SamplerHandle>  samplers;
	BufferBinding               gds {{}, 0, UINT64_MAX};
	BufferBinding               flattened_srt;
	BufferBinding               shader_data_buffer;
	std::vector<uint32_t>       shader_data;
};

[[nodiscard]] uint32_t
NativeDescriptorCount(const ShaderRecompiler::IR::DescriptorBinding& binding);

template <typename T>
[[nodiscard]] T DecodeNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	static_assert(sizeof(T) % sizeof(uint32_t) == 0);
	T result {};
	EXIT_IF(value.dword_count < sizeof(result) / sizeof(uint32_t));
	std::memcpy(&result, value.dwords.data(), sizeof(result));
	return result;
}

[[nodiscard]] bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor,
                                                   bool r128 = false);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
