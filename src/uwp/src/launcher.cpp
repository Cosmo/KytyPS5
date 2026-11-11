#include "launcher.h"

#include "common/pipelinePrewarm.h"
#include "fileAccess.h"
#include "gameImages.h"
#include "gameSource.h"
#include "input.h"
#include "settings.h"
#include "titleMusic.h"
#include "xboxLook.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// Covers are 512x512 and shown at 88 effective pixels (176 pixels on the Xbox): decoded at 256, they stay sharp.
constexpr int COVER_WIDTH = 256;
// Backgrounds are 3840x2160; decoded smaller, they stay sharp enough under the shading.
constexpr int BACKDROP_WIDTH = 1920;

constexpr wchar_t LauncherMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      xmlns:muxc="using:Microsoft.UI.Xaml.Controls"
      Background="#0B0E14">
  <Grid.Resources>
    <!-- A cover: with focus it grows a little and gets a ring. -->
    <Style x:Key="CoverStyle" TargetType="Button">
      <Setter Property="UseSystemFocusVisuals" Value="False" />
      <Setter Property="Width" Value="88" />
      <Setter Property="Height" Value="88" />
      <Setter Property="Padding" Value="0" />
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Grid RenderTransformOrigin="0.5,0.5">
              <Grid.RenderTransform>
                <ScaleTransform x:Name="Scale" />
              </Grid.RenderTransform>
              <VisualStateManager.VisualStateGroups>
                <VisualStateGroup x:Name="FocusStates">
                  <VisualStateGroup.Transitions>
                    <VisualTransition GeneratedDuration="0:0:0.12" />
                  </VisualStateGroup.Transitions>
                  <VisualState x:Name="Focused">
                    <Storyboard>
                      <DoubleAnimation Storyboard.TargetName="Scale" Storyboard.TargetProperty="ScaleX" To="1.08" Duration="0" />
                      <DoubleAnimation Storyboard.TargetName="Scale" Storyboard.TargetProperty="ScaleY" To="1.08" Duration="0" />
                      <DoubleAnimation Storyboard.TargetName="Ring" Storyboard.TargetProperty="Opacity" To="1" Duration="0" />
                      <DoubleAnimation Storyboard.TargetName="Dim" Storyboard.TargetProperty="Opacity" To="0" Duration="0" />
                    </Storyboard>
                  </VisualState>
                  <VisualState x:Name="PointerFocused">
                    <Storyboard>
                      <DoubleAnimation Storyboard.TargetName="Scale" Storyboard.TargetProperty="ScaleX" To="1.08" Duration="0" />
                      <DoubleAnimation Storyboard.TargetName="Scale" Storyboard.TargetProperty="ScaleY" To="1.08" Duration="0" />
                      <DoubleAnimation Storyboard.TargetName="Ring" Storyboard.TargetProperty="Opacity" To="1" Duration="0" />
                      <DoubleAnimation Storyboard.TargetName="Dim" Storyboard.TargetProperty="Opacity" To="0" Duration="0" />
                    </Storyboard>
                  </VisualState>
                  <VisualState x:Name="Unfocused" />
                </VisualStateGroup>
              </VisualStateManager.VisualStateGroups>
              <ContentPresenter Content="{TemplateBinding Content}" />
              <Border x:Name="Dim" Background="#0B0E14" Opacity="0.3" CornerRadius="4" IsHitTestVisible="False" />
              <Border x:Name="Ring" BorderBrush="#F2F5FA" BorderThickness="2.5" CornerRadius="6" Margin="-4"
                      Opacity="0" IsHitTestVisible="False" />
            </Grid>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
  </Grid.Resources>

  <!-- The selected game's background, cross-faded, shaded toward the text. -->
  <Image x:Name="BackdropA" Stretch="UniformToFill" HorizontalAlignment="Center" VerticalAlignment="Center" Opacity="0">
    <Image.OpacityTransition><ScalarTransition Duration="0:0:0.4" /></Image.OpacityTransition>
  </Image>
  <Image x:Name="BackdropB" Stretch="UniformToFill" HorizontalAlignment="Center" VerticalAlignment="Center" Opacity="0">
    <Image.OpacityTransition><ScalarTransition Duration="0:0:0.4" /></Image.OpacityTransition>
  </Image>
  <Rectangle IsHitTestVisible="False">
    <Rectangle.Fill>
      <LinearGradientBrush StartPoint="0,0" EndPoint="0,1">
        <GradientStop Color="#800B0E14" Offset="0" />
        <GradientStop Color="#990B0E14" Offset="0.45" />
        <GradientStop Color="#F00B0E14" Offset="1" />
      </LinearGradientBrush>
    </Rectangle.Fill>
  </Rectangle>
  <Rectangle IsHitTestVisible="False">
    <Rectangle.Fill>
      <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
        <GradientStop Color="#A60B0E14" Offset="0" />
        <GradientStop Color="#000B0E14" Offset="0.65" />
      </LinearGradientBrush>
    </Rectangle.Fill>
  </Rectangle>

  <!-- In effective pixels, which the system scales per device (a TV gets the "10-foot" size); a
       larger window gives more room, not larger text. -->
    <Grid x:Name="Layout" Padding="0,40,0,32" XYFocusKeyboardNavigation="Enabled">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="*" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
      </Grid.RowDefinitions>

      <Grid Margin="48,0">
        <StackPanel Spacing="2">
          <TextBlock Text="Library" Style="{StaticResource PageTitleTextBlockStyle}" />
          <TextBlock x:Name="Count" Style="{StaticResource BodyTextBlockStyle}"
                     Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        </StackPanel>
        <StackPanel HorizontalAlignment="Right" Spacing="16">
          <StackPanel Orientation="Horizontal" HorizontalAlignment="Right" Spacing="4">
            <!-- A controller, drawn like the trophy cup: how the Xbox controller plays a PS5 game. -->
            <Button x:Name="ControlsButton" Background="Transparent" BorderThickness="0" Padding="8"
                    ToolTipService.ToolTip="Button layout" AutomationProperties.Name="Button layout">
              <Viewbox Width="20" Height="20">
                <Canvas Width="24" Height="24">
                  <Path Stroke="{ThemeResource SystemControlForegroundBaseHighBrush}" StrokeThickness="1.5"
                        StrokeLineJoin="Round" StrokeStartLineCap="Round" StrokeEndLineCap="Round"
                        Data="M7.5,6.5 H16.5 C19.6,6.5 21.4,9.1 21.9,12.7 L22.4,16.3 C22.7,18.5 20.6,20 18.8,18.8 L15.8,16 H8.2 L5.2,18.8 C3.4,20 1.3,18.5 1.6,16.3 L2.1,12.7 C2.6,9.1 4.4,6.5 7.5,6.5 Z M7.5,9.7 V13.3 M5.7,11.5 H9.3 M15.4,10.6 A0.8,0.8 0 1 0 17,10.6 A0.8,0.8 0 1 0 15.4,10.6 Z M17.4,12.6 A0.8,0.8 0 1 0 19,12.6 A0.8,0.8 0 1 0 17.4,12.6 Z" />
                </Canvas>
              </Viewbox>
            </Button>
            <!-- A trophy cup, drawn in the gear's outline style (the icon fonts differ between a PC and the Xbox). -->
            <Button x:Name="TrophiesButton" Background="Transparent" BorderThickness="0" Padding="8"
                    ToolTipService.ToolTip="Trophies" AutomationProperties.Name="Trophies">
              <Viewbox Width="20" Height="20">
                <Canvas Width="24" Height="24">
                  <Path Stroke="{ThemeResource SystemControlForegroundBaseHighBrush}" StrokeThickness="1.5"
                        StrokeLineJoin="Round" StrokeStartLineCap="Round" StrokeEndLineCap="Round"
                        Data="M7,3.5 H17 V9 A5,5 0 0 1 7,9 Z M7,5 H4.5 V6.5 A3,3 0 0 0 7.6,9.6 M17,5 H19.5 V6.5 A3,3 0 0 1 16.4,9.6 M12,14 V17.5 M9,17.5 H15 V20.5 H9 Z" />
                </Canvas>
              </Viewbox>
            </Button>
            <Button x:Name="SettingsButton" Background="Transparent" BorderThickness="0" Padding="8"
                    ToolTipService.ToolTip="Settings" AutomationProperties.Name="Settings">
              <SymbolIcon Symbol="Setting" />
            </Button>
          </StackPanel>
          <!-- The connected controllers: a dot marks the one used last, then its number, the controller symbol and its name, in columns. -->
          <Grid x:Name="Controllers" HorizontalAlignment="Right" ColumnSpacing="8" RowSpacing="4">
            <Grid.ColumnDefinitions>
              <ColumnDefinition Width="Auto" />
              <ColumnDefinition Width="Auto" />
              <ColumnDefinition Width="Auto" />
              <ColumnDefinition Width="Auto" />
              <ColumnDefinition Width="Auto" />
            </Grid.ColumnDefinitions>
          </Grid>
        </StackPanel>
      </Grid>

      <StackPanel x:Name="Unreadable" Grid.Row="1" Margin="48,16,48,0" MaxWidth="760" HorizontalAlignment="Left"
                  Spacing="8" Visibility="Collapsed">
        <muxc:InfoBar x:Name="UnreadableInfo" IsOpen="True" IsClosable="False" Severity="Warning"
                      Title="Some game folders can't be read" />
        <TextBox x:Name="Commands" IsReadOnly="True" IsTabStop="False" FontFamily="Consolas" FontSize="12"
                 TextWrapping="Wrap" AcceptsReturn="True" />
      </StackPanel>

      <StackPanel x:Name="Empty" Grid.Row="2" Margin="48,0" VerticalAlignment="Center" MaxWidth="640"
                  HorizontalAlignment="Left" Spacing="8" Visibility="Collapsed">
        <TextBlock x:Name="EmptyTitle" Text="No games yet" Style="{StaticResource TitleTextBlockStyle}" />
        <TextBlock x:Name="EmptyText" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </StackPanel>

      <TextBlock x:Name="Title" Grid.Row="3" Margin="48,0" Style="{StaticResource TitleTextBlockStyle}"
                 TextTrimming="CharacterEllipsis" TextWrapping="NoWrap" />

      <ScrollViewer Grid.Row="4" HorizontalScrollMode="Enabled" HorizontalScrollBarVisibility="Hidden"
                    VerticalScrollMode="Disabled" VerticalScrollBarVisibility="Disabled" IsTabStop="False">
        <StackPanel x:Name="Covers" Orientation="Horizontal" Spacing="8" Padding="48,16,48,16"
                    XYFocusKeyboardNavigation="Enabled" />
      </ScrollViewer>

      <Border x:Name="Hints" Grid.Row="5" Style="{StaticResource HintBarStyle}" Margin="48,16,48,0" />
    </Grid>

  <!-- A game's options (Menu button, right click), in the system's look: its details, play, cheats, hide. -->
  <Grid x:Name="Options" Visibility="Collapsed">
    <Rectangle Fill="{ThemeResource SystemControlPageBackgroundMediumAltMediumBrush}" />
    <Grid x:Name="OptionsPanel" Width="360" HorizontalAlignment="Right" Padding="24,32,24,32"
          Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="*" />
        <RowDefinition Height="Auto" />
      </Grid.RowDefinitions>
      <TextBlock Text="Game options" Style="{StaticResource CaptionTextBlockStyle}"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <TextBlock x:Name="OptionsTitle" Grid.Row="1" Style="{StaticResource SubtitleTextBlockStyle}"
                 MaxLines="3" TextTrimming="CharacterEllipsis" Margin="0,4,0,20" />
      <StackPanel Grid.Row="2" Spacing="12" Margin="0,0,0,24">
        <StackPanel>
          <TextBlock Text="Title ID" Style="{StaticResource CaptionTextBlockStyle}"
                     Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
          <TextBlock x:Name="OptionsTitleId" Style="{StaticResource BodyTextBlockStyle}" />
        </StackPanel>
        <StackPanel>
          <TextBlock Text="Version" Style="{StaticResource CaptionTextBlockStyle}"
                     Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
          <TextBlock x:Name="OptionsVersion" Style="{StaticResource BodyTextBlockStyle}" />
        </StackPanel>
        <StackPanel>
          <TextBlock Text="Folder" Style="{StaticResource CaptionTextBlockStyle}"
                     Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
          <TextBlock x:Name="OptionsFolder" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap" />
        </StackPanel>
      </StackPanel>
      <ListView x:Name="OptionsItems" Grid.Row="3" SelectionMode="None" IsItemClickEnabled="True"
                Margin="-12,0" VerticalAlignment="Top">
        <ListView.ItemContainerStyle>
          <Style TargetType="ListViewItem">
            <Setter Property="CornerRadius" Value="0" />
          </Style>
        </ListView.ItemContainerStyle>
      </ListView>
      <Border x:Name="OptionsHints" Grid.Row="4" Style="{StaticResource HintBarStyle}" />
    </Grid>
    <Grid x:Name="Details" Visibility="Collapsed" Padding="48,40,48,32"
          Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto" />
        <RowDefinition Height="*" />
        <RowDefinition Height="Auto" />
      </Grid.RowDefinitions>
      <StackPanel Spacing="4" Margin="0,0,0,16">
        <TextBlock Text="Details" Style="{StaticResource PageTitleTextBlockStyle}" />
        <TextBlock x:Name="DetailsGame" Style="{StaticResource BodyTextBlockStyle}"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" TextWrapping="Wrap" />
      </StackPanel>
      <ScrollViewer x:Name="DetailsScroll" Grid.Row="1" IsTabStop="True" UseSystemFocusVisuals="False"
                    VerticalScrollMode="Enabled" VerticalScrollBarVisibility="Auto"
                    HorizontalScrollMode="Disabled" HorizontalScrollBarVisibility="Disabled">
        <StackPanel x:Name="DetailsRows" Spacing="8" Padding="12,8,20,16" />
      </ScrollViewer>
      <Border x:Name="DetailsHints" Grid.Row="2" Margin="0,16,0,0" Style="{StaticResource HintBarStyle}" />
    </Grid>
  </Grid>
