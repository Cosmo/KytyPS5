#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Resets and writes backend IDs and descriptor indices; do not emit the same program concurrently.
std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t push_data_start_dword = 0,
                                  bool maintenance8_enabled = false);

// The vertex stage that goes with a mesh program run as a compute shader (IR::MeshEmulation):
// it reads the records the program wrote and outputs what the program exported.
std::vector<uint32_t> EmitMeshFetchProgram(std::span<const IR::StageOutput> outputs);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */
