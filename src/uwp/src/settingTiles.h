#ifndef EMULATOR_SRC_UWP_SETTINGTILES_H_
#define EMULATOR_SRC_UWP_SETTINGTILES_H_

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Kyty::Uwp {

// Settings as the tiles of the Xbox's Settings, for the settings page and the game menu: a tile per setting, and an explanation of the one
// with the focus.

// An explanation: short paragraphs, what the setting does first, then what it costs or when to change it.
using Paragraphs = std::vector<std::wstring>;

// Shows the paragraphs as the explanation.
void ShowExplanation(winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation, const Paragraphs& paragraphs);

// Shows the explanation while `control` has the focus.
void Explain(winrt::Windows::UI::Xaml::Controls::Control const& control, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
             Paragraphs paragraphs);

// A heading over a group of tiles.
void AddHeading(winrt::Windows::UI::Xaml::Controls::Panel const& panel, const wchar_t* text);

// A setting with choices: A opens them, the current one checked; `chosen` gets the new one's index.
void AddChoice(winrt::Windows::UI::Xaml::Controls::Panel const& panel, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
               const wchar_t* name, std::vector<std::wstring> choices, size_t selected, std::function<void(size_t)> chosen, Paragraphs help = {});

// A setting that is on or off: A turns it over; `changed` gets the new state.
void AddSwitch(winrt::Windows::UI::Xaml::Controls::Panel const& panel, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
               const wchar_t* name, bool on, std::function<void(bool)> changed, Paragraphs help = {});

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_SETTINGTILES_H_
