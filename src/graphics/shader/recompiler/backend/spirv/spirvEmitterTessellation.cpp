#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

bool IsTessellationInput(IR::TessellationAttribute kind) {
	using Attribute = IR::TessellationAttribute;
	return kind == Attribute::ControlInput || kind == Attribute::EvaluationInput ||
	       kind == Attribute::PatchInput;
}

bool IsTessellationPatch(IR::TessellationAttribute kind) {
	using Attribute = IR::TessellationAttribute;
	return kind == Attribute::PatchOutput || kind == Attribute::PatchInput;
}

// IR::TessEmulation: one of its buffers, dwords, at register 0 of `space` (t0 when read only, else u0).
uint32_t TessBuffer(EmitterState& state, uint32_t space, bool writable, const char* name) {
	const auto array = state.builder.DecoratedType(spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}},
	                                               TypeU32(state));
	const auto block = state.builder.DecoratedType(
	    spv::OpTypeStruct, {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}}, {spv::OpDecorate, {spv::DecorationBlock}}}, array);
	const auto variable =
	    state.builder.DefineGlobalVariable(TypePointer(state, spv::StorageClassStorageBuffer, block), spv::StorageClassStorageBuffer);
	state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, space);
	state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, 0u);
	if (!writable) {
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationNonWritable);
	}
	state.builder.AddName(variable, name);
	return variable;
}

uint32_t TessDwordPointer(EmitterState& state, uint32_t buffer, uint32_t dword) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state, 32), pointer, buffer, ConstantU32(state, 0),
	                          dword);
	return pointer;
}

uint32_t TessLoad(EmitterState& state, uint32_t buffer, uint32_t dword) {
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, TessDwordPointer(state, buffer, dword));
	return value;
}

uint32_t TessParam(EmitterState& state, IR::TessEmulation::Param param) {
	return TessLoad(state, state.tess_params, ConstantU32(state, param));
}

// A signed builtin input of the vertex stage (VertexIndex, InstanceIndex) as a u32.
uint32_t TessVertexInput(EmitterState& state, IR::StageInputKind kind) {
	const auto variable = InputVariableForKind(state, kind);
	EXIT_IF(variable == 0);
	const auto value = state.builder.AllocateId();
	const auto bits  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeI32(state), value, variable);
	state.builder.AddFunction(spv::OpBitcast, TypeU32(state), bits, value);
	return bits;
}

uint32_t TessSelect(EmitterState& state, uint32_t condition, uint32_t yes, uint32_t no) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), result, condition, yes, no);
	return result;
}

uint32_t TessCompare(EmitterState& state, spv::Op opcode, uint32_t lhs, uint32_t rhs) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(opcode, TypeBool(state), result, lhs, rhs);
	return result;
}

// The patch g of the draw: the hull pass's workgroup (excess groups of the last row repeat the last patch, writing the same record),
// or the domain point's.
uint32_t TessGlobalPatch(EmitterState& state) {
	using Emulation = IR::TessEmulation;
	if (state.program.stage == ShaderType::TessellationEvaluation) {
		const auto point = TessVertexInput(state, IR::StageInputKind::VertexIndex);
		return TessLoad(state, state.tess_points,
		                EmitAddU32(state, EmitBinaryU32(state, spv::OpIMul, point, ConstantU32(state, Emulation::PointDwords)),
		                           ConstantU32(state, 2)));
	}
	const auto x     = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
	const auto y     = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1);
	const auto patch = EmitAddU32(state, EmitBinaryU32(state, spv::OpIMul, y, TessParam(state, Emulation::GroupsX)), x);
	const auto last  = EmitBinaryU32(state, spv::OpISub, TessParam(state, Emulation::PatchTotal), ConstantU32(state, 1));
	return TessSelect(state, TessCompare(state, spv::OpULessThanEqual, patch, last), patch, last);
}