</Grid>)xaml";

struct Launcher {
	LauncherActions                          actions;
	std::vector<GameEntry>                   games;
	winrt::Windows::UI::Core::CoreDispatcher dispatcher {nullptr};
	xaml::Controls::Image                    backdrops[2] {nullptr, nullptr};
	int                                      front = 0;
	uint32_t                                 selection = 0;
	size_t                                   selected  = 0;
	xaml::Controls::TextBlock                title {nullptr};
	xaml::Controls::StackPanel               covers {nullptr};
	xaml::Controls::Grid                     page {nullptr};
	xaml::Controls::Button                   settings_button {nullptr};
	xaml::Controls::Grid                     controllers {nullptr};
	std::wstring                             listed_controllers;
	bool                                     headset_mic_option = false; // the settings page's, when the page was made
	bool                                     home_screen_music  = true;  // the settings page's Home screen music, when the page was made
	// The options of the game at options_index, while open.
	xaml::FrameworkElement       options {nullptr};
	xaml::FrameworkElement       options_panel {nullptr};
	xaml::Controls::ListView     options_items {nullptr};
	xaml::FrameworkElement       details {nullptr};
	xaml::Controls::ScrollViewer details_scroll {nullptr};
	xaml::Controls::StackPanel   details_rows {nullptr};
	xaml::Controls::TextBlock    details_game {nullptr};
	size_t                       options_index = 0;
	uint32_t                     details_item_index = 0;
	uint32_t                     details_request = 0;
};

