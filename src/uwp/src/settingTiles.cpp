#include "settingTiles.h"

#include "input.h"
#include "xboxLook.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <iterator>
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
// A category's entries: 219 by 66, the symbol 20 at 17 from the left edge, the name at 50.
constexpr double ENTRY_WIDTH       = 219;
constexpr double ENTRY_HEIGHT      = 66;
constexpr double ENTRY_SYMBOL_SIZE = 20;
constexpr double ENTRY_TEXT_LEFT   = 50;
// A button's symbol in an explanation: a little larger than the text, as in the hint bar; the lines keep the text's height, so that a
// line with a symbol is as far from the next as the others.
constexpr double BUTTON_SYMBOL_SIZE = 15;
constexpr double TEXT_LINE_HEIGHT   = 20;
// The columns of a full-screen page.
constexpr double COLUMN_WIDTH = 219;

// The buttons an explanation can show as their symbols, by their names in brackets.
struct NamedButton {
	const wchar_t* name;
	XboxButton     button;
};
constexpr NamedButton TEXT_BUTTONS[] = {
    {L"A", XboxButton::A}, {L"B", XboxButton::B},       {L"X", XboxButton::X},
    {L"Y", XboxButton::Y}, {L"View", XboxButton::View}, {L"Menu", XboxButton::Menu},
};

// A setting as a tile of the Xbox's Settings: its name, and its current value under it with a chevron (A opens the choices) or a check box at
// its right (A turns it over); or a tile that opens something, with a line under its name.
struct SettingTile {
	xaml::Controls::Button    button {nullptr};
	xaml::Controls::TextBlock value {nullptr};
	xaml::UIElement           check {nullptr};
};

enum class TileKind { Choice, Switch, Action };

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
	if (kind != TileKind::Switch) {
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
	} else if (kind == TileKind::Switch) {
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

// A paragraph of an explanation, with the buttons named in brackets as their symbols (the face buttons in their colours).
xaml::Controls::TextBlock Paragraph(const std::wstring& paragraph) {
	xaml::Controls::TextBlock text;
	text.Style(AppStyle(L"BodyTextBlockStyle"));
	text.TextWrapping(xaml::TextWrapping::Wrap);
	const auto append = [&text](const std::wstring& piece) {
		if (!piece.empty()) {
			xaml::Documents::Run run;
			run.Text(piece);
			text.Inlines().Append(run);
		}
	};
	size_t start = 0;
	for (size_t open = paragraph.find(L'['); open != std::wstring::npos; open = paragraph.find(L'[', open + 1)) {
		const size_t close = paragraph.find(L']', open);
		if (close == std::wstring::npos) {
			break;
		}
		const auto  name  = paragraph.substr(open + 1, close - open - 1);
		const auto* named = std::find_if(std::begin(TEXT_BUTTONS), std::end(TEXT_BUTTONS), [&name](const NamedButton& button) { return name == button.name; });
		if (named == std::end(TEXT_BUTTONS)) {
			continue;
		}
		append(paragraph.substr(start, open - start));
		const auto           symbol = GetButtonSymbol(named->button);
		xaml::Documents::Run run;
		run.FontFamily(xaml::Media::FontFamily(symbol.font));
		run.FontSize(BUTTON_SYMBOL_SIZE);
		run.Text(symbol.glyph);
		if (const auto color = GetButtonColor(named->button)) {
			run.Foreground(xaml::Media::SolidColorBrush(*color));
		}
		text.Inlines().Append(run);
		text.LineStackingStrategy(xaml::LineStackingStrategy::BlockLineHeight);
		text.LineHeight(TEXT_LINE_HEIGHT);
		start = close + 1;
		open  = close;
	}
	append(paragraph.substr(start));
	return text;
}

} // namespace

void ShowExplanation(xaml::Controls::StackPanel const& explanation, const Paragraphs& paragraphs) {
	explanation.Children().Clear();
	for (const auto& paragraph: paragraphs) {
		explanation.Children().Append(Paragraph(paragraph));
	}
}

void ShowExplanation(xaml::Controls::StackPanel const& explanation, const std::wstring& title, const std::vector<std::wstring>& facts,
                     const Paragraphs& paragraphs) {
	ShowExplanation(explanation, paragraphs);
	// The name and the facts close together, over the paragraphs.
	xaml::Controls::StackPanel head;
	head.Spacing(2);
	auto name = Paragraph(title);
	name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
	name.FontSize(name.FontSize() + 2);
	name.Margin(xaml::ThicknessHelper::FromLengths(0, 0, 0, 4));
	head.Children().Append(name);
	const auto grey =
	    xaml::Application::Current().Resources().Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>();
	for (const auto& fact: facts) {
		auto line = Paragraph(fact);
		line.Foreground(grey);
		head.Children().Append(line);
	}
	explanation.Children().InsertAt(0, head);
}

void Explain(xaml::Controls::Control const& control, xaml::Controls::StackPanel const& explanation, Paragraphs paragraphs) {
	control.GotFocus([explanation, paragraphs = std::move(paragraphs)](auto const&, auto const&) { ShowExplanation(explanation, paragraphs); });
}

void Explain(xaml::Controls::Control const& control, xaml::Controls::StackPanel const& explanation, std::wstring title,
             std::vector<std::wstring> facts, Paragraphs paragraphs) {
	control.GotFocus([explanation, title = std::move(title), facts = std::move(facts), paragraphs = std::move(paragraphs)](auto const&,
	                                                                                                                         auto const&) {
		ShowExplanation(explanation, title, facts, paragraphs);
	});
}

