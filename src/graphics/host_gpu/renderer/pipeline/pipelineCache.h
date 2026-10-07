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

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace Libs::Graphics {

struct GraphicContext;
struct DrawRenderState;
class CommandBuffer;

namespace ShaderRecompiler::IR {
class ImageHeapSource;
} // namespace ShaderRecompiler::IR

struct ShaderProgram {
	uint64_t         id     = 0;
	vk::ShaderModule module = nullptr;

	explicit operator bool() const { return id != 0 && module != nullptr; }
};

class PipelineCache {
public:
	PipelineCache(GraphicContext& graphics, ShaderRecompiler::IR::ImageHeapSource* image_heap);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	void Save();

	struct Pipeline {
		vk::PipelineLayout      pipeline_layout       = nullptr;
		vk::Pipeline            pipeline              = nullptr;
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		bool                    uses_push_descriptors = false;
		bool                    uses_image_heap       = false;
	};

	// Reads the shaders and registers bound in `command` into the state's shader stages.
	void GetGraphicsPrograms(const CommandBuffer& command, DrawRenderState& state);
	// An empty program skips the dispatch.
	ShaderProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                ShaderComputeInputInfo&      input_info);

	Pipeline& GetGraphicsPipeline(const CommandBuffer& command, const DrawRenderState& state);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info);

private:
	// The program's shader module, made once per program (nullptr: no program).
	[[nodiscard]] const ShaderProgram* Program(const CompiledProgram& program);

	GraphicContext&                                  m_graphics;
	ShaderProgramCache                               m_programs;
	std::unordered_map<uint64_t, ShaderProgram>      m_shader_programs;
	vk::PipelineCache                                m_driver_cache = nullptr;
	PipelineCacheFile             m_driver_cache_file;
	std::chrono::steady_clock::time_point m_next_driver_cache_save;
	bool                          m_driver_cache_dirty = false;
	std::unordered_map<GraphicsPipelineKey, Pipeline, GraphicsPipelineKeyHash> m_graphics_pipelines;
	std::unordered_map<uint64_t, Pipeline>                                     m_compute_pipelines;

	void InitializeDriverCache();
	void CheckpointDriverCache();
};

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&   rendering,
                            const PipelineVertexInputState& vertex_input,
                            const DrawRenderState&          state,
                            const PipelineStaticParameters& static_params,
                            vk::PipelineCache               driver_cache);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
