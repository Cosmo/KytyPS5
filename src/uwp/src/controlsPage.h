#ifndef EMULATOR_SRC_UWP_CONTROLSPAGE_H_
#define EMULATOR_SRC_UWP_CONTROLSPAGE_H_

#include <winrt/Windows.UI.Xaml.h>

#include <functional>

namespace Kyty::Uwp {

// How the Xbox controller plays a PS5 game (the mapping in input.cpp): the buttons and sticks, the DualSense's extras with View held, what comes
// back to the controller, and the app's own chords. B (Escape, the back button) goes to `back`.
[[nodiscard]] winrt::Windows::UI::Xaml::UIElement CreateControlsPage(std::function<void()> back);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_CONTROLSPAGE_H_
