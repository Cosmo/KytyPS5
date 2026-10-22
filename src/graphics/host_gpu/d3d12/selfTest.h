#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SELFTEST_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SELFTEST_H_

namespace Libs::Graphics {
struct GraphicContext;
} // namespace Libs::Graphics

namespace Libs::Graphics::D3D12 {

class DxilCompiler;

// A diagnostic that needs no guest: on a device of its own, translates compute shaders the renderer has built in from SPIR-V to DXIL, signs them with
// the DXIL validator and creates compute pipelines, logging each step and the time it took. Returns whether every shader made it to a pipeline. It
// runs before a game starts when the app's settings ask for it, so that a console can show whether the shader route works there.
bool RunShaderSelfTest(bool debug_layer);

// Whether the device's copies place the layers of block-compressed texture arrays where its texture unit reads them: a small BC7 array (16x16,
// 3 mips, 32 layers, one solid colour per layer and mip) is uploaded from a buffer and read back by a compute shader. The Series X fails: there,
// layers whose mip level is smaller than 64 KB are packed differently by copies and by the texture unit.
bool BlockArrayCopiesWork(GraphicContext& graphics);

// Whether the device's float min/max, as the translated shaders use them (Profile::native_float_min_max), give the guest's results: a NaN operand
// yields the other one (the first for two NaNs, with its bits) and -0 orders below +0. Runs the cases of upstream's VectorMinMaxF32RuntimeEdges on
// runtime operands and logs the first wrong result.
bool NativeFloatMinMaxExact(GraphicContext& graphics, const DxilCompiler& compiler);

} // namespace Libs::Graphics::D3D12

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_SELFTEST_H_
