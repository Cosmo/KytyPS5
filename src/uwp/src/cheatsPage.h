#ifndef EMULATOR_SRC_UWP_CHEATSPAGE_H_
#define EMULATOR_SRC_UWP_CHEATSPAGE_H_

#include "gameLibrary.h"

#include <winrt/Windows.UI.Xaml.h>

#include <functional>

namespace Kyty::Uwp {

// A game's cheats, the desktop launcher's Cheats dialog for a controller and a TV (cheats.h): on the left the cheats saved on this console, each
// switched on or off with A and saved at once; on the right the HEN Cheats Collection's files for the game (the installed version first), a
// file's cheats as a preview, and Download, which saves them on this console all switched off. The switched-on cheats apply when the game
// starts. Y reads both again; B (Escape, the back button) goes to `back`.
[[nodiscard]] winrt::Windows::UI::Xaml::UIElement CreateCheatsPage(GameEntry game, std::function<void()> back);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_CHEATSPAGE_H_