constexpr wchar_t OPTION_PLAY[]       = L"Play";
constexpr wchar_t OPTION_PLAY_BUILD[] = L"Play and build known pipelines";
constexpr wchar_t OPTION_BUILD_PLAY[] = L"Build known pipelines, then play";
constexpr wchar_t OPTION_DETAILS[]    = L"Details";
constexpr wchar_t OPTION_CHEATS[]     = L"Cheats";
constexpr wchar_t OPTION_HIDE[]       = L"Hide game";

bool OptionsOpen(const Launcher& launcher) {
	return launcher.options.Visibility() == xaml::Visibility::Visible;
}

struct DetailRow {
	std::wstring label;
	std::wstring value;
};

using GameManifest = nlohmann::ordered_json;

std::wstring FirmwareVersion(std::wstring encoded) {
	// Same decimal digits as the desktop launcher's firmware column.
	if (encoded.size() != 18 || (encoded[1] != L'x' && encoded[1] != L'X') || encoded[0] != L'0' ||
	    encoded.find_first_not_of(L"0123456789", 2) < 8 ||
	    encoded.find_first_not_of(L"0123456789abcdefABCDEF", 8) != std::wstring::npos) {
		return encoded;
	}
	const auto major = encoded.substr(encoded[2] == L'0' ? 3 : 2, encoded[2] == L'0' ? 1 : 2);
	auto version = major + L"." + encoded.substr(4, 2);
	if (encoded.compare(6, 2, L"00") != 0) {
		version += L"." + encoded.substr(6, 2);
	}
	return version;
}

