#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct ShaderPixelInputInfo;
struct ShaderVertexInputInfo;

struct RectListShaders {
	std::vector<uint32_t> control;
	std::vector<uint32_t> evaluation;
};

RectListShaders BuildRectListShaders(const ShaderVertexInputInfo& vertex_info,
                                     const ShaderPixelInputInfo*  pixel_info);

// A geometry shader expanding each rect-list triangle into the same quad the tessellation
// shaders produce, for hosts that draw rect lists with geometry shaders.
std::vector<uint32_t> BuildRectListGeometryShader(const ShaderVertexInputInfo& vertex_info,
                                                  const ShaderPixelInputInfo*  pixel_info);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECTLISTSHADER_H_
