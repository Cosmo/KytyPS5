#include "graphics/host_gpu/d3d12/pipelineCache.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/d3d12/render.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/shader/rectListShader.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <set>
#include <fmt/format.h>
#include <string>
#include <type_traits>
#include <xxhash.h>

namespace Libs::Graphics {

namespace IR = ShaderRecompiler::IR;

namespace {

HostShaderLimits ShaderLimits(const GraphicContext& graphics, const D3D12::DxilCompiler& compiler) {
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options {};
	D3D12::Check(graphics.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options,
	                                                  sizeof(options)),
	             "query wave lane counts");
	return {
	    // Guest wave64 compute needs a host that runs every wave with 64 lanes.
	    .compute_wave64         = options.WaveLaneCountMin >= 64,
	    .subgroup_size          = options.WaveLaneCountMin,
	    .max_viewport_width     = D3D12_VIEWPORT_BOUNDS_MAX,
	    .max_viewport_height    = D3D12_VIEWPORT_BOUNDS_MAX,
	    .mesh_shader_enabled    = graphics.mesh_shaders && compiler.SupportsMeshShaders(),
	    // D3D12 fixes the mesh shader limits.
	    .max_mesh_invocations   = 128,
	    .max_mesh_group_size_x  = 128,
	    .max_mesh_vertices      = 256,
	    .max_mesh_primitives    = 256,
	    .max_mesh_shared_memory = 28 * 1024,
	};
}

RootLayout::RangeType RangeTypeOf(const IR::CompiledShaderInfo& program,
                                  const IR::DescriptorBinding&  binding) {
	if (binding.kind == IR::DescriptorBindingKind::Samplers) {
		return RootLayout::RangeType::Sampler;
	}
	// Buffers the program only reads are declared NonWritable, which spirv_to_dxil emits as SRVs.
	return IR::ImageBindingResourceClass(binding.kind) == IR::ImageResourceClass::Sampled ||
	               IR::IsReadOnlyBufferBinding(program.info, binding)
	           ? RootLayout::RangeType::Srv
	           : RootLayout::RangeType::Uav;
}

void Mix(std::size_t& hash, std::size_t value) {
	hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) + (hash >> 2u);
}

void UnsupportedOnce(const std::string& what) {
	static std::mutex           mutex;
	static std::set<std::string> reported;
	std::lock_guard             lock(mutex);
	if (reported.insert(what).second) {
		Log::WriteToConsoleAndLog(fmt::format("D3D12: {} is not supported; the draws that need it are skipped\n", what));
	}
}

void WarnOnce(std::atomic_bool& warned, const char* message) {
	if (!warned.exchange(true, std::memory_order_relaxed)) {
		LOGF("D3D12: %s\n", message);
		std::printf("Warning: D3D12: %s\n", message);
	}
}

D3D12_COMPARISON_FUNC ComparisonFunc(vk::CompareOp op) {
	switch (op) {
		case vk::CompareOp::eNever: return D3D12_COMPARISON_FUNC_NEVER;
		case vk::CompareOp::eLess: return D3D12_COMPARISON_FUNC_LESS;
		case vk::CompareOp::eEqual: return D3D12_COMPARISON_FUNC_EQUAL;
		case vk::CompareOp::eLessOrEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
		case vk::CompareOp::eGreater: return D3D12_COMPARISON_FUNC_GREATER;
		case vk::CompareOp::eNotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
		case vk::CompareOp::eGreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		case vk::CompareOp::eAlways: return D3D12_COMPARISON_FUNC_ALWAYS;
	}
	EXIT("D3D12: unknown compare op %d\n", static_cast<int>(op));
	return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_STENCIL_OP StencilOp(vk::StencilOp op) {
	switch (op) {
		case vk::StencilOp::eKeep: return D3D12_STENCIL_OP_KEEP;
		case vk::StencilOp::eZero: return D3D12_STENCIL_OP_ZERO;
		case vk::StencilOp::eReplace: return D3D12_STENCIL_OP_REPLACE;
		case vk::StencilOp::eIncrementAndClamp: return D3D12_STENCIL_OP_INCR_SAT;
		case vk::StencilOp::eDecrementAndClamp: return D3D12_STENCIL_OP_DECR_SAT;
		case vk::StencilOp::eInvert: return D3D12_STENCIL_OP_INVERT;
		case vk::StencilOp::eIncrementAndWrap: return D3D12_STENCIL_OP_INCR;
		case vk::StencilOp::eDecrementAndWrap: return D3D12_STENCIL_OP_DECR;
	}
	EXIT("D3D12: unknown stencil op %d\n", static_cast<int>(op));
	return D3D12_STENCIL_OP_KEEP;
}

D3D12_DEPTH_STENCILOP_DESC StencilFace(const vk::StencilOpState& state) {
	return {StencilOp(state.failOp), StencilOp(state.depthFailOp), StencilOp(state.passOp),
	        ComparisonFunc(state.compareOp)};
}

D3D12_DEPTH_STENCIL_DESC1 DepthStencilDesc(const RenderDepthInfo&          depth,
                                           const PipelineRenderingState&   rendering,
                                           const PipelineStaticParameters& params) {
	D3D12_DEPTH_STENCIL_DESC1 desc {};
	desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	desc.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.FrontFace      = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
	                       D3D12_COMPARISON_FUNC_ALWAYS};
	desc.BackFace       = desc.FrontFace;
	if (rendering.depth_format != vk::Format::eUndefined && depth.depth_test_enable) {
		desc.DepthEnable    = TRUE;
		desc.DepthWriteMask = depth.depth_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL
		                                               : D3D12_DEPTH_WRITE_MASK_ZERO;
		desc.DepthFunc      = ComparisonFunc(depth.depth_compare_op);
	}
	if (rendering.stencil_format != vk::Format::eUndefined && depth.stencil_test_enable) {
		const auto& front = depth.stencil_front;
		const auto& back  = depth.stencil_back;
		if (front.compareMask != back.compareMask || front.writeMask != back.writeMask) {
			static std::atomic_bool warned = false;
			WarnOnce(warned, "stencil masks differ per face; using the front face masks");
		}
		desc.StencilEnable    = TRUE;
		desc.StencilReadMask  = static_cast<UINT8>(front.compareMask);
		desc.StencilWriteMask = static_cast<UINT8>(front.writeMask);
		desc.FrontFace        = StencilFace(front);
		desc.BackFace         = StencilFace(back);
	}
	desc.DepthBoundsTestEnable =
	    rendering.depth_format != vk::Format::eUndefined && params.depth_bounds_test_enable;
	return desc;
}