// Keep each value on its own row, including members of nested objects and arrays.
void AppendMetadataRows(std::vector<DetailRow>& rows, std::wstring label, const GameManifest& value, size_t depth = 0) {
	if (value.is_structured()) {
		if (value.empty() || depth == 32) {
			rows.push_back({std::move(label), value.empty() ? L"Empty" : L"Additional nested fields"});
			return;
		}
		for (auto item = value.begin(); item != value.end(); ++item) {
			const auto key = value.is_object() ? std::wstring(winrt::to_hstring(item.key()).c_str())
			                                   : std::to_wstring(item - value.begin() + 1);
			AppendMetadataRows(rows, label + L" / " + key, *item, depth + 1);
		}
		return;
	}
	std::wstring text;
	if (value.is_string()) {
		text = winrt::to_hstring(value.get_ref<const std::string&>()).c_str();
		if (text.empty()) {
			text = L"Empty";
		}
	} else if (value.is_boolean()) {
		text = value.get<bool>() ? L"Yes" : L"No";
	} else if (value.is_null()) {
		text = L"None";
	} else {
		text = winrt::to_hstring(value.dump()).c_str();
	}
	rows.push_back({std::move(label), std::move(text)});
}

std::vector<DetailRow> ReadDetails(const GameEntry& game) {
	std::vector<DetailRow> rows;
	const auto text = ReadGameText(game.folder / L"sce_sys" / L"param.json");
	const auto manifest = text ? GameManifest::parse(*text, nullptr, false) : GameManifest {};
	const auto field = [&](const char* key) -> std::wstring {
		const auto value = manifest.find(key);
		return value != manifest.end() && value->is_string() ? winrt::to_hstring(value->get_ref<const std::string&>()).c_str() : L"";
	};

	if (manifest.is_object()) {
		const auto flags = [&](const char* key) -> std::optional<uint32_t> {
			const auto value = manifest.find(key);
			if (value == manifest.end() || !value->is_number_integer()) {
				return std::nullopt;
			}
			return value->get<uint32_t>();
		};
		// The PS5 Pro bit is the desktop launcher's (attribute3 bit 22). Other attribute bits are not shown as features: their meanings
		// are not confirmed; their values are among the metadata below.
		const auto attribute3 = flags("attribute3");
		rows.push_back({L"Graphics", {}});
		rows.push_back({L"PS5 Pro mode", !attribute3 ? L"Unknown" : (*attribute3 & (1u << 22)) != 0 ? L"Supported" : L"Not supported"});
		if (const auto psml = manifest.find("psml"); psml != manifest.end() && psml->is_object()) {
			const auto version = psml->find("mfsrVersion");
			if (version != psml->end() && version->is_string() && !version->get_ref<const std::string&>().empty()) {
				rows.push_back({L"PSML version", winrt::to_hstring(version->get_ref<const std::string&>()).c_str()});
			}
		}
	}

	rows.push_back({L"Game information", {}});
	const auto title_id = field("titleId");
	auto version = field("appVersion");
	if (version.empty()) {
		version = field("contentVersion");
	}
	const auto saved = [](const std::string& value) -> std::wstring {
		return value.empty() ? L"Unknown" : winrt::to_hstring(value).c_str();
	};
	rows.push_back({L"Title ID", title_id.empty() ? saved(game.title_id) : title_id});
	rows.push_back({L"Version", version.empty() ? saved(game.version) : version});
	if (const auto firmware = field("requiredSystemSoftwareVersion"); !firmware.empty()) {
		rows.push_back({L"Required PS5 system software", FirmwareVersion(firmware)});
	}
	if (auto content = field("contentId"); !content.empty()) {
		rows.push_back({L"Content ID", std::move(content)});
	}
	rows.push_back({L"Folder or archive", std::filesystem::path(game.folder).make_preferred().wstring()});
	if (!manifest.is_object()) {
		rows.push_back({L"Game metadata", text ? L"sce_sys/param.json is invalid." : L"Could not read sce_sys/param.json."});
		return rows;
	}

	rows.push_back({L"Additional metadata", {}});
	for (const auto& [key, value]: manifest.items()) {
		if (key == "titleId" || key == "appVersion" || key == "contentId" ||
		    (key == "contentVersion" && value.is_string() && version == winrt::to_hstring(value.get_ref<const std::string&>()).c_str())) {
			continue;
		}
		AppendMetadataRows(rows, winrt::to_hstring(key).c_str(), value);
	}
	return rows;
}

void AddDetailRow(const xaml::Controls::StackPanel& rows, const DetailRow& detail) {
	const auto resources = xaml::Application::Current().Resources();
	const auto style = [&](const wchar_t* key) { return resources.Lookup(winrt::box_value(key)).as<xaml::Style>(); };
	xaml::Controls::TextBlock label;
	label.Text(detail.label);
	label.TextWrapping(xaml::TextWrapping::Wrap);
	if (detail.value.empty()) {
		label.Style(style(L"SubtitleTextBlockStyle"));
		label.Margin(xaml::ThicknessHelper::FromLengths(0, 10, 0, 2));
		rows.Children().Append(label);
		return;
	}

	label.Style(style(L"BodyTextBlockStyle"));
	label.Foreground(resources.Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>());
	xaml::Controls::TextBlock value;
	value.Text(detail.value);
	value.TextWrapping(xaml::TextWrapping::Wrap);
	value.Style(style(L"BodyTextBlockStyle"));

	xaml::Controls::Grid row;
	row.ColumnSpacing(16);
	for (double width: {1.0, 2.0}) {
		xaml::Controls::ColumnDefinition column;
		column.Width(xaml::GridLengthHelper::FromValueAndType(width, xaml::GridUnitType::Star));
		row.ColumnDefinitions().Append(column);
	}
	xaml::Controls::Grid::SetColumn(value, 1);
	row.Children().Append(label);
	row.Children().Append(value);
	rows.Children().Append(row);
}

