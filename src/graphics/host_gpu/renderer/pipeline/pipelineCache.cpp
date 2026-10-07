#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "common/threads.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

constexpr auto DriverCacheSaveInterval = std::chrono::minutes {5};

std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	// Driver cache entries already distinguish shader code and pipeline state across builds.
	return fmt::format("KytyPC2:{:08x}:{:08x}:{:08x}:{}\n",
	                   properties.vendorID, properties.deviceID, properties.driverVersion, uuid);
}

HostShaderLimits ShaderLimits(const GraphicContext& graphics) {
	const auto& mesh     = graphics.mesh_shader_properties;
	const auto& viewport = graphics.GetPhysicalDeviceProperties().limits.maxViewportDimensions;
	return {
	    .compute_wave64          = graphics.SupportsComputeWave64(),
	    .subgroup_size           = graphics.subgroup_size,
	    .max_viewport_width      = viewport[0],
	    .max_viewport_height     = viewport[1],
	    .mesh_shader_enabled     = graphics.mesh_shader_enabled,
	    .max_mesh_invocations    = mesh.maxMeshWorkGroupInvocations,
	    .max_mesh_group_size_x   = mesh.maxMeshWorkGroupSize[0],
	    .max_mesh_vertices       = mesh.maxMeshOutputVertices,
	    .max_mesh_primitives     = mesh.maxMeshOutputPrimitives,
	    .max_mesh_shared_memory  = mesh.maxMeshSharedMemorySize,
	    .max_compute_shared_memory = graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize,
	    .maintenance8            = graphics.maintenance8_enabled,
	    // AMD GPUs (vendor 0x1002) run NMin/NMax as v_min/v_max, which give the guest's NaN and
	    // signed-zero results; GLSL.std.450 alone does not order -0 below +0.
	    .native_float_min_max    = graphics.GetPhysicalDeviceProperties().vendorID == 0x1002u,
	    .device_clock_period_ps  = graphics.device_clock_period_ps,
	};
}

} // namespace

PipelineCache::PipelineCache(GraphicContext&                         graphics,
                             ShaderRecompiler::IR::ImageHeapSource* image_heap)
    : m_graphics(graphics), m_programs(ShaderLimits(graphics), image_heap),
      m_next_driver_cache_save(std::chrono::steady_clock::now() + DriverCacheSaveInterval) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
}

PipelineCache::~PipelineCache() {
	Save();
	for (const auto& [id, program]: m_shader_programs) {
		(void)id;
		m_graphics.device.destroyShaderModule(program.module, nullptr);
	}
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline.pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline.descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	// Driver cache entries already distinguish shader code and pipeline state across builds.
	m_driver_cache_file = PipelineCacheFile("Vulkan pipeline cache", ".bin", PipelineCacheFile::Builds::Any);
	if (!m_driver_cache_file.Enabled()) {
		return;
	}
	auto initial_data =
	    m_driver_cache_file.Load(DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties()));

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		m_driver_cache_file.Log("driver rejected the cached data ({}); starting empty",
		                        vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		m_driver_cache_file.Log("disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		m_driver_cache_file.Log("loaded {:.2f} MB", static_cast<double>(initial_data.size()) / 1'000'000.0);
	} else {
		m_driver_cache_file.Log("initialized empty");
	}
}

void PipelineCache::CheckpointDriverCache() {
	if (m_driver_cache == nullptr) {
		return;
	}
	m_driver_cache_dirty = true;
	if (std::chrono::steady_clock::now() >= m_next_driver_cache_save) {
		Save();
	}
}

void PipelineCache::Save() {
	if (m_driver_cache == nullptr || !m_driver_cache_dirty) {
		return;
	}
	m_next_driver_cache_save = std::chrono::steady_clock::now() + DriverCacheSaveInterval;

	size_t               size = 0;
	vk::Result           result;
	std::vector<uint8_t> payload;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		m_driver_cache_file.Log("save failed ({}, {:.2f} MB)", vk::to_string(result), static_cast<double>(size) / 1'000'000.0);
		return;
	}
	payload.resize(size);
	if (!m_driver_cache_file.Save(DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties()),
	                              payload)) {
		return;
	}
	m_driver_cache_dirty = false;
}