bool operator==(const D3D12_DEPTH_STENCILOP_DESC& a, const D3D12_DEPTH_STENCILOP_DESC& b) {
	return a.StencilFailOp == b.StencilFailOp && a.StencilDepthFailOp == b.StencilDepthFailOp &&
	       a.StencilPassOp == b.StencilPassOp && a.StencilFunc == b.StencilFunc;
}

bool operator==(const D3D12_DEPTH_STENCIL_DESC1& a, const D3D12_DEPTH_STENCIL_DESC1& b) {
	return a.DepthEnable == b.DepthEnable && a.DepthWriteMask == b.DepthWriteMask &&
	       a.DepthFunc == b.DepthFunc && a.StencilEnable == b.StencilEnable &&
	       a.StencilReadMask == b.StencilReadMask && a.StencilWriteMask == b.StencilWriteMask &&
	       a.FrontFace == b.FrontFace && a.BackFace == b.BackFace &&
	       a.DepthBoundsTestEnable == b.DepthBoundsTestEnable;
}

// `alpha` selects the factor of the alpha channel, where D3D12 only accepts alpha factors.
D3D12_BLEND BlendFactor(uint32_t factor, bool alpha, bool alpha_blend_factor) {
	using F = Prospero::BlendFactor;
	switch (static_cast<F>(factor)) {
		case F::kZero: return D3D12_BLEND_ZERO;
		case F::kOne: return D3D12_BLEND_ONE;
		case F::kSrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
		case F::kOneMinusSrcColor: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
		case F::kSrcAlpha: return D3D12_BLEND_SRC_ALPHA;
		case F::kOneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
		case F::kDstAlpha: return D3D12_BLEND_DEST_ALPHA;
		case F::kOneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
		case F::kDstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
		case F::kOneMinusDstColor:
			return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
		case F::kSrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
		case F::kConstantColor: return D3D12_BLEND_BLEND_FACTOR;
		case F::kOneMinusConstantColor: return D3D12_BLEND_INV_BLEND_FACTOR;
		case F::kSrc1Color: return alpha ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC1_COLOR;
		case F::kOneMinusSrc1Color:
			return alpha ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC1_COLOR;
		case F::kSrc1Alpha: return D3D12_BLEND_SRC1_ALPHA;
		case F::kOneMinusSrc1Alpha: return D3D12_BLEND_INV_SRC1_ALPHA;
		case F::kConstantAlpha:
		case F::kOneMinusConstantAlpha: {
			const bool inverse = static_cast<F>(factor) == F::kOneMinusConstantAlpha;
			// The alpha channel of the blend factor is the constant alpha.
			if (alpha || !alpha_blend_factor) {
				if (!alpha) {
					static std::atomic_bool warned = false;
					WarnOnce(warned, "constant alpha blend factors of color channels are "
					                 "unsupported; using the constant color");
				}
				return inverse ? D3D12_BLEND_INV_BLEND_FACTOR : D3D12_BLEND_BLEND_FACTOR;
			}
			return inverse ? D3D12_BLEND_INV_ALPHA_FACTOR : D3D12_BLEND_ALPHA_FACTOR;
		}
	}
	EXIT("D3D12: unknown blend factor %u\n", factor);
	return D3D12_BLEND_ZERO;
}

