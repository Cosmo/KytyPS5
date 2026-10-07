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
	// The shaders cannot index the descriptor heap (shader model below 6.6, the Xbox's UWP games), so
	// guest memory is read through a table of the buffers' persistent descriptors, an unbounded array
	// of storage buffers (IR::GuestBufferTableSpace) indexed by the descriptor slot that the page
	// table gives, instead of through physical storage buffer pointers.
	bool guest_pointer_table = false;
	// WaveReadLaneAt needs the same lane number in every lane of the wave below shader model 6.7 (the Xbox's UWP games run at 6.4): with a number that differs from lane to
	// lane (DS_BPERMUTE, DS_SWIZZLE, the PERMLANEs, read lane with a VGPR) the Series X gives wrong values (measured by the graphics lab: about 1000 of 1024 wrong). The
	// emitter then reads each source lane in turn, with the loop counter as the (uniform) lane number, and the lane that asked for it keeps the value.
	bool lane_shuffle_loop = false;
	// A pixel shader's wave operations leave out its helper lanes below shader model 6.7 (the Xbox's UWP games). A read of the first lane
	// then uses the native instruction where every lane runs, and a helper lane that the ballot leaves out keeps its own value: in a
	// waterfall loop it would otherwise wait forever (a GPU hang on the Xbox).
	bool wave_ops_without_helper_lanes = false;
	// The BDA page table has two levels (BdaLayout, the D3D12 build: 32 MB instead of the flat table's 768 MB, for the Xbox's GPU
	// memory budget): a guest address's lookup reads the directory first.
	bool bda_two_level = false;
};

void                                    SetHostShaderFeatures(const HostShaderFeatures& features);
[[nodiscard]] const HostShaderFeatures& GetHostShaderFeatures();

} // namespace Libs::Graphics::ShaderRecompiler

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_HOSTSHADERFEATURES_H_
