#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

namespace {

[[noreturn]] void Fail(const IR::Program& program, const char* reason) {
	EXIT("SPIR-V validation failed: hash=0x%016" PRIx64 " stage=%u reason=%s\n",
	     program.shader_hash, static_cast<unsigned>(program.stage), reason);
	std::abort();
}

} // namespace

std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t push_data_start_dword, const Profile& profile,
                                  bool emit_debug_info) {
	using namespace Emitter;

	if (!program.srt_plan_complete || !program.resource_tracking_complete ||
	    !program.shader_info_complete) {
		Fail(program, "SPIR-V emitter requires a fully planned native shader program");
	}
	IR::ValidateProgram(program);
	program.bindings = {.push_data_start_dword = push_data_start_dword};
	EmitterState state(program, input_info, profile);
	if (emit_debug_info) {
		char name[64];
		std::snprintf(name, sizeof(name), "rdna2-%016" PRIx64 "-block-byte-pc-plus-one",
		              program.shader_hash);
		state.debug_source = state.builder.AddString(name);
		state.builder.EnableNames();
	}
	const auto* workgroup = ShaderWorkgroupInput(program.stage, input_info);
	state.lane_count =
	    workgroup != nullptr && program.wave_size == 64u && workgroup->host_subgroup_size == 32u
	        ? 2u
	        : 1u;
	DefineModule(state);
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
