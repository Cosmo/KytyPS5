#include "controlsPage.h"

#include "input.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
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

// Laid out like the library's home screen and the trophies: its margins, its header and its controller hints; Body text in the tables.
constexpr wchar_t PageMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Background="{ThemeResource ApplicationPageBackgroundThemeBrush}">
  <Grid.Resources>
    <!-- The home screen's controller hints. -->
    <Style x:Key="HintRingStyle" TargetType="Ellipse">
      <Setter Property="Width" Value="20" />
      <Setter Property="Height" Value="20" />
      <Setter Property="Stroke" Value="{ThemeResource SystemControlForegroundBaseHighBrush}" />
      <Setter Property="StrokeThickness" Value="1.5" />
    </Style>
    <Style x:Key="HintLetterStyle" TargetType="TextBlock">
      <Setter Property="FontSize" Value="11" />
      <Setter Property="FontWeight" Value="Bold" />
      <Setter Property="TextLineBounds" Value="Tight" />
      <Setter Property="HorizontalAlignment" Value="Center" />
      <Setter Property="VerticalAlignment" Value="Center" />
    </Style>
    <Style x:Key="HintTextStyle" TargetType="TextBlock" BasedOn="{StaticResource BodyTextBlockStyle}">
      <Setter Property="VerticalAlignment" Value="Center" />
      <Setter Property="Margin" Value="8,0,0,0" />
    </Style>
  </Grid.Resources>

  <Grid Padding="0,40,0,48">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto" />
      <RowDefinition Height="*" />
      <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>

    <StackPanel Margin="48,0" Spacing="2">
      <TextBlock Text="Controller" Style="{StaticResource SubtitleTextBlockStyle}" />
      <TextBlock Text="How the Xbox controller plays a PS5 game" Style="{StaticResource BodyTextBlockStyle}"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
    </StackPanel>

    <!-- Focusable, so that the D-pad and the stick scroll it where it does not fit. -->
    <ScrollViewer x:Name="Scroller" Grid.Row="1" Margin="0,24,0,0" Padding="48,0,48,16" IsTabStop="True" UseSystemFocusVisuals="False"
                  VerticalScrollBarVisibility="Auto">
      <Grid MaxWidth="1100" HorizontalAlignment="Left" ColumnSpacing="64">
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="*" />
          <ColumnDefinition Width="*" />
        </Grid.ColumnDefinitions>
        <StackPanel x:Name="LeftColumn" Spacing="28" />
        <StackPanel x:Name="RightColumn" Grid.Column="1" Spacing="28" />
      </Grid>
    </ScrollViewer>

    <Border Grid.Row="2" HorizontalAlignment="Right" Margin="48,16,48,0" Padding="12,8"
            Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}" CornerRadius="4">
      <StackPanel Orientation="Horizontal">
        <Grid>
          <Ellipse Style="{StaticResource HintRingStyle}" />
          <TextBlock Text="B" Foreground="#E5553F" Style="{StaticResource HintLetterStyle}" />
        </Grid>
        <TextBlock Text="Back" Style="{StaticResource HintTextStyle}" />
      </StackPanel>
    </Border>
  </Grid>
</Grid>)xaml";

// A piece of the Xbox controller's side of a line: a button as its symbol, a name ("Rumble motors"), or a word between ("+", "pressed in").
struct Part {
	enum class Kind { Button, Name, Word };
	Kind         kind   = Kind::Name;
	XboxButton   button = XboxButton::A;
	std::wstring text;
};

Part Button(XboxButton button) {
	return {Part::Kind::Button, button, {}};
}
Part Name(const wchar_t* text) {
	return {Part::Kind::Name, XboxButton::A, text};
}
Part Word(const wchar_t* text) {
	return {Part::Kind::Word, XboxButton::A, text};
}

// One line of a table: the Xbox controller's side and what it is for the game.
struct Mapping {
	std::vector<Part> xbox;
	std::wstring      ps5;
};

// The face buttons in their colours (the home screen's hints), the other buttons in the text's.
std::optional<winrt::Windows::UI::Color> ButtonColor(XboxButton button) {
	switch (button) {
		case XboxButton::A: return winrt::Windows::UI::Color {255, 0x7C, 0xC2, 0x4D};
		case XboxButton::B: return winrt::Windows::UI::Color {255, 0xE5, 0x55, 0x3F};
		case XboxButton::X: return winrt::Windows::UI::Color {255, 0x3D, 0x8F, 0xE8};
		case XboxButton::Y: return winrt::Windows::UI::Color {255, 0xF2, 0xC2, 0x30};
		default: return std::nullopt;
	}
}

// The buttons' symbols a little larger than the text, so that their shapes read.
constexpr double BUTTON_SYMBOL_SIZE = 22;

xaml::Style AppStyle(const wchar_t* key) {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(key)).as<xaml::Style>();
}