winrt::fire_and_forget LoadDetails(std::shared_ptr<Launcher> launcher, GameEntry game, uint32_t request) {
	co_await winrt::resume_background();
	const auto rows = ReadDetails(game);
	co_await winrt::resume_foreground(launcher->dispatcher);
	if (request != launcher->details_request) {
		co_return;
	}
	launcher->details_rows.Children().Clear();
	for (const auto& row: rows) {
		AddDetailRow(launcher->details_rows, row);
	}
}

void OpenDetails(const std::shared_ptr<Launcher>& launcher, const GameEntry& game) {
	launcher->details_game.Text(winrt::to_hstring(game.name));
	launcher->details_rows.Children().Clear();
	AddDetailRow(launcher->details_rows, {L"Reading game metadata…", {}});
	launcher->details.Visibility(xaml::Visibility::Visible);
	launcher->options_panel.Visibility(xaml::Visibility::Collapsed);
	launcher->details.UpdateLayout();
	launcher->details_scroll.ChangeView(nullptr, 0.0, nullptr, true);
	launcher->details_scroll.Focus(xaml::FocusState::Keyboard);
	LoadDetails(launcher, game, ++launcher->details_request);
}

void CloseDetails(const std::shared_ptr<Launcher>& launcher) {
	++launcher->details_request;
	launcher->details.Visibility(xaml::Visibility::Collapsed);
	launcher->options_panel.Visibility(xaml::Visibility::Visible);
	launcher->options_panel.UpdateLayout();
	if (auto item = launcher->options_items.ContainerFromIndex(launcher->details_item_index).try_as<xaml::Controls::Control>()) {
		item.Focus(xaml::FocusState::Keyboard);
	}
}

void CloseOptions(const std::shared_ptr<Launcher>& launcher) {
	launcher->options.Visibility(xaml::Visibility::Collapsed);
	launcher->covers.Children()
	    .GetAt(static_cast<uint32_t>(launcher->options_index))
	    .as<xaml::Controls::Control>()
	    .Focus(xaml::FocusState::Keyboard);
}

void OpenOptions(const std::shared_ptr<Launcher>& launcher, size_t index) {
	const auto& game        = launcher->games[index];
	const auto  find        = [&](const wchar_t* name) {
        return launcher->options.FindName(name).as<xaml::Controls::TextBlock>();
	};
	const auto  or_unknown  = [](const std::string& text) {
        return text.empty() ? winrt::hstring(L"Unknown") : winrt::to_hstring(text);
	};
	launcher->options_index = index;
	find(L"OptionsTitle").Text(winrt::to_hstring(game.name));
	find(L"OptionsTitleId").Text(or_unknown(game.title_id));
	find(L"OptionsVersion").Text(or_unknown(game.version));
	auto folder = game.folder;
	find(L"OptionsFolder").Text(folder.make_preferred().wstring());
	launcher->options.Visibility(xaml::Visibility::Visible);
	launcher->options.UpdateLayout();
	if (auto first = launcher->options_items.ContainerFromIndex(0).try_as<xaml::Controls::Control>()) {
		first.Focus(xaml::FocusState::Keyboard);
	}
}

xaml::UIElement CreatePage(LauncherActions actions, bool scan, std::optional<size_t> select, const std::filesystem::path& select_game = {});

// Fades to the background of the game selected now; waits a moment first, so moving quickly
// along the row doesn't load every background on the way.
winrt::fire_and_forget ShowBackdrop(std::shared_ptr<Launcher> launcher, uint32_t selection,
                                    std::filesystem::path path) {
	co_await winrt::resume_after(std::chrono::milliseconds(120));
	co_await winrt::resume_foreground(launcher->dispatcher);
	if (selection != launcher->selection) {
		co_return;
	}
	auto bitmap = co_await LoadImageAsync(launcher->dispatcher, path, BACKDROP_WIDTH);
	if (selection != launcher->selection) {
		co_return;
	}
	auto& shown  = launcher->backdrops[launcher->front];
	auto& hidden = launcher->backdrops[1 - launcher->front];
	hidden.Source(bitmap);
	hidden.Opacity(bitmap != nullptr ? 1.0 : 0.0);
	shown.Opacity(0.0);
	launcher->front = 1 - launcher->front;
}

void Select(const std::shared_ptr<Launcher>& launcher, size_t index) {
	launcher->selected = index;
	launcher->settings_button.XYFocusDown(launcher->covers.Children().GetAt(static_cast<uint32_t>(index)));
	const auto& game   = launcher->games[index];
	launcher->title.Text(winrt::to_hstring(game.name));
	ShowBackdrop(launcher, ++launcher->selection, BackdropImage(game.folder));
	if (launcher->home_screen_music) {
		TitleMusic::Play(game);
	}
}

winrt::fire_and_forget ShowCover(xaml::Controls::Border cover, std::filesystem::path path) {
	auto bitmap = co_await LoadImageAsync(cover.Dispatcher(), path, COVER_WIDTH);
	if (bitmap != nullptr) {
		xaml::Media::ImageBrush brush;
		brush.ImageSource(bitmap);
		brush.Stretch(xaml::Media::Stretch::UniformToFill);
		cover.Background(brush);
		cover.Child(nullptr);
	}
}

