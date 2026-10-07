#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, spv::StorageClass storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, spv::StorageClass storage,
                     uint32_t type, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, type), pointer,
	                          variable, index);
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, spv::StorageClass storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, value, pointer);
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::Layer: return TypeU32(state);
		case IR::StageOutputKind::ClipDistance:
		case IR::StageOutputKind::CullDistance: return TypeF32(state);
		case IR::StageOutputKind::Position:
		case IR::StageOutputKind::Parameter: return TypeF32Vector(state, 4);
		default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
}

// One of the two buffers the compute emulation writes: dwords, set by the register space.
uint32_t MeshEmulationBuffer(EmitterState& state, uint32_t space, const char* name) {
	const auto array = state.builder.DecoratedType(
	    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}},
	    TypeU32(state));
	const auto block = state.builder.DecoratedType(
	    spv::OpTypeStruct,
	    {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	     {spv::OpDecorate, {spv::DecorationBlock}}},
	    array);
	const auto variable = state.builder.DefineGlobalVariable(
	    TypePointer(state, spv::StorageClassStorageBuffer, block), spv::StorageClassStorageBuffer);
	state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, space);
	state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, 0u);
	state.builder.AddName(variable, name);
	return variable;
}

uint32_t MeshDrawParameterLoad(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state), pointer,
	                          state.push_constant_variable, ConstantU32(state, 0),
	                          ConstantU32(state, index));
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

void MeshStoreDword(EmitterState& state, uint32_t buffer, uint32_t dword, uint32_t value) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state, 32),
	                          pointer, buffer, ConstantU32(state, 0), dword);
	state.builder.AddFunction(spv::OpStore, pointer, value);
}

uint32_t MeshCompare(EmitterState& state, spv::Op opcode, uint32_t lhs, uint32_t rhs) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(opcode, TypeBool(state), result, lhs, rhs);
	return result;
}

