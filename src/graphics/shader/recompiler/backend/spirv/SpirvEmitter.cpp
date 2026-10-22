#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

void ValidateNativeProgram(const IR::Program& program, bool lds_storage) {
	std::vector<bool> live_buffers(program.info.buffers.size());
	bool uses_lds = false;
	bool uses_gds = false;
	const auto uses_mapping = [](const auto& resource) {
		return resource.indirect_root != UINT32_MAX;
	};
	bool uses_flattened_srt = std::ranges::any_of(program.info.buffers, uses_mapping) ||
	                          std::ranges::any_of(program.info.images, uses_mapping);
	const auto planning_only_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       const auto op = use.user->GetOpcode();
			       if (op != IR::ValueOpcode::LoadAddressU32 &&
			           op != IR::ValueOpcode::ReadConstBuffer) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].planning_only;
		       });
	};
	const auto indirect_buffer_handle = [&](const IR::Inst& handle) {
		return program.info.uses_dma && handle.NumArgs() == 4u && !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::BufferAccessOf(use.user->GetOpcode()) != IR::BufferAccess::Read) {
				       return false;
			       }
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::IndirectBuffer;
		       });
	};
	const auto local_flat_handle = [&](const IR::Inst& handle) {
		return !handle.Uses().empty() &&
		       std::ranges::all_of(handle.Uses(), [&](const IR::Use& use) {
			       if (IR::AddressOpcodeInfoOf(use.user->GetOpcode()).access == IR::AddressAccess::None)
				       return false;
			       const auto index = use.user->Flags<IR::MemoryFlags>().index;
			       return index < program.memory_info.size() &&
			              program.memory_info[index].kind == IR::ResourceKind::FlatLocal;
		       });
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op = inst.GetOpcode();
			uses_flattened_srt |= op == IR::ValueOpcode::ReadConst;
			if (IR::BufferAccessOf(op) != IR::BufferAccess::None ||
			    IR::SharedAccessOf(op) != IR::SharedAccess::None ||
			    IR::AddressOpcodeInfoOf(op).access != IR::AddressAccess::None) {
				const auto index = inst.Flags<IR::MemoryFlags>().index;
				if (index >= program.memory_info.size()) {
					Fail(program, "typed shader contains invalid memory metadata");
				}
				const auto& memory = program.memory_info[index];
				if (!memory.planning_only) {
					uses_lds |= memory.kind == IR::ResourceKind::FlatLocal;
					if (IR::SharedAccessOf(op) != IR::SharedAccess::None) {
						if (memory.kind != IR::ResourceKind::Lds && memory.kind != IR::ResourceKind::Gds) {
							Fail(program, "typed shader contains invalid shared-memory metadata");
						}
						uses_gds |= memory.kind == IR::ResourceKind::Gds;
						uses_lds |= memory.kind == IR::ResourceKind::Lds;
					} else if (memory.kind == IR::ResourceKind::Buffer ||
					           memory.kind == IR::ResourceKind::ScalarBuffer) {
						if (memory.resource >= program.info.buffers.size()) {
							Fail(program, "typed shader contains an invalid buffer resource");
						}
						live_buffers[memory.resource] = true;
						for (const auto child: program.info.buffers[memory.resource].indirect_resources) {
							if (child >= program.info.buffers.size()) {
								Fail(program, "typed shader contains an invalid indirect buffer resource");
							}
							live_buffers[child] = true;
						}
					}
				}
			}
			const auto dense = inst.Flags<uint32_t>();
			switch (inst.GetOpcode()) {
				case IR::ValueOpcode::GetBufferResource:
					if (planning_only_handle(inst) || indirect_buffer_handle(inst)) {
						break;
					}
					if (dense >= program.info.buffers.size()) {
						Fail(program, "typed buffer handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetBufferResourceIndex:
					if (dense >= program.info.buffers.size()) {
						Fail(program, "typed buffer selector has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetAddressResource:
					if (planning_only_handle(inst)) {
						break;
					}
					if (inst.NumArgs() != 2 || (!program.info.uses_dma && !local_flat_handle(inst))) {
						Fail(program, "typed address handle has invalid DMA metadata");
					}
					break;
				case IR::ValueOpcode::GetScratchResource:
					if (inst.NumArgs() != 0 || program.scratch_dwords == 0) {
						Fail(program, "typed scratch handle has invalid shader metadata");
					}
					break;
				case IR::ValueOpcode::GetImageResource:
					if (dense >= program.info.images.size()) {
						Fail(program, "typed image handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::GetSamplerResource:
					if (dense >= program.info.samplers.size()) {
						Fail(program, "typed sampler handle has an invalid dense resource");
					}
					break;
				case IR::ValueOpcode::ReadConst: {
					const auto slot = inst.Arg(1).Resolve();
					if (!slot.IsImmediate() || slot.GetType() != IR::Type::U32 ||
					    slot.U32() >= program.srt_reads.size()) {
						Fail(program, "flattened SRT read has an invalid dense slot");
					}
					break;
				}
				default: break;
			}
		}
	}
	using Kind = IR::DescriptorBindingKind;
	std::array<uint32_t, static_cast<size_t>(Kind::Count)> expected {};
	auto& buffer_count = expected[static_cast<size_t>(Kind::Buffers)];
	for (uint32_t index = 0; index < program.info.buffers.size(); ++index) {
		const auto slot = live_buffers[index] ? buffer_count++ : UINT32_MAX;
		if (program.info.buffers[index].live != live_buffers[index] ||
		    program.info.buffers[index].descriptor_index != slot) {
			Fail(program, "native buffer descriptor index does not match shader topology");
		}
	}
	for (const auto& image: program.info.images) {
		const auto kind = IR::DescriptorBindingForImage(image);
		if (!kind.has_value()) {
			Fail(program, "native shader plan has an invalid image class");
		}
		if (image.mip_count == 0u ||
		    (image.mip_mode != IR::ImageMipMode::Dynamic && image.mip_count != 1u)) {
			Fail(program, "native shader plan has an invalid image mip descriptor count");
		}
		auto& count = expected[static_cast<size_t>(*kind)];
		if (image.descriptor_index != count) {
			Fail(program, "native image descriptor index does not match shader topology");
		}
		count += image.mip_count;
	}
	expected[static_cast<size_t>(Kind::Samplers)] = static_cast<uint32_t>(program.info.samplers.size());
	expected[static_cast<size_t>(Kind::Gds)] = uses_gds;
	expected[static_cast<size_t>(Kind::SharedMemory)] = uses_lds && lds_storage;
	expected[static_cast<size_t>(Kind::BdaPagetable)] = program.info.uses_dma;
	expected[static_cast<size_t>(Kind::FaultBuffer)] = program.info.uses_dma;
	expected[static_cast<size_t>(Kind::FlattenedSrt)] = uses_flattened_srt;
	expected[static_cast<size_t>(Kind::ShaderData)] =
	    program.bindings.ShaderDataDwords() != 0 && !program.bindings.UsesPushData();
	uint64_t descriptor_mask = 0;
	for (uint32_t index = 0; index < expected.size(); ++index) {
		if (expected[index] != 0) descriptor_mask |= uint64_t{1} << index;
	}
	if (program.bindings.descriptor_counts != expected ||
	    program.bindings.descriptor_mask != descriptor_mask) {
		Fail(program, "native descriptor counts do not match shader topology");
	}
	const auto shader_data_dwords = program.bindings.ShaderDataDwords();
	const auto& registers = program.info.user_data_registers;
	const bool has_dispatch_threads = program.bindings.dispatch_thread_dword != IR::PushData::NoStart;
	if ((program.bindings.UsesPushData() &&
	     !IR::PushData::CanFit(program.bindings.push_data_start_dword, shader_data_dwords)) ||
	    (has_dispatch_threads && (program.stage != ShaderType::Compute ||
	                              program.bindings.dispatch_thread_dword != registers.size())) ||
	    program.bindings.memory_offset_dword != registers.size() + (has_dispatch_threads ? 3u : 0u) ||
	    !std::is_sorted(registers.begin(), registers.end()) ||
	    std::adjacent_find(registers.begin(), registers.end()) != registers.end()) {
		Fail(program, "native shader-data layout is inconsistent");
	}
}

} // namespace

std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t push_data_start_dword, const Profile& profile,
                                  bool emit_debug_info) {
	using namespace Emitter;

	if (program.stage != ShaderType::Compute && program.stage != ShaderType::Vertex &&
	    program.stage != ShaderType::Pixel && program.stage != ShaderType::Mesh &&
	    program.stage != ShaderType::Local && program.stage != ShaderType::TessellationControl &&
	    program.stage != ShaderType::TessellationEvaluation) {
		Fail(program, "binary SPIR-V emitter received an unsupported shader stage");
	}
	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	IR::ValidateProgram(program, true);
	program.bindings = {.push_data_start_dword = push_data_start_dword};
	EmitterState state(program, input_info, profile);
	if (emit_debug_info) {
		char name[64];
		std::snprintf(name, sizeof(name), "rdna2-%016" PRIx64 "-block-byte-pc-plus-one",
		              program.shader_hash);
		state.debug_source = state.builder.AddString(name);
	}
	const auto* workgroup = ShaderWorkgroupInput(program.stage, input_info);
	state.lane_count =
	    workgroup != nullptr && program.wave_size == 64u && workgroup->host_subgroup_size == 32u
	        ? 2u
	        : 1u;
	DefineModule(state);
	ValidateNativeProgram(program,
	                      program.stage == ShaderType::Compute && input_info.compute->lds_storage);
	EmitProgram(state);
	state.builder.AddEntryPoint(state.mesh_compute ? spv::ExecutionModelGLCompute
	                                               : ExecutionModelForStage(state.program.stage),
	                            state.main_func, "main", state.interface_variables);

	return state.builder.Build();
}

std::vector<uint32_t> EmitFloatMinMaxCheckProgram() {
	Builder b(0x00010300u);
	b.RequireCapability(spv::CapabilityShader);
	const auto glsl          = b.Import("GLSL.std.450");
	const auto type_void     = b.Type(spv::OpTypeVoid);
	const auto type_function = b.Type(spv::OpTypeFunction, type_void);
	const auto type_u32      = b.Type(spv::OpTypeInt, 32, 0);
	const auto type_f32      = b.Type(spv::OpTypeFloat, 32);
	const auto type_uvec3    = b.Type(spv::OpTypeVector, type_u32, 3);
	const auto constant      = [&](uint32_t value) { return b.Constant(spv::OpConstant, type_u32, value); };
	const auto pointer_type  = [&](spv::StorageClass storage, uint32_t type) {
		return b.Type(spv::OpTypePointer, storage, type);
	};

	const auto array_type = b.DecoratedType(
	    spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, type_u32);
	const auto block_type = b.DecoratedType(spv::OpTypeStruct,
	                                        {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                         {spv::OpDecorate, {spv::DecorationBlock}}},
	                                        array_type);
	const auto buffer = [&](uint32_t binding, bool read_only) {
		const auto variable = b.DefineGlobalVariable(
		    pointer_type(spv::StorageClassStorageBuffer, block_type), spv::StorageClassStorageBuffer);
		b.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, 0u);
		b.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding, binding);
		if (read_only) {
			b.AddAnnotation(spv::OpDecorate, variable, spv::DecorationNonWritable);
		}
		return variable;
	};
	const auto operands = buffer(0, true);
	const auto results  = buffer(1, false);
	const auto local_id = b.DefineGlobalVariable(pointer_type(spv::StorageClassInput, type_uvec3),
	                                             spv::StorageClassInput);
	b.AddAnnotation(spv::OpDecorate, local_id, spv::DecorationBuiltIn,
	                spv::BuiltInLocalInvocationId);

	const auto main = b.AllocateId();
	b.AddFunction(spv::OpFunction, type_void, main, spv::FunctionControlMaskNone, type_function);
	b.AddFunction(spv::OpLabel, b.AllocateId());
	const auto id = b.AllocateId();
	b.AddFunction(spv::OpLoad, type_uvec3, id, local_id);
	const auto lane = b.AllocateId();
	b.AddFunction(spv::OpCompositeExtract, type_u32, lane, id, 0u);
	const auto first = b.AllocateId();
	b.AddFunction(spv::OpIMul, type_u32, first, lane, constant(2));
	const auto second = b.AllocateId();
	b.AddFunction(spv::OpIAdd, type_u32, second, first, constant(1));
	const auto element = [&](uint32_t variable, uint32_t index) {
		const auto pointer = b.AllocateId();
		b.AddFunction(spv::OpAccessChain, pointer_type(spv::StorageClassStorageBuffer, type_u32),
		              pointer, variable, constant(0), index);
		return pointer;
	};
	const auto load_f32 = [&](uint32_t index) {
		const auto raw = b.AllocateId();
		b.AddFunction(spv::OpLoad, type_u32, raw, element(operands, index));
		const auto value = b.AllocateId();
		b.AddFunction(spv::OpBitcast, type_f32, value, raw);
		return value;
	};
	const auto lhs = load_f32(first);
	const auto rhs = load_f32(second);
	const auto store = [&](uint32_t index, GLSLstd450 instruction) {
		const auto value = b.AllocateId();
		b.AddFunction(spv::OpExtInst, type_f32, value, glsl, instruction, lhs, rhs);
		const auto bits = b.AllocateId();
		b.AddFunction(spv::OpBitcast, type_u32, bits, value);
		b.AddFunction(spv::OpStore, element(results, index), bits);
	};
	store(first, GLSLstd450NMin);
	store(second, GLSLstd450NMax);
	b.AddFunction(spv::OpReturn);
	b.AddFunction(spv::OpFunctionEnd);

	b.AddExecutionMode(main, spv::ExecutionModeLocalSize, 64u, 1u, 1u);
	b.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
	b.AddEntryPoint(spv::ExecutionModelGLCompute, main, "main", {local_id});
	return b.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