const ShaderProgram* PipelineCache::Program(const CompiledProgram& program) {
	if (!program) {
		return nullptr;
	}
	auto [found, inserted] = m_shader_programs.try_emplace(program.id);
	if (inserted) {
		found->second = {.id = program.id, .module = CompileSPV(*program.spirv, m_graphics.device)};
		EXIT_IF(found->second.module == nullptr);
	}
	return &found->second;
}

void PipelineCache::GetGraphicsPrograms(const CommandBuffer& command, DrawRenderState& state) {
	const auto programs = m_programs.GetGraphicsPrograms(
	    command.GetShaders(), command.GetRegisters(), command.GetUserConfig(), state.vertex_info,
	    state.ps_input_info, state.ps_active, state.mrt_mask);
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		state.vertex_info[i].stage.handle = Program(programs.vertex[i]);
	}
	state.ps_input_info.stage.handle = Program(programs.pixel);
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               ShaderComputeInputInfo&      input_info) {
	const auto* program     = Program(m_programs.GetComputeProgram(regs, input_info));
	input_info.stage.handle = program;
	return program != nullptr ? *program : ShaderProgram {};
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(const CommandBuffer&   command,
                                                            const DrawRenderState& state) {
	const auto  vertex_info    = state.VertexStages();
	const auto* ps_input_info  = state.ps_active ? &state.ps_input_info : nullptr;
	const auto& vs_input_info  = vertex_info.front();
	const auto* vertex_program = vs_input_info.stage.handle;
	const auto* pixel_program  = state.ps_input_info.stage.handle;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(vertex_program == nullptr);
	const bool ps_active = state.ps_active;
	EXIT_IF(ps_active && pixel_program == nullptr);

	const auto        vs_id = vertex_program->id;
	const auto        ps_id = ps_active ? pixel_program->id : 0;
	std::array<uint64_t, 3> vertex_ids {};
	for (uint32_t i = 0; i < vertex_info.size(); i++) {
		vertex_ids[i] = vertex_info[i].stage.handle->id;
	}
	auto key = MakeGraphicsPipelineKey(m_graphics, std::span {state.color_info, state.color_count},
	                                   state.depth_info, vertex_info, command, ps_input_info,
	                                   state.topology, state.primitive_restart_enable, vertex_ids,
	                                   ps_id);
	const auto& rendering     = key.rendering;
	const auto& static_params = key.static_params;

	auto [iter, inserted] = m_graphics_pipelines.try_emplace(key);
	auto& pipeline        = iter->second;
	if (!inserted) {
		return pipeline;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program->module), ps_id,
		     ps_active ? static_cast<void*>(pixel_program->module) : nullptr);
	}

	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	{
		Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::PipelinesCreated,
		                                         Common::DebugCounters::Counter::PipelineCreateNs,
		                                         Common::DebugCounters::Peak::PipelineCreateNs);
		CreatePipelineInternal(m_graphics, pipeline, rendering, key.vertex_input, state,
		                       static_params, m_driver_cache);
	}
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);
	CheckpointDriverCache();

	return pipeline;
}

PipelineCache::Pipeline& PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(input_info.stage.handle == nullptr);
	const auto& compute_program = *input_info.stage.handle;

	auto [iter, inserted] = m_compute_pipelines.try_emplace(compute_program.id);
	auto& pipeline        = iter->second;
	if (!inserted) {
		return pipeline;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	{
		Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::PipelinesCreated,
		                                         Common::DebugCounters::Counter::PipelineCreateNs,
		                                         Common::DebugCounters::Peak::PipelineCreateNs);
		CreatePipelineInternal(m_graphics, pipeline, input_info, compute_program.module,
		                       m_driver_cache);
	}

	EXIT_NOT_IMPLEMENTED(pipeline.pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(pipeline.pipeline_layout == nullptr);
	CheckpointDriverCache();

	return pipeline;
}
} // namespace Libs::Graphics
