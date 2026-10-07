#include "graphics/shader/programCache.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/drawState.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/pipeline/blendMapping.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/MeshEmulation.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <fmt/format.h>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics {

namespace {

void Mix(std::size_t& hash, std::size_t value) {
	hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
}

bool ReadShaderGuestMemory(void*, uint64_t address, std::span<uint32_t> values) {
	// Scalar and unformatted buffer dependencies use the same backing as native raw loads.
	// Image synchronization belongs to formatted buffer bindings, not these reads.
	return !values.empty() &&
	       Libs::LibKernel::Memory::TryReadBufferBacking(address, values.data(), values.size_bytes());
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}.bin", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(code.data(), code.size_bytes());
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv, bool maintenance8_enabled) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	spvtools::ValidatorOptions validator_options;
	validator_options.SetAllowOffsetTextureOperand(maintenance8_enabled);
	if (tools.Validate(spirv.data(), spirv.size(), validator_options)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

} // namespace

struct ShaderProgramCache::Impl {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		std::vector<uint32_t>                       function_code;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		CompiledProgram                              handle;
	};

	struct SourceEntry {
		SourceEntry(ShaderRecompiler::IR::ResourcePlan plan,
		            std::unique_ptr<ShaderRecompiler::ShaderSource> call_source)
		    : call_source(std::move(call_source)), resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		std::span<const uint32_t> FunctionCode() const {
			return call_source
			    ? std::span<const uint32_t>(call_source->linked->code).subspan(call_source->code.size())
			    : std::span<const uint32_t>{};
		}

		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime) {
			const auto revision = call_source->revision;
			ShaderRecompiler::RefreshShaderSource(*call_source, runtime);
			resource_plan.source_reads = call_source->reads;
			return revision != call_source->revision;
		}

		template <bool HasCalls>
		Permutation* FindPermutation(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                             uint32_t push_data_cursor) {
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    resource_plan, runtime, resources, specialization));
			for (auto& candidate: permutations) {
				const auto& layout = candidate.program.bindings;
				if (layout.push_data_start_dword != ShaderRecompiler::IR::PushData::StartFor(
				        push_data_cursor, layout.ShaderDataDwords()) ||
				    candidate.specialization != specialization) {
					continue;
				}
				if constexpr (HasCalls) {
					if (!std::ranges::equal(candidate.function_code, FunctionCode())) continue;
				}
				return &candidate;
			}
			return nullptr;
		}

		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		std::vector<Permutation>                    permutations;
		bool                                       skip_dispatch = false;
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			Mix(hash, key.user_data_count);
			Mix(hash, key.code_size);
			Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing the full state first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 32 + ShaderVertexInputInfo::RES_MAX * 6;

	Permutation CompilePermutation(const char*                                  stage_name,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword,
	                               std::span<const uint32_t> function_code) {
		Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::ShadersCompiled,
		                                         Common::DebugCounters::Counter::ShaderCompileNs,
		                                         Common::DebugCounters::Peak::ShaderCompileNs);
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv,
		                         options.profile->maintenance8)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

		if (options.dump_ir) {
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		// A mesh program that runs as a compute shader gets the vertex stage that draws its output.
		std::shared_ptr<const std::vector<uint32_t>> mesh_fetch;
		if (options.stage == ShaderType::Mesh && options.input_info.vertex != nullptr &&
		    options.input_info.vertex->mesh.compute_emulation) {
			auto fetch = ShaderRecompiler::Spirv::EmitMeshFetchProgram(
			    result.program.info.outputs, options.input_info.vertex->user_clip_planes);
			const bool valid = ValidateShaderSpirv("ShaderRecompiler MS fetch", options.shader_hash, fetch,
			                                       options.profile->maintenance8);
			DumpShaderSpirv("msfetch", options.shader_hash, fetch);
			if (!valid) {
				EXIT("ShaderRecompiler MS fetch failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n",
				     options.shader_hash);
			}
			mesh_fetch = std::make_shared<const std::vector<uint32_t>>(std::move(fetch));
		}
		return {
		    .function_code  = {function_code.begin(), function_code.end()},
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id         = ++next_shader_id,
		                       .spirv      = std::make_shared<const std::vector<uint32_t>>(
		                           std::move(result.spirv)),
		                       .mesh_fetch = std::move(mesh_fetch)},
		};
	}

	template <typename InputInfo>
	CompiledProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		const auto user_data = std::span(params.user_data).first(params.user_data_count);
		ShaderRecompiler::IR::SrtRuntime runtime {
		    .user_data                  = user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .image_heap                 = image_heap,
		};
		ShaderRecompiler::CompileOptions options {.wave_size = input_info.wave_size};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.stage            = input_info.logical_stage;
			options.input_info.vertex = &input_info;
			options.user_data_base   = 8;
			if (options.stage == ShaderType::Mesh ||
			    options.stage == ShaderType::TessellationControl) {
				options.user_data_base = 0;
				options.wave_size =
				    options.stage == ShaderType::Mesh ? input_info.mesh.wave_size : 64u;
			}
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			options.stage            = ShaderType::Pixel;
			options.input_info.pixel = &input_info;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			options.stage              = ShaderType::Compute;
			options.input_info.compute = &input_info;
			runtime.workgroup_counts   = input_info.workgroup_counts;
			runtime.indirect_args_address = input_info.indirect_args_address;
		}
		lookup_key.stage           = options.stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = params.user_data_count;
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto entry = programs.find(lookup_key);
		const auto bind = [&](const Permutation& permutation) {
			input_info.stage = {.program = &permutation.program, .resources = &entry->second.resources};
			permutation.program.bindings.AdvancePushData(push_data_cursor);
			return permutation.handle;
		};
		bool source_changed = false;
		if (entry != programs.end()) {
			auto& cached = entry->second;
			if (cached.skip_dispatch) return {};
			Permutation* permutation = nullptr;
			if (cached.call_source) {
				source_changed = cached.RefreshCallSource(runtime);
				if (!source_changed) permutation = cached.FindPermutation<true>(runtime, push_data_cursor);
			} else {
				permutation = cached.FindPermutation<false>(runtime, push_data_cursor);
			}
			if (permutation != nullptr) return bind(*permutation);
		}

		const char* label = nullptr;
		const char* stage_name = nullptr;
		switch (options.stage) {
			case ShaderType::Vertex: label = "ShaderRecompiler VS"; stage_name = "vs"; break;
			case ShaderType::Mesh: label = "ShaderRecompiler MS"; stage_name = "ms"; break;
			case ShaderType::Local: label = "ShaderRecompiler LS"; stage_name = "ls"; break;
			case ShaderType::TessellationControl: label = "ShaderRecompiler HS"; stage_name = "hs"; break;
			case ShaderType::TessellationEvaluation: label = "ShaderRecompiler DS"; stage_name = "ds"; break;
			case ShaderType::Pixel: label = "ShaderRecompiler PS"; stage_name = "ps"; break;
			case ShaderType::Compute: label = "ShaderRecompiler CS"; stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		options.profile     = &profile;
		options.shader_hash = params.hash;
		options.user_data   = user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::LogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.emit_debug_info = Config::GraphicsDebugDumpEnabled();
		options.dump_label  = label;
		DumpShaderOriginal(stage_name, options.shader_hash, params.code);
		ShaderRecompiler::ShaderSource source;
		if (entry == programs.end()) source = ShaderRecompiler::PrepareShaderSource(params.code, options);
		if (source.unsupported_call_pc != UINT32_MAX) {
			Log::WriteToConsoleAndLog(fmt::format("!!! TEMPORARY COMPUTE DISPATCH SKIP !!! shader=0x{:016x} pc=0x{:08x} "
			                                      "reason=unsupported GPU-selected callable target; rendering is incomplete\n",
			                                      params.hash, source.unsupported_call_pc));
			programs.try_emplace(lookup_key, ShaderRecompiler::IR::ResourcePlan{}, nullptr)
			    .first->second.skip_dispatch = true;
			return {};
		}
		auto* current = entry == programs.end() ? &source : entry->second.call_source.get();
		ShaderRecompiler::TranslateResult translated;
		if (current != nullptr) {
			if (!current->calls.empty()) {
				if (entry == programs.end()) ShaderRecompiler::RefreshShaderSource(*current, runtime);
				translated = ShaderRecompiler::TranslateProgram(current->linked->decoded, options);
				translated.program.source_reads = current->reads;
			} else {
				translated = ShaderRecompiler::TranslateProgram(current->decoded, options);
			}
		} else {
			translated = ShaderRecompiler::TranslateProgram(params.code, options);
		}
		if (entry == programs.end()) {
			auto retained = !source.calls.empty() ? std::make_unique<ShaderRecompiler::ShaderSource>(std::move(source)) : nullptr;
			entry = programs.try_emplace(lookup_key,
			    ShaderRecompiler::IR::ExtractResourcePlan(translated.program), std::move(retained)).first;
			EXIT_IF(!ShaderRecompiler::IR::MaterializeResources(
			    entry->second.resource_plan, runtime, entry->second.resources,
			    entry->second.specialization));
		} else if (source_changed) {
			entry->second.resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			if (const auto* permutation = entry->second.FindPermutation<true>(runtime, push_data_cursor)) {
				return bind(*permutation);
			}
		}
		entry->second.permutations.push_back(CompilePermutation(
		    stage_name, options, std::move(translated), entry->second.specialization, push_data_cursor,
		    entry->second.FunctionCode()));

		const auto counts = Counts();
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu | LS %zu | HS %zu | TES %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)],
		            counts[static_cast<size_t>(ShaderType::Local)],
		            counts[static_cast<size_t>(ShaderType::TessellationControl)],
		            counts[static_cast<size_t>(ShaderType::TessellationEvaluation)]);
		return bind(entry->second.permutations.back());
	}

	[[nodiscard]] ProgramCounts Counts() const {
		ProgramCounts counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		return counts;
	}

	Impl(const HostShaderLimits& limits, ShaderRecompiler::IR::ImageHeapSource* image_heap)
	    : image_heap(image_heap),
	      profile {.maintenance8           = limits.maintenance8,
	               .native_float_min_max   = limits.native_float_min_max,
	               .device_clock_period_ps = limits.device_clock_period_ps} {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	ProgramKey                                                  lookup_key;
	uint64_t                                                    next_shader_id = 0;
	ShaderRecompiler::IR::ImageHeapSource*                      image_heap;
	const ShaderRecompiler::Spirv::Profile                      profile;
};

