// The emulator's window for the UWP app, in place of the SDL window (graphics/presentation/window/window.cpp). The window is a XAML SwapChainPanel owned by the
// app: this creates the D3D12 device, the renderer and the presenter, hands the presenter's swap chain to the panel (EmulatorHost), and waits until the app
// asks the game to stop.
#include "graphics/host_gpu/frameResolution.h"
#include "graphics/presentation/window.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "emulatorHost.h"
#include "input.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/d3d12/windowContext.h"
#include "graphics/host_gpu/workCounters.h"
#include "graphics/presentation/presenter.h"
#include "kernel/fileSystem.h"
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
                if (const auto shaders = Kyty::Uwp::CurrentShaderStatistics()) {
                    Kyty::Uwp::Log("shaders: %llu drawing, %llu compute; pipelines: %llu (%llu from the cache, %llu saved)\n",
                                   static_cast<unsigned long long>(shaders->drawing_shaders), static_cast<unsigned long long>(shaders->compute_shaders),
                                   static_cast<unsigned long long>(shaders->pipelines), static_cast<unsigned long long>(shaders->cached_pipelines),
                                   static_cast<unsigned long long>(shaders->saved_pipelines));
                }
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
	// The game is running: from here the connected controllers are its players, and View + Menu opens the game menu.
	Kyty::Uwp::ActivateGameInput();
	Kyty::Uwp::Log("game input active\n");
	return presenter;
}

void WindowRun() {
	EXIT_IF(g_window == nullptr);
	// The app's UI thread runs the window; this thread only waits for the end of the game.
	while (!Kyty::Uwp::EmulatorHost::Get().WaitForExit(250)) {
	}
	// What the renderer made is kept for the next start (a dirty build, or a change since the last save, saves nothing).
	if (g_window->render_context != nullptr) {
		Common::LockGuard lock(g_window->render_context->GetMutex());
		g_window->render_context->GetPipelineCache().Save();
	}
	// Back to the launcher if the player asked for it (the process ends right after).
	Kyty::Uwp::EmulatorHost::Get().Finish();
}

void WindowShutdown() {
	g_window.reset();
}

} // namespace Libs::Graphics

namespace Kyty::Uwp {

FrameResolutions CurrentFrameResolutions() {
	const auto sizes = Libs::Graphics::FrameResolution::Last();
	return {sizes.output_width, sizes.output_height, sizes.render_width, sizes.render_height};
}

MemoryTotals CurrentMemoryTotals() {
	const auto   guest = Libs::LibKernel::Memory::GetGuestMemoryTotals();
	MemoryTotals totals {.game         = guest.resident,
	                     .swapped      = guest.swapped,
	                     .swapped_out  = guest.swapped_out,
	                     .swapped_in   = guest.swapped_in,
	                     .swap_wait_ns = guest.swap_wait_ns,
	                     .pager        = guest.pager};
	if (const auto* window = Libs::Graphics::g_window.get(); window != nullptr && window->graphic_ctx.CanReportMemoryUsage()) {
		totals.gpu = window->graphic_ctx.GetLocalMemoryUsage();
	}
	return totals;
}

// The shaders and pipelines the renderer has made so far (the overlay shows them); none before the game's renderer exists.
std::optional<ShaderStatistics> CurrentShaderStatistics() {
	const auto* window = Libs::Graphics::g_window.get();
	if (window == nullptr || window->render_context == nullptr) {
		return std::nullopt;
	}
	const auto counts = window->render_context->GetPipelineCache().GetStatistics();
	ShaderStatistics statistics;
	statistics.drawing_shaders  = counts.drawing_shaders;
	statistics.compute_shaders  = counts.compute_shaders;
	statistics.new_shaders      = counts.new_shaders;
	statistics.pipelines        = counts.graphics_pipelines + counts.compute_pipelines;
	statistics.cached_pipelines = counts.cached_pipelines;
	statistics.saved_pipelines  = counts.saved_pipelines;
	return statistics;
}

WorkTotals CurrentWorkTotals() {
	const auto& counters = Libs::Graphics::GetWorkCounters();
	return {.draws         = counters.draws.load(std::memory_order_relaxed),
	        .skipped_draws = counters.skipped_draws.load(std::memory_order_relaxed),
	        .skip_reason   = counters.skip_reason.load(std::memory_order_relaxed),
	        .gpu_ns        = counters.gpu_ns.load(std::memory_order_relaxed),
	        .compile_ns    = counters.compile_ns.load(std::memory_order_relaxed),
	        .bytes_read    = Libs::LibKernel::FileSystem::BytesRead()};
}

} // namespace Kyty::Uwp