D3D12_BLEND_OP BlendOp(uint32_t op) {
	switch (static_cast<Prospero::BlendOp>(op)) {
		case Prospero::BlendOp::kAdd: return D3D12_BLEND_OP_ADD;
		case Prospero::BlendOp::kSubtract: return D3D12_BLEND_OP_SUBTRACT;
		case Prospero::BlendOp::kMin: return D3D12_BLEND_OP_MIN;
		case Prospero::BlendOp::kMax: return D3D12_BLEND_OP_MAX;
		case Prospero::BlendOp::kReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
	}
	EXIT("D3D12: unknown blend op %u\n", op);
	return D3D12_BLEND_OP_ADD;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(vk::PrimitiveTopology topology) {
	switch (topology) {
		case vk::PrimitiveTopology::ePointList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		case vk::PrimitiveTopology::eLineList:
		case vk::PrimitiveTopology::eLineStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
		case vk::PrimitiveTopology::eTriangleList:
		case vk::PrimitiveTopology::eTriangleStrip:
		case vk::PrimitiveTopology::ePatchList: // rect lists: triangles expanded by a GS
			return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		default: EXIT("D3D12: unsupported topology %d\n", static_cast<int>(topology));
	}
	return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

DXGI_FORMAT DepthViewFormat(const PipelineRenderingState& rendering) {
	const auto format = rendering.depth_format != vk::Format::eUndefined ? rendering.depth_format
	                                                                     : rendering.stencil_format;
	return format == vk::Format::eUndefined ? DXGI_FORMAT_UNKNOWN
	                                        : D3D12::GetFormatInfo(format).depth_view;
}

// A pipeline state stream subobject (d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void*) StreamSubobject {
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
	T                                   value {};
};

// Stream subobjects are used for the depth bounds test, which the plain desc lacks.
struct GraphicsPipelineStream {
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE>          vs;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE>          gs;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>          ps;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>            blend;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>
	    rasterizer;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1>
	    depth_stencil;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC>
	    input_layout;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE,
	                D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>
	    strip_cut;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
	                D3D12_PRIMITIVE_TOPOLOGY_TYPE>
	    topology;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
	                D3D12_RT_FORMAT_ARRAY>
	    render_targets;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>
	    depth_format;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
};

// Mesh shader pipelines have no input assembler state.
struct MeshPipelineStream {
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE>          ms;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE>          ps;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC>            blend;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC>
	    rasterizer;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1>
	    depth_stencil;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
	                D3D12_RT_FORMAT_ARRAY>
	    render_targets;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>
	    depth_format;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
};

struct ComputePipelineStream {
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
	StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, D3D12_SHADER_BYTECODE>          cs;
};

D3D12_SHADER_BYTECODE Bytecode(const D3D12::DxilShader& shader) {
	return {shader.bytecode.data(), shader.bytecode.size()};
}

// A pipeline library name: a hash of everything the pipeline description contains, so a saved
// pipeline is only found for an identical description.
class PipelineName {
public:
	template <typename T>
	void Add(T value) {
		m_parts.push_back(static_cast<uint64_t>(value));
	}
	void Add(float value) { Add(std::bit_cast<uint32_t>(value)); }
	void Add(const D3D12_SHADER_BYTECODE& code) {
		Add(code.BytecodeLength == 0 ? 0 : XXH3_64bits(code.pShaderBytecode, code.BytecodeLength));
	}
	[[nodiscard]] std::wstring Get(wchar_t kind) const {
		wchar_t name[32] {};
		std::swprintf(name, std::size(name), L"%c%016llx", kind,
		              static_cast<unsigned long long>(
		                  XXH3_64bits(m_parts.data(), m_parts.size() * sizeof(uint64_t))));
		return name;
	}

private:
	std::vector<uint64_t> m_parts;
};

template <typename Stream>
std::wstring GraphicsPipelineName(const Stream&                             stream,
                                  std::span<const D3D12_INPUT_ELEMENT_DESC> elements,
                                  uint64_t                                  root_layout_hash) {
	constexpr bool mesh = std::is_same_v<Stream, MeshPipelineStream>;
	PipelineName   name;
	name.Add(root_layout_hash);
	if constexpr (mesh) {
		name.Add(stream.ms.value);
	} else {
		name.Add(stream.vs.value);
		name.Add(stream.gs.value);
	}
	name.Add(stream.ps.value);
	const auto& blend = stream.blend.value;
	name.Add(blend.AlphaToCoverageEnable);
	name.Add(blend.IndependentBlendEnable);
	for (const auto& target: blend.RenderTarget) {
		name.Add(target.BlendEnable);
		name.Add(target.LogicOpEnable);
		name.Add(target.SrcBlend);
		name.Add(target.DestBlend);
		name.Add(target.BlendOp);
		name.Add(target.SrcBlendAlpha);
		name.Add(target.DestBlendAlpha);
		name.Add(target.BlendOpAlpha);
		name.Add(target.LogicOp);
		name.Add(target.RenderTargetWriteMask);
	}
	name.Add(stream.sample_mask.value);
	const auto& rasterizer = stream.rasterizer.value;
	name.Add(rasterizer.FillMode);
	name.Add(rasterizer.CullMode);
	name.Add(rasterizer.FrontCounterClockwise);
	name.Add(rasterizer.DepthBias);
	name.Add(rasterizer.DepthBiasClamp);
	name.Add(rasterizer.SlopeScaledDepthBias);
	name.Add(rasterizer.DepthClipEnable);
	name.Add(rasterizer.MultisampleEnable);
	name.Add(rasterizer.AntialiasedLineEnable);
	name.Add(rasterizer.ForcedSampleCount);
	name.Add(rasterizer.ConservativeRaster);
	const auto& depth = stream.depth_stencil.value;
	name.Add(depth.DepthEnable);
	name.Add(depth.DepthWriteMask);
	name.Add(depth.DepthFunc);
	name.Add(depth.StencilEnable);
	name.Add(depth.StencilReadMask);
	name.Add(depth.StencilWriteMask);
	for (const auto& face: {depth.FrontFace, depth.BackFace}) {
		name.Add(face.StencilFailOp);
		name.Add(face.StencilDepthFailOp);
		name.Add(face.StencilPassOp);
		name.Add(face.StencilFunc);
	}
	name.Add(depth.DepthBoundsTestEnable);
	name.Add(elements.size());
	for (const auto& element: elements) {
		name.Add(element.SemanticIndex);
		name.Add(element.Format);
		name.Add(element.InputSlot);
		name.Add(element.AlignedByteOffset);
		name.Add(element.InputSlotClass);
		name.Add(element.InstanceDataStepRate);
	}
	if constexpr (!mesh) {
		name.Add(stream.strip_cut.value);
		name.Add(stream.topology.value);
	}
	name.Add(stream.render_targets.value.NumRenderTargets);
	for (const auto format: stream.render_targets.value.RTFormats) {
		name.Add(format);
	}
	name.Add(stream.depth_format.value);
	name.Add(stream.samples.value.Count);
	name.Add(stream.samples.value.Quality);
	return name.Get(mesh ? L'M' : L'G');
}

} // namespace

void RootLayout::Add(const IR::CompiledShaderInfo& program, bool program_runtime_data) {
	push_constants = push_constants || UsesPushConstants(program);
	runtime_data   = runtime_data || program_runtime_data;
	heap_indexing  = heap_indexing || program.info.uses_dma;
	for (const auto& binding: program.bindings.descriptors) {
		// Buffers, images and samplers are arrays sized to their resources (see the SPIR-V
		// emitter's DefineDescriptors); every other kind is a single descriptor.
		const bool array = binding.kind == IR::DescriptorBindingKind::Buffers ||
		                   binding.kind == IR::DescriptorBindingKind::Samplers ||
		                   IR::ImageBindingResourceClass(binding.kind) != IR::ImageResourceClass::None;
		const auto count = array ? static_cast<uint32_t>(binding.resources.size()) : 1u;
		EXIT_IF(count == 0);
		const Range range {IR::NativeBinding(program.stage, binding.kind), count,
		                   RangeTypeOf(program, binding)};
		(range.type == RangeType::Sampler ? samplers : views).push_back(range);
		if (HasComparisonSamplers(program, binding)) {
			samplers.push_back({range.space + D3D12::ComparisonSamplerSpaceOffset, count,
			                    RangeType::Sampler});
		}
	}
}

bool UsesPushConstants(const IR::CompiledShaderInfo& program) {
	return program.bindings.UsesPushData() || program.stage == ShaderType::Mesh;
}

