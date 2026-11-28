#include "graphics/shader/rectListShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/HostShaderFeatures.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::Spirv::Builder;

constexpr uint32_t SpirvVersion15 = 0x00010500u;

class RectListEmitter {
public:
	RectListEmitter(const std::vector<RectListParameter>& parameters_, spv::ExecutionModel model)
	    : parameters(parameters_) {
		builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);

		void_type       = builder.Type(spv::OpTypeVoid);
		uint_type       = builder.Type(spv::OpTypeInt, 32u, 0u);
		int_type        = builder.Type(spv::OpTypeInt, 32u, 1u);
		float_type      = builder.Type(spv::OpTypeFloat, 32u);
		vec4_float_type = builder.Type(spv::OpTypeVector, float_type, 4u);
		function_type   = builder.Type(spv::OpTypeFunction, void_type);

		per_vertex_type = builder.DecoratedType(
		    spv::OpTypeStruct,
		    {{spv::OpMemberDecorate, {0u, spv::DecorationBuiltIn, spv::BuiltInPosition}},
		     {spv::OpDecorate, {spv::DecorationBlock}}},
		    vec4_float_type);

		ptr_input_vec4_float  = Pointer(spv::StorageClassInput, vec4_float_type);
		ptr_output_vec4_float = Pointer(spv::StorageClassOutput, vec4_float_type);
		if (model != spv::ExecutionModelTessellationEvaluation) {
			bool_type        = builder.Type(spv::OpTypeBool);
			vec2_bool_type   = builder.Type(spv::OpTypeVector, bool_type, 2u);
			vec2_float_type  = builder.Type(spv::OpTypeVector, float_type, 2u);
			ptr_output_float = Pointer(spv::StorageClassOutput, float_type);
		} else {
			vec3_float_type = builder.Type(spv::OpTypeVector, float_type, 3u);
			ptr_input_float = Pointer(spv::StorageClassInput, float_type);
		}
	}

	std::vector<uint32_t> EmitControl() {
		DefineEntry(spv::ExecutionModelTessellationControl);
		const auto float_one = Constant(float_type, std::bit_cast<uint32_t>(1.0f));

		for (uint32_t i = 0; i < 4; i++) {
			Store(Access(ptr_output_float, tess_outer, Int(i)), float_one);
		}
		for (uint32_t i = 0; i < 2; i++) {
			Store(Access(ptr_output_float, tess_inner, Int(i)), float_one);
		}

		const auto positions  = LoadPositions();
		const auto corners    = FindCorners(positions);
		const auto invocation = Load(int_type, invocation_id);
		const auto is_fourth  = Result(spv::OpIEqual, bool_type, invocation, Int(3));
		const auto index      = Result(
            spv::OpSMod, int_type, Result(spv::OpIAdd, int_type, corners.first_vertex, invocation),
            Int(3));

		const auto position3 = Interpolate(positions[0], positions[1], positions[2], corners.weights);
		const auto position =
		    Result(spv::OpSelect, vec4_float_type, is_fourth, position3,
		           Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0))));
		Store(Access(ptr_output_vec4_float, gl_out, invocation, Int(0)), position);

		for (uint32_t i = 0; i < parameters.size(); i++) {
			const auto input0 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(0)));
			if (parameters[i].flat) {
				Store(Access(ptr_output_vec4_float, outputs[i], invocation), input0);
				continue;
			}
			const auto input1 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(1)));
			const auto input2 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(2)));
			const auto input3 = Interpolate(input0, input1, input2, corners.weights);
			const auto value =
			    Result(spv::OpSelect, vec4_float_type, is_fourth, input3,
			           Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
			Store(Access(ptr_output_vec4_float, outputs[i], invocation), value);
		}

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

	// Emits the quad's corners as a triangle strip, in the tessellation domain's corner order.
	std::vector<uint32_t> EmitGeometry() {
		DefineEntry(spv::ExecutionModelGeometry);

		const auto positions = LoadPositions();
		const auto corners   = FindCorners(positions);
		for (uint32_t corner = 0; corner < 4; corner++) {
			const bool computed = corner == 3;
			const auto index    = computed ? 0u
			                               : Result(spv::OpSMod, int_type,
			                                        Result(spv::OpIAdd, int_type, corners.first_vertex,
			                                               Int(corner)),
			                                        Int(3));
			const auto position =
			    computed ? Interpolate(positions[0], positions[1], positions[2], corners.weights)
			             : Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0)));
			Store(Access(ptr_output_vec4_float, gl_out, Int(0)), position);
			for (uint32_t i = 0; i < parameters.size(); i++) {
				const auto input0 =
				    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(0)));
				auto value = input0;
				if (!parameters[i].flat && !computed) {
					value = Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index));
				} else if (!parameters[i].flat) {
					const auto input1 =
					    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(1)));
					const auto input2 =
					    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(2)));
					value = Interpolate(input0, input1, input2, corners.weights);
				}
				Store(outputs[i], value);
			}
			builder.AddFunction(spv::OpEmitVertex);
		}
		builder.AddFunction(spv::OpEndPrimitive);

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

	// Emits the triangle with its last vertex first (2, 0, 1: the same winding), so that the host, which takes flat values from a
	// primitive's first vertex, takes them from the last as the guest asked. A strip's odd triangles reach the geometry shader with
	// their last vertex second, so those start at 1 (as Mesa's d3d12 driver does, d3d12_emit_triangles); the parity is the primitive
	// ID's, which a strip cut does not reset (Mesa's limit too).
	std::vector<uint32_t> EmitLastVertexFirst(bool strip) {
		geometry_vertices = 3;
		read_primitive_id = strip;
		DefineEntry(spv::ExecutionModelGeometry);
		const auto first =
		    strip ? Result(spv::OpISub, int_type, Int(2), Result(spv::OpBitwiseAnd, int_type, Load(int_type, primitive_id), Int(1)))
		          : Int(2);
		for (uint32_t k = 0; k < 3; k++) {
			const auto vertex = Result(spv::OpSMod, int_type, Result(spv::OpIAdd, int_type, first, Int(k)), Int(3));
			Store(Access(ptr_output_vec4_float, gl_out, Int(0)),
			      Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, vertex, Int(0))));
			for (uint32_t i = 0; i < parameters.size(); i++) {
				Store(outputs[i], Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], vertex)));
			}
			builder.AddFunction(spv::OpEmitVertex);
		}
		builder.AddFunction(spv::OpEndPrimitive);

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

	// Draws each point as a square of its point size in pixels (at least 1), its corners a triangle strip; they all take the point's
	// parameters. The size is the vertex stage's parameter at PointSizeParameterLocation, the viewport's size the host's runtime data
	// (spirv_to_dxil's dxil_spirv_vertex_runtime_data: viewport_width at byte 20, viewport_height at 24) in descriptor set
	// runtime_data_set, binding 0.
	std::vector<uint32_t> EmitPointSquares(uint32_t runtime_data_set) {
		input_vertices    = 1;
		geometry_vertices = 4;
		read_point_size   = true;
		ptr_input_float   = Pointer(spv::StorageClassInput, float_type);
		const auto runtime_type =
		    builder.DecoratedType(spv::OpTypeStruct,
		                          {{spv::OpMemberDecorate, {0u, spv::DecorationOffset, 20u}},
		                           {spv::OpMemberDecorate, {1u, spv::DecorationOffset, 24u}},
		                           {spv::OpDecorate, {spv::DecorationBlock}}},
		                          float_type, float_type);
		const auto runtime_data = AddInterface(spv::StorageClassUniform, runtime_type);
		Decorate(runtime_data, spv::DecorationDescriptorSet, runtime_data_set);
		Decorate(runtime_data, spv::DecorationBinding, 0u);
		DefineEntry(spv::ExecutionModelGeometry);

		const auto ptr_uniform_float = Pointer(spv::StorageClassUniform, float_type);
		const auto float_one         = Constant(float_type, std::bit_cast<uint32_t>(1.0f));
		const auto float_zero        = Constant(float_type, 0u);
		const auto position = Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, Int(0), Int(0)));
		const auto read     = Load(float_type, Access(ptr_input_float, point_size, Int(0)));
		const auto size     = Result(spv::OpSelect, float_type, Result(spv::OpFOrdLessThan, bool_type, read, float_one), float_one, read);
		// Half the square in clip space: size / 2 pixels are size / viewport of the -1 to 1 range, times w.
		const auto w      = Result(spv::OpCompositeExtract, float_type, position, 3u);
		const auto width  = Load(float_type, Access(ptr_uniform_float, runtime_data, Int(0)));
		const auto height = Load(float_type, Access(ptr_uniform_float, runtime_data, Int(1)));
		const auto half_x = Result(spv::OpFMul, float_type, Result(spv::OpFDiv, float_type, size, width), w);
		const auto half_y = Result(spv::OpFMul, float_type, Result(spv::OpFDiv, float_type, size, height), w);
		const auto left   = Result(spv::OpFNegate, float_type, half_x);
		const auto top    = Result(spv::OpFNegate, float_type, half_y);
		for (const auto& [x, y]: {std::pair {left, top}, std::pair {half_x, top}, std::pair {left, half_y}, std::pair {half_x, half_y}}) {
			const auto offset = Result(spv::OpCompositeConstruct, vec4_float_type, x, y, float_zero, float_zero);
			Store(Access(ptr_output_vec4_float, gl_out, Int(0)), Result(spv::OpFAdd, vec4_float_type, position, offset));
			for (uint32_t i = 0; i < parameters.size(); i++) {
				Store(outputs[i], Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(0))));
			}
			builder.AddFunction(spv::OpEmitVertex);
		}
		builder.AddFunction(spv::OpEndPrimitive);

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

	std::vector<uint32_t> EmitEvaluation() {
		DefineEntry(spv::ExecutionModelTessellationEvaluation);

		const auto x     = Load(float_type, Access(ptr_input_float, tess_coord, Int(0)));
		const auto y     = Load(float_type, Access(ptr_input_float, tess_coord, Int(1)));
		const auto index =
		    Result(spv::OpIAdd, int_type,
		           Result(spv::OpIMul, int_type, Result(spv::OpConvertFToS, int_type, y), Int(2)),
		           Result(spv::OpConvertFToS, int_type, x));

		const auto position =
		    Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0)));
		Store(Access(ptr_output_vec4_float, gl_out, Int(0)), position);
		for (uint32_t i = 0; i < parameters.size(); i++) {
			Store(outputs[i],
			      Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
		}

		builder.AddFunction(spv::OpReturn);
		builder.AddFunction(spv::OpFunctionEnd);
		return builder.Build();
	}

