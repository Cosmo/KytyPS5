#include "settingsPage.h"

#include "fileAccess.h"
#include "settings.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

struct Resolution {
	uint32_t       width;
	uint32_t       height;
	const wchar_t* name;
};
constexpr std::array RESOLUTIONS = {
    Resolution {1280, 720, L"1280 × 720"},
    Resolution {1920, 1080, L"1920 × 1080"},
    Resolution {2560, 1440, L"2560 × 1440"},
    Resolution {3840, 2160, L"3840 × 2160"},
};

// By the system's language ID, as the desktop launcher lists them.
constexpr std::array CONSOLE_LANGUAGES = {
    L"Japanese",
    L"English (United States)",
    L"French (France)",
    L"Spanish (Spain)",
    L"German",
    L"Italian",
    L"Dutch",
    L"Portuguese (Portugal)",
    L"Russian",
    L"Korean",
    L"Chinese (Traditional)",
    L"Chinese (Simplified)",
    L"Finnish",
    L"Swedish",
    L"Danish",
    L"Norwegian",
    L"Polish",
    L"Portuguese (Brazil)",
    L"English (United Kingdom)",
    L"Turkish",
    L"Spanish (Latin America)",
    L"Arabic",
    L"French (Canada)",
    L"Czech",
    L"Hungarian",
    L"Greek",
    L"Romanian",
    L"Thai",
    L"Vietnamese",
    L"Indonesian",
};

constexpr wchar_t SettingsMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Background="{ThemeResource ApplicationPageBackgroundThemeBrush}">
  <Grid.RowDefinitions>
    <RowDefinition Height="Auto" />
    <RowDefinition Height="*" />
  </Grid.RowDefinitions>
  <TextBlock Text="Settings" Style="{StaticResource TitleTextBlockStyle}" Margin="48,32,48,12" />
  <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto" Padding="48,0,48,32">
    <StackPanel MaxWidth="640" HorizontalAlignment="Left" Spacing="12">
      <TextBlock Text="Display" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,12,0,0" />
      <ComboBox x:Name="Resolution" Header="Resolution the games render at" MinWidth="280" />
      <ToggleSwitch x:Name="VSync" Header="V-Sync" OffContent="Off: each frame as soon as it's done"
                    OnContent="On: frames at the display's refresh rate" />
      <ComboBox x:Name="OverlaySize" Header="Size of the overlay over games" MinWidth="280" />

      <TextBlock Text="User" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <TextBox x:Name="UserName" Header="User name (players 2 to 4 get a number added)" MaxLength="16"
               MinWidth="280" HorizontalAlignment="Left" />
      <ComboBox x:Name="Language" Header="Console language" MinWidth="280" />

      <TextBlock Text="Game folders" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <TextBlock Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="KytyPS5 looks for games (folders with eboot.bin) in its own Games folder and in these folders. Folders must give apps read access; the library shows how." />
      <StackPanel x:Name="Folders" Spacing="8" />
      <Button x:Name="AddFolder" Content="Add folder" />

      <TextBlock Text="Developer" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <ToggleSwitch x:Name="DebugLayer" Header="Graphics debug layer (slower; reports graphics errors in the log)" />
      <ToggleSwitch x:Name="GameOutput"
                    Header="Detailed log: the game's output and the emulator's trace (slower; thousands of lines a second)" />

      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" Margin="0,24,0,0"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Changes apply to the next game you start." />
    </StackPanel>
  </ScrollViewer>
</Grid>)xaml";

struct SettingsState {
	AppSettings                               settings = LoadSettings();
	bool                                      folders_changed = false;
	bool                                      left            = false;
	std::function<void(bool)>                 back;
	xaml::Controls::StackPanel                folders {nullptr};
};

void ListFolders(const std::shared_ptr<SettingsState>& state);

