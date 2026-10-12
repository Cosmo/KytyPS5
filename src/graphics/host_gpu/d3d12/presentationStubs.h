#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PRESENTATIONSTUBS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PRESENTATIONSTUBS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace Libs::Graphics {

// The D3D12 backend draws no system overlay: a host that shows trophy unlocks itself (the UWP app) registers here. The handler runs on the thread
// that unlocked the trophy; the name and the icon (PNG, may be empty) are valid during the call only.
using TrophyUnlockHandler = std::function<void(std::string_view name, int32_t grade, std::span<const std::byte> icon_png)>;
void SetTrophyUnlockHandler(TrophyUnlockHandler handler);

// The start screen (the PS5's splash screen) is the host's: the handler runs, on the game's thread, when the game says it is ready to be seen
// (sceSystemServiceHideSplashScreen).
void SetSplashHiddenHandler(std::function<void()> handler);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_PRESENTATIONSTUBS_H_