// A cover tile: the game's icon0, or its name until (or unless) that loads.
xaml::Controls::Button CreateCover(const std::shared_ptr<Launcher>& launcher, size_t index,
                                   xaml::Style const& style) {
	const auto& game = launcher->games[index];

	xaml::Controls::TextBlock name;
	name.Text(winrt::to_hstring(game.name));
	name.TextWrapping(xaml::TextWrapping::WrapWholeWords);
	name.TextAlignment(xaml::TextAlignment::Center);
	name.VerticalAlignment(xaml::VerticalAlignment::Center);
	name.Margin({6, 6, 6, 6});
	name.FontSize(12);
	name.MaxLines(4);
	name.TextTrimming(xaml::TextTrimming::CharacterEllipsis);

	xaml::Controls::Border cover;
	cover.CornerRadius({4, 4, 4, 4});
	cover.Background(xaml::Media::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x1C, 0x23, 0x30)));
	cover.Child(name);
	ShowCover(cover, CoverImage(game.folder));

	xaml::Controls::Button button;
	button.Style(style);
	button.Content(cover);
	button.GotFocus([launcher, index](auto const&, auto const&) { Select(launcher, index); });
	button.XYFocusUp(launcher->settings_button);
	// The mouse selects what it moves over; not when the row scrolls under a resting cursor.
	button.PointerMoved([](auto const& sender, auto const&) {
		auto tile = sender.template as<xaml::Controls::Button>();
		if (tile.FocusState() == xaml::FocusState::Unfocused) {
			tile.Focus(xaml::FocusState::Pointer);
		}
	});
	button.Click([launcher, index](auto const&, auto const&) {
		launcher->actions.play(launcher->games[index]);
	});

	// The game's options, from the Menu button, a right click or the context menu key.
	button.ContextRequested([launcher, index](auto const&, xaml::Input::ContextRequestedEventArgs const& args) {
		args.Handled(true);
		OpenOptions(launcher, index);
	});
	return button;
}

// Controller and keyboard navigation starts from the focused cover. When none has focus (the
// mouse took it, or the window was inactive), gives it back to the selected one.
bool EnsureCoverFocus(const std::shared_ptr<Launcher>& launcher) {
	// A game's open options have the focus for their own navigation.
	if (launcher->games.empty() || OptionsOpen(*launcher)) {
		return false;
	}
	for (auto element = xaml::Input::FocusManager::GetFocusedElement().try_as<xaml::DependencyObject>();
	     element != nullptr; element = xaml::Media::VisualTreeHelper::GetParent(element)) {
		if (element == launcher->page) {
			return false;
		}
	}
	launcher->covers.Children()
	    .GetAt(static_cast<uint32_t>(launcher->selected))
	    .as<xaml::Controls::Control>()
	    .Focus(xaml::FocusState::Keyboard);
	return true;
}

// A line per connected controller, a dot at the one used last and the headset after one with a headset microphone; rebuilt only when that
// changes.
void ListControllers(const std::shared_ptr<Launcher>& launcher) {
	const auto   controllers = ConnectedControllers();
	std::wstring listed;
	for (const auto& controller: controllers) {
		listed += std::to_wstring(controller.player) + controller.name + (controller.latest ? L"*" : L"") + (controller.headset_mic ? L"h" : L"") + L"\n";
	}
	if (listed == launcher->listed_controllers) {
		return;
	}
	launcher->listed_controllers = listed;
	auto& grid = launcher->controllers;
	grid.Children().Clear();
	grid.RowDefinitions().Clear();
	const auto resource = [](const wchar_t* key) {
		return xaml::Application::Current().Resources().Lookup(winrt::box_value(key));
	};
	const auto high   = resource(L"SystemControlForegroundBaseHighBrush").as<xaml::Media::Brush>();
	const auto medium = resource(L"SystemControlForegroundBaseMediumBrush").as<xaml::Media::Brush>();
	const auto caption = resource(L"CaptionTextBlockStyle").as<xaml::Style>();
	const auto symbol  = GetControllerSymbol();
	const auto place   = [&grid](xaml::FrameworkElement const& element, int32_t row, int32_t column) {
		element.VerticalAlignment(xaml::VerticalAlignment::Center);
		xaml::Controls::Grid::SetRow(element, row);
		xaml::Controls::Grid::SetColumn(element, column);
		grid.Children().Append(element);
	};
	for (const auto& controller: controllers) {
		xaml::Controls::RowDefinition row_definition;
		row_definition.Height(xaml::GridLengthHelper::Auto());
		grid.RowDefinitions().Append(row_definition);
		const auto row   = static_cast<int32_t>(grid.RowDefinitions().Size()) - 1;
		const auto brush = controller.latest ? high : medium;

		xaml::Shapes::Ellipse dot;
		dot.Width(6);
		dot.Height(6);
		dot.Fill(high);
		dot.Opacity(controller.latest ? 1.0 : 0.0);
		place(dot, row, 0);

		xaml::Controls::TextBlock number;
		number.Style(caption);
		number.Text(controller.player < 0 ? std::wstring(L"–") : L"P" + std::to_wstring(controller.player + 1));
		number.Foreground(brush);
		place(number, row, 1);

		// A DSU controller: the network symbol of Segoe MDL2 Assets.
		xaml::Controls::TextBlock icon;
		icon.FontFamily(xaml::Media::FontFamily(controller.network ? L"Segoe MDL2 Assets" : symbol.font));
		icon.Text(controller.network ? L"" : symbol.glyph);
		icon.Foreground(brush);
		place(icon, row, 2);

		xaml::Controls::TextBlock name;
		name.Style(caption);
		name.Text(controller.name);
		name.Foreground(brush);
		place(name, row, 3);

		if (controller.headset_mic) {
			xaml::Controls::TextBlock headset;
			headset.FontFamily(xaml::Media::FontFamily(HEADSET_SYMBOL_FONT));
			headset.Text(HEADSET_SYMBOL);
			headset.Foreground(brush);
			headset.Opacity(launcher->headset_mic_option ? 1.0 : 0.4);
			const auto* description = launcher->headset_mic_option ? L"Microphone" : L"Microphone (off in the settings)";
			xaml::Controls::ToolTipService::SetToolTip(headset, winrt::box_value(description));
			xaml::Automation::AutomationProperties::SetName(headset, description);
			place(headset, row, 4);
		}
	}
}

