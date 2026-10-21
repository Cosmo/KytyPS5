#ifndef EMULATOR_SRC_UWP_LAUNCHER_H_
#define EMULATOR_SRC_UWP_LAUNCHER_H_

#include "gameLibrary.h"

#include <winrt/Windows.UI.Xaml.h>

#include <filesystem>
#include <functional>

namespace Kyty::Uwp {

// The library: the games of the game folders (as of the last scan) as a row of covers over the
// selected game's background. A controller, the keyboard or the mouse picks a game; `play` starts
// it. A game's options (the Menu button, a right click) start it, open its cheats, or hide it until Y (or F5) scans
// the game folders again.
struct LauncherActions {
	std::function<void(const GameEntry&)> play;          // starts a game
	std::function<void()>                 open_settings; // the View button or the gear button
	std::function<void()>                 open_trophies; // the trophy button left of the gear
	std::function<void()>                 open_controls; // the controller button, the first of them
	std::function<void(const GameEntry&)> open_cheats;   // a game's options: Cheats
};
// `scan`: scan the game folders first (as Y does), for when they changed. `select`: the folder of the game to select (the one whose page
// the player comes back from); the game played last when it is empty or not in the library.
[[nodiscard]] winrt::Windows::UI::Xaml::UIElement CreateLauncherPage(LauncherActions actions, bool scan = false,
                                                                     const std::filesystem::path& select = {});

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_LAUNCHER_H_