winrt::fire_and_forget AddFolder(std::shared_ptr<SettingsState> state) {
	winrt::Windows::Storage::Pickers::FolderPicker picker;
	picker.FileTypeFilter().Append(L"*");
	const auto folder = co_await picker.PickSingleFolderAsync();
	if (folder == nullptr || folder.Path().empty()) {
		co_return;
	}
	const std::filesystem::path path(std::wstring(folder.Path()));
	auto& folders = state->settings.game_folders;
	if (std::find(folders.begin(), folders.end(), path) == folders.end()) {
		folders.push_back(path);
		SaveSettings(state->settings);
		state->folders_changed = true;
		ListFolders(state);
	}
}

// A row per game folder: the path, whether the app may read it, and Remove.
void ListFolders(const std::shared_ptr<SettingsState>& state) {
	state->folders.Children().Clear();
	if (state->settings.game_folders.empty()) {
		xaml::Controls::TextBlock none;
		none.Text(L"No folders added.");
		state->folders.Children().Append(none);
	}
	for (size_t index = 0; index < state->settings.game_folders.size(); index++) {
		auto folder = state->settings.game_folders[index];

		xaml::Controls::TextBlock path;
		path.Text(folder.make_preferred().wstring());
		path.TextWrapping(xaml::TextWrapping::Wrap);
		xaml::Controls::TextBlock status;
		const auto error = ListError(folder);
		status.Text(error == 0 ? winrt::hstring(L"Readable")
		                       : L"Can't be read (Windows error " + winrt::to_hstring(error) +
		                             L"): see the library for the fix");
		status.Style(xaml::Application::Current().Resources().Lookup(winrt::box_value(L"CaptionTextBlockStyle"))
		                 .as<xaml::Style>());
		xaml::Controls::StackPanel text;
		text.Children().Append(path);
		text.Children().Append(status);

		xaml::Controls::Button remove;
		remove.Content(winrt::box_value(L"Remove"));
		remove.VerticalAlignment(xaml::VerticalAlignment::Center);
		remove.Click([state, folder](auto const&, auto const&) {
			auto& folders = state->settings.game_folders;
			folders.erase(std::remove(folders.begin(), folders.end(), folder), folders.end());
			SaveSettings(state->settings);
			state->folders_changed = true;
			ListFolders(state);
		});

		xaml::Controls::Grid row;
		row.ColumnSpacing(16);
		row.ColumnDefinitions().Append(xaml::Controls::ColumnDefinition());
		xaml::Controls::ColumnDefinition button_column;
		button_column.Width(xaml::GridLengthHelper::Auto());
		row.ColumnDefinitions().Append(button_column);
		row.Children().Append(text);
		row.Children().Append(remove);
		xaml::Controls::Grid::SetColumn(remove, 1);
		state->folders.Children().Append(row);
	}
}

} // namespace