bool IsNavigationKey(winrt::Windows::System::VirtualKey key) {
	using winrt::Windows::System::VirtualKey;
	switch (key) {
		case VirtualKey::Left:
		case VirtualKey::Right:
		case VirtualKey::Up:
		case VirtualKey::Down:
		case VirtualKey::Enter:
		case VirtualKey::Space:
		case VirtualKey::GamepadA:
		case VirtualKey::GamepadDPadLeft:
		case VirtualKey::GamepadDPadRight:
		case VirtualKey::GamepadDPadUp:
		case VirtualKey::GamepadDPadDown:
		case VirtualKey::GamepadLeftThumbstickLeft:
		case VirtualKey::GamepadLeftThumbstickRight:
		case VirtualKey::GamepadLeftThumbstickUp:
		case VirtualKey::GamepadLeftThumbstickDown: return true;
		default: return false;
	}
}

} // namespace

namespace {

// The launcher page. `scan`: scan the game folders (showing hidden games again) instead of
// using the last scan. `select`: the game to start on (by position), else the one played last.
xaml::UIElement CreatePage(LauncherActions actions, bool scan, std::optional<size_t> select, const std::filesystem::path& select_game) {
	auto page = xaml::Markup::XamlReader::Load(LauncherMarkup).as<xaml::Controls::Grid>();
	const auto find = [&page](const wchar_t* name) { return page.FindName(name); };

	auto launcher     = std::make_shared<Launcher>();
	launcher->actions = std::move(actions);
	launcher->page            = page;
	launcher->controllers     = find(L"Controllers").as<xaml::Controls::Grid>();
	launcher->headset_mic_option = LoadSettings().headset_mic;
	launcher->home_screen_music  = LoadSettings().home_screen_music;
	launcher->settings_button = find(L"SettingsButton").as<xaml::Controls::Button>();
	launcher->settings_button.Click(
	    [launcher](auto const&, auto const&) { launcher->actions.open_settings(); });
	find(L"TrophiesButton").as<xaml::Controls::Button>().Click(
	    [launcher](auto const&, auto const&) { launcher->actions.open_trophies(); });
	find(L"ControlsButton").as<xaml::Controls::Button>().Click(
	    [launcher](auto const&, auto const&) { launcher->actions.open_controls(); });
	if (scan) {
		ShowAllGames();
	}
	auto games = scan ? ScanGames() : LoadGameLibrary();
	if (games.empty() && !scan) {
		games = ScanGames();
	}
	const auto hidden_folders = HiddenGames();
	size_t     hidden         = 0;
	for (auto& game: games) {
		if (std::find(hidden_folders.begin(), hidden_folders.end(), game.folder) != hidden_folders.end()) {
			hidden++;
		} else {
			launcher->games.push_back(std::move(game));
		}
	}
	launcher->dispatcher   = page.Dispatcher();
	launcher->backdrops[0] = find(L"BackdropA").as<xaml::Controls::Image>();
	launcher->backdrops[1] = find(L"BackdropB").as<xaml::Controls::Image>();
	launcher->title        = find(L"Title").as<xaml::Controls::TextBlock>();
	launcher->options      = find(L"Options").as<xaml::FrameworkElement>();
	launcher->options_panel = find(L"OptionsPanel").as<xaml::FrameworkElement>();
	launcher->options_items = find(L"OptionsItems").as<xaml::Controls::ListView>();
	launcher->details       = find(L"Details").as<xaml::FrameworkElement>();
	launcher->details_scroll = find(L"DetailsScroll").as<xaml::Controls::ScrollViewer>();
	launcher->details_rows   = find(L"DetailsRows").as<xaml::Controls::StackPanel>();
	launcher->details_game   = find(L"DetailsGame").as<xaml::Controls::TextBlock>();
	SetHints(find(L"Hints").as<xaml::Controls::Border>(),
	         {{XboxButton::A, L"Play"}, {XboxButton::Y, L"Scan again"}, {XboxButton::Menu, L"Options"}});
	SetHints(find(L"OptionsHints").as<xaml::Controls::Border>(), {{XboxButton::A, L"Select"}, {XboxButton::B, L"Back"}});
	launcher->options_items.Items().Append(winrt::box_value(OPTION_PLAY));
	launcher->options_items.Items().Append(winrt::box_value(OPTION_PLAY_BUILD));
	launcher->options_items.Items().Append(winrt::box_value(OPTION_BUILD_PLAY));
	SetHints(find(L"DetailsHints").as<xaml::Controls::Border>(), {{XboxButton::B, L"Back"}});
	launcher->details_item_index = launcher->options_items.Items().Size();
	launcher->options_items.Items().Append(winrt::box_value(OPTION_DETAILS));
	launcher->options_items.Items().Append(winrt::box_value(OPTION_CHEATS));
	launcher->options_items.Items().Append(winrt::box_value(OPTION_HIDE));
	launcher->options_items.ItemClick([launcher](auto const&, xaml::Controls::ItemClickEventArgs const& args) {
		const auto  item = winrt::unbox_value<winrt::hstring>(args.ClickedItem());
		const auto& game = launcher->games[launcher->options_index];
		if (item == OPTION_PLAY) {
			launcher->actions.play(game);
		} else if (item == OPTION_PLAY_BUILD) {
			// The pipeline cache takes the request at the game's first frame and builds in the background.
			Common::PipelinePrewarm::Request();
			launcher->actions.play(game);
		} else if (item == OPTION_BUILD_PLAY) {
			// The game's code starts once the pipeline cache built them (Emulator::Run); the launch screen shows how far it is.
			Common::PipelinePrewarm::RequestBeforeStart();
			launcher->actions.play(game);
		} else if (item == OPTION_DETAILS) {
			OpenDetails(launcher, game);
		} else if (item == OPTION_CHEATS) {
			launcher->actions.open_cheats(game);
		} else {
			HideGame(game.folder);
			xaml::Window::Current().Content(CreatePage(launcher->actions, false, launcher->options_index));
		}
	});
	// B (or Escape) closes the options.
	launcher->options.KeyDown([launcher](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadB ||
		    args.Key() == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			if (launcher->details.Visibility() == xaml::Visibility::Visible) {
				CloseDetails(launcher);
			} else {
				CloseOptions(launcher);
			}
		}
	});
	launcher->covers       = find(L"Covers").as<xaml::Controls::StackPanel>();

