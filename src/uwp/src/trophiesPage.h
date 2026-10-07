#ifndef EMULATOR_SRC_UWP_TROPHIESPAGE_H_
#define EMULATOR_SRC_UWP_TROPHIESPAGE_H_

#include <winrt/Windows.UI.Xaml.h>

#include <functional>

namespace Kyty::Uwp {

// The trophies of the games in the library, adapted from the desktop launcher's trophy viewer for a controller and a TV: the games with trophies
// and how far each is, a game's trophies, and a trophy's details (hidden ones only when asked). The games' trophy packages
// (sce_sys/trophy2/trophyNN.ucp) and the unlocks the emulator saved in LocalState\_Trophies are read with Common::Trophies. B (Escape, the
// back button) goes from a game back to the list and from the list to `back`.
[[nodiscard]] winrt::Windows::UI::Xaml::UIElement CreateTrophiesPage(std::function<void()> back);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_TROPHIESPAGE_H_
