#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_HOSTSHADERFEATURES_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_HOSTSHADERFEATURES_H_

namespace Libs::Graphics::ShaderRecompiler {

// What the host GPU's shaders can do, where it changes the SPIR-V the recompiler emits. The
// backend sets it when it creates its device, before any shader is compiled.
struct HostShaderFeatures {
	// Fragment shader barycentrics (SPV_KHR_fragment_shader_barycentric, SV_Barycentrics), which
	// the Xbox's UWP games don't have. Without them, a pixel shader interpolating its inputs itself
	// from their vertex values (v_interp_mov: P0 + i * P10 + j * P20) gets the interpolated value
	// as P0 and zeros as P10 and P20: the same result for linear interpolation. Reads of the
	// barycentrics (i, j) themselves give zeros.
	bool barycentrics = true;
};

void                                    SetHostShaderFeatures(const HostShaderFeatures& features);
[[nodiscard]] const HostShaderFeatures& GetHostShaderFeatures();

} // namespace Libs::Graphics::ShaderRecompiler

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_HOSTSHADERFEATURES_H_