// The emulated stages' tessellation attributes: dwords of the capture buffer (LS outputs, HS inputs) or of the patch's record.
uint32_t EmulatedTessellationPointer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state    = ctx.state;
	using Emulation      = IR::TessEmulation;
	using Kind           = IR::TessellationAttribute;
	const auto  kind     = static_cast<Kind>(inst.Arg(0).U32());
	const auto& tess     = state.input_info.vertex->tess;
	const auto  u32      = [&](uint32_t value) { return ConstantU32(state, value); };
	const auto  mul      = [&](uint32_t a, uint32_t b) { return EmitBinaryU32(state, spv::OpIMul, a, b); };
	const auto  add      = [&](uint32_t a, uint32_t b) { return EmitAddU32(state, a, b); };
	const auto  sub      = [&](uint32_t a, uint32_t b) { return EmitBinaryU32(state, spv::OpISub, a, b); };
	const auto  dword_in = [&](uint32_t address, uint32_t stride) {
		return EmitBinaryU32(state, spv::OpShiftRightLogical, EmitBinaryU32(state, spv::OpUMod, address, u32(stride)), u32(2));
	};
	if (kind == Kind::Factor) {
		EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate() || inst.Arg(1).U32() / 4u >= 6u);
		const auto record = mul(TessGlobalPatch(state), u32(Emulation::RecordDwords(tess)));
		return TessDwordPointer(state, state.tess_records, add(record, u32(inst.Arg(1).U32() / 4u)));
	}
	const auto address = ctx.Arg(inst, 1);
	if (kind == Kind::LocalOutput) {
		// The vertex's slot: the slots of the instances before it, then its index from the capture's first.
		const auto instance = sub(TessVertexInput(state, IR::StageInputKind::InstanceIndex), TessParam(state, Emulation::BaseInstance));
		const auto vertex   = sub(TessVertexInput(state, IR::StageInputKind::VertexIndex), TessParam(state, Emulation::CaptureBase));
		const auto slot     = add(mul(instance, TessParam(state, Emulation::CaptureVertices)), vertex);
		return TessDwordPointer(state, state.tess_capture, add(mul(slot, u32(Emulation::CaptureDwords(tess))), dword_in(address, tess.ls_stride)));
	}
	const auto patch = TessGlobalPatch(state);
	if (kind == Kind::ControlInput) {
		// Control point `point` of the patch is the vertex at position patch * input points + point of its instance.
		const auto count     = TessParam(state, Emulation::PatchCount);
		const auto instance  = EmitBinaryU32(state, spv::OpUDiv, patch, count);
		const auto primitive = EmitBinaryU32(state, spv::OpUMod, patch, count);
		const auto point     = EmitBinaryU32(state, spv::OpUDiv, address, u32(tess.ls_stride));
		const auto position  = add(mul(primitive, u32(tess.input_control_points)), point);
		const auto indexed   = TessCompare(state, spv::OpINotEqual, TessParam(state, Emulation::Indexed), u32(0));
		const auto vertex    = TessSelect(state, indexed, TessLoad(state, state.tess_slots, position), position);
		const auto slot      = add(mul(instance, TessParam(state, Emulation::CaptureVertices)), vertex);
		return TessDwordPointer(state, state.tess_capture, add(mul(slot, u32(Emulation::CaptureDwords(tess))), dword_in(address, tess.ls_stride)));
	}
	const auto record = mul(patch, u32(Emulation::RecordDwords(tess)));
	if (kind == Kind::PatchOutput || kind == Kind::PatchInput) {
		const auto offset = EmitBinaryU32(state, spv::OpShiftRightLogical, sub(address, u32(tess.patch_base)), u32(2));
		return TessDwordPointer(state, state.tess_records, add(record, add(u32(Emulation::PatchConstantDword(tess)), offset)));
	}
	EXIT_NOT_IMPLEMENTED(kind != Kind::ControlOutput && kind != Kind::EvaluationInput);
	// An HS writes only its own control point but may read any other.
	const auto point = kind == Kind::ControlOutput && inst.GetOpcode() == IR::ValueOpcode::SetTessellationAttribute
	                       ? EmitLaneId(state)
	                       : EmitBinaryU32(state, spv::OpUDiv, address, u32(tess.hs_stride));
	return TessDwordPointer(state, state.tess_records,
	                        add(record, add(u32(Emulation::FactorDwords),
	                                        add(mul(point, u32(Emulation::ControlPointDwords(tess))), dword_in(address, tess.hs_stride)))));
}