bool HasComparisonSamplers(const IR::CompiledShaderInfo& program,
                           const IR::DescriptorBinding&  binding) {
	return binding.kind == IR::DescriptorBindingKind::Samplers &&
	       std::ranges::any_of(binding.resources, [&](uint32_t resource) {
		       return program.info.samplers.at(resource).depth_compare;
	       });
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
	              (layout.graphics ? 4u : 0u) | (layout.heap_indexing ? 8u : 0u));
	return hash;
}

bool PipelineCache::GraphicsKey::operator==(const GraphicsKey& other) const {
	return state == other.state && depth_stencil == other.depth_stencil &&
	       depth_bias == other.depth_bias && depth_bias_clamp == other.depth_bias_clamp &&
	       slope_scaled == other.slope_scaled && strip_cut == other.strip_cut;
}

std::size_t PipelineCache::GraphicsKeyHash::operator()(const GraphicsKey& key) const {
	auto        hash = GraphicsPipelineKeyHash {}(key.state);
	const auto& ds   = key.depth_stencil;
	Mix(hash, ds.DepthEnable | (ds.DepthWriteMask << 1u) | (ds.DepthFunc << 2u) |
	              (ds.StencilEnable << 6u) | (ds.DepthBoundsTestEnable << 7u) |
	              (ds.StencilReadMask << 8u) | (ds.StencilWriteMask << 16u));
	for (const auto& face: {ds.FrontFace, ds.BackFace}) {
		Mix(hash, face.StencilFailOp | (face.StencilDepthFailOp << 4u) |
		              (face.StencilPassOp << 8u) | (face.StencilFunc << 12u));
	}
	Mix(hash, static_cast<uint32_t>(key.depth_bias));
	Mix(hash, std::hash<float> {}(key.depth_bias_clamp));
	Mix(hash, std::hash<float> {}(key.slope_scaled));
	Mix(hash, key.strip_cut);
	return hash;
}

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_compiler(graphics.device),
      m_programs(ShaderLimits(graphics, m_compiler)) {
	D3D12::Check(graphics.device->QueryInterface(IID_PPV_ARGS(&m_device)),
	             "query ID3D12Device2");
#if defined(KYTY_PLATFORM_UWP)
	// Constant-alpha blend factors fail at pipeline creation on the Xbox (E_INVALIDARG, guard G4): the constant color factors stand in.
	m_alpha_blend_factor = false;
#else
	D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13 {};
	m_alpha_blend_factor =
	    SUCCEEDED(graphics.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options13,
	                                                   sizeof(options13))) &&
	    options13.AlphaBlendFactorSupported;
#endif
	InitializeDiskCache();
}

std::string PipelineCache::CacheSignature() const {
	return fmt::format("KytyD3D12PC2:{}:{}\n", KYTY_GIT_REVISION, m_compiler.CacheSignature());
}

// The cache file holds a u64 count of its pipelines, a u64 size and the saved translations
// (DxilCompiler), then the serialized pipeline library.
void PipelineCache::InitializeDiskCache() {
	m_cache_file = PipelineCacheFile("D3D12 pipeline cache", ".d3d12.bin");
	if (!m_cache_file.Enabled()) {
		return;
	}
	const auto payload = m_cache_file.Load(CacheSignature());
	if (!payload.empty()) {
		uint64_t         pipelines    = 0;
		uint64_t         translations = 0;
		constexpr size_t header       = sizeof(pipelines) + sizeof(translations);
		if (payload.size() >= header) {
			std::memcpy(&pipelines, payload.data(), sizeof(pipelines));
			std::memcpy(&translations, payload.data() + sizeof(pipelines), sizeof(translations));
		}
		const auto rest = std::span {payload}.subspan(std::min(payload.size(), header));
		if (payload.size() < header || translations > rest.size() ||
		    !m_compiler.LoadCache(rest.first(translations))) {
			m_cache_file.Log("invalidating the saved data (malformed)");
		} else {
			const auto library = rest.subspan(translations);
			m_library_data.assign(library.begin(), library.end());
			m_file_pipelines  = pipelines;
			m_saved_pipelines = pipelines;
			m_cache_file.Log("loaded {} bytes of translated shaders ({} pipelines)", translations,
			                 pipelines);
		}
	}

	// The Xbox's driver (SraKmd) removes the device in CreatePipelineLibrary: its pipelines aren't
	// kept, the translated shaders are.
	if (std::string_view(m_graphics.DeviceName()).starts_with("SraKmd")) {
		m_cache_file.Log("pipeline library off (this driver removes the device with one)");
		return;
	}
	D3D12::ComPtr<ID3D12Device1> device;
	if (FAILED(m_graphics.device->QueryInterface(IID_PPV_ARGS(&device)))) {
		m_cache_file.Log("pipeline libraries are not supported");
		return;
	}
	auto result = device->CreatePipelineLibrary(
	    m_library_data.empty() ? nullptr : m_library_data.data(), m_library_data.size(),
	    IID_PPV_ARGS(&m_library));
	if (FAILED(result) && !m_library_data.empty()) {
		// E.g. another driver version: the translations stay valid, the pipelines do not.
		m_cache_file.Log("driver rejected the saved pipelines (0x{:08x}); starting empty",
		                 static_cast<uint32_t>(result));
		m_library_data.clear();
		result = device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_library));
	}
	if (FAILED(result)) {
		m_cache_file.Log("pipeline library disabled (0x{:08x})", static_cast<uint32_t>(result));
		m_library.Reset();
		return;
	}
	if (!m_library_data.empty()) {
		m_cache_file.Log("loaded {} bytes of pipelines", m_library_data.size());
	}
}