ShaderProgramCache::ShaderProgramCache(const HostShaderLimits&                limits,
                                       ShaderRecompiler::IR::ImageHeapSource* image_heap)
    : m_limits(limits), m_impl(std::make_unique<Impl>(limits, image_heap)) {}

ShaderProgramCache::~ShaderProgramCache() = default;

ShaderProgramCache::ProgramCounts ShaderProgramCache::GetProgramCounts() const {
	return m_impl->Counts();
}

ShaderProgramCache::GraphicsPrograms ShaderProgramCache::GetGraphicsPrograms(
    const HW::Shader& shaders, const HW::Context& context, const HW::UserConfig& user_config,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info,
    bool& pixel_active, uint32_t& mrt_mask) {
	const auto& sh                  = context.GetShaderRegisters();
	const auto& vertex_regs         = shaders.GetVs();
	const auto& pixel_regs          = shaders.GetPs();
	const bool  object_primitive_id = context.HasObjectPrimitiveIdOutput();
	const auto  color_output_mask   = DrawColorOutputMask(context);
	// Without color outputs, a pixel shader still runs for its depth or coverage side effects.
	pixel_active = DrawHasActivePixelShader(context, shaders);
	const bool tess_active = user_config.GetPrimType() == Prospero::PrimitiveType::kPatch;
	std::array<ShaderParams, 3> vertex_params;
	if (tess_active) {
		vertex_params = PrepareTessellationPrograms(vertex_regs, context, vertex_info);
	} else {
		vertex_params[0] = PrepareProgram(vertex_regs, context, user_config, vertex_info[0]);
	}
	const bool mesh_active = vertex_info[0].logical_stage == ShaderType::Mesh;
	if (object_primitive_id) {
		// CxObjectIdMode: register source plus a 32-bit pipelined primitive-ID addition.
		EXIT_NOT_IMPLEMENTED(sh.m_paClObjprimIdCntl != 2u || mesh_active || tess_active ||
		                     !pixel_active || (sh.m_vgtDrawPayloadCntl & 8u) == 0 ||
		                     user_config.GetPrimType() != Prospero::PrimitiveType::kTriList);
		const auto& target = context.GetRenderTarget(7);
		EXIT_NOT_IMPLEMENTED(Prospero::ResolveRenderTargetFormat(target.info.format,
		                                                       target.info.channel_type)
		                         .buffer_format != Prospero::BufferFormat::k32UInt);
	}
	if (mesh_active) {
		auto& mesh              = vertex_info[0].mesh;
		mesh.compute_emulation  = !m_limits.mesh_shader_enabled;
		mesh.host_subgroup_size = m_limits.subgroup_size;
		static std::atomic_bool reported = false;
		const auto              report_once = [&](const char* message) {
			if (!reported.exchange(true, std::memory_order_relaxed)) {
				std::printf("Shaders: %s\n", message);
			}
		};
		if (mesh.compute_emulation) {
			// A guest merged geometry stage needs host mesh shaders or the compute emulation (D3D12); a Vulkan device
			// without mesh shaders stops here, as upstream.
			EXIT_NOT_IMPLEMENTED(!m_limits.mesh_compute_emulation);
			// The emulation keeps the mesh program's group shared memory, the counts of its group
			// and one layer value per vertex in the compute shader's group shared memory.
			const auto shared_bytes = mesh.lds_size_dwords * sizeof(uint32_t) + 2 * sizeof(uint32_t) +
			                          mesh.max_vertices * sizeof(uint32_t);
			if (shared_bytes > m_limits.max_compute_shared_memory) {
				report_once("a merged geometry stage needs more group shared memory than the host compute shaders have; its draws are skipped");
				return {};
			}
		} else {
			const auto logical_threads =
			    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
			const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
			                          std::min(mesh.host_subgroup_size, mesh.wave_size);
			if (host_threads > m_limits.max_mesh_invocations ||
			    host_threads > m_limits.max_mesh_group_size_x ||
			    mesh.max_vertices > m_limits.max_mesh_vertices ||
			    mesh.max_primitives > m_limits.max_mesh_primitives ||
			    mesh.lds_size_dwords * sizeof(uint32_t) > m_limits.max_mesh_shared_memory) {
				EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
				     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
			}
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		const auto target_export_mapping = RenderTargetExportMapping(context);
		pixel_params      = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
		pixel_info.object_primitive_id = object_primitive_id;
		for (uint32_t export_index = 0; export_index < 8u; export_index++) {
			const auto slot = ShaderPixelExportTarget(pixel_info.target_shader_mask, export_index);
			if (slot >= 8u || pixel_info.target_output_mode[export_index] == 0u) continue;
			const auto& rt = context.GetRenderTarget(slot);
			const auto target_mask = render_target_mask_slot(context.GetRenderTargetMask(), slot);
			if (rt.base.addr == 0 || target_mask == 0u) continue;
			const auto format = Prospero::ResolveRenderTargetFormat(rt.info.format, rt.info.channel_type);
			if (format.buffer_format != Prospero::BufferFormat::k11_11_10UNorm) continue;
			const auto mapping = pixel_info.target_export_mapping[export_index];
			const auto shader_mask = render_target_mask_slot(pixel_info.target_shader_mask, slot);
			const bool blend = context.GetBlendControl(slot).enable && !rt.info.blend_bypass;
			if (blend || (mapping.ApplyMask(target_mask) & 7u) != 7u ||
			    (mapping.ApplyMask(shader_mask) & 7u) != 7u ||
			    pixel_info.target_output_mode[export_index] != 9u) {
				EXIT("unsupported packed UNorm render-target export: slot=%u export=%u mode=%u "
				     "target_mask=0x%x shader_mask=0x%x mapping=0x%02x blend=%u\n",
				     slot, export_index, pixel_info.target_output_mode[export_index], target_mask,
				     shader_mask, mapping.packed, static_cast<uint32_t>(blend));
			}
			pixel_info.target_packed_unorm_mask |= 1u << export_index;
			pixel_info.target_round_mode |= static_cast<uint8_t>(rt.info.round_mode) << export_index;
		}
		if (!mesh_active && !tess_active && Prospero::IsRectList(user_config.GetPrimType())) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::Rectangle;
		} else if (context.GetModeControl().provoking_vtx_last) {
			pixel_info.parameter_mode = ShaderPixelParameterMode::LastVertex;
		} else {
			pixel_info.parameter_mode = ShaderPixelParameterMode::FirstVertex;
		}
		const auto& blend = context.GetBlendControl(0);
		pixel_info.dual_source_blending =
		    blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		    (BlendFactorIsDualSource(blend.color_srcblend) ||
		     BlendFactorIsDualSource(blend.color_destblend) ||
		     (blend.separate_alpha_blend && (BlendFactorIsDualSource(blend.alpha_srcblend) ||
		                                     BlendFactorIsDualSource(blend.alpha_destblend))));
		if (pixel_info.dual_source_blending) {
			// MRT1 supplies the second blend source for target 0.
			pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
			pixel_info.target_export_mapping[1] = pixel_info.target_export_mapping[0];
		} else if (blend.enable && !context.GetRenderTarget(0).info.blend_bypass &&
		           pixel_info.target_output_mode[0] != 0 && pixel_info.target_output_mode[0] != 7 &&
		           std::all_of(std::begin(pixel_info.target_output_mode) + 1,
		                       std::end(pixel_info.target_output_mode),
		                       [](uint8_t mode) { return mode == 0; })) {
			switch (ClassifyBlendMapping(blend, pixel_info.target_export_mapping[0])) {
				case BlendMappingSupport::SourceAlpha:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlpha;
					break;
				case BlendMappingSupport::SourceAlphaOne:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
					break;
				case BlendMappingSupport::SourceAlphaZero:
					pixel_info.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaZero;
					break;
				default: break;
			}
			if (pixel_info.alpha_blend_source != ShaderAlphaBlendSource::None) {
				pixel_info.dual_source_blending     = true;
				pixel_info.target_output_mode[1]    = pixel_info.target_output_mode[0];
				pixel_info.target_export_mapping[1] = {};
			}
		}
	}
	auto&       last_stage   = vertex_info[tess_active ? 2u : 0u];
	const auto& clip_control = context.GetClipControl();
	if (clip_control.clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		auto&       clip     = last_stage.clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(m_limits.max_viewport_width, 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(m_limits.max_viewport_height, 16384u)) * 0.5f;
		clip.enabled = true;
	} else if (clip_control.user_clip_planes != 0) {
		// CLIP_DISABLE also disables user planes. How they combine with shader clip and cull
		// distances is unverified; draw push constants only reach VS/mesh and PS, and the
		// generated RECTLIST stages do not forward clip distances.
		EXIT_NOT_IMPLEMENTED((last_stage.pa_cl_vs_out_cntl & 0xffffu) != 0 || tess_active ||
		                     Prospero::IsRectList(user_config.GetPrimType()));
		last_stage.user_clip_planes = clip_control.user_clip_planes;
	}
	// A mesh program's own draw parameters (or the object ID) come first in the push data, then the user
	// clip planes. (A mesh program run as a compute shader has its draw parameters in the compute push
	// data and its planes in the graphics one, both before the cursor: 6 + 4 per plane > DrawDwordCount.)
	static_assert(ShaderRecompiler::IR::PushData::UserClipPlaneDword + 4u >
	              ShaderRecompiler::IR::MeshEmulation::DrawDwordCount);
	uint32_t          push_data_cursor =
	    !mesh_active || last_stage.user_clip_planes != 0
	        ? ShaderRecompiler::IR::PushData::GraphicsDrawDwords(mesh_active, object_primitive_id,
	                                                             last_stage.user_clip_planes)
	    : vertex_info[0].mesh.compute_emulation ? ShaderRecompiler::IR::MeshEmulation::DrawDwordCount
	                                            : ShaderRecompiler::IR::PushData::MeshDrawDwordCount;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_impl->Get(pixel_params, pixel_info, push_data_cursor);
		for (const auto& output: pixel_info.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt ||
			    output.kind == ShaderRecompiler::IR::StageOutputKind::ObjectPrimitiveId) {
				mrt_mask |= 1u << output.location;
			}
		}
		mrt_mask &= color_output_mask;
	}
	for (uint32_t i = 0; i < (tess_active ? 3u : 1u); i++) {
		result.vertex[i] = m_impl->Get(vertex_params[i], vertex_info[i], push_data_cursor);
	}
	return result;
}

CompiledProgram ShaderProgramCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                                     ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_limits.compute_wave64 ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, input_info);
	input_info.lds_storage = input_info.lds_size_dwords * 4u > m_limits.max_compute_shared_memory;
	uint32_t          push_data_cursor = 0;
	return m_impl->Get(params, input_info, push_data_cursor);
}


} // namespace Libs::Graphics
