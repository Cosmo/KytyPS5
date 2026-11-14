#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_TESSEMULATION_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_TESSEMULATION_H_

#include "graphics/shader/shader.h"

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

// A host whose tessellator is not used (the Series X's emits no domain points) runs a tessellated draw in four passes:
//  1. the capture draw: the local program (LS) as the vertex stage of the guest's draw, as points that rasterize nothing; its outputs go
//     to the capture buffer, one slot per vertex;
//  2. the hull pass: the control program (HS) as a compute shader, one workgroup of output-control-point invocations per patch; its
//     control points, patch constants and tessellation factors go to the patch's record;
//  3. the tessellator: compute kernels make each patch's domain points and triangles from its factors (the D3D11 reference tessellator);
//  4. the domain draw: the evaluation program (TES) as the vertex stage of an indirect indexed draw of those triangles, each vertex
//     reading its domain point and its patch's record; then the guest's pixel stage as usual.
// Instances are folded into patches: patch g of the draw is patch g % PatchCount of instance g / PatchCount.
struct TessEmulation {
	// Register spaces of its buffers (root descriptors), outside the guest resource spaces and the mesh emulation's.
	static constexpr uint32_t ParamSpace   = 1004; // t0, all three stages: the draw's parameters (Param)
	static constexpr uint32_t CaptureSpace = 1005; // u0 in the capture draw, t0 in the hull pass: the LS outputs per slot
	static constexpr uint32_t SlotSpace    = 1006; // t0 in the hull pass: an indexed draw's capture slot per control point position
	static constexpr uint32_t RecordSpace  = 1007; // u0 in the hull pass, t0 in the domain draw: the patch records
	static constexpr uint32_t PointSpace   = 1008; // t0 in the domain draw: the domain points

	// The parameters, dwords of the parameter buffer.
	enum Param : uint32_t {
		CaptureBase,     // the vertex index of capture slot 0 (indexed: the smallest index plus the vertex offset; else the first vertex)
		CaptureVertices, // slots per instance
		BaseInstance,    // the draw's first instance
		PatchCount,      // patches per instance
		Indexed,         // nonzero: the slot buffer gives each control point's slot; else the slot is its position in the draw
		GroupsX,         // the hull dispatch's width in workgroups (patch g runs in group (g % GroupsX, g / GroupsX))
		PatchTotal,      // the patches of all instances
		ParamCount
	};

	// A patch record: the tessellation factors as the HS wrote them (outer then inner, 6 used), the control points (hs_stride bytes
	// each), then the patch constants (patch_size bytes, from patch_base).
	static constexpr uint32_t FactorDwords = 8;

	[[nodiscard]] static uint32_t ControlPointDwords(const ShaderTessellationInputInfo& tess) { return tess.hs_stride / 4u; }
	[[nodiscard]] static uint32_t PatchConstantDword(const ShaderTessellationInputInfo& tess) {
		return FactorDwords + tess.output_control_points * ControlPointDwords(tess);
	}
	[[nodiscard]] static uint32_t RecordDwords(const ShaderTessellationInputInfo& tess) {
		return PatchConstantDword(tess) + (tess.patch_size + 3u) / 4u;
	}
	[[nodiscard]] static uint32_t CaptureDwords(const ShaderTessellationInputInfo& tess) { return tess.ls_stride / 4u; }

	// A domain point: u and v (float bits), the patch g, and a spare dword.
	static constexpr uint32_t PointDwords = 4;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_TESSEMULATION_H_