	// Folders the app may not read, with the command that fixes them.
	std::wstring commands;
	std::wstring searched;
	for (const auto& folder: GameFolders()) {
		searched += L"\n" + folder.wstring();
		if (ListError(folder) != 0) {
			commands += (commands.empty() ? L"" : L"\r\n") + GrantCommand(folder);
		}
	}
	if (!commands.empty()) {
		find(L"UnreadableInfo")
		    .as<winrt::Microsoft::UI::Xaml::Controls::InfoBar>()
		    .Message(IsXbox() ? L"Apps need read access to them, which a PC gives: plug the drive (NTFS) "
		                        L"into a PC, open each folder's Properties > Security > Edit > Add, "
		                        L"enter ALL APPLICATION PACKAGES and keep Read & execute, or run the "
		                        L"command below there, with the drive's letter for X:. Then plug it "
		                        L"back in and press Y to scan again."
		                      : L"Give apps read access to them (Properties > Security > Edit > Add, "
		                        L"ALL APPLICATION PACKAGES, Read & execute) or run the command below, "
		                        L"then press Y to scan again.");
		find(L"Commands").as<xaml::Controls::TextBox>().Text(commands);
		find(L"Unreadable").as<xaml::UIElement>().Visibility(xaml::Visibility::Visible);
	}

	const auto count = launcher->games.size();
	find(L"Count").as<xaml::Controls::TextBlock>().Text(
	    (count == 1 ? winrt::hstring(L"1 game")
	                : winrt::to_hstring(static_cast<uint64_t>(count)) + L" games") +
	    (hidden != 0 ? L"  \u00B7  " + winrt::to_hstring(static_cast<uint64_t>(hidden)) + L" hidden"
	                 : winrt::hstring()));
	if (count == 0 && hidden != 0) {
		find(L"EmptyTitle").as<xaml::Controls::TextBlock>().Text(L"All games are hidden");
		find(L"EmptyText").as<xaml::Controls::TextBlock>().Text(
		    IsXbox() ? L"Press Y to scan the game folders again and show them."
		             : L"Press Y (or F5) to scan the game folders again and show them.");
	} else if (count == 0) {
		find(L"EmptyText").as<xaml::Controls::TextBlock>().Text(
		    L"KytyPS5 looks for games (folders with eboot.bin, and .zar archives) in:" + searched +
		    L"\n\nCopy a game there, or add a game folder in Settings " + wchar_t {0x203A} + L" Storage.");
	}
	if (count == 0) {
		find(L"Empty").as<xaml::UIElement>().Visibility(xaml::Visibility::Visible);
		find(L"Hints").as<xaml::UIElement>().Visibility(xaml::Visibility::Collapsed);
	}

	// The row of covers, starting at the game asked for or the one played last.
	auto       covers   = launcher->covers;
	const auto style    = page.Resources().Lookup(winrt::box_value(L"CoverStyle")).as<xaml::Style>();
	const auto last     = LastPlayed();
	size_t     selected = 0;
	bool       found    = false;
	for (size_t index = 0; index < count; index++) {
		covers.Children().Append(CreateCover(launcher, index, style));
		if (!select_game.empty() && launcher->games[index].folder == select_game) {
			selected = index;
			found    = true;
		} else if (!found && !last.empty() && launcher->games[index].title_id == last) {
			selected = index;
		}
	}
	launcher->selected = select && count != 0 ? std::min(*select, count - 1) : selected;

	// Keys for the whole window, so they work whatever has focus: Y on a controller (F5 on a
	// keyboard) scans the game folders again; navigation first brings the focus back to the covers.
	auto window    = xaml::Window::Current();
	auto key_token = window.CoreWindow().KeyDown(
	    [launcher](auto const&, winrt::Windows::UI::Core::KeyEventArgs const& args) {
		    const auto key = args.VirtualKey();
		    if (OptionsOpen(*launcher)) {
			    return;
		    }
		    if (key == winrt::Windows::System::VirtualKey::GamepadY ||
		        key == winrt::Windows::System::VirtualKey::F5) {
			    args.Handled(true);
			    xaml::Window::Current().Content(CreatePage(launcher->actions, true, std::nullopt));
		    } else if (IsNavigationKey(key) && EnsureCoverFocus(launcher)) {
			    args.Handled(true);
		    }
	    });
	auto activated_token = window.Activated(
	    [launcher](auto const&, winrt::Windows::UI::Core::WindowActivatedEventArgs const& args) {
		    if (args.WindowActivationState() != winrt::Windows::UI::Core::CoreWindowActivationState::Deactivated) {
			    EnsureCoverFocus(launcher);
		    }
	    });
	page.Loaded([launcher](auto const&, auto const&) { EnsureCoverFocus(launcher); });
	// The controllers, refreshed while the launcher shows.
	ListControllers(launcher);
	xaml::DispatcherTimer controller_timer;
	controller_timer.Interval(std::chrono::milliseconds(250));
	controller_timer.Tick([launcher](auto const&, auto const&) { ListControllers(launcher); });
	controller_timer.Start();
	page.Unloaded([window, key_token, activated_token, controller_timer](auto const&, auto const&) {
		TitleMusic::Stop(); // a game starts, or another page opens
		controller_timer.Stop();
		window.CoreWindow().KeyDown(key_token);
		window.Activated(activated_token);
	});
	return page;
}

} // namespace

xaml::UIElement CreateLauncherPage(LauncherActions actions, bool scan, const std::filesystem::path& select) {
	return CreatePage(std::move(actions), scan, std::nullopt, select);
}

} // namespace Kyty::Uwp
