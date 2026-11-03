#ifndef EMULATOR_SRC_UWP_SYSTEMDIALOG_H_
#define EMULATOR_SRC_UWP_SYSTEMDIALOG_H_

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <cstdint>
#include <functional>

// The system's dialogs a game opens and waits for: the PlayStation Network sign-in, which is unavailable, and an error with its code. A box
// over the game with the desktop overlay's title, text and button; the button (A), or B for the sign-in, closes it and the game goes on. The
// emulator keeps the dialogs' state (Libs::Dialog::SystemDialog, as for the desktop's overlay); this shows it.
namespace Kyty::Uwp {

// UI thread: adds the box to the game's page, at `index` among its children. `shown` runs on the UI thread when the box shows or goes.
void SetUpSystemDialog(winrt::Windows::UI::Xaml::Controls::Grid const& page, uint32_t index, std::function<void(bool shown)> shown);

// UI thread: whether the box shows.
[[nodiscard]] bool SystemDialogShown();

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_SYSTEMDIALOG_H_