private:
	// The rectangle's corners: the input vertex opposite its diagonal comes first, and the
	// weights of the input vertices that extrapolate the missing fourth corner.
	struct Corners {
		std::array<uint32_t, 3> weights {};
		uint32_t                first_vertex = 0;
	};

	std::array<uint32_t, 3> LoadPositions() {
		std::array<uint32_t, 3> positions {};
		for (uint32_t i = 0; i < positions.size(); i++) {
			positions[i] =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, Int(i), Int(0)));
		}
		return positions;
	}

	Corners FindCorners(const std::array<uint32_t, 3>& positions) {
		std::array<uint32_t, 3> coordinate_equal {};
		for (uint32_t i = 0; i < coordinate_equal.size(); i++) {
			const auto left =
			    Result(spv::OpVectorShuffle, vec2_float_type, positions[i], positions[i], 0u, 1u);
			const auto right = Result(spv::OpVectorShuffle, vec2_float_type,
			                          positions[(i + 1u) % 3u], positions[(i + 1u) % 3u], 0u, 1u);
			coordinate_equal[i] = Result(spv::OpFOrdEqual, vec2_bool_type, left, right);
		}

		Corners                 corners;
		std::array<uint32_t, 3> edge_vertex {};
		const auto float_one       = Constant(float_type, std::bit_cast<uint32_t>(1.0f));
		const auto float_minus_one = Constant(float_type, std::bit_cast<uint32_t>(-1.0f));
		for (uint32_t i = 0; i < edge_vertex.size(); i++) {
			const auto previous = (i + 2u) % 3u;
			const auto xy =
			    Result(spv::OpLogicalAnd, bool_type,
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[i], 0u),
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[previous], 1u));
			const auto yx =
			    Result(spv::OpLogicalAnd, bool_type,
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[i], 1u),
			           Result(spv::OpCompositeExtract, bool_type, coordinate_equal[previous], 0u));
			edge_vertex[i] = Result(spv::OpLogicalOr, bool_type, xy, yx);
			corners.weights[i] =
			    Result(spv::OpSelect, float_type, edge_vertex[i], float_minus_one, float_one);
		}

		auto vertex_index    = Result(spv::OpSelect, int_type, edge_vertex[2], Int(2), Int(0));
		corners.first_vertex = Result(spv::OpSelect, int_type, edge_vertex[1], Int(1), vertex_index);
		return corners;
	}

	uint32_t Constant(uint32_t type, uint32_t value) {
		return builder.Constant(spv::OpConstant, type, value);
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return builder.Type(spv::OpTypePointer, storage, type);
	}

	uint32_t Array(uint32_t type, uint32_t size) {
		return builder.Type(spv::OpTypeArray, type, Uint(size));
	}

	template <typename... Args>
	uint32_t Result(spv::Op opcode, uint32_t type, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, type, id, operands...);
		return id;
	}

	template <typename... Args>
	uint32_t ResultWithoutType(spv::Op opcode, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction(opcode, id, operands...);
		return id;
	}

	template <typename... Args>
	uint32_t Access(uint32_t pointer_type, uint32_t base, Args... indices) {
		return Result(spv::OpAccessChain, pointer_type, base, indices...);
	}

	uint32_t Load(uint32_t type, uint32_t pointer) { return Result(spv::OpLoad, type, pointer); }

	void Store(uint32_t pointer, uint32_t value) { builder.AddFunction(spv::OpStore, pointer, value); }

	uint32_t Int(uint32_t value) { return Constant(int_type, value); }

	uint32_t Uint(uint32_t value) { return Constant(uint_type, value); }

	uint32_t AddInterface(spv::StorageClass storage, uint32_t type) {
		const auto variable = builder.DefineGlobalVariable(Pointer(storage, type), storage);
		interfaces.push_back(variable);
		return variable;
	}

	void Decorate(uint32_t target, spv::Decoration decoration, uint32_t value) {
		builder.AddAnnotation(spv::OpDecorate, target, decoration, value);
	}

	void DefineEntry(spv::ExecutionModel model) {
		builder.RequireCapability(spv::CapabilityShader);
		builder.RequireCapability(model == spv::ExecutionModelGeometry ? spv::CapabilityGeometry
		                                                               : spv::CapabilityTessellation);
		main = Result(spv::OpFunction, void_type, spv::FunctionControlMaskNone, function_type);
		if (model == spv::ExecutionModelTessellationControl) {
			builder.AddExecutionMode(main, spv::ExecutionModeOutputVertices, 4u);
		} else if (model == spv::ExecutionModelGeometry) {
			builder.AddExecutionMode(main, input_vertices == 1 ? spv::ExecutionModeInputPoints : spv::ExecutionModeTriangles);
			builder.AddExecutionMode(main, spv::ExecutionModeInvocations, 1u);
			builder.AddExecutionMode(main, spv::ExecutionModeOutputTriangleStrip);
			builder.AddExecutionMode(main, spv::ExecutionModeOutputVertices, geometry_vertices);
		} else {
			builder.AddExecutionMode(main, spv::ExecutionModeQuads);
			builder.AddExecutionMode(main, spv::ExecutionModeSpacingEqual);
			builder.AddExecutionMode(main, spv::ExecutionModeVertexOrderCw);
		}
		DefineInputs(model);
		DefineOutputs(model);
		builder.AddEntryPoint(model, main, "main", interfaces);
		ResultWithoutType(spv::OpLabel);
	}

	void DefineInputs(spv::ExecutionModel model) {
		// The control and geometry shaders read the rect-list triangle; evaluation reads the quad.
		const auto tess_control = model != spv::ExecutionModelTessellationEvaluation;
		if (model == spv::ExecutionModelTessellationControl) {
			invocation_id = AddInterface(spv::StorageClassInput, int_type);
			Decorate(invocation_id, spv::DecorationBuiltIn, spv::BuiltInInvocationId);
		} else if (model == spv::ExecutionModelTessellationEvaluation) {
			tess_coord = AddInterface(spv::StorageClassInput, vec3_float_type);
			Decorate(tess_coord, spv::DecorationBuiltIn, spv::BuiltInTessCoord);
		}
		if (read_primitive_id) {
			primitive_id = AddInterface(spv::StorageClassInput, int_type);
			Decorate(primitive_id, spv::DecorationBuiltIn, spv::BuiltInPrimitiveId);
		}
		if (read_point_size) {
			point_size = AddInterface(spv::StorageClassInput, Array(float_type, input_vertices));
			Decorate(point_size, spv::DecorationLocation, ShaderRecompiler::PointSizeParameterLocation);
		}
		gl_in = AddInterface(spv::StorageClassInput, Array(per_vertex_type, tess_control ? input_vertices : 4u));

		inputs.resize(parameters.size());
		std::array<uint32_t, ShaderVertexInputInfo::RES_MAX> locations {};
		for (uint32_t i = 0; i < parameters.size(); i++) {
			const auto location =
			    tess_control ? parameters[i].input_location : parameters[i].output_location;
			if (tess_control && locations[location] != 0) {
				inputs[i] = locations[location];
				continue;
			}
			inputs[i] = AddInterface(spv::StorageClassInput,
			                         Array(vec4_float_type, tess_control ? input_vertices : 4u));
			Decorate(inputs[i], spv::DecorationLocation, location);
			locations[location] = inputs[i];
		}
	}

	void DefineOutputs(spv::ExecutionModel model) {
		const auto tess_control = model == spv::ExecutionModelTessellationControl;
		if (tess_control) {
			gl_out     = AddInterface(spv::StorageClassOutput, Array(per_vertex_type, 4u));
			tess_inner = AddInterface(spv::StorageClassOutput, Array(float_type, 2u));
			Decorate(tess_inner, spv::DecorationBuiltIn, spv::BuiltInTessLevelInner);
			builder.AddAnnotation(spv::OpDecorate, tess_inner, spv::DecorationPatch);
			tess_outer = AddInterface(spv::StorageClassOutput, Array(float_type, 4u));
			Decorate(tess_outer, spv::DecorationBuiltIn, spv::BuiltInTessLevelOuter);
			builder.AddAnnotation(spv::OpDecorate, tess_outer, spv::DecorationPatch);
		} else {
			gl_out = AddInterface(spv::StorageClassOutput, per_vertex_type);
		}

		outputs.resize(parameters.size());
		for (uint32_t i = 0; i < parameters.size(); i++) {
			outputs[i] = AddInterface(spv::StorageClassOutput,
			                          tess_control ? Array(vec4_float_type, 4u) : vec4_float_type);
			Decorate(outputs[i], spv::DecorationLocation, parameters[i].output_location);
		}
	}

	uint32_t Interpolate(uint32_t v0, uint32_t v1, uint32_t v2,
	                     const std::array<uint32_t, 3>& barycentric) {
		const auto p0 = Result(spv::OpVectorTimesScalar, vec4_float_type, v0, barycentric[0]);
		const auto p1 = Result(spv::OpVectorTimesScalar, vec4_float_type, v1, barycentric[1]);
		const auto p2 = Result(spv::OpVectorTimesScalar, vec4_float_type, v2, barycentric[2]);
		return Result(spv::OpFAdd, vec4_float_type, p0,
		              Result(spv::OpFAdd, vec4_float_type, p1, p2));
	}

	Builder                               builder {SpirvVersion15};
	const std::vector<RectListParameter>& parameters;
	std::vector<uint32_t>                 interfaces;
	std::vector<uint32_t>                 inputs;
	std::vector<uint32_t>                 outputs;
	uint32_t                              main                  = 0;
	uint32_t                              input_vertices        = 3; // a control or geometry shader's input vertices
	uint32_t                              geometry_vertices     = 4; // a geometry shader's output vertices
	bool                                  read_primitive_id     = false;
	bool                                  read_point_size       = false;
	uint32_t                              primitive_id          = 0;
	uint32_t                              point_size            = 0;
	uint32_t                              void_type             = 0;
	uint32_t                              bool_type             = 0;
	uint32_t                              uint_type             = 0;
	uint32_t                              int_type              = 0;
	uint32_t                              float_type            = 0;
	uint32_t                              vec2_bool_type        = 0;
	uint32_t                              vec2_float_type       = 0;
	uint32_t                              vec3_float_type       = 0;
	uint32_t                              vec4_float_type       = 0;
	uint32_t                              function_type         = 0;
	uint32_t                              per_vertex_type       = 0;
	uint32_t                              ptr_input_float       = 0;
	uint32_t                              ptr_input_vec4_float  = 0;
	uint32_t                              ptr_output_float      = 0;
	uint32_t                              ptr_output_vec4_float = 0;
	uint32_t                              gl_in                 = 0;
	uint32_t                              gl_out                = 0;
	uint32_t                              tess_inner            = 0;
	uint32_t                              tess_outer            = 0;
	uint32_t                              tess_coord            = 0;
	uint32_t                              invocation_id         = 0;
};

} // namespace

