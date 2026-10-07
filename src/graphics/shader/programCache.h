#ifndef EMULATOR_SRC_GRAPHICS_SHADER_PROGRAMCACHE_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_PROGRAMCACHE_H_

#include "common/common.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/shader.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
struct ComputeShaderInfo;
struct PixelShaderInfo;
struct ShaderRegisters;
struct VertexShaderInfo;
} // namespace HW

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
};

// A translated guest shader: a stable id plus its SPIR-V. Backends create their shader objects
// from the SPIR-V once per id.
struct CompiledProgram {
	uint64_t                                     id = 0;
	std::shared_ptr<const std::vector<uint32_t>> spirv;

	explicit operator bool() const { return id != 0 && spirv != nullptr; }
};

// Translates guest shaders from their registers to SPIR-V and caches the results per guest
// shader, static state and resource specialization. Shared by all host GPU backends.
class ShaderProgramCache {
public:
	explicit ShaderProgramCache(const HostShaderLimits& limits);
	~ShaderProgramCache();
	KYTY_CLASS_NO_COPY(ShaderProgramCache);

	struct GraphicsPrograms {
		std::array<CompiledProgram, 3> vertex;
		CompiledProgram                pixel;

		[[nodiscard]] uint32_t VertexStageCount() const { return vertex[1] ? 3u : 1u; }
	};

	GraphicsPrograms
	GetGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                    const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                    const HW::Context& context, const HW::UserConfig& user_config,
	                    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                    ShaderPixelInputInfo& pixel_info);
	CompiledProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                  const HW::ShaderRegisters&   sh,
	                                  ShaderComputeInputInfo&      input_info);

private:
	struct Impl;

	HostShaderLimits      m_limits;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_PROGRAMCACHE_H_
