#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct ShaderPixelInputInfo;

namespace ShaderRecompiler::IR {
struct CompiledShaderInfo;
} // namespace ShaderRecompiler::IR

// A pixel parameter the generated stages forward from vertex export input_location.
struct RectListParameter {
	uint32_t input_location  = 0;
	uint32_t output_location = 0;
	bool     flat            = false;
};

struct RectListShaders {
	std::vector<uint32_t> control;
	std::vector<uint32_t> evaluation;
};

// Every parameter a rectangle-list pixel program reads.
std::vector<RectListParameter> RectListParameters(
    const ShaderPixelInputInfo& pixel_info, const ShaderRecompiler::IR::CompiledShaderInfo& program);
// Forwards the parameters the vertex stage exports, one per output location.
RectListShaders BuildRectListShaders(std::span<const RectListParameter> parameters,
                                     uint32_t                           param_export_mask);

// A geometry shader expanding each rect-list triangle into the same quad the tessellation
// shaders produce, for hosts that draw rect lists with geometry shaders.
std::vector<uint32_t> BuildRectListGeometryShader(std::span<const RectListParameter> parameters,
                                                  uint32_t                           param_export_mask);
// A geometry shader that passes each triangle on with its last vertex first, for hosts that take flat values from the first vertex
// (D3D12) when the guest takes them from the last: it forwards the position and the parameters, as the rect-list one does. `strip`:
// the triangles are a strip's.
std::vector<uint32_t> BuildLastVertexFirstGeometryShader(std::span<const RectListParameter> parameters, uint32_t param_export_mask,
                                                         bool strip);
// A geometry shader drawing each point as a square of the point size the vertex stage exports as its parameter at
// ShaderRecompiler::PointSizeParameterLocation, for hosts without point sizes (D3D12). It reads the viewport's size from
// spirv_to_dxil's runtime data, binding 0 of descriptor set runtime_data_set.
std::vector<uint32_t> BuildPointSquareGeometryShader(std::span<const RectListParameter> parameters, uint32_t param_export_mask,
                                                     uint32_t runtime_data_set);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