std::vector<RectListParameter> RectListParameters(
    const ShaderPixelInputInfo& pixel_info, const ShaderRecompiler::IR::CompiledShaderInfo& program) {
	EXIT_IF(pixel_info.input_num > ShaderVertexInputInfo::RES_MAX);
	std::vector<uint32_t> active_inputs;
	for (const auto& input: program.info.inputs) {
		if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter) {
			active_inputs.push_back(input.location);
		}
	}
	std::vector<RectListParameter> parameters;
	for (const auto input: active_inputs) {
		parameters.push_back({ShaderPixelParameterMappedLocation(pixel_info, input),
		                      ShaderPixelParameterLocation(pixel_info, active_inputs, input),
		                      ShaderPixelParameterIsFlat(pixel_info, input)});
	}
	return parameters;
}

// The parameters the vertex stage exports, one per output location.
static std::vector<RectListParameter> ExportedParameters(std::span<const RectListParameter> parameters, uint32_t param_export_mask) {
	std::vector<RectListParameter> exported;
	std::array<bool, 32>           output_locations {};
	for (const auto& parameter: parameters) {
		if ((param_export_mask & (1u << parameter.input_location)) != 0 && !output_locations[parameter.output_location]) {
			exported.push_back(parameter);
			output_locations[parameter.output_location] = true;
		}
	}
	return exported;
}

