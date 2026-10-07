#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_WINDOWCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_WINDOWCONTEXT_H_

#include "common/threads.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

struct IDXGISwapChain1;

namespace Libs::Graphics {

class Presenter;
class RenderContext;

// What the presenter needs from the host window. The host (the UWP app's XAML page) provides the surface and shows what is presented to it.
struct WindowContext {
	WindowContext();
	~WindowContext();
	KYTY_CLASS_NO_COPY(WindowContext);

	GraphicContext                 graphic_ctx;
	std::unique_ptr<RenderContext> render_context;
	std::unique_ptr<Presenter>     presenter;
	std::atomic_bool               paused = false;

	// The host's surface size in pixels.
	std::function<std::pair<uint32_t, uint32_t>()> surface_size;
	// Gives the host the swap chain to show; called once, from the emulator thread.
	std::function<void(IDXGISwapChain1*)> attach_swapchain;
	// Called after every presented frame.
	std::function<void()> frame_presented;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_WINDOWCONTEXT_H_
