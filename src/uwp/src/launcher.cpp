#include "launcher.h"

#include "fileAccess.h"
#include "gameImages.h"
#include "input.h"
#include "settings.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
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
#include <optional>
#include <string>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// Covers are 512x512 and shown up to about that size on large screens.
constexpr int COVER_WIDTH = 512;
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
      <Setter Property="Width" Value="128" />
      <Setter Property="Height" Value="128" />
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
    <!-- Controller hints, as on the Xbox: an outlined button with its letter in the button's color. -->
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
    <Grid x:Name="Layout" Padding="0,40,0,48" XYFocusKeyboardNavigation="Enabled">
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
          <TextBlock Text="Library" Style="{StaticResource SubtitleTextBlockStyle}" />
          <TextBlock x:Name="Count" Style="{StaticResource BodyTextBlockStyle}"
                     Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        </StackPanel>
        <StackPanel HorizontalAlignment="Right" Spacing="16">
          <Button x:Name="SettingsButton" Background="Transparent" BorderThickness="0" Padding="8"
                  HorizontalAlignment="Right" ToolTipService.ToolTip="Settings">
            <SymbolIcon Symbol="Setting" />
          </Button>
          <!-- The connected controllers; a dot marks the one used last. -->
          <StackPanel x:Name="Controllers" Spacing="4" HorizontalAlignment="Right" />
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
        <StackPanel x:Name="Covers" Orientation="Horizontal" Spacing="12" Padding="48,16,48,16"
                    XYFocusKeyboardNavigation="Enabled" />
      </ScrollViewer>

      <Border x:Name="Hints" Grid.Row="5" HorizontalAlignment="Right" Margin="48,16,48,0" Padding="12,8"
              Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}" CornerRadius="4">
        <StackPanel Orientation="Horizontal" Spacing="24">
          <StackPanel Orientation="Horizontal">
            <Grid>
              <Ellipse Style="{StaticResource HintRingStyle}" />
              <TextBlock Text="A" Foreground="#7CC24D" Style="{StaticResource HintLetterStyle}" />
            </Grid>
            <TextBlock Text="Play" Style="{StaticResource HintTextStyle}" />
          </StackPanel>
          <StackPanel Orientation="Horizontal">
            <Grid>
              <Ellipse Style="{StaticResource HintRingStyle}" />
              <TextBlock Text="Y" Foreground="#F2C230" Style="{StaticResource HintLetterStyle}" />
            </Grid>
            <TextBlock Text="Scan again" Style="{StaticResource HintTextStyle}" />
          </StackPanel>
          <StackPanel Orientation="Horizontal">
            <Grid>
              <Ellipse Style="{StaticResource HintRingStyle}" />
              <!-- A FontIcon centers the symbol's own box; tight text bounds would use the cap height. -->
              <FontIcon Glyph="&#xE700;" FontFamily="Segoe MDL2 Assets" FontSize="10"
                        HorizontalAlignment="Center" VerticalAlignment="Center" />
            </Grid>
            <TextBlock Text="Options" Style="{StaticResource HintTextStyle}" />
          </StackPanel>
        </StackPanel>
      </Border>
    </Grid>

  <!-- A game's options (Menu button, right click), in the system's look: its details, play, hide. -->
  <Grid x:Name="Options" Visibility="Collapsed">
    <Rectangle Fill="{ThemeResource SystemControlPageBackgroundMediumAltMediumBrush}" />
    <Grid Width="360" HorizontalAlignment="Right" Padding="24,32,24,24"
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
      <StackPanel Grid.Row="4" Orientation="Horizontal" Spacing="24">
        <StackPanel Orientation="Horizontal">
          <Grid>
            <Ellipse Style="{StaticResource HintRingStyle}" Width="20" Height="20" />
            <TextBlock Text="A" Foreground="#7CC24D" Style="{StaticResource HintLetterStyle}" FontSize="11" />
          </Grid>
          <TextBlock Text="Select" Style="{StaticResource BodyTextBlockStyle}" VerticalAlignment="Center" Margin="8,0,0,0" />
        </StackPanel>
        <StackPanel Orientation="Horizontal">
          <Grid>
            <Ellipse Style="{StaticResource HintRingStyle}" Width="20" Height="20" />
            <TextBlock Text="B" Foreground="#E5553F" Style="{StaticResource HintLetterStyle}" FontSize="11" />
          </Grid>
          <TextBlock Text="Back" Style="{StaticResource BodyTextBlockStyle}" VerticalAlignment="Center" Margin="8,0,0,0" />
        </StackPanel>
      </StackPanel>
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
	xaml::Controls::StackPanel               controllers {nullptr};
	std::wstring                             listed_controllers;
	// The options of the game at options_index, while open.
	xaml::FrameworkElement    options {nullptr};
	xaml::Controls::ListView  options_items {nullptr};
	size_t                    options_index = 0;
};

constexpr wchar_t OPTION_PLAY[] = L"Play";
constexpr wchar_t OPTION_HIDE[] = L"Hide game";

bool OptionsOpen(const Launcher& launcher) {
	return launcher.options.Visibility() == xaml::Visibility::Visible;
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

xaml::UIElement CreatePage(LauncherActions actions, bool scan, std::optional<size_t> select);

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
	name.Margin({10, 10, 10, 10});
	name.FontSize(13);

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

// A line per connected controller, a dot at the one used last; rebuilt only when that changes.
void ListControllers(const std::shared_ptr<Launcher>& launcher) {
	const auto   controllers = ConnectedControllers();
	std::wstring listed;
	for (const auto& controller: controllers) {
		listed += std::to_wstring(controller.player) + controller.name + (controller.latest ? L"*" : L"") + L"\n";
	}
	if (listed == launcher->listed_controllers) {
		return;
	}
	launcher->listed_controllers = listed;
	launcher->controllers.Children().Clear();
	for (const auto& controller: controllers) {
		const auto resource = [](const wchar_t* key) {
			return xaml::Application::Current().Resources().Lookup(winrt::box_value(key));
		};
		const auto high   = resource(L"SystemControlForegroundBaseHighBrush").as<xaml::Media::Brush>();
		const auto medium = resource(L"SystemControlForegroundBaseMediumBrush").as<xaml::Media::Brush>();

		xaml::Shapes::Ellipse dot;
		dot.Width(6);
		dot.Height(6);
		dot.VerticalAlignment(xaml::VerticalAlignment::Center);
		dot.Fill(high);
		dot.Opacity(controller.latest ? 1.0 : 0.0);

		xaml::Controls::TextBlock text;
		text.Style(resource(L"CaptionTextBlockStyle").as<xaml::Style>());
		text.Text(L"Player " + std::to_wstring(controller.player + 1) + L"  \u00B7  " + controller.name);
		text.Foreground(controller.latest ? high : medium);

		xaml::Controls::StackPanel line;
		line.Orientation(xaml::Controls::Orientation::Horizontal);
		line.HorizontalAlignment(xaml::HorizontalAlignment::Right);
		line.Spacing(8);
		line.Children().Append(dot);
		line.Children().Append(text);
		launcher->controllers.Children().Append(line);
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
xaml::UIElement CreatePage(LauncherActions actions, bool scan, std::optional<size_t> select) {
	auto page = xaml::Markup::XamlReader::Load(LauncherMarkup).as<xaml::Controls::Grid>();
	const auto find = [&page](const wchar_t* name) { return page.FindName(name); };

	auto launcher     = std::make_shared<Launcher>();
	launcher->actions = std::move(actions);
	launcher->page            = page;
	launcher->controllers     = find(L"Controllers").as<xaml::Controls::StackPanel>();
	launcher->settings_button = find(L"SettingsButton").as<xaml::Controls::Button>();
	launcher->settings_button.Click(
	    [launcher](auto const&, auto const&) { launcher->actions.open_settings(); });
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
	launcher->options_items = find(L"OptionsItems").as<xaml::Controls::ListView>();
	launcher->options_items.Items().Append(winrt::box_value(OPTION_PLAY));
	launcher->options_items.Items().Append(winrt::box_value(OPTION_HIDE));
	launcher->options_items.ItemClick([launcher](auto const&, xaml::Controls::ItemClickEventArgs const& args) {
		const auto  item = winrt::unbox_value<winrt::hstring>(args.ClickedItem());
		const auto& game = launcher->games[launcher->options_index];
		if (item == OPTION_PLAY) {
			launcher->actions.play(game);
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
			CloseOptions(launcher);
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
		    L"Press Y (or F5) to scan the game folders again and show them.");
	} else if (count == 0) {
		find(L"EmptyText").as<xaml::Controls::TextBlock>().Text(
		    L"KytyPS5 looks for games (folders with eboot.bin, and .zar archives) in:" + searched +
		    L"\n\nCopy a game there, or add a game folder with uwp.ps1 folders -Add <folder>.");
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
	for (size_t index = 0; index < count; index++) {
		covers.Children().Append(CreateCover(launcher, index, style));
		if (!last.empty() && launcher->games[index].title_id == last) {
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
		controller_timer.Stop();
		window.CoreWindow().KeyDown(key_token);
		window.Activated(activated_token);
	});
	return page;
}

} // namespace

xaml::UIElement CreateLauncherPage(LauncherActions actions, bool scan) {
	return CreatePage(std::move(actions), scan, std::nullopt);
}

} // namespace Kyty::Uwp