// A heading and the table under it: the Xbox controller's side in the first column, the game's in the second.
xaml::Controls::StackPanel Section(const wchar_t* title, const std::vector<Mapping>& rows) {
	const auto body   = AppStyle(L"BodyTextBlockStyle");
	const auto medium = xaml::Application::Current().Resources().Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>();

	xaml::Controls::StackPanel section;
	section.Spacing(10);
	xaml::Controls::TextBlock heading;
	heading.Style(body);
	heading.Text(title);
	heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
	heading.Foreground(medium);
	section.Children().Append(heading);

	xaml::Controls::Grid table;
	table.ColumnSpacing(16);
	table.RowSpacing(10);
	xaml::Controls::ColumnDefinition xbox_column;
	xbox_column.Width(xaml::GridLengthHelper::FromPixels(184));
	table.ColumnDefinitions().Append(xbox_column);
	table.ColumnDefinitions().Append(xaml::Controls::ColumnDefinition {});
	for (const auto& row: rows) {
		xaml::Controls::RowDefinition line;
		line.Height(xaml::GridLengthHelper::Auto());
		table.RowDefinitions().Append(line);
		const auto index = static_cast<int32_t>(table.RowDefinitions().Size()) - 1;

		// The buttons as their symbols, joined by words in the quieter colour; a name (a part of the controller) as text.
		xaml::Controls::StackPanel xbox;
		xbox.Orientation(xaml::Controls::Orientation::Horizontal);
		xbox.Spacing(6);
		xbox.VerticalAlignment(xaml::VerticalAlignment::Center);
		for (const auto& part: row.xbox) {
			xaml::Controls::TextBlock piece;
			piece.VerticalAlignment(xaml::VerticalAlignment::Center);
			if (part.kind == Part::Kind::Button) {
				const auto symbol = GetButtonSymbol(part.button);
				piece.FontFamily(xaml::Media::FontFamily(symbol.font));
				piece.FontSize(BUTTON_SYMBOL_SIZE);
				piece.Text(symbol.glyph);
				if (const auto color = ButtonColor(part.button)) {
					piece.Foreground(xaml::Media::SolidColorBrush(*color));
				}
			} else {
				piece.Style(body);
				piece.Text(part.text);
				if (part.kind == Part::Kind::Name) {
					piece.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
				} else {
					piece.Foreground(medium);
				}
			}
			xbox.Children().Append(piece);
		}
		xaml::Controls::Grid::SetRow(xbox, index);
		table.Children().Append(xbox);

		xaml::Controls::TextBlock ps5;
		ps5.Style(body);
		ps5.Text(row.ps5);
		ps5.TextWrapping(xaml::TextWrapping::Wrap);
		ps5.VerticalAlignment(xaml::VerticalAlignment::Center);
		xaml::Controls::Grid::SetRow(ps5, index);
		xaml::Controls::Grid::SetColumn(ps5, 1);
		table.Children().Append(ps5);
	}
	section.Children().Append(table);
	return section;
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

	// The mapping of input.cpp (BUTTONS, MapReading, the chords).
	const auto left_column  = root.FindName(L"LeftColumn").as<xaml::Controls::StackPanel>();
	const auto right_column = root.FindName(L"RightColumn").as<xaml::Controls::StackPanel>();
	using B = XboxButton;
	left_column.Children().Append(Section(L"Buttons and sticks",
	                                      {
	                                          {{Button(B::A)}, L"Cross"},
	                                          {{Button(B::B)}, L"Circle"},
	                                          {{Button(B::X)}, L"Square"},
	                                          {{Button(B::Y)}, L"Triangle"},
	                                          {{Button(B::LeftBumper), Word(L"/"), Button(B::RightBumper)}, L"L1  /  R1"},
	                                          {{Button(B::LeftTrigger), Word(L"/"), Button(B::RightTrigger)}, L"L2  /  R2, as far as they are pressed"},
	                                          {{Button(B::LeftStick)}, L"Left stick, pressed in: L3"},
	                                          {{Button(B::RightStick)}, L"Right stick, pressed in: R3"},
	                                          {{Button(B::DPad)}, L"Directional buttons"},
	                                          {{Button(B::Menu)}, L"Options"},
	                                      }));
	right_column.Children().Append(Section(L"Hold View for the DualSense's extras",
	                                       {
	                                           {{Button(B::View), Word(L"+"), Button(B::LeftStick)}, L"A finger on the touchpad; pressed in: the touchpad click"},
	                                           {{Button(B::View), Word(L"+"), Button(B::RightStick)},
	                                            L"Tilts the controller for the motion sensors: up and down tip it, left and right lean it"},
	                                           {{Button(B::View), Word(L"+"), Button(B::RightStick), Word(L"pressed in")}, L"Left and right turn the controller as it lies"},
	                                       }));
	right_column.Children().Append(Section(L"KytyPS5",
	                                       {
	                                           {{Button(B::View), Word(L"+"), Button(B::Menu)}, L"The game menu"},
	                                           {{Button(B::View), Word(L"+"), Button(B::Menu), Word(L"+"), Button(B::LeftTrigger), Word(L"+"),
	                                             Button(B::RightTrigger)},
	                                            L"The overlay, on or off"},
	                                           {{Name(L"Xbox button")}, L"The Xbox's guide; a PS5 game has no PS button or Create button here"},
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
	// The focus on the table, for B and for scrolling.
	auto scroller = root.FindName(L"Scroller").as<xaml::Controls::ScrollViewer>();
	root.Loaded([weak = winrt::make_weak(scroller)](auto const&, auto const&) {
		if (auto shown = weak.get()) {
			shown.Focus(xaml::FocusState::Programmatic);
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
