#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineState.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/programCache.h"
#include "graphics/shader/shader.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace Libs::Graphics {

struct GraphicContext;
struct RenderColorInfo;
struct RenderDepthInfo;
class CommandBuffer;

namespace HW {
class Context;
class Shader;
class UserConfig;
struct ComputeShaderInfo;
} // namespace HW

struct ShaderProgram {
	uint64_t         id     = 0;
	vk::ShaderModule module = nullptr;

	explicit operator bool() const { return id != 0 && module != nullptr; }
};

class PipelineCache {
public:
	explicit PipelineCache(GraphicContext& graphics);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	void Save();

	struct Pipeline {
		vk::PipelineLayout      pipeline_layout       = nullptr;
		vk::Pipeline            pipeline              = nullptr;
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		bool                    uses_push_descriptors = false;
	};

	struct GraphicsPrograms {
		std::array<ShaderProgram, 3> vertex;
		ShaderProgram pixel;

		[[nodiscard]] uint32_t VertexStageCount() const { return vertex[1] ? 3u : 1u; }
	};

	GraphicsPrograms
	GetGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                    const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                    const HW::Context& context, const HW::UserConfig& user_config,
	                    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                    ShaderPixelInputInfo& pixel_info);
	ShaderProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                const HW::ShaderRegisters&   sh,
	                                ShaderComputeInputInfo&      input_info);

	Pipeline& GetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              const GraphicsPrograms& programs);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info,
	                             const ShaderProgram&          compute_program);

private:
	[[nodiscard]] ShaderProgram Module(const CompiledProgram& program);

	GraphicContext&                                  m_graphics;
	ShaderProgramCache                               m_programs;
	std::unordered_map<uint64_t, vk::ShaderModule>   m_modules;
	vk::PipelineCache                                m_driver_cache = nullptr;
	PipelineCacheFile             m_driver_cache_file;
	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<Pipeline>, GraphicsPipelineKeyHash>
	                                                        m_graphics_pipelines;
	std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> m_compute_pipelines;

	void InitializeDriverCache();
};

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache                      driver_cache);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