// The tail of the compute emulation: the group's vertices and triangles go to the slots of the
// group in the two buffers (IR::MeshEmulation). A triangle the group did not make, or culled,
// becomes a degenerate one that the draw rasterizes to nothing.
void EmitMeshComputeEpilogue(EmitterState& state, uint32_t vertices, uint32_t primitives) {
	const auto& mesh   = state.input_info.vertex->mesh;
	const auto  layout = IR::MeshEmulation::LayoutFor(state.program.info.outputs);
	EXIT_IF(layout.outputs.size() != state.outputs.size());

	const auto group_x     = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
	const auto group_y     = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1);
	const auto chunk_width = MeshDrawParameterLoad(state, IR::MeshEmulation::ChunkWidthParameter);
	const auto slot =
	    EmitAddU32(state, EmitBinaryU32(state, spv::OpIMul, group_y, chunk_width), group_x);
	const auto vertex_base = EmitBinaryU32(state, spv::OpIMul, slot,
	                                       ConstantU32(state, mesh.EmulatedVertexSlots()));
	const auto primitive_base = EmitBinaryU32(state, spv::OpIMul, slot,
	                                          ConstantU32(state, mesh.EmulatedPrimitiveSlots()));

	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half  = half;
		const auto index = EmitLocalInvocationIndex(state);

		EmitIfCondition(state, MeshCompare(state, spv::OpULessThan, index, vertices), [&] {
			const auto record =
			    EmitBinaryU32(state, spv::OpIMul, EmitAddU32(state, vertex_base, index),
			                  ConstantU32(state, layout.StrideDwords()));
			for (size_t i = 0; i < state.outputs.size(); i++) {
				const auto& output = state.outputs[i];
				const auto  first  = EmitAddU32(
                    state, record,
                    ConstantU32(state, layout.outputs[i].vec4 * 4u + layout.outputs[i].component));
				switch (output.kind) {
					case IR::StageOutputKind::Layer:
						// One value per vertex, kept in group shared memory by the guest's stores.
						MeshStoreDword(state, state.mesh_records, first,
						               MeshLoad(state, output.mesh_data_variable,
						                        spv::StorageClassWorkgroup, TypeU32(state), index));
						break;
					case IR::StageOutputKind::ClipDistance:
					case IR::StageOutputKind::CullDistance: {
						const auto value =
						    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate,
						             TypeF32(state), ConstantU32(state, half));
						MeshStoreDword(state, state.mesh_records, first,
						               Unary(state, spv::OpBitcast, TypeU32(state), value));
						break;
					}
					default: {
						const auto value =
						    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate,
						             TypeF32Vector(state, 4), ConstantU32(state, half));
						for (uint32_t component = 0; component < 4; component++) {
							const auto scalar = state.builder.AllocateId();
							state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), scalar,
							                          value, component);
							MeshStoreDword(state, state.mesh_records,
							               EmitAddU32(state, first, ConstantU32(state, component)),
							               Unary(state, spv::OpBitcast, TypeU32(state), scalar));
						}
						break;
					}
				}
			}
		});

		const auto in_slots = MeshCompare(state, spv::OpULessThan, index,
		                                  ConstantU32(state, mesh.EmulatedPrimitiveSlots()));
		EmitIfCondition(state, in_slots, [&] {
			const auto in_range = MeshCompare(state, spv::OpULessThan, index, primitives);
			const auto packed =
			    MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
			             ConstantU32(state, half));
			const auto culled =
			    MeshCompare(state, spv::OpINotEqual,
			                EmitBinaryU32(state, spv::OpBitwiseAnd, packed,
			                              ConstantU32(state, 0x80000000u)),
			                ConstantU32(state, 0));
			const auto not_culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpLogicalNot, TypeBool(state), not_culled, culled);
			const auto made = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), made, in_range, not_culled);
			const auto triangle = EmitBinaryU32(state, spv::OpIMul,
			                                    EmitAddU32(state, primitive_base, index),
			                                    ConstantU32(state, 3));
			for (uint32_t component = 0; component < 3; component++) {
				const auto vertex = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), vertex, packed,
				                          ConstantU32(state, component * 10u), ConstantU32(state, 10));
				const auto kept = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpSelect, TypeU32(state), kept, made, vertex,
				                          ConstantU32(state, 0));
				MeshStoreDword(state, state.mesh_indices,
				               EmitAddU32(state, triangle, ConstantU32(state, component)),
				               EmitAddU32(state, vertex_base, kept));
			}
		});
	}
}

} // namespace

