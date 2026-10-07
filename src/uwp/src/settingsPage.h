#ifndef EMULATOR_SRC_UWP_SETTINGSPAGE_H_
#define EMULATOR_SRC_UWP_SETTINGSPAGE_H_

#include <winrt/Windows.UI.Xaml.h>

#include <functional>

namespace Kyty::Uwp {

// The settings page, in the system's look: the games' resolution and V-Sync, the user's name and
// the console language, the game folders and developer options (AppSettings). Changes are saved
// at once and apply to the next game. B (or Escape, or the window's back button) calls `back`,
// with whether the game folders changed.
[[nodiscard]] winrt::Windows::UI::Xaml::UIElement
CreateSettingsPage(std::function<void(bool folders_changed)> back);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_SETTINGSPAGE_H_