void PipelineCache::Save() {
	Common::LockGuard lock(m_mutex);
	const auto        changes = m_changes.load();
	if (!m_cache_file.Enabled() || changes == m_saved_changes) {
		return;
	}
	const auto     translations = m_compiler.SaveCache();
	const uint64_t size         = translations.size();
	// The pipelines the file had, and those made since without it.
	const uint64_t pipelines = m_file_pipelines + m_compute_pipelines.size() +
	                           m_graphics_pipelines.size() - m_cached_pipelines;
	std::vector<uint8_t> payload(sizeof(pipelines) + sizeof(size));
	std::memcpy(payload.data(), &pipelines, sizeof(pipelines));
	std::memcpy(payload.data() + sizeof(pipelines), &size, sizeof(size));
	payload.insert(payload.end(), translations.begin(), translations.end());
	if (m_library != nullptr) {
		const auto offset       = payload.size();
		const auto library_size = m_library->GetSerializedSize();
		payload.resize(offset + library_size);
		if (FAILED(m_library->Serialize(payload.data() + offset, library_size))) {
			m_cache_file.Log("failed to serialize the pipeline library");
			payload.resize(offset);
		}
	}
	m_cache_file.Log("saving {} bytes of translated shaders and {} pipelines ({} were loaded)",
	                 translations.size(), m_compute_pipelines.size() + m_graphics_pipelines.size(),
	                 m_loaded_pipelines);
	if (m_cache_file.Save(CacheSignature(), payload)) {
		m_saved_changes   = changes;
		m_saved_pipelines = pipelines;
	}
}

void PipelineCache::SaveWhenIdle() {
	// Pipelines are compiled in bursts (a game's start, new areas): the save follows one, in the
	// background. Frames don't wait for it; only a new pipeline meanwhile would.
	constexpr uint64_t IDLE_MS = 5000;
	const auto         changes = m_changes.load();
	if (changes == 0 || changes == m_saved_changes || m_saving ||
	    GetTickCount64() - m_last_change_ms.load() < IDLE_MS) {
		return;
	}
	if (m_save_thread.joinable()) {
		m_save_thread.join();
	}
	m_saving      = true;
	m_save_thread = std::thread([this] {
		Save();
		m_saving = false;
	});
}

// The features a DXIL shader requires (the container's SFI0 part), by name.
static std::string RequiredFeatures(const std::vector<uint8_t>& dxil) {
	static constexpr const char* names[] = {
	    "doubles", "raw/structured buffers (FL 10)", "UAVs at every stage (FL 11.1)",
	    "64 UAVs (FL 11.1)", "minimum precision", "11.1 double extensions", "11.1 shader extensions",
	    "level 9 comparison filtering", "tiled resources", "stencil ref", "inner coverage",
	    "typed UAV load formats", "ROVs", "viewport/RT array index from any stage", "wave ops",
	    "int64 ops", "view ID", "barycentrics", "native 16-bit ops", "shading rate",
	    "raytracing 1.1", "sampler feedback", "int64 atomics (typed)", "int64 atomics (groupshared)",
	    "derivatives in mesh/amplification", "resource descriptor heap indexing",
	    "sampler descriptor heap indexing", "wave MMA", "int64 atomics (heap)",
	    "advanced texture ops", "writable MSAA textures", "sample-compare gradient/bias",
	    "extended command info"};
	// DXBC header: fourCC, digest (16), version (4), size, part count; then the part offsets.
	const auto read32 = [&](size_t offset) {
		uint32_t value = 0;
		if (offset + sizeof(value) <= dxil.size()) {
			std::memcpy(&value, dxil.data() + offset, sizeof(value));
		}
		return value;
	};
	const auto parts = read32(28);
	for (uint32_t i = 0; i < parts; i++) {
		const auto part = read32(32 + 4 * i);
		if (read32(part) != 0x30494653u /* SFI0 */ || part + 16 > dxil.size()) {
			continue;
		}
		uint64_t flags = 0;
		std::memcpy(&flags, dxil.data() + part + 8, sizeof(flags));
		std::string text;
		for (size_t bit = 0; bit < 64; bit++) {
			if ((flags >> bit & 1u) != 0) {
				text += (text.empty() ? "" : ", ") +
				        (bit < std::size(names) ? std::string(names[bit]) : fmt::format("bit {}", bit));
			}
		}
		return text.empty() ? "none" : text;
	}
	return "unknown (no SFI0 part)";
}

ID3D12PipelineState* PipelineCache::LoadOrCreate(const std::wstring&                    name,
                                                 const D3D12_PIPELINE_STATE_STREAM_DESC& desc,
                                                 std::initializer_list<const D3D12::DxilShader*> shaders) {
	ID3D12PipelineState* pipeline = nullptr;
	if (m_library != nullptr &&
	    SUCCEEDED(m_library->LoadPipeline(name.c_str(), &desc, IID_PPV_ARGS(&pipeline)))) {
		m_loaded_pipelines++;
		return pipeline;
	}
	const HRESULT result = m_device->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline));
	if (FAILED(result)) {
		// The device's limits are the usual reason (the Xbox's UWP games get feature level 11.0). The pipeline is skipped, its shaders' required features
		// are reported and the shaders saved.
		std::string                           report;
		std::vector<std::span<const uint8_t>> dxil;
		for (const auto* shader: shaders) {
			if (shader != nullptr && !shader->bytecode.empty()) {
				report += fmt::format("  shader {} ({} bytes) requires: {}\n", dxil.size(), shader->bytecode.size(), RequiredFeatures(shader->bytecode));
				dxil.emplace_back(shader->bytecode);
			}
		}
		Log::WriteToConsoleAndLog(fmt::format("D3D12: CreatePipelineState failed (0x{:08x}); the draws that need this pipeline are skipped. Shaders saved to {}:\n{}",
		                                      static_cast<uint32_t>(result),
		                                      D3D12::SaveShaderDump(fmt::format("rejected_pipeline_{}", ++m_rejected_pipelines), dxil, {}), report));
		return nullptr;
	}
	// Fails only for a name already stored, which then describes another pipeline: the file
	// wouldn't change, so that's no reason to save it.
	if (m_library == nullptr || SUCCEEDED(m_library->StorePipeline(name.c_str(), pipeline))) {
		m_last_change_ms = GetTickCount64();
		m_changes++;
	}
	return pipeline;
}

