#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_DRAWSTATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_DRAWSTATE_H_

#include "graphics/guest_gpu/gpu_defs.h"

#include <array>
#include <cstdint>

// Draw decisions derived from guest registers only, shared by all host GPU backends.

namespace Libs::Graphics {

namespace HW {
class Context;
class Shader;
struct ShaderRegisters;
} // namespace HW

inline constexpr uint32_t GuestColorTargetCount = 8;

[[nodiscard]] bool DrawHasValidVertexShader(const HW::Shader& shaders);
[[nodiscard]] bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs);
// Bit per color target the pixel shader exports to and the guest enables.
[[nodiscard]] uint32_t DrawColorOutputMask(const HW::Context& registers);
// The pixel shader runs if it exports color to an enabled target or has depth/coverage effects.
[[nodiscard]] bool DrawHasActivePixelShader(const HW::Context& registers, const HW::Shader& shaders);
// Export mapping of each enabled color target's format; unused slots keep the default mapping.
[[nodiscard]] std::array<Prospero::ColorComponentMapping, GuestColorTargetCount>
RenderTargetExportMapping(const HW::Context& registers);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_DRAWSTATE_H_
