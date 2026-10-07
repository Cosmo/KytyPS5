#ifndef EMULATOR_SRC_GRAPHICS_SHADER_PROGRAMCACHE_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_PROGRAMCACHE_H_

#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class Shader;
class UserConfig;
struct ComputeShaderInfo;
struct PixelShaderInfo;
struct ShaderRegisters;
struct VertexShaderInfo;
} // namespace HW

namespace ShaderRecompiler::IR {
class ImageHeapSource;
} // namespace ShaderRecompiler::IR

// Host GPU capabilities that affect guest shader translation.
struct HostShaderLimits {
	bool     compute_wave64         = false;
	uint32_t subgroup_size          = 0;
	uint32_t max_viewport_width     = 0;
	uint32_t max_viewport_height    = 0;
	bool     mesh_shader_enabled    = false;
	uint32_t max_mesh_invocations   = 0;
	uint32_t max_mesh_group_size_x  = 0;
	uint32_t max_mesh_vertices      = 0;
	uint32_t max_mesh_primitives    = 0;
	uint32_t max_mesh_shared_memory = 0;
	uint32_t max_compute_shared_memory = 0;
	// Vulkan's maintenance8: texture samples may take offsets the shader computes.
	bool     maintenance8           = false;
	// The device's own float min/max give the guest's NaN and signed-zero results (AMD GPUs).
	bool     native_float_min_max   = false;
	// The shader-readable device clock's tick in picoseconds; 0 without one.
	uint32_t device_clock_period_ps = 0;
};

// A translated guest shader: a stable id plus its SPIR-V. Backends create their shader objects
// from the SPIR-V once per id.
struct CompiledProgram {
	uint64_t                                     id = 0;
	std::shared_ptr<const std::vector<uint32_t>> spirv;

	explicit operator bool() const { return id != 0 && spirv != nullptr; }
};

// A compiled stage (ShaderStageRuntime::module): the program plus what its translation found and
// the resources it was specialized for.
struct ShaderModule: CompiledProgram {
	ShaderRecompiler::IR::CompiledShaderInfo     info;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	std::vector<uint32_t>                        function_code;
};

// Translates guest shaders from their registers to SPIR-V and caches the results per guest
// shader, static state and resource specialization. Shared by all host GPU backends.
class ShaderProgramCache {
public:
	// image_heap keeps GPU-indexed image tables resident; nullptr: the backend has none.
	ShaderProgramCache(const HostShaderLimits& limits, ShaderRecompiler::IR::ImageHeapSource* image_heap);
	~ShaderProgramCache();
	KYTY_CLASS_NO_COPY(ShaderProgramCache);

	struct GraphicsPrograms {
		std::array<CompiledProgram, 3> vertex;
		CompiledProgram                pixel;

		[[nodiscard]] uint32_t VertexStageCount() const { return vertex[1] ? 3u : 1u; }
	};

	// Translates the bound shaders into the stages' input infos; also gives whether the pixel
	// program runs and the color targets it writes (mrt_mask).
	GraphicsPrograms GetGraphicsPrograms(const HW::Shader& shaders, const HW::Context& context,
	                                     const HW::UserConfig&                 user_config,
	                                     std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                                     ShaderPixelInputInfo& pixel_info, bool& pixel_active,
	                                     uint32_t& mrt_mask);
	// The programs compiled so far by ShaderType, as the log's "Shaders:" line counts them.
	using ProgramCounts = std::array<size_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1>;
	[[nodiscard]] ProgramCounts GetProgramCounts() const;

	// An empty program skips the dispatch.
	CompiledProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                  ShaderComputeInputInfo&      input_info);

private:
	struct Impl;

	HostShaderLimits      m_limits;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_PROGRAMCACHE_H_
