#include "graphics/host_gpu/d3d12/pipelineCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include <cinttypes>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace IR = ShaderRecompiler::IR;

namespace {

HostShaderLimits ShaderLimits(const GraphicContext& graphics) {
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options {};
	D3D12::Check(graphics.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options,
	                                                  sizeof(options)),
	             "query wave lane counts");
	return {
	    // Guest wave64 compute needs a host that runs every wave with 64 lanes.
	    .compute_wave64      = options.WaveLaneCountMin >= 64,
	    .subgroup_size       = options.WaveLaneCountMin,
	    .max_viewport_width  = D3D12_VIEWPORT_BOUNDS_MAX,
	    .max_viewport_height = D3D12_VIEWPORT_BOUNDS_MAX,
	    // fxc and dxc both reject more than 32 KiB of group shared memory (port guard G15); the shader cache clamps the guest's LDS to it.
	    .max_compute_shared_memory = 32768,
	};
}

RootLayout::RangeType RangeTypeOf(IR::DescriptorBindingKind kind) {
	if (kind == IR::DescriptorBindingKind::Samplers) {
		return RootLayout::RangeType::Sampler;
	}
	// Storage buffers are never declared read-only, so spirv_to_dxil emits them as UAVs.
	return IR::ImageBindingResourceClass(kind) == IR::ImageResourceClass::Sampled
	           ? RootLayout::RangeType::Srv
	           : RootLayout::RangeType::Uav;
}

void Mix(std::size_t& hash, std::size_t value) {
	hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
}

} // namespace

void RootLayout::Add(const IR::CompiledShaderInfo& program, bool program_runtime_data) {
	push_constants = push_constants || program.bindings.UsesPushData();
	runtime_data   = runtime_data || program_runtime_data;
	for (const auto& binding: program.bindings.descriptors) {
		// Buffers, images and samplers are arrays sized to their resources (see the SPIR-V
		// emitter's DefineDescriptors); every other kind is a single descriptor.
		const bool array = binding.kind == IR::DescriptorBindingKind::Buffers ||
		                   binding.kind == IR::DescriptorBindingKind::Samplers ||
		                   IR::ImageBindingResourceClass(binding.kind) != IR::ImageResourceClass::None;
		const auto count = array ? static_cast<uint32_t>(binding.resources.size()) : 1u;
		EXIT_IF(count == 0);
		const Range range {IR::NativeBinding(program.stage, binding.kind), count,
		                   RangeTypeOf(binding.kind)};
		(range.type == RangeType::Sampler ? samplers : views).push_back(range);
	}
}

std::size_t PipelineCache::RootLayoutHash::operator()(const RootLayout& layout) const {
	std::size_t hash = 0;
	for (const auto* ranges: {&layout.views, &layout.samplers}) {
		Mix(hash, ranges->size());
		for (const auto& range: *ranges) {
			Mix(hash, range.space);
			Mix(hash, range.count);
			Mix(hash, static_cast<std::size_t>(range.type));
		}
	}
	Mix(hash, (layout.push_constants ? 1u : 0u) | (layout.runtime_data ? 2u : 0u) |
	              (layout.graphics ? 4u : 0u));
	return hash;
}

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_programs(ShaderLimits(graphics)), m_compiler(graphics.device) {}

PipelineCache::~PipelineCache() {
	for (const auto& [id, pipeline]: m_compute_pipelines) {
		(void)id;
		if (pipeline->pipeline != nullptr) {
			pipeline->pipeline->Release();
		}
	}
	for (const auto& [layout, root_signature]: m_root_signatures) {
		(void)layout;
		root_signature->Release();
	}
}

const PipelineCache::Shader* PipelineCache::Translate(const CompiledProgram& program,
                                                      ShaderType stage, bool last_vertex_stage) {
	if (!program) {
		return nullptr;
	}
	Common::LockGuard lock(m_mutex);
	auto [found, inserted] = m_shaders.try_emplace(program.id);
	if (inserted) {
		KYTY_PROFILER_BLOCK("D3D12 translate shader");
		auto shader   = std::make_unique<Shader>();
		shader->id    = program.id;
		shader->stage = stage;
		shader->dxil  = m_compiler.Compile(*program.spirv, stage, program.id, last_vertex_stage);
		if (shader->dxil.IsValid()) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "D3D12 shader {}: stage={} SPIR-V={} bytes DXIL={} bytes\n", program.id,
			    static_cast<int>(stage), program.spirv->size() * sizeof(uint32_t),
			    shader->dxil.bytecode.size()));
		}
		found->second = std::move(shader);
	}
	return found->second.get();
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const auto programs =
	    m_programs.GetGraphicsPrograms(vertex_regs, pixel_regs, sh, context, user_config,
	                                   target_export_mapping, pixel_active, vertex_info, pixel_info);
	const auto stages = programs.VertexStageCount();
	GraphicsPrograms result;
	for (uint32_t i = 0; i < stages; i++) {
		result.vertex[i] =
		    Translate(programs.vertex[i], vertex_info[i].logical_stage, i + 1 == stages);
	}
	result.pixel = Translate(programs.pixel, ShaderType::Pixel, false);
	return result;
}