uint32_t TessellationPointer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state          = ctx.state;
	if (state.tess_compute) {
		return EmulatedTessellationPointer(ctx, inst);
	}
	using Attribute      = IR::TessellationAttribute;
	const auto  kind     = static_cast<Attribute>(inst.Arg(0).U32());
	const auto& tess     = state.input_info.vertex->tess;
	const auto  variable = state.tess_variables.at(static_cast<uint32_t>(kind));
	EXIT_IF(variable == 0);
	const bool patch = IsTessellationPatch(kind);
	const auto storage =
	    IsTessellationInput(kind) ? spv::StorageClassInput : spv::StorageClassOutput;
	const auto pointer = state.builder.AllocateId();
	if (kind == Attribute::Factor) {
		EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
		const auto index       = inst.Arg(1).U32() / 4u;
		const bool quad        = tess.domain == 2u;
		const auto outer_count = quad ? 4u : 3u;
		const bool outer       = index < outer_count;
		EXIT_NOT_IMPLEMENTED(index >= (quad ? 6u : 4u));
		state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, TypeF32(state)),
		                          pointer, outer ? variable : state.tess_inner_variable,
		                          ConstantU32(state, outer ? index : index - outer_count));
		return pointer;
	}
	auto address = ctx.Arg(inst, 1);
	if (patch) {
		address = EmitBinaryU32(state, spv::OpISub, address, ConstantU32(state, tess.patch_base));
	}
	const bool local  = kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
	const auto stride = local ? tess.ls_stride : tess.hs_stride;
	const auto offset =
	    patch ? address : EmitBinaryU32(state, spv::OpUMod, address, ConstantU32(state, stride));
	const auto attribute =
	    EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 4));
	const auto component =
	    EmitBinaryU32(state, spv::OpBitwiseAnd,
	                  EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 2)),
	                  ConstantU32(state, 3));
	const auto type = TypePointer(state, storage, TypeU32(state));
	if (kind == Attribute::LocalOutput || patch) {
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, attribute,
		                          component);
	} else {
		// An HS writes only its own control point but may read any other.
		const auto vertex =
		    kind == Attribute::ControlOutput &&
		            inst.GetOpcode() == IR::ValueOpcode::SetTessellationAttribute
		        ? EmitLaneId(state)
		        : EmitBinaryU32(state, spv::OpUDiv, address, ConstantU32(state, stride));
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, vertex, attribute,
		                          component);
	}
	return pointer;
}

} // namespace

void DefineTessellationInterfaces(EmitterState& state) {
	using Attribute = IR::TessellationAttribute;
	// Tessellation lowering emits attribute accesses only in these stages.
	const auto stage = state.program.stage;
	if (stage != ShaderType::Local && stage != ShaderType::TessellationControl &&
	    stage != ShaderType::TessellationEvaluation) {
		return;
	}
	if (state.tess_compute) {
		// The buffers of IR::TessEmulation each stage uses (the parameters always).
		using Emulation   = IR::TessEmulation;
		state.tess_params = TessBuffer(state, Emulation::ParamSpace, false, "tess_params");
		if (stage == ShaderType::Local) {
			state.tess_capture = TessBuffer(state, Emulation::CaptureSpace, true, "tess_capture");
		} else if (stage == ShaderType::TessellationControl) {
			state.tess_capture = TessBuffer(state, Emulation::CaptureSpace, false, "tess_capture");
			state.tess_slots   = TessBuffer(state, Emulation::SlotSpace, false, "tess_slots");
			state.tess_records = TessBuffer(state, Emulation::RecordSpace, true, "tess_records");
		} else {
			state.tess_records = TessBuffer(state, Emulation::RecordSpace, false, "tess_records");
			state.tess_points  = TessBuffer(state, Emulation::PointSpace, false, "tess_points");
		}
		return;
	}
	std::array<bool, IR::NumTessellationAttributes> used {};
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == IR::ValueOpcode::GetTessellationAttribute ||
			    inst.GetOpcode() == IR::ValueOpcode::SetTessellationAttribute)
				used.at(inst.Arg(0).U32()) = true;
		}
	}
	if (std::ranges::none_of(used, [](bool value) { return value; })) return;
	const auto& tess  = state.input_info.vertex->tess;
	const auto  array = [&](uint32_t type, uint32_t count) {
		return state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	};
	for (uint32_t index = 0; index < used.size(); index++) {
		if (!used[index]) continue;
		const auto kind  = static_cast<Attribute>(index);
		const bool patch = IsTessellationPatch(kind);
		const auto storage =
		    IsTessellationInput(kind) ? spv::StorageClassInput : spv::StorageClassOutput;
		uint32_t type;
		if (kind == Attribute::Factor) {
			type = array(TypeF32(state), 4u);
		} else if (patch) {
			// HS and TES share the reflected patch layout so their interfaces match.
			type = array(TypeU32Vector(state, 4), (tess.patch_size + 15u) / 16u);
		} else {
			const bool local  = kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
			const auto stride = local ? tess.ls_stride : tess.hs_stride;
			type              = array(TypeU32Vector(state, 4), (stride + 15u) / 16u);
			if (kind != Attribute::LocalOutput) {
				type = array(type, local ? tess.input_control_points : tess.output_control_points);
			}
		}
		auto& variable = state.tess_variables[index];
		variable       = DefineInterfaceVariable(state, type, storage, "tess_attributes");
		if (kind == Attribute::Factor) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn,
			                            spv::BuiltInTessLevelOuter);
			state.tess_inner_variable =
			    DefineInterfaceVariable(state, array(TypeF32(state), 2u), storage, "tess_inner");
			state.builder.AddAnnotation(spv::OpDecorate, state.tess_inner_variable,
			                            spv::DecorationBuiltIn, spv::BuiltInTessLevelInner);
			state.builder.AddAnnotation(spv::OpDecorate, state.tess_inner_variable,
			                            spv::DecorationPatch);
		} else {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation,
			                            patch ? (tess.hs_stride + 15u) / 16u : 0u);
		}
		if (kind == Attribute::Factor || patch) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPatch);
		}
	}
}

