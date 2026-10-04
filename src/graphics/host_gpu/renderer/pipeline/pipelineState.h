#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTATE_H_

#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <xxhash.h>

// The pipeline state of a draw, derived from guest registers and shared by the host GPU backends.
// Each backend creates its graphics pipelines from a GraphicsPipelineKey.

namespace Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct RenderColorInfo;
struct RenderDepthInfo;
struct ShaderBufferResource;

namespace HW {
struct PolyOffset;
} // namespace HW

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	bool                       depth_bounds_test_enable = false;
	float                      depth_min_bounds         = 0.0f;
	float                      depth_max_bounds         = 0.0f;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
	bool                       cull_front                                         = false;
	bool                       cull_back                                          = false;
	bool                       face                                               = false;
	bool                       provoking_vtx_last                                 = false;
	vk::PolygonMode            polygon_mode                                       = vk::PolygonMode::eFill;
	uint8_t                    color_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	uint8_t                    alpha_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	bool                       separate_alpha_blend[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       blend_enable[RENDER_COLOR_ATTACHMENTS_MAX]         = {};
	bool                       blend_alpha_source_remap                           = false;

	bool operator==(const PipelineStaticParameters& other) const noexcept;
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PipelineStaticParameters>);
static_assert(std::is_standard_layout_v<PipelineStaticParameters>);
static_assert(alignof(PipelineStaticParameters) == 1);
static_assert(sizeof(PipelineStaticParameters) == 126);

struct PipelineRenderingState {
	std::array<vk::Format, RENDER_COLOR_ATTACHMENTS_MAX> color_formats {};
	vk::Format                                           depth_format   = vk::Format::eUndefined;
	vk::Format                                           stencil_format = vk::Format::eUndefined;
	uint32_t                                             color_count    = 0;

	bool operator==(const PipelineRenderingState&) const = default;
};

struct PipelineVertexInputState {
	struct Binding {
		uint32_t stride                           = 0;
		bool     instance                         = false;
		bool     operator==(const Binding&) const = default;
	};
	struct Attribute {
		uint32_t offset                             = 0;
		uint8_t  binding                            = 0;
		bool     operator==(const Attribute&) const = default;
	};

	std::array<Binding, ShaderVertexInputInfo::RES_MAX>   bindings {};
	std::array<Attribute, ShaderVertexInputInfo::RES_MAX> attributes {};
	uint8_t                                               binding_count   = 0;
	uint8_t                                               attribute_count = 0;

	bool operator==(const PipelineVertexInputState&) const = default;
};

struct GraphicsPipelineKey {
	PipelineRenderingState   rendering;
	std::array<uint64_t, 3>  vertex_shader_ids {};
	uint64_t                 ps_shader_id = 0;
	PipelineVertexInputState vertex_input;
	PipelineStaticParameters static_params;

	bool operator==(const GraphicsPipelineKey& other) const {
		return rendering == other.rendering && vertex_shader_ids == other.vertex_shader_ids &&
		       ps_shader_id == other.ps_shader_id && vertex_input == other.vertex_input &&
		       static_params == other.static_params;
	}
};

struct PipelineKeyHash {
	static void Mix(std::size_t& hash, std::size_t value) {
		hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
		        (hash >> 2u);
	}

	static void MixRendering(std::size_t& hash, const PipelineRenderingState& rendering) {
		Mix(hash, rendering.color_count);
		for (uint32_t i = 0; i < rendering.color_count; i++) {
			Mix(hash, static_cast<uint32_t>(rendering.color_formats[i]));
		}
		Mix(hash, static_cast<uint32_t>(rendering.depth_format));
		Mix(hash, static_cast<uint32_t>(rendering.stencil_format));
	}
};

struct GraphicsPipelineKeyHash {
	std::size_t operator()(const GraphicsPipelineKey& key) const {
		std::size_t hash = 0;
		PipelineKeyHash::MixRendering(hash, key.rendering);
		for (const auto id: key.vertex_shader_ids) {
			PipelineKeyHash::Mix(hash, id);
		}
		PipelineKeyHash::Mix(hash, key.ps_shader_id);
		PipelineKeyHash::Mix(hash, key.vertex_input.binding_count);
		for (uint32_t i = 0; i < key.vertex_input.binding_count; i++) {
			PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].stride);
			PipelineKeyHash::Mix(hash, key.vertex_input.bindings[i].instance);
		}
		PipelineKeyHash::Mix(hash, key.vertex_input.attribute_count);
		for (uint32_t i = 0; i < key.vertex_input.attribute_count; i++) {
			PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].offset);
			PipelineKeyHash::Mix(hash, key.vertex_input.attributes[i].binding);
		}
		PipelineKeyHash::Mix(hash, XXH3_64bits(&key.static_params, sizeof(key.static_params)));
		return hash;
	}
};

// The pipeline state of a draw: guest registers, its resolved targets and translated programs.
[[nodiscard]] GraphicsPipelineKey
MakeGraphicsPipelineKey(const GraphicContext& graphics, std::span<const RenderColorInfo> colors,
                        const RenderDepthInfo& depth, std::span<const ShaderVertexInputInfo> vertex_info,
                        const CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
                        vk::PrimitiveTopology topology, bool primitive_restart_enable,
                        const std::array<uint64_t, 3>& vertex_program_ids, uint64_t pixel_program_id);

// The host depth bias constant of a guest polygon offset on a `host_depth_format` target.
[[nodiscard]] float PolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format);

// The host format of a vertex attribute, narrowed to the components the shader reads.
// `size` returns its component count.
void GetInputFormat(const ShaderBufferResource& res, vk::Format& format, uint32_t& size,
                    uint32_t used_components);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTATE_H_