PipelineCache::~PipelineCache() {
	if (m_save_thread.joinable()) {
		m_save_thread.join();
	}
	Save();
	for (const auto& [id, pipeline]: m_compute_pipelines) {
		(void)id;
		if (pipeline->pipeline != nullptr) {
			pipeline->pipeline->Release();
		}
	}
	for (const auto& [key, pipeline]: m_graphics_pipelines) {
		(void)key;
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
                                                      ShaderType             stage) {
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
		shader->dxil  = m_compiler.Compile(*program.spirv, stage, program.id, false, {},
		                                   &shader->from_cache);
		Log::WriteToConsoleAndLog(fmt::format(
		    "D3D12 shader {}: stage={} SPIR-V={} bytes DXIL={} bytes\n", program.id,
		    static_cast<int>(stage), program.spirv->size() * sizeof(uint32_t),
		    shader->dxil.bytecode.size()));
		found->second = std::move(shader);
	}
	return found->second.get();
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    std::array<ShaderVertexInputInfo, 3>& vertex_info, ShaderPixelInputInfo& pixel_info) {
	return m_programs.GetGraphicsPrograms(vertex_regs, pixel_regs, sh, context, user_config,
	                                      target_export_mapping, pixel_active, vertex_info,
	                                      pixel_info);
}

const PipelineCache::Shader& PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                                              const HW::ShaderRegisters&   sh,
                                                              ShaderComputeInputInfo& input_info) {
	const auto* shader =
	    Translate(m_programs.GetComputeProgram(regs, sh, input_info), ShaderType::Compute);
	// No program: the dispatch is skipped (ray tracing isn't implemented).
	static const Shader skipped;
	return shader != nullptr ? *shader : skipped;
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
		parameter.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameter.Constants.Num32BitValues = D3D12::RuntimeDataDwords;
		parameter.Constants.RegisterSpace  = D3D12::RuntimeDataSpace;
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
	if (layout.heap_indexing) {
		desc.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
	}

	D3D12::ComPtr<ID3DBlob> blob;
	D3D12::ComPtr<ID3DBlob> error;
	HRESULT result = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
	if (SUCCEEDED(result)) {
		result = m_graphics.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&found->second));
	}
	if (FAILED(result)) {
		// E.g. shaders that index the descriptor heap directly (shader model 6.6) on a device without it: reported once, their pipelines are skipped.
		Log::WriteToConsoleAndLog(fmt::format("D3D12: a root signature could not be created (0x{:08x}{}{}); the pipelines that need it are skipped\n",
		                                      static_cast<uint32_t>(result), error != nullptr ? ": " : "",
		                                      error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : ""));
		found->second = nullptr;
	}
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
	NameNewPipeline(*pipeline, fmt::format("cs {:x}", shader.id));
	pipeline->trace.dxil = {&shader.dxil};
	pipeline->layout.Add(*input_info.stage.program, shader.dxil.requires_runtime_data);
	pipeline->root_signature = GetRootSignature(pipeline->layout);
	if (!shader.dxil.IsValid() || pipeline->root_signature == nullptr) {
		found->second = std::move(pipeline);
		return *found->second;
	}

	ComputePipelineStream stream {};
	stream.root.value = pipeline->root_signature;
	stream.cs.value   = Bytecode(shader.dxil);
	PipelineName name;
	name.Add(RootLayoutHash {}(pipeline->layout));
	name.Add(stream.cs.value);
	const auto loaded_before = m_loaded_pipelines;
	pipeline->pipeline = LoadOrCreate(name.Get(L'C'), {sizeof(stream), &stream}, {&shader.dxil});
	CountNewPipeline(loaded_before, shader.from_cache);
	found->second = std::move(pipeline);
	return *found->second;
}

const PipelineCache::LinkedPrograms& PipelineCache::Link(const GraphicsPrograms& programs,
                                                         bool rect_list, bool sample_rate_shading,
                                                         bool                         clip_halfz,
                                                         const ShaderVertexInputInfo& vertex_info,
                                                         const ShaderPixelInputInfo* ps_input_info) {
	const auto& vertex = programs.vertex[0];
	const auto& pixel  = programs.pixel;
	const bool  ps     = ps_input_info != nullptr;
	auto [found, inserted] =
	    m_linked.try_emplace(
	        LinkKey {vertex.id, ps ? pixel.id : 0, rect_list, sample_rate_shading, clip_halfz});
	if (!inserted) {
		return found->second;
	}
	KYTY_PROFILER_BLOCK("D3D12 link shaders");
	using Kind = D3D12::LinkedStage::Kind;
	const bool mesh = vertex_info.stage.program->stage == ShaderType::Mesh;
	// The vertex stage flips Y/Z; the rect-list corners a GS adds are affine combinations of
	// flipped positions, so they are flipped as well.
	std::vector<D3D12::LinkedStage> stages {
	    {*vertex.spirv, mesh ? Kind::Mesh : Kind::Vertex, true, false, clip_halfz}};
	std::vector<uint32_t>           geometry;
	if (rect_list) {
		geometry = BuildRectListGeometryShader(vertex_info, ps_input_info);
		stages.push_back({geometry, Kind::Geometry});
	}
	if (ps) {
		stages.push_back({*pixel.spirv, Kind::Pixel, false, sample_rate_shading});
	}
	auto& linked  = found->second;
	auto  dxil    = m_compiler.CompileLinked(stages, vertex.id ^ (pixel.id * 31), &linked.from_cache);
	linked.vertex = std::move(dxil[0]);
	if (rect_list) {
		linked.geometry = std::move(dxil[1]);
	}
	if (ps) {
		linked.pixel = std::move(dxil.back());
	}
	return linked;
}

