#include "graphics/host_gpu/d3d12/presentationStubs.h"

#include "common/logging/log.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/systemOverlay.h"

#include <fmt/format.h>

#include <mutex>
#include <utility>

// The RenderDoc integration, the system overlay and the startup splash screen are Vulkan only; the D3D12 backend has none.

namespace Libs::Graphics {

void InitializeSplashScreen(const std::filesystem::path& /*sce_sys*/) {}

void HideSplashScreen() {}

void RenderDocInit() {}

void RenderDocRequestCapture() {}

void RenderDocOnGuestFlip(RenderContext& /*renderer*/) {}

// The performance panel draws with the system overlay's ImGui on Vulkan; the D3D12 backend has none.
void PerformanceOverlayRecordFlip(uint32_t /*guest_width*/, uint32_t /*guest_height*/) noexcept {}

bool PerformanceOverlayEnabled() noexcept {
	return false;
}

void TogglePerformanceOverlay() noexcept {}

void TogglePerformanceOverlayDetails() noexcept {}

uint64_t PerformanceOverlayRevision() noexcept {
	return 0;
}

namespace {

std::mutex          g_trophy_handler_mutex;
TrophyUnlockHandler g_trophy_handler;

} // namespace

void SetTrophyUnlockHandler(TrophyUnlockHandler handler) {
	std::scoped_lock lock(g_trophy_handler_mutex);
	g_trophy_handler = std::move(handler);
}

// There is no overlay to show a trophy in: the unlock is written to the log and handed to the host, when it shows unlocks.
void NotifyTrophyUnlocked(std::string_view name, int32_t grade, std::span<const std::byte> icon_png) {
	Log::WriteToConsoleAndLog(fmt::format("Trophy unlocked: {} (grade {})\n", name, grade));
	TrophyUnlockHandler handler;
	{
		std::scoped_lock lock(g_trophy_handler_mutex);
		handler = g_trophy_handler;
	}
	if (handler) {
		handler(name, grade, icon_png);
	}
}

} // namespace Libs::Graphics