RectListShaders BuildRectListShaders(std::span<const RectListParameter> parameters,
                                     uint32_t                           param_export_mask) {
	const auto      exported = ExportedParameters(parameters, param_export_mask);
	RectListEmitter control(exported, spv::ExecutionModelTessellationControl);
	RectListEmitter evaluation(exported, spv::ExecutionModelTessellationEvaluation);
	return {control.EmitControl(), evaluation.EmitEvaluation()};
}

std::vector<uint32_t> BuildRectListGeometryShader(std::span<const RectListParameter> parameters,
                                                  uint32_t                           param_export_mask) {
	const auto      exported = ExportedParameters(parameters, param_export_mask);
	RectListEmitter geometry(exported, spv::ExecutionModelGeometry);
	return geometry.EmitGeometry();
}

std::vector<uint32_t> BuildLastVertexFirstGeometryShader(std::span<const RectListParameter> parameters, uint32_t param_export_mask,
                                                         bool strip) {
	const auto      exported = ExportedParameters(parameters, param_export_mask);
	RectListEmitter geometry(exported, spv::ExecutionModelGeometry);
	return geometry.EmitLastVertexFirst(strip);
}

std::vector<uint32_t> BuildPointSquareGeometryShader(std::span<const RectListParameter> parameters, uint32_t param_export_mask,
                                                     uint32_t runtime_data_set) {
	const auto      exported = ExportedParameters(parameters, param_export_mask);
	RectListEmitter geometry(exported, spv::ExecutionModelGeometry);
	return geometry.EmitPointSquares(runtime_data_set);
}

} // namespace Libs::Graphics