#include "controlsPage.h"

#include "input.h"
#include "settings.h"
#include "xboxLook.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// Laid out like the library's home screen and the trophies: its margins, its header and its controller hints. The mapping is in cards, as the
// Xbox's own Settings show controller shortcuts: a row of them that scrolls sideways, the D-pad moving from card to card.
constexpr wchar_t PageMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Background="{ThemeResource ApplicationPageBackgroundThemeBrush}">
  <Grid Padding="0,40,0,32">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto" />
      <RowDefinition Height="*" />
      <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>

    <TextBlock Text="Controller" Style="{StaticResource PageTitleTextBlockStyle}" Margin="48,0" />

    <ScrollViewer Grid.Row="1" Margin="0,24,0,0" HorizontalScrollMode="Enabled" HorizontalScrollBarVisibility="Hidden"
                  VerticalScrollMode="Disabled" VerticalScrollBarVisibility="Disabled">
      <StackPanel x:Name="Cards" Orientation="Horizontal" Spacing="24" Padding="48,0" XYFocusKeyboardNavigation="Enabled" />
    </ScrollViewer>

    <Border x:Name="Hints" Grid.Row="2" Style="{StaticResource HintBarStyle}" Margin="48,16,48,0" />
  </Grid>
</Grid>)xaml";

// A piece of the Xbox controller's side of a row: a name ("A button"), a button's symbol, the headset's symbol, or a word between ("+", "/").
struct Part {
	enum class Kind { Name, Button, Headset, Word };
	Kind         kind   = Kind::Name;
	XboxButton   button = XboxButton::A;
	std::wstring text;
};

Part Name(const wchar_t* text) {
	return {Part::Kind::Name, XboxButton::A, text};
}
Part Button(XboxButton button) {
	return {Part::Kind::Button, button, {}};
}
Part Headset() {
	return {Part::Kind::Headset, XboxButton::A, {}};
}
Part Word(const wchar_t* text) {
	return {Part::Kind::Word, XboxButton::A, text};
}

// One row of a card: the Xbox controller's side and what it does in the game.
struct Mapping {
	std::vector<Part> xbox;
	std::wstring      ps5;
};

// As the Xbox's cards of controller shortcuts (measured at 1920x1080, in effective pixels): #303030 (#444444 with the focus), the names with
// small white symbols, a line between the rows. A card is 400 wide, so that two and a part of the next fit on the screen.
constexpr double CARD_WIDTH         = 400;
constexpr double NAME_COLUMN_WIDTH  = 170;
constexpr double BUTTON_SYMBOL_SIZE = 16;

xaml::Style AppStyle(const wchar_t* key) {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(key)).as<xaml::Style>();
}

// The Xbox controller's side of a row as one text, the symbols in it, so that it wraps like the text.
xaml::Controls::TextBlock XboxSide(const std::vector<Part>& parts) {
	xaml::Controls::TextBlock text;
	text.Style(AppStyle(L"BodyTextBlockStyle"));
	text.TextWrapping(xaml::TextWrapping::Wrap);
	for (size_t index = 0; index < parts.size(); index++) {
		const auto& part = parts[index];
		if (index > 0) {
			winrt::Windows::UI::Xaml::Documents::Run space;
			space.Text(L" ");
			text.Inlines().Append(space);
		}
		winrt::Windows::UI::Xaml::Documents::Run run;
		if (part.kind == Part::Kind::Button || part.kind == Part::Kind::Headset) {
			const auto symbol = part.kind == Part::Kind::Button ? GetButtonSymbol(part.button) : ControllerSymbol {HEADSET_SYMBOL_FONT, {HEADSET_SYMBOL[0], 0}};
			run.FontFamily(xaml::Media::FontFamily(symbol.font));
			run.FontSize(BUTTON_SYMBOL_SIZE);
			run.Text(symbol.glyph);
		} else {
			run.Text(part.text);
		}
		text.Inlines().Append(run);
	}
	return text;
}

