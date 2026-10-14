#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_

#include "common/assert.h"
#include "common/common.h"

#include <cstdint>
#include <vector>

struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace Libs::Graphics {

class CommandScheduler;
class Image;
struct GraphicContext;

// Graphics-pipeline image conversions, as upstream's BlitHelper (renderer/image/blitHelper.cpp): the texture cache's color image
// reinterpreted as multisampled depth, with the same shaders (a fullscreen triangle; each sample's depth is a channel of the
// color texel). The pipelines are made on first use, per sample count and depth format.
class BlitHelper {
public:
	BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler);
	~BlitHelper();
	KYTY_CLASS_NO_COPY(BlitHelper);

	void ReinterpretColorAsMsDepth(Image& source, Image& destination);

private:
	struct Pipeline {
		uint32_t             samples = 0;
		uint32_t             format  = 0; // the depth target's DXGI_FORMAT
		ID3D12PipelineState* state   = nullptr;
	};

	[[nodiscard]] ID3D12PipelineState* GetPipeline(uint32_t samples, uint32_t format);

	GraphicContext&       m_graphics;
	CommandScheduler&     m_scheduler;
	ID3D12RootSignature*  m_root_signature = nullptr; // made with the first pipeline
	std::vector<uint8_t>  m_vertex_shader;
	std::vector<uint8_t>  m_pixel_shader;
	std::vector<Pipeline> m_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_
