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
	// emitter then reads each source lane in turn, with the loop counter as the (uniform) lane number, and the lane that asked for it keeps the value: only
	// the lanes the instruction can ask for, up to the host's wave size. Where every lane asks for the same lane (read lane, read first lane, append)
	// it uses the shuffle as it is, and for a shuffle within quads (quad_perm, DS_SWIZZLE's quad mode) the quad operations of pixel and compute shaders.
	bool lane_shuffle_loop = false;
	// A pixel shader's wave operations leave out its helper lanes below shader model 6.7 (the Xbox's UWP games). A read of the first lane
	// then uses the native instruction where every lane runs, and a helper lane that the ballot leaves out keeps its own value: in a
	// waterfall loop it would otherwise wait forever (a GPU hang on the Xbox).
	bool wave_ops_without_helper_lanes = false;
	// A sample's texel offset must be a constant without Vulkan's maintenance8 (D3D12: spirv_to_dxil below shader model 6.7). With this, an
	// offset the shader computes moves the sample's coordinates instead of stopping the game (the emitter's MoveCoordinatesByOffset).
	bool sample_offset_by_coordinates = false;
	// The BDA page table has two levels (BdaLayout, the D3D12 build: 32 MB instead of the flat table's 768 MB, for the Xbox's GPU
	// memory budget): a guest address's lookup reads the directory first.
	bool bda_two_level = false;
	// 64-bit image atomics, which need shader model 6.6 in D3D12 (not the Xbox's UWP games), run under a lock: the image is a
	// two-dword image, and each atomic takes a lock word of a table in the guest buffer table (IR::ImageAtomic64LockSlot), reads
	// the texel, writes the result and releases the lock. Exact; slower where many lanes hit one texel.
	bool image_atomic64_lock = false;
	// 64-bit atomics on buffers and group shared memory, which need shader model 6.6 in D3D12 too, run under a lock of the same table:
	// the lane that gets it reads both dwords with 32-bit atomics, writes the result and releases the lock.
	bool int64_atomics_lock = false;
	// Vertex inputs in formats the host's input assembler cannot read come in as integers, and the vertex shader converts them
	// (D3D12 has no USCALED/SSCALED formats and no 10:10:10:2 SNORM vertex format): scaled formats as their integers, 10:10:10:2
	// SNORM as its unsigned components, sign extended and normalized as Vulkan does.
	bool vertex_formats_as_integers = false;
	// Samplers with unnormalized coordinates (texel positions), which D3D12 has none of: the shader divides the coordinates by the
	// size of the image's first level (ResourceSpecialization::unnormalized_samplers) and samples with a normalized sampler.
	bool unnormalized_samplers_in_shader = false;
	// Comparison samples with a bias or gradients, which D3D12 has from shader model 6.8 only (spirv_to_dxil below it drops the
	// comparison and returns the depth): pixel shaders sample them at the level the hardware picks, other stages at level 0. Exact for
	// depth textures of one level, the usual shadow maps; with more levels the bias or gradients no longer move the level.
	bool sample_compare_without_bias_grad = false;
	// Storage images of one or two dimensions are declared as arrays of them and written at layer 0 of the view: a D3D12 storage
	// view of a plain 1D or 2D image starts at the image's first layer, and games write later layers of arrays through such views
	// (the host's view is then an array view of the one layer).
	bool storage_images_as_arrays = false;
};

void                                    SetHostShaderFeatures(const HostShaderFeatures& features);
[[nodiscard]] const HostShaderFeatures& GetHostShaderFeatures();

} // namespace Libs::Graphics::ShaderRecompiler

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_HOSTSHADERFEATURES_H_