// A card: its title and what it is about, then its rows, each the Xbox controller's side and what it does, with a line between them. It takes
// the focus, so that the D-pad goes from card to card.
xaml::Controls::ContentControl Card(const wchar_t* title, const wchar_t* about, const std::vector<Mapping>& rows) {
	const auto body   = AppStyle(L"BodyTextBlockStyle");
	const auto medium = xaml::Application::Current().Resources().Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>();
	const xaml::Media::SolidColorBrush normal(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x30, 0x30, 0x30));
	const xaml::Media::SolidColorBrush focused(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x44, 0x44, 0x44));
	const xaml::Media::SolidColorBrush line(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x55, 0x55, 0x55));

	xaml::Controls::StackPanel content;
	xaml::Controls::TextBlock  heading;
	heading.Style(body);
	heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
	heading.Text(title);
	content.Children().Append(heading);
	xaml::Controls::TextBlock intro;
	intro.Style(body);
	intro.Foreground(medium);
	intro.TextWrapping(xaml::TextWrapping::Wrap);
	intro.Text(about);
	intro.Margin(xaml::ThicknessHelper::FromLengths(0, 2, 0, 10));
	content.Children().Append(intro);
	for (const auto& row: rows) {
		xaml::Controls::Grid table_row;
		table_row.ColumnSpacing(16);
		xaml::Controls::ColumnDefinition name_column;
		name_column.Width(xaml::GridLengthHelper::FromPixels(NAME_COLUMN_WIDTH));
		table_row.ColumnDefinitions().Append(name_column);
		table_row.ColumnDefinitions().Append(xaml::Controls::ColumnDefinition {});
		const auto xbox = XboxSide(row.xbox);
		table_row.Children().Append(xbox);
		xaml::Controls::TextBlock ps5;
		ps5.Style(body);
		ps5.TextWrapping(xaml::TextWrapping::Wrap);
		ps5.Text(row.ps5);
		xaml::Controls::Grid::SetColumn(ps5, 1);
		table_row.Children().Append(ps5);
		xaml::Controls::Border separated;
		separated.BorderBrush(line);
		separated.BorderThickness(xaml::ThicknessHelper::FromLengths(0, 1, 0, 0));
		separated.Padding(xaml::ThicknessHelper::FromLengths(0, 7, 0, 7));
		separated.Child(table_row);
		content.Children().Append(separated);
	}

	xaml::Controls::Border frame;
	frame.Background(normal);
	frame.Padding(xaml::ThicknessHelper::FromLengths(20, 16, 20, 16));
	frame.Child(content);
	xaml::Controls::ContentControl card;
	card.Width(CARD_WIDTH);
	card.IsTabStop(true);
	card.UseSystemFocusVisuals(true);
	card.HorizontalContentAlignment(xaml::HorizontalAlignment::Stretch);
	card.VerticalContentAlignment(xaml::VerticalAlignment::Stretch);
	card.Content(frame);
	card.GotFocus([frame, focused](auto const&, auto const&) { frame.Background(focused); });
	card.LostFocus([frame, normal](auto const&, auto const&) { frame.Background(normal); });
	return card;
}

struct Page {
	std::function<void()> back;
	bool                  left = false;
	xaml::Controls::Grid  root {nullptr};
	winrt::event_token    unloaded;

	void Back() {
		if (!left) {
			left = true;
			back();
		}
	}
};

} // namespace

