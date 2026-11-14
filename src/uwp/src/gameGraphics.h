#ifndef EMULATOR_SRC_UWP_GAMEGRAPHICS_H_
#define EMULATOR_SRC_UWP_GAMEGRAPHICS_H_

#include "settings.h"

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <functional>
#include <memory>

namespace Kyty::Uwp {

// The game menu's Graphics page: the settings a running game takes at once (Config's setters), saved as the settings page saves them.
// `output_changed` gets whether the picture now matches the TV's mode (Output resolution, Xbox only).
void AddGameGraphicsSettings(winrt::Windows::UI::Xaml::Controls::Panel const&      tiles,
                             winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation, std::shared_ptr<AppSettings> settings,
                             std::function<void(bool)> output_changed);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_GAMEGRAPHICS_H_
