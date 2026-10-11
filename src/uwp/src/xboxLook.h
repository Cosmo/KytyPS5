#ifndef EMULATOR_SRC_UWP_XBOXLOOK_H_
#define EMULATOR_SRC_UWP_XBOXLOOK_H_

#include "input.h"

#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include <initializer_list>
#include <optional>
#include <vector>

namespace Kyty::Uwp {

// The Xbox's own look, measured on the Series X's Settings at 1920x1080 (2 pixels per effective pixel there), for all of the app's pages.

// The app's resources for it (App merges them after the XAML controls' resources): PageTitleTextBlockStyle for the pages' titles and HintBarStyle
// for the bar of controller hints (SetHints fills it).
[[nodiscard]] winrt::Windows::UI::Xaml::ResourceDictionary XboxLookResources();

// The face buttons in the Xbox's colours, the other buttons in the text's (none).
[[nodiscard]] std::optional<winrt::Windows::UI::Color> GetButtonColor(XboxButton button);

// One controller hint: the button and what it does.
struct Hint {
	XboxButton     button;
	const wchar_t* text;
};
// Fills a page's hint bar (a Border with HintBarStyle, in the bottom right corner of the page): each button's own symbol in its colour, then
// what it does, as the Xbox shows them. The labels, in the hints' order, for a page that changes one.
std::vector<winrt::Windows::UI::Xaml::Controls::TextBlock> SetHints(winrt::Windows::UI::Xaml::Controls::Border const& bar,
                                                                    std::initializer_list<Hint>                    hints);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_XBOXLOOK_H_