xaml::UIElement CreateControlsPage(std::function<void()> back) {
	auto page  = std::make_shared<Page>();
	page->back = std::move(back);
	page->root = xaml::Markup::XamlReader::Load(PageMarkup).as<xaml::Controls::Grid>();
	const auto& root = page->root;
	SetHints(root.FindName(L"Hints").as<xaml::Controls::Border>(), {{XboxButton::B, L"Back"}});

	// The mapping of input.cpp (BUTTONS, MapReading, the chords), the headset of the settings page's controller options, and the app's own
	// shortcuts.
	const auto cards = root.FindName(L"Cards").as<xaml::Controls::StackPanel>();
	using B          = XboxButton;
	cards.Children().Append(Card(L"Buttons", L"Each Xbox button works as a DualSense button.",
	                             {
	                                 {{Name(L"A button"), Button(B::A)}, L"Cross"},
	                                 {{Name(L"B button"), Button(B::B)}, L"Circle"},
	                                 {{Name(L"X button"), Button(B::X)}, L"Square"},
	                                 {{Name(L"Y button"), Button(B::Y)}, L"Triangle"},
	                                 {{Name(L"D-pad"), Button(B::DPad)}, L"Directional buttons"},
	                                 {{Name(L"Menu button"), Button(B::Menu)}, L"Options"},
	                             }));
	cards.Children().Append(Card(L"Sticks, bumpers and triggers", L"They work as on a DualSense.",
	                             {
	                                 {{Name(L"Left stick"), Button(B::LeftStick)}, L"Left stick. Pressed in, L3."},
	                                 {{Name(L"Right stick"), Button(B::RightStick)}, L"Right stick. Pressed in, R3."},
	                                 {{Name(L"Left/right bumper"), Button(B::LeftBumper), Word(L"/"), Button(B::RightBumper)}, L"L1 / R1"},
	                                 {{Name(L"Left/right trigger"), Button(B::LeftTrigger), Word(L"/"), Button(B::RightTrigger)}, L"L2 / R2"},
	                             }));
	cards.Children().Append(Card(L"DualSense extras", L"What the Xbox controller does not have. Hold View for the touchpad and the motion sensors.",
	                             {
	                                 {{Name(L"View"), Button(B::View), Word(L"+"), Name(L"left stick"), Button(B::LeftStick)},
	                                  L"Touchpad. Move the stick or press it in to touch and click it. Let go of View to lift the finger."},
	                                 {{Name(L"View"), Button(B::View), Word(L"+"), Name(L"right stick"), Button(B::RightStick)},
	                                  L"Motion sensors. Up and down tip the controller, left and right lean it."},
	                                 {{Name(L"View"), Button(B::View), Word(L"+"), Name(L"right stick pressed in"), Button(B::RightStick)},
	                                  L"Motion sensors. Left and right turn the controller as it lies flat. Up and down still tip it."},
	                                 {{Name(L"Headset speaker"), Headset()}, L"Player 1's DualSense speaker"},
	                                 {{Name(L"Headset microphone"), Headset()}, L"Player 1's DualSense microphone. Turn it on in Settings, Controller."},
	                             }));
	cards.Children().Append(Card(L"KytyPS5", L"Shortcuts while a game runs.",
	                             {
	                                 {{Name(L"View"), Button(B::View), Word(L"+"), Name(L"Menu"), Button(B::Menu)}, L"Game menu"},
	                                 {{Name(L"View"), Button(B::View), Word(L"+"), Name(L"Menu"), Button(B::Menu), Word(L"+"), Button(B::LeftTrigger),
	                                   Word(L"+"), Button(B::RightTrigger)},
	                                  L"Overlay on or off"},
	                             }));

	// Back: B, Escape or the window's back button (shown while the page is open).
	root.KeyDown([weak = std::weak_ptr<Page>(page)](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadB || args.Key() == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			if (auto shared = weak.lock()) {
				shared->Back();
			}
		}
	});
	auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	navigation.AppViewBackButtonVisibility(winrt::Windows::UI::Core::AppViewBackButtonVisibility::Visible);
	auto back_token = navigation.BackRequested(
	    [weak = std::weak_ptr<Page>(page)](auto const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
		    args.Handled(true);
		    if (auto shared = weak.lock()) {
			    shared->Back();
		    }
	    });
	// The focus on the first card, for B and for going from card to card.
	root.Loaded([weak = winrt::make_weak(cards)](auto const&, auto const&) {
		if (auto shown = weak.get(); shown && shown.Children().Size() > 0) {
			shown.Children().GetAt(0).as<xaml::Controls::Control>().Focus(xaml::FocusState::Keyboard);
		}
	});
	// The page's state lives while the page is shown: this handler holds it and removes itself when the page goes.
	page->unloaded = root.Unloaded([page, navigation, back_token](auto const&, auto const&) {
		navigation.BackRequested(back_token);
		navigation.AppViewBackButtonVisibility(winrt::Windows::UI::Core::AppViewBackButtonVisibility::Collapsed);
		page->left = true;
		page->root.Unloaded(page->unloaded);
	});
	return root;
}

} // namespace Kyty::Uwp
