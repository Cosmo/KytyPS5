#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_

#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace Libs::Graphics {

class Buffer;
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

	// Image::ClearSelected for devices whose predication does not work (the Series X's: D3D12::PredicatedClearsWork): each slice's
	// selected clears as fullscreen draws whose pixel shader reads the check's predicate and keeps the pixel where it is zero.
	// Single-sampled targets. PrepareClearSelected makes room for the view: it may submit, so it runs before the texture cache takes
	// its lock.
	void PrepareClearSelected();
	void ClearSelected(Image& image, vk::Format format, uint32_t level, uint32_t layer, uint32_t layers, uint32_t keys,
	                   std::span<const vk::ClearAttachment> clears, const Buffer& predicates);

private:
	struct Pipeline {
		uint32_t             samples = 0;
		uint32_t             format  = 0; // the depth target's DXGI_FORMAT
		ID3D12PipelineState* state   = nullptr;
	};

	[[nodiscard]] ID3D12PipelineState* GetPipeline(uint32_t samples, uint32_t format);

	// The clear pipelines, per render target format and kind (0 float, 1 uint, 2 sint).
	struct ClearPipeline {
		uint32_t             format = 0; // the target's DXGI_FORMAT
		uint32_t             kind   = 0;
		ID3D12PipelineState* state  = nullptr;
	};
	[[nodiscard]] ID3D12PipelineState* GetClearPipeline(uint32_t format, uint32_t kind);

	GraphicContext&       m_graphics;
	CommandScheduler&     m_scheduler;
	ID3D12RootSignature*  m_root_signature = nullptr; // made with the first pipeline
	std::vector<uint8_t>  m_vertex_shader;
	std::vector<uint8_t>  m_pixel_shader;
	std::vector<Pipeline> m_pipelines;
	ID3D12RootSignature*                m_clear_root_signature = nullptr; // made with the first clear pipeline
	std::array<std::vector<uint8_t>, 3> m_clear_vertex_shaders;
	std::array<std::vector<uint8_t>, 3> m_clear_pixel_shaders;
	std::vector<ClearPipeline>          m_clear_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_
