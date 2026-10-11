#include "settingTiles.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>

#include <memory>
#include <utility>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// The tiles of the Xbox's Settings, in effective pixels (measured at 1920x1080): 219 wide, at least 48 high, #303030 (#444444 with the focus),
// the text 11 from the left edge, 8 between tiles. A check box is 16.
constexpr double TILE_MIN_HEIGHT = 48;
constexpr double CHECK_BOX_SIZE  = 16;
// In Segoe MDL2 Assets.
constexpr wchar_t CHEVRON_DOWN[] = {0xE70D, 0};
constexpr wchar_t CHECK_MARK[]   = {0xE73E, 0};

// A setting as a tile of the Xbox's Settings: its name, and its current value under it with a chevron (A opens the choices) or a check box at
// its right (A turns it over).
struct SettingTile {
	xaml::Controls::Button    button {nullptr};
	xaml::Controls::TextBlock value {nullptr};
	xaml::UIElement           check {nullptr};
};

enum class TileKind { Choice, Switch };

xaml::Style AppStyle(const wchar_t* key) {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(key)).as<xaml::Style>();
}

SettingTile MakeTile(const wchar_t* name, TileKind kind) {
	const xaml::Media::SolidColorBrush normal(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x30, 0x30, 0x30));
	const xaml::Media::SolidColorBrush focused(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x44, 0x44, 0x44));
	SettingTile                        tile;
	tile.button = xaml::Controls::Button();
	tile.button.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
	tile.button.HorizontalContentAlignment(xaml::HorizontalAlignment::Stretch);
	tile.button.VerticalContentAlignment(xaml::VerticalAlignment::Center);
	tile.button.MinHeight(TILE_MIN_HEIGHT);
	tile.button.Padding(xaml::ThicknessHelper::FromLengths(11, 8, 11, 8));
	tile.button.CornerRadius(xaml::CornerRadiusHelper::FromUniformRadius(0));
	tile.button.BorderThickness(xaml::ThicknessHelper::FromUniformLength(0));
	tile.button.Background(normal);
	tile.button.GotFocus([button = tile.button, focused](auto const&, auto const&) { button.Background(focused); });
	tile.button.LostFocus([button = tile.button, normal](auto const&, auto const&) { button.Background(normal); });

	xaml::Controls::Grid content;
	content.ColumnSpacing(12);
	content.ColumnDefinitions().Append(xaml::Controls::ColumnDefinition());
	xaml::Controls::ColumnDefinition right_column;
	right_column.Width(xaml::GridLengthHelper::Auto());
	content.ColumnDefinitions().Append(right_column);
	xaml::Controls::StackPanel text;
	text.VerticalAlignment(xaml::VerticalAlignment::Center);
	xaml::Controls::TextBlock title;
	title.Style(AppStyle(L"BodyTextBlockStyle"));
	title.Text(name);
	title.TextWrapping(xaml::TextWrapping::Wrap);
	text.Children().Append(title);
	if (kind == TileKind::Choice) {
		tile.value = xaml::Controls::TextBlock();
		tile.value.Style(AppStyle(L"BodyTextBlockStyle"));
		tile.value.TextWrapping(xaml::TextWrapping::Wrap);
		tile.value.Foreground(
		    xaml::Application::Current().Resources().Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>());
		text.Children().Append(tile.value);
	}
	content.Children().Append(text);

	xaml::Controls::FontIcon icon;
	icon.FontFamily(xaml::Media::FontFamily(L"Segoe MDL2 Assets"));
	icon.VerticalAlignment(xaml::VerticalAlignment::Center);
	if (kind == TileKind::Choice) {
		icon.Glyph(CHEVRON_DOWN);
		icon.FontSize(12);
		xaml::Controls::Grid::SetColumn(icon, 1);
		content.Children().Append(icon);
	} else {
		// A check box: a white square, with a check mark while the setting is on.
		xaml::Controls::Border box;
		box.Width(CHECK_BOX_SIZE);
		box.Height(CHECK_BOX_SIZE);
		box.BorderThickness(xaml::ThicknessHelper::FromUniformLength(1));
		box.BorderBrush(xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
		box.VerticalAlignment(xaml::VerticalAlignment::Center);
		icon.Glyph(CHECK_MARK);
		icon.FontSize(11);
		icon.HorizontalAlignment(xaml::HorizontalAlignment::Center);
		box.Child(icon);
		tile.check = icon;
		xaml::Controls::Grid::SetColumn(box, 1);
		content.Children().Append(box);
	}
	tile.button.Content(content);
	return tile;
}

} // namespace

void ShowExplanation(xaml::Controls::StackPanel const& explanation, const Paragraphs& paragraphs) {
	explanation.Children().Clear();
	for (const auto& paragraph: paragraphs) {
		xaml::Controls::TextBlock text;
		text.Style(AppStyle(L"BodyTextBlockStyle"));
		text.TextWrapping(xaml::TextWrapping::Wrap);
		text.Text(paragraph);
		explanation.Children().Append(text);
	}
}

void Explain(xaml::Controls::Control const& control, xaml::Controls::StackPanel const& explanation, Paragraphs paragraphs) {
	control.GotFocus([explanation, paragraphs = std::move(paragraphs)](auto const&, auto const&) { ShowExplanation(explanation, paragraphs); });
}

void AddHeading(xaml::Controls::Panel const& panel, const wchar_t* text) {
	xaml::Controls::TextBlock heading;
	heading.Style(AppStyle(L"BodyTextBlockStyle"));
	heading.Text(text);
	heading.Margin(xaml::ThicknessHelper::FromLengths(0, 0, 0, 4));
	panel.Children().Append(heading);
}

void AddChoice(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& explanation, const wchar_t* name, std::vector<std::wstring> choices,
               size_t selected, std::function<void(size_t)> chosen, Paragraphs help) {
	auto tile = MakeTile(name, TileKind::Choice);
	tile.value.Text(selected < choices.size() ? winrt::hstring(choices[selected]) : winrt::hstring());
	auto current = std::make_shared<size_t>(selected);
	tile.button.Click([button = tile.button, value = tile.value, choices = std::move(choices), current, chosen = std::move(chosen)](auto const&, auto const&) {
		xaml::Controls::MenuFlyout flyout;
		for (size_t index = 0; index < choices.size(); index++) {
			xaml::Controls::ToggleMenuFlyoutItem item;
			item.Text(choices[index]);
			item.IsChecked(index == *current);
			item.Click([index, value, current, chosen, text = choices[index]](auto const&, auto const&) {
				*current = index;
				value.Text(text);
				chosen(index);
			});
			flyout.Items().Append(item);
		}
		flyout.ShowAt(button);
	});
	Explain(tile.button, explanation, help);
	panel.Children().Append(tile.button);
}

void AddSwitch(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& explanation, const wchar_t* name, bool on,
               std::function<void(bool)> changed, Paragraphs help) {
	auto tile = MakeTile(name, TileKind::Switch);
	tile.check.Opacity(on ? 1.0 : 0.0);
	auto state = std::make_shared<bool>(on);
	tile.button.Click([check = tile.check, state, changed = std::move(changed)](auto const&, auto const&) {
		*state = !*state;
		check.Opacity(*state ? 1.0 : 0.0);
		changed(*state);
	});
	Explain(tile.button, explanation, help);
	panel.Children().Append(tile.button);
}

} // namespace Kyty::Uwp