xaml::UIElement CreateSettingsPage(std::function<void(bool folders_changed)> back) {
	auto page       = xaml::Markup::XamlReader::Load(SettingsMarkup).as<xaml::Controls::Grid>();
	auto state      = std::make_shared<SettingsState>();
	state->back     = std::move(back);
	state->folders  = page.FindName(L"Folders").as<xaml::Controls::StackPanel>();
	auto& settings  = state->settings;
	const auto save = [state] { SaveSettings(state->settings); };

	auto resolution = page.FindName(L"Resolution").as<xaml::Controls::ComboBox>();
	for (size_t index = 0; index < RESOLUTIONS.size(); index++) {
		resolution.Items().Append(winrt::box_value(RESOLUTIONS[index].name));
		if (RESOLUTIONS[index].width == settings.screen_width &&
		    RESOLUTIONS[index].height == settings.screen_height) {
			resolution.SelectedIndex(static_cast<int32_t>(index));
		}
	}
	resolution.SelectionChanged([state, resolution, save](auto const&, auto const&) {
		if (const auto index = resolution.SelectedIndex(); index >= 0) {
			state->settings.screen_width  = RESOLUTIONS[static_cast<size_t>(index)].width;
			state->settings.screen_height = RESOLUTIONS[static_cast<size_t>(index)].height;
			save();
		}
	});

	auto vsync = page.FindName(L"VSync").as<xaml::Controls::ToggleSwitch>();
	vsync.IsOn(settings.vsync);
	vsync.Toggled([state, vsync, save](auto const&, auto const&) {
		state->settings.vsync = vsync.IsOn();
		save();
	});

	auto overlay_size = page.FindName(L"OverlaySize").as<xaml::Controls::ComboBox>();
	for (const auto* name: {L"Small", L"Medium", L"Large"}) {
		overlay_size.Items().Append(winrt::box_value(name));
	}
	overlay_size.SelectedIndex(static_cast<int32_t>(std::min<uint32_t>(settings.overlay_size, 2)));
	overlay_size.SelectionChanged([state, overlay_size, save](auto const&, auto const&) {
		if (const auto index = overlay_size.SelectedIndex(); index >= 0) {
			state->settings.overlay_size = static_cast<uint32_t>(index);
			save();
		}
	});

	auto user_name = page.FindName(L"UserName").as<xaml::Controls::TextBox>();
	user_name.Text(winrt::to_hstring(settings.user_name));
	user_name.TextChanged([state, user_name, save](auto const&, auto const&) {
		// Up to 16 bytes for the games; a longer (or empty) name keeps the default.
		state->settings.user_name = winrt::to_string(user_name.Text());
		save();
	});

	auto language = page.FindName(L"Language").as<xaml::Controls::ComboBox>();
	for (const auto* name: CONSOLE_LANGUAGES) {
		language.Items().Append(winrt::box_value(name));
	}
	language.SelectedIndex(static_cast<int32_t>(
	    std::min<size_t>(settings.console_language, CONSOLE_LANGUAGES.size() - 1)));
	language.SelectionChanged([state, language, save](auto const&, auto const&) {
		if (const auto index = language.SelectedIndex(); index >= 0) {
			state->settings.console_language = static_cast<uint32_t>(index);
			save();
		}
	});

	ListFolders(state);
	page.FindName(L"AddFolder").as<xaml::Controls::Button>().Click(
	    [state](auto const&, auto const&) { AddFolder(state); });

	auto debug_layer = page.FindName(L"DebugLayer").as<xaml::Controls::ToggleSwitch>();
	debug_layer.IsOn(settings.debug_layer);
	debug_layer.Toggled([state, debug_layer, save](auto const&, auto const&) {
		state->settings.debug_layer = debug_layer.IsOn();
		save();
	});
	auto game_output = page.FindName(L"GameOutput").as<xaml::Controls::ToggleSwitch>();
	game_output.IsOn(settings.game_output);
	game_output.Toggled([state, game_output, save](auto const&, auto const&) {
		state->settings.game_output = game_output.IsOn();
		save();
	});

	// Back: B, Escape or the window's back button (shown while the page is open).
	const auto leave = [state] {
		if (!state->left) {
			state->left = true;
			state->back(state->folders_changed);
		}
	};
	page.KeyDown([leave](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadB ||
		    args.Key() == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			leave();
		}
	});
	auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	navigation.AppViewBackButtonVisibility(winrt::Windows::UI::Core::AppViewBackButtonVisibility::Visible);
	auto back_token = navigation.BackRequested(
	    [leave](auto const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
		    args.Handled(true);
		    leave();
	    });
	page.Loaded([resolution](auto const&, auto const&) { resolution.Focus(xaml::FocusState::Keyboard); });
	page.Unloaded([navigation, back_token](auto const&, auto const&) {
		navigation.BackRequested(back_token);
		navigation.AppViewBackButtonVisibility(
		    winrt::Windows::UI::Core::AppViewBackButtonVisibility::Collapsed);
	});
	return page;
}

} // namespace Kyty::Uwp