bool FocusFirst(xaml::DependencyObject const& root, winrt::hstring const& tag) {
	if (root == nullptr) {
		return false;
	}
	if (auto viewer = root.try_as<xaml::Controls::ScrollViewer>()) {
		return FocusFirst(viewer.Content().try_as<xaml::DependencyObject>(), tag);
	}
	if (auto panel = root.try_as<xaml::Controls::Panel>()) {
		for (const auto& child: panel.Children()) {
			if (FocusFirst(child, tag)) {
				return true;
			}
		}
		return false;
	}
	if (auto border = root.try_as<xaml::Controls::Border>()) {
		return FocusFirst(border.Child(), tag);
	}
	auto control = root.try_as<xaml::Controls::Control>();
	if (control == nullptr || control.Visibility() != xaml::Visibility::Visible) {
		return false;
	}
	const bool wanted = tag.empty() ? control.IsEnabled() && control.IsTabStop() : winrt::unbox_value_or<winrt::hstring>(control.Tag(), L"") == tag;
	return wanted && control.Focus(xaml::FocusState::Keyboard);
}

xaml::Controls::StackPanel AddColumn(xaml::Controls::StackPanel const& columns, const wchar_t* heading) {
	xaml::Controls::StackPanel column;
	column.Width(COLUMN_WIDTH);
	column.Spacing(8);
	if (heading != nullptr) {
		AddHeading(column, heading);
	}
	columns.Children().Append(column);
	return column;
}

void SetPageTitle(xaml::Controls::TextBlock const& title, const wchar_t* place, const wchar_t* page) {
	title.Inlines().Clear();
	xaml::Documents::Run where;
	where.Text(place);
	where.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
	xaml::Documents::Run name;
	name.Text(std::wstring(L"  ") + page);
	name.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
	title.Inlines().Append(where);
	title.Inlines().Append(name);
}

xaml::Controls::Button MakeEntry(const wchar_t* symbol, const wchar_t* name, std::function<void()> clicked) {
	const xaml::Media::SolidColorBrush normal(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x30, 0x30, 0x30));
	const xaml::Media::SolidColorBrush focused(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x44, 0x44, 0x44));
	xaml::Controls::Button             entry;
	entry.Width(ENTRY_WIDTH);
	entry.Height(ENTRY_HEIGHT);
	entry.HorizontalContentAlignment(xaml::HorizontalAlignment::Left);
	entry.VerticalContentAlignment(xaml::VerticalAlignment::Center);
	entry.Padding(xaml::ThicknessHelper::FromLengths(17, 0, 11, 0));
	entry.CornerRadius(xaml::CornerRadiusHelper::FromUniformRadius(0));
	entry.BorderThickness(xaml::ThicknessHelper::FromUniformLength(0));
	entry.Background(normal);
	entry.GotFocus([entry, focused](auto const&, auto const&) { entry.Background(focused); });
	entry.LostFocus([entry, normal](auto const&, auto const&) { entry.Background(normal); });
	xaml::Controls::StackPanel content;
	content.Orientation(xaml::Controls::Orientation::Horizontal);
	xaml::Controls::FontIcon icon;
	icon.FontFamily(xaml::Media::FontFamily(L"Segoe MDL2 Assets"));
	icon.Glyph(symbol);
	icon.FontSize(ENTRY_SYMBOL_SIZE);
	icon.Width(ENTRY_TEXT_LEFT - 17);
	icon.HorizontalAlignment(xaml::HorizontalAlignment::Left);
	icon.VerticalAlignment(xaml::VerticalAlignment::Center);
	content.Children().Append(icon);
	xaml::Controls::TextBlock label;
	label.Style(AppStyle(L"BodyTextBlockStyle"));
	label.Text(name);
	label.VerticalAlignment(xaml::VerticalAlignment::Center);
	content.Children().Append(label);
	entry.Content(content);
	entry.Click([clicked = std::move(clicked)](auto const&, auto const&) { clicked(); });
	return entry;
}

void AddHeading(xaml::Controls::Panel const& panel, const wchar_t* text) {
	xaml::Controls::TextBlock heading;
	heading.Style(AppStyle(L"BodyTextBlockStyle"));
	heading.Text(text);
	heading.Margin(xaml::ThicknessHelper::FromLengths(0, 0, 0, 4));
	panel.Children().Append(heading);
}

xaml::Controls::Button AddChoice(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& explanation, const wchar_t* name,
                                 std::vector<std::wstring> choices, size_t selected, std::function<void(size_t)> chosen, Paragraphs help) {
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
	return tile.button;
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

xaml::Controls::Button AddAction(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& explanation, const wchar_t* name,
                                 const wchar_t* detail, std::function<void()> clicked, Paragraphs help) {
	auto tile = MakeTile(name, TileKind::Action);
	tile.value.Text(detail);
	if (*detail == 0) {
		tile.value.Visibility(xaml::Visibility::Collapsed);
	}
	tile.button.Click([clicked = std::move(clicked)](auto const&, auto const&) { clicked(); });
	Explain(tile.button, explanation, help);
	panel.Children().Append(tile.button);
	return tile.button;
}

} // namespace Kyty::Uwp
