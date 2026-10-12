#ifndef EMULATOR_SRC_UWP_KEYBOARD_H_
#define EMULATOR_SRC_UWP_KEYBOARD_H_

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <cstdint>
#include <functional>

// When a game asks the player for text, a name for example: a box over the game with the game's text in it, typed with the Xbox's on-screen
// keyboard (on a PC, with the keyboard). Done or Enter gives the text to the game, B or Escape cancels. The emulator keeps the keyboard's state
// (libs/ime.h for a game's own text field, libs/imeDialog.h for the system's keyboard dialog, as for the desktop's overlay); this shows it, for
// games that let the controller type.
namespace Kyty::Uwp {

// UI thread: adds the box to the game's page, at `index` among its children. `shown` runs on the UI thread when the box shows or goes.
void SetUpGameKeyboard(winrt::Windows::UI::Xaml::Controls::Grid const& page, uint32_t index, std::function<void(bool shown)> shown);

// UI thread: whether the box shows.
[[nodiscard]] bool GameKeyboardShown();

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_KEYBOARD_H_