void DefineMeshOutputs(EmitterState& state, uint32_t clip_distance_count,
                       uint32_t cull_distance_count) {
	const auto& mesh = state.input_info.vertex->mesh;
	for (auto& output: state.outputs) {
		const auto type = MeshOutputType(state, output.kind);
		const bool clip = output.kind == IR::StageOutputKind::ClipDistance;
		const bool cull = output.kind == IR::StageOutputKind::CullDistance;
		if (!state.mesh_compute) {
			auto& variable = clip ? state.clip_distance_variable
			                 : cull ? state.cull_distance_variable : output.variable_id;
			if (variable == 0) {
				const auto element_type = clip || cull
				                              ? state.builder.Type(spv::OpTypeArray, type,
				                                                   ConstantU32(state, clip ? clip_distance_count
				                                                                           : cull_distance_count))
				                              : type;
				variable = MeshArray(state, spv::StorageClassOutput, element_type,
				                     output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives
				                                                             : mesh.max_vertices);
				state.interface_variables.push_back(variable);
				state.builder.AddName(variable, output.debug_name.c_str());
				if (output.kind == IR::StageOutputKind::Parameter) {
					state.builder.AddAnnotation(spv::OpDecorate, variable,
					                            spv::DecorationLocation, output.location);
				} else {
					const auto builtin = clip ? spv::BuiltInClipDistance
					                     : cull ? spv::BuiltInCullDistance
					                     : output.kind == IR::StageOutputKind::Layer ? spv::BuiltInLayer
					                                                                 : spv::BuiltInPosition;
					state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
				}
				if (output.kind == IR::StageOutputKind::Layer) {
					state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPerPrimitiveEXT);
				}
			}
			output.variable_id = variable;
		}
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? mesh.max_vertices : state.lane_count);
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
	if (state.mesh_compute) {
		state.mesh_records = MeshEmulationBuffer(state, IR::MeshEmulation::RecordSpace, "mesh_records");
		state.mesh_indices = MeshEmulationBuffer(state, IR::MeshEmulation::IndexSpace, "mesh_indices");
		return;
	}
	state.mesh_primitives =
	    MeshArray(state, spv::StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.mesh_cull =
	    MeshArray(state, spv::StorageClassOutput, TypeBool(state), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.interface_variables.push_back(state.mesh_cull);
	state.builder.AddAnnotation(
	    spv::OpDecorate, state.mesh_primitives, spv::DecorationBuiltIn,
	    spv::BuiltInPrimitiveTriangleIndicesEXT); // PrimitiveTriangleIndicesEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationBuiltIn,
	                            spv::BuiltInCullPrimitiveEXT); // CullPrimitiveEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationPerPrimitiveEXT);
}

uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index) {
	const auto output = std::ranges::find_if(state.outputs, [=](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
	if (output == state.outputs.end()) {
		EXIT("mesh export has no output binding: kind=%u index=%u\n", static_cast<uint32_t>(kind),
		     index);
	}
	const bool shared = kind == IR::StageOutputKind::Layer;
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
	                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), value, allocation,
			                          ConstantU32(state, field * 12u),
			                          ConstantU32(state, field == 0 ? 10u : 11u));
			const auto pointer =
			    MeshElement(state, state.mesh_allocation, spv::StorageClassWorkgroup,
			                TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction(spv::OpStore, pointer, value);
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	if (state.mesh_compute) {
		EmitMeshComputeEpilogue(state, vertices, primitives);
		state.lane_half = 0;
		state.builder.AddFunction(spv::OpReturn);
		state.builder.AddFunction(spv::OpFunctionEnd);
		return;
	}
	state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
	                          primitives); // OpSetMeshOutputsEXT
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				uint32_t pointer;
				if (output.kind == IR::StageOutputKind::ClipDistance ||
				    output.kind == IR::StageOutputKind::CullDistance) {
					pointer = state.builder.AllocateId();
					state.builder.AddFunction(
					    spv::OpAccessChain, TypePointer(state, spv::StorageClassOutput, type), pointer,
					    output.variable_id, index, ConstantU32(state, output.index));
				} else {
					pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput, type, index);
				}
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), vertex[component], packed,
				    ConstantU32(state, component * 10u), ConstantU32(state, 10));
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                          vertex[0], vertex[1], vertex[2]);
			const auto triangle_pointer =
			    MeshElement(state, state.mesh_primitives, spv::StorageClassOutput,
			                TypeU32Vector(state, 3), index);
			state.builder.AddFunction(spv::OpStore, triangle_pointer, triangle);
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			state.builder.AddFunction(spv::OpStore,
			                          MeshElement(state, state.mesh_cull, spv::StorageClassOutput,
			                                      TypeBool(state), index),
			                          culled);
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto layer = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassWorkgroup, TypeU32(state),
				                            vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction(spv::OpStore, pointer, layer);
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// The vertex stage of the compute emulation: vertex `i` of the indexed draw is record `i` of the
// records buffer, whose layout IR::MeshEmulation::LayoutFor gives. It outputs what the mesh
// program exported, with the same locations and built-ins. With user clip planes it outputs the
// distance of the position to each enabled plane, read from the draw's push constants at
// PushData::UserClipPlaneDword, as the vertex stage of a draw without the emulation does.
std::vector<uint32_t> EmitMeshFetchProgram(std::span<const IR::StageOutput> outputs,
                                           uint32_t                         user_clip_planes) {
	Builder b(0x00010300u);
	const auto layout = IR::MeshEmulation::LayoutFor(outputs);

	b.RequireCapability(spv::CapabilityShader);
	const auto type_void     = b.Type(spv::OpTypeVoid);
	const auto type_function = b.Type(spv::OpTypeFunction, type_void);
	const auto type_u32      = b.Type(spv::OpTypeInt, 32, 0);
	const auto type_i32      = b.Type(spv::OpTypeInt, 32, 1);
	const auto type_f32      = b.Type(spv::OpTypeFloat, 32);
	const auto type_vec4     = b.Type(spv::OpTypeVector, type_f32, 4);
	const auto constant      = [&](uint32_t value) { return b.Constant(spv::OpConstant, type_u32, value); };
	const auto pointer_type  = [&](spv::StorageClass storage, uint32_t type) {
        return b.Type(spv::OpTypePointer, storage, type);
	};
	std::vector<uint32_t> interface;

	// The records, read only: set and binding make D3D12 register t0 of the record space.
	const auto array_type = b.DecoratedType(
	    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, type_u32);
	const auto block_type = b.DecoratedType(spv::OpTypeStruct,
	                                        {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                         {spv::OpDecorate, {spv::DecorationBlock}}},
	                                        array_type);
	const auto records =
	    b.DefineGlobalVariable(pointer_type(spv::StorageClassStorageBuffer, block_type),
	                           spv::StorageClassStorageBuffer);
	b.AddAnnotation(spv::OpDecorate, records, spv::DecorationDescriptorSet,
	                IR::MeshEmulation::RecordSpace);
	b.AddAnnotation(spv::OpDecorate, records, spv::DecorationBinding, 0u);
	b.AddAnnotation(spv::OpDecorate, records, spv::DecorationNonWritable);
	b.AddName(records, "mesh_records");

	const auto vertex_index = b.DefineGlobalVariable(
	    pointer_type(spv::StorageClassInput, type_i32), spv::StorageClassInput);
	b.AddAnnotation(spv::OpDecorate, vertex_index, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);
	b.AddName(vertex_index, "gl_VertexIndex");
	interface.push_back(vertex_index);

	// The output variables; the clip and the cull distances are one array each.
	uint32_t clip_count = 0;
	uint32_t cull_count = 0;
	for (const auto& output: outputs) {
		if (output.kind == IR::StageOutputKind::ClipDistance) {
			clip_count = std::max(clip_count, output.index + 1u);
		} else if (output.kind == IR::StageOutputKind::CullDistance) {
			cull_count = std::max(cull_count, output.index + 1u);
		}
	}
	uint32_t              clip_variable = 0;
	uint32_t              cull_variable = 0;
	std::vector<uint32_t> variables(outputs.size(), 0);
	const auto            define_output = [&](uint32_t type, const char* name) {
        const auto variable = b.DefineGlobalVariable(pointer_type(spv::StorageClassOutput, type),
                                                     spv::StorageClassOutput);
        b.AddName(variable, name);
        interface.push_back(variable);
        return variable;
	};
	// The user clip planes: the push constants, and a clip distance per plane.
	uint32_t push_constants = 0;
	if (user_clip_planes != 0) {
		// PipelineCache rejects UCP_ENA with shader clip or cull distances.
		EXIT_IF(clip_count + cull_count != 0);
		const auto push_array = b.DecoratedType(
		    spv::OpTypeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, type_u32,
		    constant(IR::PushData::DwordCount));
		const auto push_block =
		    b.DecoratedType(spv::OpTypeStruct,
		                    {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
		                     {spv::OpDecorate, {spv::DecorationBlock}}},
		                    push_array);
		push_constants = b.DefineGlobalVariable(pointer_type(spv::StorageClassPushConstant, push_block),
		                                        spv::StorageClassPushConstant);
		b.AddName(push_constants, "push_data");
		clip_count    = static_cast<uint32_t>(std::popcount(user_clip_planes));
		clip_variable = define_output(b.Type(spv::OpTypeArray, type_f32, constant(clip_count)),
		                              "gl_ClipDistance");
		b.AddAnnotation(spv::OpDecorate, clip_variable, spv::DecorationBuiltIn,
		                spv::BuiltInClipDistance);
		b.RequireCapability(spv::CapabilityClipDistance);
	}
	for (size_t i = 0; i < outputs.size(); i++) {
		const auto& output = outputs[i];
		switch (output.kind) {
			case IR::StageOutputKind::Position: {
				const auto type = b.DecoratedType(
				    spv::OpTypeStruct,
				    {{spv::OpMemberDecorate, {0, spv::DecorationBuiltIn, spv::BuiltInPosition}},
				     {spv::OpDecorate, {spv::DecorationBlock}}},
				    type_vec4);
				variables[i] = define_output(type, "outPerVertex");
				break;
			}
			case IR::StageOutputKind::Parameter:
				variables[i] = define_output(type_vec4, output.debug_name.c_str());
				b.AddAnnotation(spv::OpDecorate, variables[i], spv::DecorationLocation,
				                output.location);
				break;
			case IR::StageOutputKind::ClipDistance:
			case IR::StageOutputKind::CullDistance: {
				const bool clip = output.kind == IR::StageOutputKind::ClipDistance;
				auto&      variable = clip ? clip_variable : cull_variable;
				if (variable == 0) {
					const auto type = b.Type(spv::OpTypeArray, type_f32,
					                         constant(clip ? clip_count : cull_count));
					variable = define_output(type, clip ? "gl_ClipDistance" : "gl_CullDistance");
					b.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn,
					                clip ? spv::BuiltInClipDistance : spv::BuiltInCullDistance);
					b.RequireCapability(clip ? spv::CapabilityClipDistance
					                         : spv::CapabilityCullDistance);
				}
				variables[i] = variable;
				break;
			}
			case IR::StageOutputKind::Layer:
				variables[i] = define_output(type_u32, "gl_Layer");
				b.AddAnnotation(spv::OpDecorate, variables[i], spv::DecorationBuiltIn,
				                spv::BuiltInLayer);
				b.RequireVersion(0x00010500u);
				b.RequireCapability(spv::CapabilityShaderLayer);
				break;
			default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(output.kind));
		}
	}

	const auto main = b.AllocateId();
	b.AddFunction(spv::OpFunction, type_void, main, spv::FunctionControlMaskNone, type_function);
	b.AddFunction(spv::OpLabel, b.AllocateId());
	const auto index_i32 = b.AllocateId();
	b.AddFunction(spv::OpLoad, type_i32, index_i32, vertex_index);
	const auto index = b.AllocateId();
	b.AddFunction(spv::OpBitcast, type_u32, index, index_i32);
	const auto record = b.AllocateId();
	b.AddFunction(spv::OpIMul, type_u32, record, index, constant(layout.StrideDwords()));

	const auto load_f32 = [&](uint32_t dword) {
		const auto at = b.AllocateId();
		b.AddFunction(spv::OpIAdd, type_u32, at, record, constant(dword));
		const auto pointer = b.AllocateId();
		b.AddFunction(spv::OpAccessChain, pointer_type(spv::StorageClassStorageBuffer, type_u32),
		              pointer, records, constant(0), at);
		const auto raw = b.AllocateId();
		b.AddFunction(spv::OpLoad, type_u32, raw, pointer);
		const auto value = b.AllocateId();
		b.AddFunction(spv::OpBitcast, type_f32, value, raw);
		return value;
	};
	for (size_t i = 0; i < outputs.size(); i++) {
		const auto& output = outputs[i];
		const auto& slot   = layout.outputs[i];
		switch (output.kind) {
			case IR::StageOutputKind::Position:
			case IR::StageOutputKind::Parameter: {
				uint32_t scalars[4] {};
				for (uint32_t component = 0; component < 4; component++) {
					scalars[component] = load_f32(slot.vec4 * 4u + component);
				}
				const auto value = b.AllocateId();
				b.AddFunction(spv::OpCompositeConstruct, type_vec4, value, scalars[0], scalars[1],
				              scalars[2], scalars[3]);
				uint32_t target = variables[i];
				if (output.kind == IR::StageOutputKind::Position) {
					target = b.AllocateId();
					b.AddFunction(spv::OpAccessChain, pointer_type(spv::StorageClassOutput, type_vec4),
					              target, variables[i], constant(0));
				}
				b.AddFunction(spv::OpStore, target, value);
				if (output.kind != IR::StageOutputKind::Position || push_constants == 0) {
					break;
				}
				// The clipper keeps the half space where dot(plane, position) >= 0 for each plane.
				uint32_t distance_index = 0;
				for (auto mask = user_clip_planes; mask != 0; mask &= mask - 1u) {
					const auto dword = IR::PushData::UserClipPlaneDword +
					                   4u * static_cast<uint32_t>(std::countr_zero(mask));
					uint32_t components[4] {};
					for (uint32_t component = 0; component < 4; component++) {
						const auto pointer = b.AllocateId();
						b.AddFunction(spv::OpAccessChain,
						              pointer_type(spv::StorageClassPushConstant, type_u32), pointer,
						              push_constants, constant(0), constant(dword + component));
						const auto raw = b.AllocateId();
						b.AddFunction(spv::OpLoad, type_u32, raw, pointer);
						components[component] = b.AllocateId();
						b.AddFunction(spv::OpBitcast, type_f32, components[component], raw);
					}
					const auto plane = b.AllocateId();
					b.AddFunction(spv::OpCompositeConstruct, type_vec4, plane, components[0],
					              components[1], components[2], components[3]);
					const auto distance = b.AllocateId();
					b.AddFunction(spv::OpDot, type_f32, distance, plane, value);
					const auto pointer = b.AllocateId();
					b.AddFunction(spv::OpAccessChain, pointer_type(spv::StorageClassOutput, type_f32),
					              pointer, clip_variable, constant(distance_index++));
					b.AddFunction(spv::OpStore, pointer, distance);
				}
				break;
			}
			case IR::StageOutputKind::ClipDistance:
			case IR::StageOutputKind::CullDistance: {
				const auto value   = load_f32(slot.vec4 * 4u + slot.component);
				const auto pointer = b.AllocateId();
				b.AddFunction(spv::OpAccessChain, pointer_type(spv::StorageClassOutput, type_f32),
				              pointer, variables[i], constant(output.index));
				b.AddFunction(spv::OpStore, pointer, value);
				break;
			}
			default: { // Layer
				const auto at = b.AllocateId();
				b.AddFunction(spv::OpIAdd, type_u32, at, record, constant(slot.vec4 * 4u));
				const auto pointer = b.AllocateId();
				b.AddFunction(spv::OpAccessChain,
				              pointer_type(spv::StorageClassStorageBuffer, type_u32), pointer, records,
				              constant(0), at);
				const auto raw = b.AllocateId();
				b.AddFunction(spv::OpLoad, type_u32, raw, pointer);
				b.AddFunction(spv::OpStore, variables[i], raw);
				break;
			}
		}
	}
	b.AddFunction(spv::OpReturn);
	b.AddFunction(spv::OpFunctionEnd);
	b.AddName(main, "main");
	b.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
	b.AddEntryPoint(spv::ExecutionModelVertex, main, "main", interface);
	return b.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
