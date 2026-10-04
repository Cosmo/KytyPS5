#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PIPELINECACHE_H_

#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"
#include "graphics/host_gpu/d3d12/gpuTrace.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineState.h"
#include "graphics/shader/programCache.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct RenderColorInfo;
struct RenderDepthInfo;

// Resources a pipeline binds, in root signature order: root constants (push data), root
// constants (spirv_to_dxil runtime data), a CBV/SRV/UAV table and a sampler table.
struct RootLayout {
	enum class RangeType : uint8_t { Srv, Uav, Sampler };
	struct Range {
		uint32_t  space = 0; // ShaderRecompiler::IR::NativeBinding of the guest resource kind
		uint32_t  count = 0;
		RangeType type  = RangeType::Srv;

		bool operator==(const Range&) const = default;
	};

	std::vector<Range> views;
	std::vector<Range> samplers;
	bool               push_constants = false;
	bool               runtime_data   = false;
	bool               graphics       = false;
	// The programs read guest memory by address through bindless descriptors (BdaPageTable).
	bool               heap_indexing  = false;

	bool operator==(const RootLayout&) const = default;

	// The root parameter of the runtime data, which follows the push constants.
	[[nodiscard]] uint32_t RuntimeDataParameter() const noexcept { return push_constants ? 1 : 0; }

	void Add(const ShaderRecompiler::IR::CompiledShaderInfo& program, bool program_runtime_data);
};

// Whether the root signature needs push constants for `program`: its push data, or the draw
// parameters of a mesh program, which come first.
[[nodiscard]] bool UsesPushConstants(const ShaderRecompiler::IR::CompiledShaderInfo& program);

// Whether `program` samples with depth comparison through the samplers of `binding`; the layout
// then has a second sampler range for them (D3D12::ComparisonSamplerSpaceOffset).
[[nodiscard]] bool HasComparisonSamplers(const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                         const ShaderRecompiler::IR::DescriptorBinding&  binding);

class PipelineCache {
public:
	explicit PipelineCache(GraphicContext& graphics);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);

	// Writes translated shaders and compiled pipelines to the title's cache file, which the next
	// run starts from (the D3D12 counterpart of the Vulkan pipeline cache), when there are new ones.
	void Save();
	// Called every frame: saves, in the background, once no new pipeline was compiled for a few
	// seconds, so the file is current even when the app isn't closed properly (on the Xbox, it may
	// be ended any time).
	void SaveWhenIdle();

	// A guest compute program translated to DXIL.
	struct Shader {
		uint64_t          id    = 0;
		ShaderType        stage = ShaderType::Unknown;
		D3D12::DxilShader dxil;
		bool              from_cache = false; // translated by an earlier run

		explicit operator bool() const { return id != 0; }
	};

	// Guest graphics programs as SPIR-V. They become DXIL together, per pipeline, so the
	// interfaces between their stages match.
	using GraphicsPrograms = ShaderProgramCache::GraphicsPrograms;

	struct Pipeline {
		RootLayout           layout;
		ID3D12RootSignature* root_signature = nullptr;
		ID3D12PipelineState* pipeline       = nullptr;
		// For the GPU trace: a device removal report names the pipeline the GPU stopped in and
		// saves its shaders.
		D3D12::TracedPipeline trace;
	};

	GraphicsPrograms
	GetGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                    const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                    const HW::Context& context, const HW::UserConfig& user_config,
	                    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                    ShaderPixelInputInfo& pixel_info);
	const Shader& GetComputeProgram(const HW::ComputeShaderInfo& regs, const HW::ShaderRegisters& sh,
	                                ShaderComputeInputInfo& input_info);
	Pipeline&     GetComputePipeline(const ShaderComputeInputInfo& input_info, const Shader& shader);
	// `strip_cut` is the index that restarts strips (a D3D12_INDEX_BUFFER_STRIP_CUT_VALUE).
	Pipeline& GetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              uint32_t strip_cut, const GraphicsPrograms& programs);

	struct Statistics {
		uint64_t shaders            = 0;
		uint64_t compute_pipelines  = 0;
		uint64_t graphics_pipelines = 0;
		uint64_t root_signatures    = 0;
		uint64_t loaded_pipelines   = 0; // taken from the saved pipeline library
		// Made from the saved cache: taken from the pipeline library, or (where there is none,
		// the Xbox) with shaders translated before.
		uint64_t cached_pipelines   = 0;
		// In the cache file: the pipelines it had at the start, plus those made since without it,
		// once saved.
		uint64_t saved_pipelines    = 0;
		// The game's programs compiled so far (ShaderProgramCache::GetProgramCounts): compute, and
		// the drawing stages (vertex, pixel and the others).
		uint64_t compute_shaders    = 0;
		uint64_t drawing_shaders    = 0;
	};
	[[nodiscard]] Statistics GetStatistics() const;

	[[nodiscard]] const D3D12::DxilCompiler& GetCompiler() const noexcept { return m_compiler; }