void DefineTessellationExecutionModes(EmitterState& state) {
	const auto& tess = state.input_info.vertex->tess;
	if (state.tess_compute) {
		// The hull pass runs one workgroup per patch, one invocation per output control point; the domain stage is a vertex stage.
		if (state.program.stage == ShaderType::TessellationControl) {
			state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeLocalSize, tess.output_control_points, 1u, 1u);
		}
		return;
	}
	// Triangle or quad domain, integer or fractional partitioning, triangle output.
	EXIT_NOT_IMPLEMENTED((tess.domain != 1u && tess.domain != 2u) ||
	                     (tess.partitioning != 0u && tess.partitioning != 2u && tess.partitioning != 3u) ||
	                     (tess.output_topology != 2u && tess.output_topology != 3u));
	state.builder.RequireCapability(spv::CapabilityTessellation);
	if (state.program.stage == ShaderType::TessellationControl) {
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeOutputVertices,
		                               tess.output_control_points);
	} else {
		state.builder.AddExecutionMode(state.main_func, tess.domain == 1u
		                                                   ? spv::ExecutionModeTriangles
		                                                   : spv::ExecutionModeQuads);
		state.builder.AddExecutionMode(
		    state.main_func, tess.partitioning == 0u ? spv::ExecutionModeSpacingEqual
		                     : tess.partitioning == 2u ? spv::ExecutionModeSpacingFractionalOdd
		                                                : spv::ExecutionModeSpacingFractionalEven);
		state.builder.AddExecutionMode(state.main_func, tess.output_topology == 2u
		                                                   ? spv::ExecutionModeVertexOrderCw
		                                                   : spv::ExecutionModeVertexOrderCcw);
	}
}

uint32_t EmitTessEmulationInput(EmitterState& state, IR::StageInputKind kind, uint32_t component) {
	using Emulation = IR::TessEmulation;
	switch (kind) {
		case IR::StageInputKind::InvocationId: return EmitInputComponentU32(state, IR::StageInputKind::LocalInvocationId, 0);
		case IR::StageInputKind::PrimitiveId:
			return EmitBinaryU32(state, spv::OpUMod, TessGlobalPatch(state), TessParam(state, Emulation::PatchCount));
		case IR::StageInputKind::TessCoord: {
			if (component >= 2u) {
				return ConstantU32(state, 0);
			}
			const auto point = TessVertexInput(state, IR::StageInputKind::VertexIndex);
			return TessLoad(state, state.tess_points,
			                EmitAddU32(state, EmitBinaryU32(state, spv::OpIMul, point, ConstantU32(state, Emulation::PointDwords)),
			                           ConstantU32(state, component)));
		}
		default: EXIT("tessellation emulation: no input %u\n", static_cast<uint32_t>(kind));
	}
	return 0;
}

uint32_t EmitGetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, 2), [&] {
		const auto value = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), value,
		                              TessellationPointer(ctx, inst));
		return value;
	});
}

void EmitSetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, 3), [&] {
		auto value = ctx.Arg(inst, 2);
		// (The emulation's records keep the factors' bits.)
		if (inst.Arg(0).U32() == static_cast<uint32_t>(IR::TessellationAttribute::Factor) && !ctx.state.tess_compute) {
			const auto floating = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpBitcast, TypeF32(ctx.state), floating, value);
			value = floating;
		}
		ctx.state.builder.AddFunction(spv::OpStore, TessellationPointer(ctx, inst), value);
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
