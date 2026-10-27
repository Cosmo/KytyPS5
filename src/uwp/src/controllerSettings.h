#ifndef EMULATOR_SRC_UWP_CONTROLLERSETTINGS_H_
#define EMULATOR_SRC_UWP_CONTROLLERSETTINGS_H_

#include "settingTiles.h"
#include "settings.h"

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <functional>
#include <memory>

namespace Kyty::Uwp {

// The controllers' pages, the same on the settings page (Settings › Controller) and in the game menu (Controllers). A change is saved
// (`save`) and applies at once, also mid-game (input.h, SetControllerAssignment). While the tiles show, they follow the controllers and
// the servers as they come and go.

// Both are full-screen pages, their columns added to `columns` (AddColumn).

// Players: an Assign column with "Assign controllers" (automatically or manually) and the DSU servers tile (A runs `open_servers`), a
// Controllers column with the connected controllers by player (players 1 to 4, then those not playing; A chooses the player, Y makes the
// controller vibrate), and the text of the tile with the focus in `explanation`. `hints` is the page's hint bar (Y while a controller's
// tile has the focus).
void AddPlayersColumns(winrt::Windows::UI::Xaml::Controls::StackPanel const& columns, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
                       winrt::Windows::UI::Xaml::Controls::Border const& hints, std::shared_ptr<AppSettings> settings, std::function<void()> save,
                       std::function<void()> open_servers);

// The same in one panel (the game menu's): the tiles one under the other, the text in `explanation` (Players) or `about` (DSU servers);
// no hint bar.
void AddPlayersTiles(winrt::Windows::UI::Xaml::Controls::Panel const& panel, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
                     std::shared_ptr<AppSettings> settings, std::function<void()> save, std::function<void()> open_servers);
void AddServerTiles(winrt::Windows::UI::Xaml::Controls::Panel const& panel, winrt::Windows::UI::Xaml::Controls::StackPanel const& about,
                    std::shared_ptr<AppSettings> settings, std::function<void()> save);

// DSU servers: a Servers column with a tile per server and what it answers (A offers Remove server and Cancel) and Add server (a box for
// the address), and next to it a box about DSU servers.
void AddServerColumns(winrt::Windows::UI::Xaml::Controls::StackPanel const& columns, std::shared_ptr<AppSettings> settings,
                      std::function<void()> save);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_CONTROLLERSETTINGS_H_
