#ifndef EMULATOR_SRC_UWP_TROPHIESPAGE_H_
#define EMULATOR_SRC_UWP_TROPHIESPAGE_H_

#include "gameLibrary.h"

#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.h>

#include <cstddef>
#include <functional>
#include <string>

namespace Kyty::Uwp {

// The trophies of the games in the library, adapted from the desktop launcher's trophy viewer for a controller and a TV: the games with trophies
// and how far each is, a game's trophies, and a trophy's details (hidden ones only when asked). The games' trophy packages
// (sce_sys/trophy2/trophyNN.ucp) and the unlocks the emulator saved in the user data folder's _Trophies are read with Common::Trophies. B (Escape, the
// back button) goes from a game back to the list and from the list to `back`.
// With `title_id` and `trophy_name` (a trophy notification's link) it reads only that game and opens at its trophy list with that trophy's
// details; B goes from that list straight to `back`. Without such a game in the library, the whole page.
[[nodiscard]] winrt::Windows::UI::Xaml::UIElement CreateTrophiesPage(std::function<void()> back, std::string title_id = {},
                                                                     std::string trophy_name = {});

// A trophy's details over what is shown (during a game), reading only its game; `closed` runs when they close, or at once when the trophy
// can't be found. UI thread.
void ShowTrophyDetails(std::string title_id, std::string trophy_name, std::function<void()> closed);

// The running game's trophies for the game menu's side panel, in `host` (filled anew): its progress, then a row per trophy; A on one shows its
// details over the game. They are read on a background thread each time, so trophies earned in this session show too. The focus goes to the
// first trophy, or to the line that says why there are none, so that B still goes back. UI thread.
void ShowGameTrophies(winrt::Windows::UI::Xaml::Controls::Grid const& host, GameEntry game);

// Removes the trophies earned in all games: the unlock files in the user data folder's _Trophies (trophies_*.json in a folder per game). The
// number of games that had some.
[[nodiscard]] size_t ResetAllTrophies();

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_TROPHIESPAGE_H_
