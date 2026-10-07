#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DRAWLOG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DRAWLOG_H_

#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <string_view>

// Debugging (the frame dump's draw log, see frameDump.h): the text of what a draw or dispatch reads and writes.
namespace Libs::Graphics::DrawLog {

[[nodiscard]] inline const char* BindingKind(TextureCache::BindingType type) {
	switch (type) {
		case TextureCache::BindingType::Texture: return "sampled";
		case TextureCache::BindingType::Storage: return "storage";
		case TextureCache::BindingType::RenderTarget: return "render-target";
		case TextureCache::BindingType::DepthTarget: return "depth-target";
		case TextureCache::BindingType::VideoOut: return "video-out";
	}
	return "?";
}

// A render target or depth target: guest address, size and format, the mip and layer it renders to.
[[nodiscard]] inline std::string Target(const TextureCache::ImageDesc& desc, uint32_t mip, uint32_t layer) {
	const auto& info = desc.info;
	return fmt::format("0x{:x} {}x{} {} samples {} mip {} layer {}", info.data.address, info.extent.width, info.extent.height, vk::to_string(info.pixel_format), info.samples, mip, layer);
}

// A bound texture with the shader's use of it (`resource` may be null).
[[nodiscard]] inline std::string Texture(const TextureBinding& binding, const ShaderRecompiler::IR::ImageResource* resource) {
	const auto& info = binding.desc.info;
	const auto& view = binding.desc.view_info;
	auto        text = fmt::format("0x{:x} {}x{}x{} {} levels {} layers {} | {} view {} format {} mips {}+{} layers {}+{}", info.data.address, info.extent.width, info.extent.height, info.extent.depth,
	                               vk::to_string(info.pixel_format), info.resources.levels, info.resources.layers, BindingKind(binding.desc.type), vk::to_string(view.type), vk::to_string(view.format),
	                               view.base_level, view.level_count, view.base_layer, view.layer_count);
	if (info.host_skip != 0) {
		text += fmt::format(" (host image without {} top mips)", info.host_skip);
	}
	if (resource != nullptr) {
		text += fmt::format(" | shader dim {}{}{}{}", static_cast<uint32_t>(resource->dimension), resource->cube ? " cube" : "", resource->depth_compare ? " compare" : "", resource->written ? " written" : "");
	}
	return text;
}

// The images of one stage's bindings, one line each.
[[nodiscard]] inline std::string Textures(std::string_view stage, const PreparedBindings& bindings) {
	std::string text;
	const auto* program = bindings.runtime != nullptr ? bindings.runtime->program : nullptr;
	for (size_t i = 0; i < bindings.images.size(); i++) {
		const ShaderRecompiler::IR::ImageResource* resource = program != nullptr && i < program->info.images.size() ? &program->info.images[i] : nullptr;
		text += fmt::format("    {} image {}: {}\n", stage, i, Texture(bindings.images[i], resource));
	}
	return text;
}

// The buffers a stage's program writes (their guest ranges), one line each.
[[nodiscard]] inline std::string WrittenBuffers(std::string_view stage, const PreparedBindings& bindings) {
	std::string text;
	const auto* program = bindings.runtime != nullptr ? bindings.runtime->program : nullptr;
	if (program == nullptr || program->info.buffers.size() != bindings.buffer_sources.size()) {
		return text;
	}
	for (size_t i = 0; i < bindings.buffer_sources.size(); i++) {
		if (program->info.buffers[i].written) {
			text += fmt::format("    {} buffer {} (written): 0x{:x} size 0x{:x}\n", stage, i, bindings.buffer_sources[i].address, bindings.buffer_sources[i].size);
		}
	}
	return text;
}

} // namespace Libs::Graphics::DrawLog

#endif