private:
	struct RootLayoutHash {
		std::size_t operator()(const RootLayout& layout) const;
	};

	// The shared pipeline state plus the state D3D12 fixes in pipelines rather than in commands.
	struct GraphicsKey {
		GraphicsPipelineKey      state;
		D3D12_DEPTH_STENCIL_DESC1 depth_stencil {};
		int32_t                  depth_bias       = 0;
		float                    depth_bias_clamp = 0.0f;
		float                    slope_scaled     = 0.0f;
		uint32_t                 strip_cut        = 0;

		bool operator==(const GraphicsKey& other) const;
	};
	struct GraphicsKeyHash {
		std::size_t operator()(const GraphicsKey& key) const;
	};

	// The DXIL of a vertex (or mesh)/pixel program pair, linked; stages without bytecode are
	// absent.
	struct LinkedPrograms {
		D3D12::DxilShader vertex;
		D3D12::DxilShader geometry;
		D3D12::DxilShader pixel;
		bool              from_cache = false; // translated by an earlier run
	};
	// Vertex and pixel program ids, rect list, sample-rate shading, OpenGL clip-space depth.
	using LinkKey = std::tuple<uint64_t, uint64_t, bool, bool, bool>;

	const Shader*         Translate(const CompiledProgram& program, ShaderType stage);
	const LinkedPrograms& Link(const GraphicsPrograms& programs, bool rect_list,
	                           bool sample_rate_shading, bool clip_halfz,
	                           const ShaderVertexInputInfo& vertex_info,
	                           const ShaderPixelInputInfo*  ps_input_info);
	ID3D12RootSignature*  GetRootSignature(const RootLayout& layout);
	std::unique_ptr<Pipeline> CreateGraphicsPipeline(const GraphicsKey& key,
	                                                 const ShaderVertexInputInfo& vs_input_info,
	                                                 const ShaderPixelInputInfo* ps_input_info,
	                                                 const GraphicsPrograms&     programs);
	// Sets the state of a graphics pipeline stream after its shaders: blending, rasterization,
	// depth-stencil and the attachment formats.
	template <typename Stream>
	void SetOutputState(Stream& stream, const GraphicsKey& key) const;
	// Loads the pipeline `name` names from the pipeline library, or creates and stores it.
	ID3D12PipelineState* LoadOrCreate(const std::wstring&                    name,
	                                  const D3D12_PIPELINE_STATE_STREAM_DESC& desc,
	                                  std::initializer_list<const D3D12::DxilShader*> shaders);
	void                 InitializeDiskCache();
	[[nodiscard]] std::string CacheSignature() const;

	GraphicContext&                                                     m_graphics;
	D3D12::ComPtr<ID3D12Device2>                                        m_device;
	bool                                                                m_alpha_blend_factor = false;
	uint32_t                                                            m_rejected_pipelines = 0;
	D3D12::DxilCompiler                                                 m_compiler;
	ShaderProgramCache                                                  m_programs;
	std::unordered_map<uint64_t, std::unique_ptr<Shader>>               m_shaders;
	void NameNewPipeline(Pipeline& pipeline, std::string description);
	// After a new pipeline was made: counts it when it came from the saved cache (Statistics).
	void CountNewPipeline(uint64_t loaded_before, bool translated_from_cache);

	std::map<LinkKey, LinkedPrograms>                                   m_linked;
	uint32_t                                                            m_pipeline_count = 0;
	std::unordered_map<RootLayout, ID3D12RootSignature*, RootLayoutHash> m_root_signatures;
	std::unordered_map<uint64_t, std::unique_ptr<Pipeline>>             m_compute_pipelines;
	std::unordered_map<GraphicsKey, std::unique_ptr<Pipeline>, GraphicsKeyHash>
	                      m_graphics_pipelines;
	PipelineCacheFile     m_cache_file;
	// Compiled pipelines by name; they are read from m_library_data, which must outlive it.
	std::vector<uint8_t>                  m_library_data;
	D3D12::ComPtr<ID3D12PipelineLibrary1> m_library;
	uint64_t                              m_loaded_pipelines = 0;
	// Pipelines compiled since the start, when the last one was, and how many the file has.
	std::atomic_uint64_t                  m_changes {0};
	std::atomic_uint64_t                  m_last_change_ms {0};
	std::atomic_uint64_t                  m_saved_changes {0};
	// The save SaveWhenIdle started, in the background.
	std::thread                           m_save_thread;
	std::atomic_bool                      m_saving {false};
	mutable Common::Mutex                 m_mutex;
	// GetStatistics' last numbers, for while m_mutex is held.
	mutable std::mutex                    m_statistics_mutex;
	mutable Statistics                    m_statistics;
	// Pipelines made since the start that came from the saved cache (Statistics).
	uint64_t                              m_cached_pipelines = 0;
	// The cache file's pipelines, at the start and after the last save (Statistics).
	uint64_t                              m_file_pipelines  = 0;
	uint64_t                              m_saved_pipelines = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PIPELINECACHE_H_
