#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_FIDELITYFX_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_FIDELITYFX_H_

#include "graphics/host_gpu/d3d12/d3d12Common.h"

#include <memory>

namespace Libs::Graphics {
namespace Pssr {
struct Request;
}

class CommandBuffer;
class RenderContext;

// PSSR and flip preparation use this on the guest GPU thread, under the renderer lock.
class FidelityFx final {
public:
	explicit FidelityFx(RenderContext& renderer);
	~FidelityFx();

	bool BeginPssr(const Pssr::Request& request, uint32_t width, uint32_t height);
	enum class Input { Color, Depth, Motion, Exposure };
	bool CopyInput(CommandBuffer& command, Input input, D3D12_CPU_DESCRIPTOR_HANDLE source,
	               uint32_t width, uint32_t height, uint32_t x, uint32_t y);
	D3D12_CPU_DESCRIPTOR_HANDLE Upscale(CommandBuffer& command);
	void FinishUpscale(CommandBuffer& command);
	void SkipFlip();
	bool Interpolate(CommandBuffer& command, ID3D12Resource* color, DXGI_FORMAT format, bool hdr,
	                 D3D12::ComPtr<ID3D12Resource>& generated);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif
