#ifndef EMULATOR_SRC_UWP_TROPHYTOAST_H_
#define EMULATOR_SRC_UWP_TROPHYTOAST_H_

#include <string>

namespace Kyty::Uwp {

// A system notification for a trophy a game unlocked: the Xbox's popup (and its list of notifications), or Windows' notification on a PC. Two
// lines, the trophy's name and "Trophy earned!", which the Xbox shows as one; no picture, as the Xbox refuses every picture in a notification
// (0x80004005, even the package's logo), so it shows the app's logo. Selecting it activates the app with `link` (App::OnActivated). UI thread.
void ShowTrophyToast(const std::string& name, const std::wstring& link);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_TROPHYTOAST_H_
