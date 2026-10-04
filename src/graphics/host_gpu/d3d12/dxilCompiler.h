#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DXILCOMPILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DXILCOMPILER_H_

#include "common/common.h"
#include "common/threads.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ID3D12Device;
struct IDxcValidator;

namespace Libs::Graphics::D3D12 {

// Register spaces outside the range the SPIR-V emitter uses for guest resources
// (one space per ShaderRecompiler::IR::NativeBinding).
inline constexpr uint32_t PushConstantSpace = 1000;
inline constexpr uint32_t RuntimeDataSpace  = 1001;
// Root constants holding spirv_to_dxil's runtime data (dxil_spirv_vertex_runtime_data, 9 dwords,
// or dxil_spirv_compute_runtime_data, 7 dwords). Constants avoid tying a buffer's state to it.
inline constexpr uint32_t RuntimeDataDwords = 12;
// Added to the space of a sampler array for its comparison-sampler declaration, which a shader
// that also samples through it regularly needs besides the regular one. Guest spaces are below.
inline constexpr uint32_t ComparisonSamplerSpaceOffset = 500;

struct SpecializationConstant {
	uint32_t id    = 0;
	uint32_t value = 0;
};

// A stage of a graphics pipeline compiled by DxilCompiler::CompileLinked.
struct LinkedStage {
	enum class Kind : uint8_t { Vertex, Geometry, Pixel, Mesh };

	std::span<const uint32_t> spirv;
	Kind                      kind = Kind::Vertex;
	// Enables the per-draw Y/Z flip of clip-space positions.
	bool                      flip = false;
	// Runs a pixel stage once per sample.
	bool                      sample_rate_shading = false;
	// Maps OpenGL clip-space depth ([-w, w]) to D3D's ([0, w]).
	bool                      clip_halfz = false;
};

// spirv_to_dxil's runtime data of vertex stages (dxil_spirv_vertex_runtime_data).
struct VertexRuntimeData {
	uint32_t first_vertex    = 0; // the vertex offset of indexed draws
	uint32_t base_instance   = 0;
	uint32_t is_indexed_draw = 0;
	uint32_t yz_flip_mask    = 0; // Y flips in bits 0-15, Z flips in bits 16-31, per viewport
	uint32_t draw_id         = 0;
	float    viewport_width  = 0.0f;
	float    viewport_height = 0.0f;
	uint32_t view_index      = 0;
	float    depth_bias      = 0.0f;
};
inline constexpr uint32_t ZFlipShift = 16;

// Saves shaders for reproducing them with spirv2dxil, in _ShaderDumps relative to the working
// directory: <name>.<n>.dxil and <name>.<n>.spv. Returns the folder.
std::string SaveShaderDump(const std::string& name, std::span<const std::span<const uint8_t>> dxil,
                           std::span<const std::span<const uint32_t>> spirv);

struct DxilShader {
	// Empty when the shader could not be translated or did not pass validation (reported once in the log); a pipeline with such a shader is not
	// created and the draws and dispatches that use it are skipped.
	std::vector<uint8_t> bytecode;
	[[nodiscard]] bool   IsValid() const noexcept { return !bytecode.empty(); }
	// The shader reads spirv_to_dxil's runtime data CBV (RuntimeDataSpace, b0).
	bool requires_runtime_data = false;
	bool needs_draw_sysvals    = false;
};

// Translates the SPIR-V produced by the shared shader recompiler to signed DXIL. Translations
// are kept by SPIR-V content and options; PipelineCache saves them with its pipelines, so a
// later run skips translation and validation.
class DxilCompiler {
public:
	explicit DxilCompiler(ID3D12Device* device);
	~DxilCompiler();
	KYTY_CLASS_NO_COPY(DxilCompiler);

	// `last_vertex_stage` enables the per-draw Y/Z flip of clip-space positions. `from_cache`
	// tells whether the translation came from the saved cache (LoadCache).
	[[nodiscard]] DxilShader Compile(std::span<const uint32_t> spirv, ShaderType stage,
	                                 uint64_t shader_hash, bool last_vertex_stage,
	                                 std::span<const SpecializationConstant> constants = {},
	                                 bool* from_cache = nullptr) const;
	// Compiles the stages of one graphics pipeline, given in pipeline order, with their
	// interfaces linked so the DXIL signatures of consecutive stages match.
	[[nodiscard]] std::vector<DxilShader> CompileLinked(std::span<const LinkedStage> stages,
	                                                    uint64_t pipeline_hash,
	                                                    bool*    from_cache = nullptr) const;

	// Mesh shaders need shader model 6.5.
	[[nodiscard]] bool SupportsMeshShaders() const noexcept;

	// Identifies the translator: saved translations are only valid for the same one.
	[[nodiscard]] std::string CacheSignature() const;
	// Adds saved translations; false when `data` is malformed (nothing is added then).
	bool                               LoadCache(std::span<const uint8_t> data);
	[[nodiscard]] std::vector<uint8_t> SaveCache() const;

private:
	[[nodiscard]] DxilShader Translate(std::span<const uint32_t> spirv, ShaderType stage,
	                                   uint64_t shader_hash, bool last_vertex_stage,
	                                   std::span<const SpecializationConstant> constants) const;
	[[nodiscard]] std::vector<DxilShader> TranslateLinked(std::span<const LinkedStage> stages,
	                                                      uint64_t pipeline_hash) const;
	// `sources`: the SPIR-V the shader came from, saved with it when validation fails.
	void Sign(DxilShader& shader, uint64_t shader_hash,
	          std::span<const std::span<const uint32_t>> sources) const;

	uint32_t       m_shader_model      = 0; // enum dxil_shader_model
	uint32_t       m_validator_version = 0; // enum dxil_validator_version
	IDxcValidator* m_validator         = nullptr;
	void*          m_dxil_module       = nullptr;
	uint64_t       m_translator_hash   = 0; // of the spirv_to_dxil library file

	mutable Common::Mutex                                         m_cache_mutex;
	mutable std::unordered_map<uint64_t, std::vector<DxilShader>> m_cache;
	std::unordered_set<uint64_t>                                  m_loaded; // keys LoadCache added
};

} // namespace Libs::Graphics::D3D12

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DXILCOMPILER_H_
