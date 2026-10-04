#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SELFTEST_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SELFTEST_H_

namespace Libs::Graphics::D3D12 {

// A diagnostic that needs no guest: on a device of its own, translates compute shaders the renderer has built in from SPIR-V to DXIL, signs them with
// the DXIL validator and creates compute pipelines, logging each step and the time it took. Returns whether every shader made it to a pipeline. It
// runs before a game starts when the app's settings ask for it, so that a console can show whether the shader route works there.
bool RunShaderSelfTest(bool debug_layer);

} // namespace Libs::Graphics::D3D12

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SELFTEST_H_
