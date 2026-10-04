#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PIPELINECACHE_H_

#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"
#include "graphics/shader/programCache.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

struct ID3D12RootSignature;
struct ID3D12PipelineState;

namespace Libs::Graphics {

struct GraphicContext;

// Resources a pipeline binds, in root signature order: root constants (push data), a root CBV
// (spirv_to_dxil runtime data), a CBV/SRV/UAV table and a sampler table.
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

	bool operator==(const RootLayout&) const = default;

	void Add(const ShaderRecompiler::IR::CompiledShaderInfo& program, bool program_runtime_data);
};

class PipelineCache {
public:
	explicit PipelineCache(GraphicContext& graphics);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);

	void Save() {}

	// A guest program translated to DXIL.
	struct Shader {
		uint64_t          id    = 0;
		ShaderType        stage = ShaderType::Unknown;
		D3D12::DxilShader dxil;
	};

	struct GraphicsPrograms {
		std::array<const Shader*, 3> vertex {};
		const Shader*                pixel = nullptr;
	};

	struct Pipeline {
		RootLayout           layout;
		ID3D12RootSignature* root_signature = nullptr;
		ID3D12PipelineState* pipeline       = nullptr;
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

	struct Statistics {
		uint64_t shaders           = 0;
		uint64_t compute_pipelines = 0;
		uint64_t root_signatures   = 0;
	};
	[[nodiscard]] Statistics GetStatistics() const;
	[[nodiscard]] const D3D12::DxilCompiler& GetCompiler() const noexcept { return m_compiler; }

private:
	struct RootLayoutHash {
		std::size_t operator()(const RootLayout& layout) const;
	};

	const Shader*        Translate(const CompiledProgram& program, ShaderType stage,
	                               bool last_vertex_stage);
	ID3D12RootSignature* GetRootSignature(const RootLayout& layout);

	GraphicContext&                                                     m_graphics;
	ShaderProgramCache                                                  m_programs;
	D3D12::DxilCompiler                                                 m_compiler;
	std::unordered_map<uint64_t, std::unique_ptr<Shader>>               m_shaders;
	std::unordered_map<RootLayout, ID3D12RootSignature*, RootLayoutHash> m_root_signatures;
	std::unordered_map<uint64_t, std::unique_ptr<Pipeline>>             m_compute_pipelines;
	mutable Common::Mutex                                               m_mutex;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PIPELINECACHE_H_
