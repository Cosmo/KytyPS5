#include "xboxLook.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// The Xbox's pages' titles are about 24 effective pixels. The hint bar: #050505 with slightly rounded corners, 28 high, 10 from its left edge to the
// first symbol and a little more after the last word; symbols of 16, 10.5 from a symbol to its word and 18 to the next hint as the eye sees them
// (the spacings are 1.5 less: the glyphs' own side bearings make up the rest); the words white at 13.
constexpr wchar_t ResourcesMarkup[] = LR"xaml(
<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Style x:Key="PageTitleTextBlockStyle" TargetType="TextBlock">
    <Setter Property="FontSize" Value="24" />
    <Setter Property="FontWeight" Value="SemiBold" />
    <Setter Property="TextWrapping" Value="NoWrap" />
    <Setter Property="TextTrimming" Value="CharacterEllipsis" />
  </Style>
  <Style x:Key="HintBarStyle" TargetType="Border">
    <Setter Property="Background" Value="#050505" />
    <Setter Property="CornerRadius" Value="2" />
    <Setter Property="Height" Value="28" />
    <Setter Property="Padding" Value="10,0,16,0" />
    <Setter Property="HorizontalAlignment" Value="Right" />
    <Setter Property="VerticalAlignment" Value="Bottom" />
  </Style>
</ResourceDictionary>)xaml";

constexpr double HINT_SYMBOL_SIZE = 16;
constexpr double HINT_TEXT_SIZE   = 13;

} // namespace

xaml::ResourceDictionary XboxLookResources() {
	return xaml::Markup::XamlReader::Load(ResourcesMarkup).as<xaml::ResourceDictionary>();
}

std::optional<winrt::Windows::UI::Color> GetButtonColor(XboxButton button) {
	// X and Y as the Xbox's own hints show them; A in the Xbox's green (its focus frame's, #76B948), B in a red of the same strength.
	switch (button) {
		case XboxButton::A: return winrt::Windows::UI::Color {255, 0x76, 0xB9, 0x48};
		case XboxButton::B: return winrt::Windows::UI::Color {255, 0xE5, 0x55, 0x3F};
		case XboxButton::X: return winrt::Windows::UI::Color {255, 0x00, 0x94, 0xCE};
		case XboxButton::Y: return winrt::Windows::UI::Color {255, 0xFF, 0xA2, 0x1A};
		default: return std::nullopt;
	}
}

std::vector<xaml::Controls::TextBlock> SetHints(xaml::Controls::Border const& bar, std::initializer_list<Hint> hints) {
	const xaml::Media::SolidColorBrush white(winrt::Windows::UI::Colors::White());
	xaml::Controls::StackPanel         row;
	row.Orientation(xaml::Controls::Orientation::Horizontal);
	row.Spacing(16.5);
	row.VerticalAlignment(xaml::VerticalAlignment::Center);
	std::vector<xaml::Controls::TextBlock> labels;
	for (const auto& hint: hints) {
		xaml::Controls::StackPanel item;
		item.Orientation(xaml::Controls::Orientation::Horizontal);
		item.Spacing(8.5);
		const auto                symbol = GetButtonSymbol(hint.button);
		xaml::Controls::TextBlock glyph;
		glyph.FontFamily(xaml::Media::FontFamily(symbol.font));
		glyph.FontSize(HINT_SYMBOL_SIZE);
		glyph.Text(symbol.glyph);
		glyph.VerticalAlignment(xaml::VerticalAlignment::Center);
		const auto color = GetButtonColor(hint.button);
		glyph.Foreground(color ? xaml::Media::SolidColorBrush(*color) : white);
		xaml::Controls::TextBlock label;
		label.FontSize(HINT_TEXT_SIZE);
		label.Text(hint.text);
		label.Foreground(white);
		label.VerticalAlignment(xaml::VerticalAlignment::Center);
		item.Children().Append(glyph);
		item.Children().Append(label);
		row.Children().Append(item);
		labels.push_back(label);
	}
	bar.Child(row);
	return labels;
}

} // namespace Kyty::Uwp
