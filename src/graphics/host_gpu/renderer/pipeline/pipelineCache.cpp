#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "common/threads.h"

#include "common/alignment.h"
#include "common/assert.h"
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
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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


std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC1:{}:{:08x}:{:08x}:{:08x}:{}\n", KYTY_GIT_REVISION,
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
	};
}

} // namespace

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_programs(ShaderLimits(graphics)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
}

PipelineCache::~PipelineCache() {
	Save();
	for (const auto& [id, module]: m_modules) {
		(void)id;
		m_graphics.device.destroyShaderModule(module, nullptr);
	}
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	m_driver_cache_file = PipelineCacheFile("Vulkan pipeline cache", ".bin");
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
		m_driver_cache_file.Log("loaded {} bytes", initial_data.size());
	} else {
		m_driver_cache_file.Log("initialized empty");
	}
}

void PipelineCache::Save() {
	if (m_driver_cache == nullptr) {
		return;
	}

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
		m_driver_cache_file.Log("save failed ({}, {} bytes)", vk::to_string(result), size);
		return;
	}
	payload.resize(size);
	if (!m_driver_cache_file.Save(DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties()),
	                              payload)) {
		return;
	}
	m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	m_driver_cache = nullptr;
}

ShaderProgram PipelineCache::Module(const CompiledProgram& program) {
	if (!program) {
		return {};
	}
	auto [module, inserted] = m_modules.try_emplace(program.id);
	if (inserted) {
		module->second = CompileSPV(*program.spirv, m_graphics.device);
		EXIT_IF(module->second == nullptr);
	}
	return {.id = program.id, .module = module->second};
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const auto programs =
	    m_programs.GetGraphicsPrograms(vertex_regs, pixel_regs, sh, context, user_config,
	                                   target_export_mapping, pixel_active, vertex_info, pixel_info);
	GraphicsPrograms result;
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		result.vertex[i] = Module(programs.vertex[i]);
	}
	result.pixel = Module(programs.pixel);
	return result;
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	return Module(m_programs.GetComputeProgram(regs, sh, input_info));
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const GraphicsPrograms& programs) {
	const auto& vs_input_info  = vertex_info.front();
	const auto& vertex_program = programs.vertex[0];
	const auto& pixel_program  = programs.pixel;
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);

	const auto        vs_id = vertex_program.id;
	const auto        ps_id = ps_active ? pixel_program.id : 0;
	std::array<uint64_t, 3> vertex_ids {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		vertex_ids[i] = programs.vertex[i].id;
	}
	auto key = MakeGraphicsPipelineKey(m_graphics, colors, depth, vertex_info, command, ps_input_info,
	                                   topology, primitive_restart_enable, vertex_ids, ps_id);
	const auto& rendering     = key.rendering;
	const auto& static_params = key.static_params;

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vertex_info,
	                       ps_input_info, programs, static_params, m_driver_cache);
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                  const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);


	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);

	return *iter->second;
}
} // namespace Libs::Graphics