std::unique_ptr<PipelineCache::Pipeline>
PipelineCache::CreateGraphicsPipeline(const GraphicsKey& key, const ShaderVertexInputInfo& vs_input_info,
                                      const ShaderPixelInputInfo* ps_input_info,
                                      const GraphicsPrograms&     programs) {
	const auto& params    = key.state.static_params;
	const bool  mesh      = vs_input_info.stage.program->stage == ShaderType::Mesh;
	// Mesh programs expand rect lists themselves.
	const bool  rect_list = !mesh && params.topology == vk::PrimitiveTopology::ePatchList;
	const auto& linked = Link(programs, rect_list, params.sample_shading_enable,
	                          params.negative_one_to_one, vs_input_info, ps_input_info);

	auto pipeline             = std::make_unique<Pipeline>();
	pipeline->layout.graphics = true;
	NameNewPipeline(*pipeline, ps_input_info != nullptr
	                               ? fmt::format("{} {:x} ps {:x}", mesh ? "ms" : "vs",
	                                             programs.vertex[0].id, programs.pixel.id)
	                               : fmt::format("{} {:x}", mesh ? "ms" : "vs", programs.vertex[0].id));
	pipeline->trace.dxil  = {&linked.vertex};
	pipeline->trace.spirv = {programs.vertex[0].spirv};
	if (rect_list) {
		pipeline->trace.dxil.push_back(&linked.geometry);
	}
	if (ps_input_info != nullptr) {
		pipeline->trace.dxil.push_back(&linked.pixel);
		pipeline->trace.spirv.push_back(programs.pixel.spirv);
	}
	pipeline->layout.Add(*vs_input_info.stage.program,
	                     linked.vertex.requires_runtime_data ||
	                         linked.geometry.requires_runtime_data);
	if (ps_input_info != nullptr) {
		pipeline->layout.Add(*ps_input_info->stage.program, linked.pixel.requires_runtime_data);
	}
	pipeline->root_signature = GetRootSignature(pipeline->layout);
	if (pipeline->root_signature == nullptr || !linked.vertex.IsValid() || (ps_input_info != nullptr && !linked.pixel.IsValid()) || (rect_list && !linked.geometry.IsValid())) {
		return pipeline; // reported where it failed; the draws that need it are skipped
	}

	// Vertex attributes: spirv_to_dxil names the input at location N TEXCOORD<N>.
	const auto&                           vertex_input = key.state.vertex_input;
	std::vector<D3D12_INPUT_ELEMENT_DESC> elements(vertex_input.attribute_count);
	for (uint32_t index = 0; index < vertex_input.attribute_count; index++) {
		const auto& attribute = vertex_input.attributes[index];
		const auto  registers = vs_input_info.resources_dst[index].registers_num;
		const auto  compiled  = vs_input_info.stage.program->info.vertex_fetch_components[index];
		const auto  used      = compiled > 0 ? static_cast<uint32_t>(compiled)
		                                     : static_cast<uint32_t>(registers);
		vk::Format format     = vk::Format::eUndefined;
		uint32_t   size       = 0;
		GetInputFormat(vs_input_info.resources[index], format, size, used);
		if (vs_input_info.resources[index].AddTid() || vs_input_info.resources[index].SwizzleEnabled()) {
			UnsupportedOnce("vertex fetch with an index or thread id offset or a swizzle");
			return pipeline;
		}
		const bool instance = vertex_input.bindings[attribute.binding].instance;
		auto&      element  = elements[index];
		element.SemanticName         = "TEXCOORD";
		element.SemanticIndex        = index;
		element.Format               = D3D12::VertexFormat(format);
		element.InputSlot            = attribute.binding;
		element.AlignedByteOffset    = attribute.offset;
		element.InputSlotClass       = instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
		                                        : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		element.InstanceDataStepRate = instance ? 1 : 0;
		if (element.Format == DXGI_FORMAT_UNKNOWN) {
			UnsupportedOnce(fmt::format("vertex format {}", vk::to_string(format)));
			return pipeline;
		}
	}

	const auto root_layout_hash = RootLayoutHash {}(pipeline->layout);
	if (mesh) {
		MeshPipelineStream stream {};
		stream.root.value = pipeline->root_signature;
		stream.ms.value   = Bytecode(linked.vertex);
		stream.ps.value   = Bytecode(linked.pixel);
		SetOutputState(stream, key);
		const auto loaded_before = m_loaded_pipelines;
		pipeline->pipeline = LoadOrCreate(GraphicsPipelineName(stream, elements, root_layout_hash),
		                                  {sizeof(stream), &stream}, {&linked.vertex, &linked.pixel});
		CountNewPipeline(loaded_before, linked.from_cache);
		return pipeline;
	}

	GraphicsPipelineStream stream {};
	stream.root.value = pipeline->root_signature;
	stream.vs.value   = Bytecode(linked.vertex);
	stream.gs.value   = Bytecode(linked.geometry);
	stream.ps.value   = Bytecode(linked.pixel);
	SetOutputState(stream, key);
	stream.input_layout.value = {elements.data(), static_cast<UINT>(elements.size())};
	stream.strip_cut.value    = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(key.strip_cut);
	stream.topology.value     = TopologyType(params.topology);

	const auto loaded_before = m_loaded_pipelines;
	pipeline->pipeline = LoadOrCreate(GraphicsPipelineName(stream, elements, root_layout_hash),
	                                  {sizeof(stream), &stream}, {&linked.vertex, &linked.geometry, &linked.pixel});
	CountNewPipeline(loaded_before, linked.from_cache);
	return pipeline;
}