const PipelineCache::Shader& PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                                              const HW::ShaderRegisters&   sh,
                                                              ShaderComputeInputInfo& input_info) {
	const auto* shader =
	    Translate(m_programs.GetComputeProgram(regs, sh, input_info), ShaderType::Compute, false);
	EXIT_IF(shader == nullptr);
	return *shader;
}

ID3D12RootSignature* PipelineCache::GetRootSignature(const RootLayout& layout) {
	auto [found, inserted] = m_root_signatures.try_emplace(layout, nullptr);
	if (!inserted) {
		return found->second;
	}

	const auto to_ranges = [](const std::vector<RootLayout::Range>& ranges) {
		std::vector<D3D12_DESCRIPTOR_RANGE1> result;
		result.reserve(ranges.size());
		for (const auto& range: ranges) {
			D3D12_DESCRIPTOR_RANGE1 d3d {};
			switch (range.type) {
				case RootLayout::RangeType::Srv: d3d.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; break;
				case RootLayout::RangeType::Uav: d3d.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; break;
				case RootLayout::RangeType::Sampler:
					d3d.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
					break;
			}
			d3d.NumDescriptors                    = range.count;
			d3d.BaseShaderRegister                = 0;
			d3d.RegisterSpace                     = range.space;
			d3d.Flags                             = range.type == RootLayout::RangeType::Sampler
			                                            ? D3D12_DESCRIPTOR_RANGE_FLAG_NONE
			                                            : D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
			d3d.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
			result.push_back(d3d);
		}
		return result;
	};
	const auto view_ranges    = to_ranges(layout.views);
	const auto sampler_ranges = to_ranges(layout.samplers);

	std::vector<D3D12_ROOT_PARAMETER1> parameters;
	if (layout.push_constants) {
		D3D12_ROOT_PARAMETER1 parameter {};
		parameter.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameter.Constants.Num32BitValues = IR::PushData::DwordCount;
		parameter.Constants.RegisterSpace  = D3D12::PushConstantSpace;
		parameters.push_back(parameter);
	}
	if (layout.runtime_data) {
		D3D12_ROOT_PARAMETER1 parameter {};
		parameter.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
		parameter.Descriptor.RegisterSpace  = D3D12::RuntimeDataSpace;
		parameter.Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
		parameters.push_back(parameter);
	}
	for (const auto* ranges: {&view_ranges, &sampler_ranges}) {
		if (ranges->empty()) {
			continue;
		}
		D3D12_ROOT_PARAMETER1 parameter {};
		parameter.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameter.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges->size());
		parameter.DescriptorTable.pDescriptorRanges   = ranges->data();
		parameters.push_back(parameter);
	}

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
	desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
	desc.Desc_1_1.pParameters   = parameters.data();
	desc.Desc_1_1.Flags = layout.graphics ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
	                                      : D3D12_ROOT_SIGNATURE_FLAG_NONE;

	D3D12::ComPtr<ID3DBlob> blob;
	D3D12::ComPtr<ID3DBlob> error;
	if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
		EXIT("D3D12: root signature serialization failed: %s\n",
		     error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
	}
	D3D12::Check(m_graphics.device->CreateRootSignature(0, blob->GetBufferPointer(),
	                                                    blob->GetBufferSize(),
	                                                    IID_PPV_ARGS(&found->second)),
	             "CreateRootSignature");
	return found->second;
}

PipelineCache::Pipeline& PipelineCache::GetComputePipeline(const ShaderComputeInputInfo& input_info,
                                                           const Shader&                 shader) {
	Common::LockGuard lock(m_mutex);
	auto [found, inserted] = m_compute_pipelines.try_emplace(shader.id);
	if (!inserted) {
		return *found->second;
	}
	KYTY_PROFILER_BLOCK("D3D12 create compute pipeline");
	auto pipeline = std::make_unique<Pipeline>();
	if (shader.dxil.IsValid()) {
		pipeline->layout.Add(*input_info.stage.program, shader.dxil.requires_runtime_data);
		pipeline->root_signature = GetRootSignature(pipeline->layout);

		D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
		desc.pRootSignature     = pipeline->root_signature;
		desc.CS.pShaderBytecode = shader.dxil.bytecode.data();
		desc.CS.BytecodeLength  = shader.dxil.bytecode.size();
		const HRESULT result    = m_graphics.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline->pipeline));
		if (FAILED(result)) {
			pipeline->pipeline = nullptr;
			Log::WriteToConsoleAndLog(fmt::format("D3D12: CreateComputePipelineState failed (0x{:08x}), shader {} is skipped\n", static_cast<uint32_t>(result), shader.id));
		}
	}
	found->second = std::move(pipeline);
	return *found->second;
}

PipelineCache::Statistics PipelineCache::GetStatistics() const {
	Common::LockGuard lock(m_mutex);
	return {m_shaders.size(), m_compute_pipelines.size(), m_root_signatures.size()};
}

} // namespace Libs::Graphics
