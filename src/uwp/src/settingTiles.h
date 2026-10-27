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

// An explanation: short paragraphs, what the setting does first, then what it costs or when to change it. A button named in brackets
// shows as its symbol, as on the controls page: "Press [Y] to ..." ([A], [B], [X], [Y], [View], [Menu]).
using Paragraphs = std::vector<std::wstring>;

// Shows the paragraphs as the explanation.
void ShowExplanation(winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation, const Paragraphs& paragraphs);
// The same for a thing rather than a setting (a controller): its name over facts in grey, then the paragraphs.
void ShowExplanation(winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation, const std::wstring& title,
                     const std::vector<std::wstring>& facts, const Paragraphs& paragraphs);

// Shows the explanation while `control` has the focus.
void Explain(winrt::Windows::UI::Xaml::Controls::Control const& control, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
             Paragraphs paragraphs);
void Explain(winrt::Windows::UI::Xaml::Controls::Control const& control, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
             std::wstring title, std::vector<std::wstring> facts, Paragraphs paragraphs);

// Puts the focus on the first control in `root` that takes it, or with `tag` on the one with that tag; it searches panels, borders and
// scroll viewers in order. Whether it found one.
bool FocusFirst(winrt::Windows::UI::Xaml::DependencyObject const& root, winrt::hstring const& tag = {});

// A column of a full-screen page, as the Xbox's Settings put a page's tiles in columns: 219 wide, the heading over its tiles when there is
// one; added to `columns` (a horizontal StackPanel).
winrt::Windows::UI::Xaml::Controls::StackPanel AddColumn(winrt::Windows::UI::Xaml::Controls::StackPanel const& columns,
                                                         const wchar_t*                                        heading = nullptr);

// The title of a full-screen page as the Xbox's: where it is in bold ("Controller", "Game menu"), then the page.
void SetPageTitle(winrt::Windows::UI::Xaml::Controls::TextBlock const& title, const wchar_t* place, const wchar_t* page);

// An entry of a category, as the Xbox's Settings show a category's pages before one is opened: a wide tile with the page's symbol (Segoe
// MDL2 Assets) and its name; A runs `clicked`.
[[nodiscard]] winrt::Windows::UI::Xaml::Controls::Button MakeEntry(const wchar_t* symbol, const wchar_t* name, std::function<void()> clicked);

// A heading over a group of tiles.
void AddHeading(winrt::Windows::UI::Xaml::Controls::Panel const& panel, const wchar_t* text);

// A setting with choices: A opens them, the current one checked; `chosen` gets the new one's index. The tile.
winrt::Windows::UI::Xaml::Controls::Button AddChoice(winrt::Windows::UI::Xaml::Controls::Panel const&      panel,
                                                     winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation, const wchar_t* name,
                                                     std::vector<std::wstring> choices, size_t selected, std::function<void(size_t)> chosen,
                                                     Paragraphs help = {});

// A setting that is on or off: A turns it over; `changed` gets the new state.
void AddSwitch(winrt::Windows::UI::Xaml::Controls::Panel const& panel, winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation,
               const wchar_t* name, bool on, std::function<void(bool)> changed, Paragraphs help = {});

// A tile that opens something: its name, and `detail` under it (none when empty); A runs `clicked`. The tile.
winrt::Windows::UI::Xaml::Controls::Button AddAction(winrt::Windows::UI::Xaml::Controls::Panel const&      panel,
                                                     winrt::Windows::UI::Xaml::Controls::StackPanel const& explanation, const wchar_t* name,
                                                     const wchar_t* detail, std::function<void()> clicked, Paragraphs help = {});

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_SETTINGTILES_H_
