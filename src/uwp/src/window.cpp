// The emulator's window for the UWP app, in place of the SDL window (graphics/presentation/window/window.cpp). The window is a XAML SwapChainPanel owned by the
// app: this creates the D3D12 device, the renderer and the presenter, hands the presenter's swap chain to the panel (EmulatorHost), and waits until the app
// asks the game to stop.
#include "graphics/presentation/window.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "emulatorHost.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/d3d12/windowContext.h"
#include "graphics/presentation/presenter.h"
#include "kernel/memory.h"
#include "log.h"

#include <windows.h>

#include <memory>
#include <string>

namespace Libs::Graphics {

namespace {

std::unique_ptr<WindowContext> g_window;

} // namespace

WindowContext::WindowContext() = default;

WindowContext::~WindowContext() {
	presenter.reset();
	LibKernel::Memory::InstallGpuResources(nullptr);
	render_context.reset();
	graphic_ctx.Destroy();
}

Presenter& WindowInit(uint32_t width, uint32_t height) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	EXIT_IF(g_window != nullptr);

	using Kyty::Uwp::EmulatorHost;

	auto window                       = std::make_unique<WindowContext>();
	window->graphic_ctx.screen_width  = width;
	window->graphic_ctx.screen_height = height;
	window->surface_size              = [] { return EmulatorHost::Get().PanelPixelSize(); };
	window->attach_swapchain          = [](IDXGISwapChain1* swapchain) { EmulatorHost::Get().AttachSwapChain(swapchain); };
	window->frame_presented           = [] {
        EmulatorHost::Get().FramePresented();
        // The window title shows the progress (`uwp.ps1 run` reads it): the frame count and the frames of the last second.
        static uint64_t frames        = 0;
        static uint64_t second_start  = GetTickCount64();
        static uint64_t second_frames = 0;
        static uint64_t last_logged   = 0;
        frames++;
        second_frames++;
        const auto now = GetTickCount64();
        if (now - second_start >= 1000) {
            EmulatorHost::Get().SetTitle("KytyPS5 frame: " + std::to_string(frames) + ", fps: " + std::to_string(second_frames * 1000 / (now - second_start)));
            // the same, every 5 seconds, in the app log (a console shows no window title)
            if (now - last_logged >= 5000) {
                Kyty::Uwp::Log("frame: %llu, fps: %llu\n", static_cast<unsigned long long>(frames),
                               static_cast<unsigned long long>(second_frames * 1000 / (now - second_start)));
                last_logged = now;
            }
            second_start  = now;
            second_frames = 0;
        }
	};

	window->graphic_ctx.Create(Config::VulkanValidationEnabled());
	window->render_context = std::make_unique<RenderContext>(window->graphic_ctx);
	LibKernel::Memory::InstallGpuResources(window->render_context.get());
	window->presenter = std::make_unique<Presenter>(*window);

	auto& presenter = *window->presenter;
	g_window        = std::move(window);
	return presenter;
}

void WindowRun() {
	EXIT_IF(g_window == nullptr);
	// The app's UI thread runs the window; this thread only waits for the end of the game.
	while (!Kyty::Uwp::EmulatorHost::Get().WaitForExit(250)) {
	}
	// Back to the launcher if the player asked for it (the process ends right after).
	Kyty::Uwp::EmulatorHost::Get().Finish();
}

void WindowShutdown() {
	g_window.reset();
}

} // namespace Libs::Graphics
