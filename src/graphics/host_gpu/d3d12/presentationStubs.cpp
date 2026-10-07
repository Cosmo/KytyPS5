#include "common/logging/log.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/systemOverlay.h"

#include <fmt/format.h>

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

// There is no overlay to show a trophy in: the unlock is only written to the log.
void NotifyTrophyUnlocked(std::string_view name, int32_t grade, std::span<const std::byte> /*icon_png*/) {
	Log::WriteToConsoleAndLog(fmt::format("Trophy unlocked: {} (grade {})\n", name, grade));
}

} // namespace Libs::Graphics
