#ifndef EMULATOR_SRC_UWP_CONTROLSPAGE_H_
#define EMULATOR_SRC_UWP_CONTROLSPAGE_H_

#include <winrt/Windows.UI.Xaml.h>

#include <functional>

namespace Kyty::Uwp {

// Button layout: how the Xbox controller plays a PS5 game (the mapping in input.cpp): the buttons, the sticks and triggers, the DualSense's
// extras with View held, and the app's own shortcuts, on one screen. Its title is "Button layout", after `place` in bold when given (the
// settings' "Controller"). B (Escape, the back button) goes to `back`.
[[nodiscard]] winrt::Windows::UI::Xaml::UIElement CreateControlsPage(std::function<void()> back, const wchar_t* place = nullptr);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_CONTROLSPAGE_H_