template <typename Stream>
void PipelineCache::SetOutputState(Stream& stream, const GraphicsKey& key) const {
	const auto& rendering = key.state.rendering;
	const auto& params    = key.state.static_params;

	auto& blend                  = stream.blend.value;
	blend.IndependentBlendEnable = TRUE;
	for (auto& target: blend.RenderTarget) {
		target = {FALSE,          FALSE,           D3D12_BLEND_ONE,  D3D12_BLEND_ZERO,
		          D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
		          D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
	}
	for (uint32_t i = 0; i < rendering.color_count; i++) {
		auto& target = blend.RenderTarget[i];
		// D3D12 and Vulkan share the channel bit order.
		target.RenderTargetWriteMask = static_cast<UINT8>(params.color_mask[i] & 0x0fu);
		target.BlendEnable           = params.blend_enable[i] ? TRUE : FALSE;
		target.SrcBlend  = BlendFactor(params.color_srcblend[i], false, m_alpha_blend_factor);
		target.DestBlend = BlendFactor(params.color_destblend[i], false, m_alpha_blend_factor);
		target.BlendOp   = BlendOp(params.color_comb_fcn[i]);
		const bool separate = params.separate_alpha_blend[i];
		target.SrcBlendAlpha = BlendFactor(
		    separate ? params.alpha_srcblend[i] : params.color_srcblend[i], true, false);
		target.DestBlendAlpha = BlendFactor(
		    separate ? params.alpha_destblend[i] : params.color_destblend[i], true, false);
		target.BlendOpAlpha = BlendOp(separate ? params.alpha_comb_fcn[i] : params.color_comb_fcn[i]);
		stream.render_targets.value.RTFormats[i] =
		    rendering.color_formats[i] == vk::Format::eUndefined
		        ? DXGI_FORMAT_UNKNOWN
		        : D3D12::GetFormatInfo(rendering.color_formats[i]).view;
	}
	stream.render_targets.value.NumRenderTargets = rendering.color_count;
	stream.sample_mask.value                     = UINT_MAX;

	auto& rasterizer = stream.rasterizer.value;
	rasterizer.FillMode =
	    params.polygon_mode == vk::PolygonMode::eLine ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
	if (params.polygon_mode == vk::PolygonMode::ePoint) {
		static std::atomic_bool warned = false;
		WarnOnce(warned, "point fill mode is unsupported; filling polygons");
	}
	// Culling both faces draws nothing; RenderExecutor skips such triangle draws.
	rasterizer.CullMode = params.cull_front == params.cull_back ? D3D12_CULL_MODE_NONE
	                      : params.cull_front                   ? D3D12_CULL_MODE_FRONT
	                                                            : D3D12_CULL_MODE_BACK;
	// Both APIs decide the winding in render target space with Y pointing down.
	rasterizer.FrontCounterClockwise = params.face ? FALSE : TRUE;
	rasterizer.DepthBias             = key.depth_bias;
	rasterizer.DepthBiasClamp        = key.depth_bias_clamp;
	rasterizer.SlopeScaledDepthBias  = key.slope_scaled;
	rasterizer.DepthClipEnable       = params.depth_clip_enable ? TRUE : FALSE;
	rasterizer.MultisampleEnable     = params.samples > 1 ? TRUE : FALSE;
	if (params.provoking_vtx_last) {
		static std::atomic_bool warned = false;
		WarnOnce(warned, "the last provoking vertex is unsupported; using the first");
	}

	stream.depth_stencil.value = key.depth_stencil;
	stream.depth_format.value  = DepthViewFormat(rendering);
	const bool attachments     = rendering.color_count != 0 || stream.depth_format.value != DXGI_FORMAT_UNKNOWN;
	if (attachments) {
		stream.samples.value = {params.samples, 0};
	} else {
		// Without attachments the rasterizer sample count is forced.
		stream.samples.value          = {1, 0};
		rasterizer.ForcedSampleCount = params.samples > 1 ? params.samples : 0;
	}
}

PipelineCache::Pipeline& PipelineCache::GetGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    std::span<const ShaderVertexInputInfo> vertex_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, uint32_t strip_cut, const GraphicsPrograms& programs) {
	KYTY_PROFILER_BLOCK("PipelineCache::GetGraphicsPipeline");
	const auto& vs_input_info = vertex_info.front();
	EXIT_IF(!programs.vertex[0] || (ps_input_info != nullptr && !programs.pixel));

	std::array<uint64_t, 3> vertex_ids {};
	for (uint32_t i = 0; i < programs.vertex.size(); i++) {
		vertex_ids[i] = programs.vertex[i].id;
	}
	GraphicsKey key;
	key.state = MakeGraphicsPipelineKey(m_graphics, colors, depth, vertex_info, command,
	                                    ps_input_info, topology, primitive_restart_enable,
	                                    vertex_ids, ps_input_info != nullptr ? programs.pixel.id : 0);
	key.depth_stencil = DepthStencilDesc(depth, key.state.rendering, key.state.static_params);
	key.strip_cut     = strip_cut;

	const auto& ctx         = command.GetRegisters();
	const auto& mode        = ctx.GetModeControl();
	const auto& poly_offset = ctx.GetPolyOffset();
	const bool  use_front   = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back    = mode.poly_offset_back_enable && !mode.cull_back;
	if ((use_front || use_back) && key.state.rendering.depth_format != vk::Format::eUndefined) {
		// D3D12 has one bias for both faces. Prefer a visible front face when both are enabled.
		const float constant = PolygonOffsetConstantFactor(
		    use_front ? poly_offset.front_offset : poly_offset.back_offset, poly_offset,
		    depth.desc.view_info.format);
		key.depth_bias       = static_cast<int32_t>(std::lround(constant));
		key.depth_bias_clamp = poly_offset.clamp;
		key.slope_scaled = (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
	}

	Common::LockGuard lock(m_mutex);
	if (auto found = m_graphics_pipelines.find(key); found != m_graphics_pipelines.end()) {
		return *found->second;
	}
	auto pipeline = CreateGraphicsPipeline(key, vs_input_info, ps_input_info, programs);
	auto [found, inserted] = m_graphics_pipelines.emplace(key, std::move(pipeline));
	EXIT_IF(!inserted);
	return *found->second;
}

void PipelineCache::NameNewPipeline(Pipeline& pipeline, std::string description) {
	description = fmt::format("pipeline {}: {}", ++m_pipeline_count, description);
	pipeline.trace.name.assign(description.begin(), description.end());
}

PipelineCache::Statistics PipelineCache::GetStatistics() const {
	// The app's UI asks for these while a pipeline may be compiling for many seconds with the
	// lock held (the Xbox ends a game whose UI stops responding): it gets the last numbers then.
	std::lock_guard statistics_lock(m_statistics_mutex);
	if (m_mutex.TryLock()) {
		m_statistics = {m_shaders.size(), m_compute_pipelines.size(), m_graphics_pipelines.size(),
		                m_root_signatures.size(), m_loaded_pipelines, m_cached_pipelines,
		                m_saved_pipelines};
		const auto programs = m_programs.GetProgramCounts();
		for (size_t stage = 0; stage < programs.size(); stage++) {
			(stage == static_cast<size_t>(ShaderType::Compute) ? m_statistics.compute_shaders
			                                                   : m_statistics.drawing_shaders) +=
			    programs[stage];
		}
		m_mutex.Unlock();
	}
	return m_statistics;
}

void PipelineCache::CountNewPipeline(uint64_t loaded_before, bool translated_from_cache) {
	if (m_loaded_pipelines != loaded_before || translated_from_cache) {
		m_cached_pipelines++;
	}
}

} // namespace Libs::Graphics
