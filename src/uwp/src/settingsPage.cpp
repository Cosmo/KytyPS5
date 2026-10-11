#include "settingsPage.h"

#include "fileAccess.h"
#include "input.h"
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
#include <string>
#include <utility>
#include <vector>

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
      <ComboBox x:Name="OverlayLogLines" Header="Log lines in the overlay" MinWidth="280" />

      <TextBlock Text="User" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <TextBox x:Name="UserName" Header="User name (players 2 to 4 get a number added)" MaxLength="16"
               MinWidth="280" HorizontalAlignment="Left" />
      <ComboBox x:Name="Language" Header="Console language" MinWidth="280" />

      <TextBlock Text="Controller" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <ToggleSwitch x:Name="ImpulseTriggers" Header="Adaptive triggers as Impulse Triggers" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Where a game makes the DualSense's triggers resist or vibrate, the motors in the Xbox controller's triggers rumble softly: while you press against the resistance, while the trigger vibrates, and once when a trigger gives way. Pressed all the way, a trigger is still." />
      <ToggleSwitch x:Name="HapticRumble" Header="Haptic feedback as gentle rumble" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Games play the DualSense's haptic feedback like sound for its two actuators. The Xbox controller's small rumble motor gives a short, soft bump where they strike (a knock, a step); haptics that go on stay still. The game's sound still plays as before." />
      <ToggleSwitch x:Name="HeadsetSpeaker" Header="Controller speaker on the headset" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="What a game plays on the DualSense's speaker plays on a headset plugged into the Xbox controller. Without a headset it plays on the TV with the rest of the sound." />
      <ToggleSwitch x:Name="HeadsetMic" Header="Headset microphone as the controller's microphone" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Games hear the microphone of a headset plugged into the Xbox controller as the DualSense's microphone (to blow into, or for voice chat); without a headset, the microphone for chat (on a PC, its default one). Windows asks once whether the app may use the microphone." />

      <TextBlock Text="Game folders" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <TextBlock Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="KytyPS5 looks for games (folders with eboot.bin, and .zar archives) in its own Games folder and in these folders. Folders must give apps read access; the library shows how." />
      <StackPanel x:Name="Folders" Spacing="8" />
      <Button x:Name="AddFolder" Content="Add folder" />

      <TextBlock Text="User Data Path" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <TextBlock Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Where saves, patches and trophies are kept (the folders _SaveData, _Patches and _Trophies), for example on a USB drive. What is in the app's folder already is not moved: copy those folders over yourself." />
      <TextBlock x:Name="UserDataPath" TextWrapping="Wrap" />
      <TextBlock x:Name="UserDataStatus" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap" IsTextSelectionEnabled="True" />
      <StackPanel Orientation="Horizontal" Spacing="8">
        <Button x:Name="ChooseUserData" Content="Choose folder" />
        <Button x:Name="ResetUserData" Content="Use the app's folder" />
      </StackPanel>

      <TextBlock Text="Memory and quality" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <TextBlock Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Applies to the next game you start. Turning a memory option off can crash games that need the memory." />

      <ComboBox x:Name="MemorySaver" Header="Game memory paging" MinWidth="280" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Moves game memory that isn't in use to a file and loads it back when needed. Automatic turns on when the device has limited memory." />

      <ComboBox x:Name="TextureQuality" Header="Texture detail" MinWidth="280" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Big textures are loaded at lower resolution to save GPU memory; surfaces seen up close look blurrier." />

      <ComboBox x:Name="TextureFrom" Header="Textures count as big from" MinWidth="280" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="The larger side of a texture, in pixels. Smaller textures stay at full resolution." />

      <ToggleSwitch x:Name="TextureUncompressed" Header="Shrink uncompressed textures too" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Also loads uncompressed textures at lower resolution: more memory saved, but text and interface images can get hard to read." />

      <ComboBox x:Name="TextureLod" Header="Texture sharpness" MinWidth="280" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Textures are drawn from their smaller versions: faster, blurrier." />

      <ToggleSwitch x:Name="Anisotropic" Header="Anisotropic filtering" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Keeps textures on surfaces seen at an angle sharp. Off is faster." />

      <ToggleSwitch x:Name="GpuSavings" Header="Compact GPU memory allocation" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Allocates GPU memory in smaller pieces: about 300 MB less used, no visible difference." />

      <ToggleSwitch x:Name="VideoThreads" Header="Multi-threaded video decoding" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Decodes videos on all CPU cores." />

      <TextBlock Text="Developer" Style="{StaticResource SubtitleTextBlockStyle}" Margin="0,24,0,0" />
      <ToggleSwitch x:Name="DebugLayer" Header="Graphics debug layer (slower; reports graphics errors in the log)" />
      <ToggleSwitch x:Name="GameOutput"
                    Header="Detailed log: the game's output and the emulator's trace (slower; thousands of lines a second)" />
      <ToggleSwitch x:Name="GpuMemoryLog"
                    Header="Detailed memory report in the log every 10 seconds: GPU, swap file and game memory (short pauses while it is written)" />
      <ToggleSwitch x:Name="GpuCrashDiagnostics"
                    Header="GPU crash diagnostics: a GPU hang or crash names the draw (slower)" />
      <ToggleSwitch x:Name="ShaderCacheAnyBuild" Header="Keep compiled shaders across app builds" />
      <TextBlock Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Games start without compiling their shaders again after an update of the app. Can lead to a crash or wrong pictures when the update changed how shaders are translated: then start the game once with this off, which compiles them afresh." />
      <StackPanel Orientation="Horizontal" Spacing="12">
        <Button x:Name="RemoveShaders" Content="Remove compiled shaders" />
        <TextBlock x:Name="RemoveShadersStatus" Style="{StaticResource CaptionTextBlockStyle}" VerticalAlignment="Center" TextWrapping="Wrap" />
      </StackPanel>

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
	xaml::Controls::TextBlock                 user_data_path {nullptr};
	xaml::Controls::TextBlock                 user_data_status {nullptr};
};

void ListFolders(const std::shared_ptr<SettingsState>& state);

// The user data folder, and whether the app may write in it (with the command that allows it where it may not).
void ShowUserData(const std::shared_ptr<SettingsState>& state) {
	auto path = state->settings.user_data_path;
	if (path.empty()) {
		state->user_data_path.Text(L"The app's own folder");
		state->user_data_status.Text(L"");
		return;
	}
	state->user_data_path.Text(path.make_preferred().wstring());
	const auto error = WriteError(path);
	state->user_data_status.Text(error == 0 ? std::wstring(L"Writable")
	                                        : L"Can't be written (Windows error " + std::to_wstring(error) +
	                                              L"). On a PC with the drive plugged in, run as administrator: " + GrantCommand(path, true));
}

winrt::fire_and_forget ChooseUserData(std::shared_ptr<SettingsState> state) {
	winrt::Windows::Storage::Pickers::FolderPicker picker;
	picker.FileTypeFilter().Append(L"*");
	const auto folder = co_await picker.PickSingleFolderAsync();
	if (folder == nullptr || folder.Path().empty()) {
		co_return;
	}
	state->settings.user_data_path = std::wstring(folder.Path());
	SaveSettings(state->settings);
	ShowUserData(state);
}

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

	// 2 to 20 lines (the overlay keeps the last 20).
	auto overlay_log_lines = page.FindName(L"OverlayLogLines").as<xaml::Controls::ComboBox>();
	for (uint32_t lines = 2; lines <= 20; lines++) {
		overlay_log_lines.Items().Append(winrt::box_value(std::to_wstring(lines) + L" lines"));
	}
	overlay_log_lines.SelectedIndex(static_cast<int32_t>(std::clamp<uint32_t>(settings.overlay_log_lines, 2, 20) - 2));
	overlay_log_lines.SelectionChanged([state, overlay_log_lines, save](auto const&, auto const&) {
		if (const auto index = overlay_log_lines.SelectedIndex(); index >= 0) {
			state->settings.overlay_log_lines = static_cast<uint32_t>(index) + 2;
			save();
		}
	});

	auto impulse_triggers = page.FindName(L"ImpulseTriggers").as<xaml::Controls::ToggleSwitch>();
	impulse_triggers.IsOn(settings.impulse_triggers);
	impulse_triggers.Toggled([state, impulse_triggers, save](auto const&, auto const&) {
		state->settings.impulse_triggers = impulse_triggers.IsOn();
		save();
	});

	auto haptic_rumble = page.FindName(L"HapticRumble").as<xaml::Controls::ToggleSwitch>();
	haptic_rumble.IsOn(settings.haptic_rumble);
	haptic_rumble.Toggled([state, haptic_rumble, save](auto const&, auto const&) {
		state->settings.haptic_rumble = haptic_rumble.IsOn();
		save();
	});

	auto headset_speaker = page.FindName(L"HeadsetSpeaker").as<xaml::Controls::ToggleSwitch>();
	headset_speaker.IsOn(settings.headset_speaker);
	headset_speaker.Toggled([state, headset_speaker, save](auto const&, auto const&) {
		state->settings.headset_speaker = headset_speaker.IsOn();
		save();
	});

	auto headset_mic = page.FindName(L"HeadsetMic").as<xaml::Controls::ToggleSwitch>();
	headset_mic.IsOn(settings.headset_mic);
	headset_mic.Toggled([state, headset_mic, save](auto const&, auto const&) {
		state->settings.headset_mic = headset_mic.IsOn();
		save();
		if (headset_mic.IsOn()) {
			RequestMicrophoneAccess();
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

	// Game memory paging: Automatic is "not set" (on where the app's memory limit is small), the others are the cap in MB.
	auto memory_saver = page.FindName(L"MemorySaver").as<xaml::Controls::ComboBox>();
	{
		std::vector<std::pair<uint32_t, std::wstring>> choices = {{0xFFFFFFFFu, L"Automatic"},         {0, L"Off (all game memory stays in RAM)"},
		                                                          {1500, L"Up to 1500 MB in RAM"},  {2000, L"Up to 2000 MB in RAM"},
		                                                          {3000, L"Up to 3000 MB in RAM"},  {4000, L"Up to 4000 MB in RAM"},
		                                                          {5120, L"Up to 5120 MB in RAM"}};
		if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.first == settings.guest_ram_budget_mb; })) {
			choices.emplace_back(settings.guest_ram_budget_mb, L"Up to " + std::to_wstring(settings.guest_ram_budget_mb) + L" MB in RAM (from the settings file)");
		}
		for (size_t index = 0; index < choices.size(); index++) {
			memory_saver.Items().Append(winrt::box_value(winrt::hstring(choices[index].second)));
			if (choices[index].first == settings.guest_ram_budget_mb) {
				memory_saver.SelectedIndex(static_cast<int32_t>(index));
			}
		}
		memory_saver.SelectionChanged([state, memory_saver, choices, save](auto const&, auto const&) {
			if (const auto index = memory_saver.SelectedIndex(); index >= 0 && static_cast<size_t>(index) < choices.size()) {
				state->settings.guest_ram_budget_mb = choices[static_cast<size_t>(index)].first;
				save();
			}
		});
	}
	auto texture_quality = page.FindName(L"TextureQuality").as<xaml::Controls::ComboBox>();
	for (const auto* name: {L"Full", L"Half resolution", L"Quarter resolution", L"Eighth resolution", L"Sixteenth resolution"}) {
		texture_quality.Items().Append(winrt::box_value(name));
	}
	texture_quality.SelectedIndex(static_cast<int32_t>(std::min<uint32_t>(settings.texture_mip_skip, 4)));
	texture_quality.SelectionChanged([state, texture_quality, save](auto const&, auto const&) {
		if (const auto index = texture_quality.SelectedIndex(); index >= 0) {
			state->settings.texture_mip_skip = static_cast<uint32_t>(index);
			save();
		}
	});
	auto texture_from = page.FindName(L"TextureFrom").as<xaml::Controls::ComboBox>();
	constexpr std::array<uint32_t, 5> TEXTURE_SIZES = {256, 512, 1024, 2048, 4096};
	for (size_t index = 0; index < TEXTURE_SIZES.size(); index++) {
		texture_from.Items().Append(winrt::box_value(winrt::hstring(std::to_wstring(TEXTURE_SIZES[index]) + L" px")));
		if (TEXTURE_SIZES[index] == settings.texture_mip_skip_min_size) {
			texture_from.SelectedIndex(static_cast<int32_t>(index));
		}
	}
	if (texture_from.SelectedIndex() < 0) {
		texture_from.SelectedIndex(3);
	}
	texture_from.SelectionChanged([state, texture_from, TEXTURE_SIZES, save](auto const&, auto const&) {
		if (const auto index = texture_from.SelectedIndex(); index >= 0 && static_cast<size_t>(index) < TEXTURE_SIZES.size()) {
			state->settings.texture_mip_skip_min_size = TEXTURE_SIZES[static_cast<size_t>(index)];
			save();
		}
	});
	auto texture_uncompressed = page.FindName(L"TextureUncompressed").as<xaml::Controls::ToggleSwitch>();
	texture_uncompressed.IsOn(settings.texture_mip_skip_uncompressed);
	texture_uncompressed.Toggled([state, texture_uncompressed, save](auto const&, auto const&) {
		state->settings.texture_mip_skip_uncompressed = texture_uncompressed.IsOn();
		save();
	});
	auto texture_lod = page.FindName(L"TextureLod").as<xaml::Controls::ComboBox>();
	for (const auto* name: {L"Full", L"One level softer", L"Two levels softer", L"Three levels softer", L"Four levels softer"}) {
		texture_lod.Items().Append(winrt::box_value(name));
	}
	texture_lod.SelectedIndex(static_cast<int32_t>(std::min<uint32_t>(settings.texture_lod_bias, 4)));
	texture_lod.SelectionChanged([state, texture_lod, save](auto const&, auto const&) {
		if (const auto index = texture_lod.SelectedIndex(); index >= 0) {
			state->settings.texture_lod_bias = static_cast<uint32_t>(index);
			save();
		}
	});
	auto anisotropic = page.FindName(L"Anisotropic").as<xaml::Controls::ToggleSwitch>();
	anisotropic.IsOn(settings.anisotropic_filtering);
	anisotropic.Toggled([state, anisotropic, save](auto const&, auto const&) {
		state->settings.anisotropic_filtering = anisotropic.IsOn();
		save();
	});
	auto gpu_savings = page.FindName(L"GpuSavings").as<xaml::Controls::ToggleSwitch>();
	gpu_savings.IsOn(settings.gpu_memory_savings);
	gpu_savings.Toggled([state, gpu_savings, save](auto const&, auto const&) {
		state->settings.gpu_memory_savings = gpu_savings.IsOn();
		save();
	});
	auto video_threads = page.FindName(L"VideoThreads").as<xaml::Controls::ToggleSwitch>();
	video_threads.IsOn(settings.video_decode_threads);
	video_threads.Toggled([state, video_threads, save](auto const&, auto const&) {
		state->settings.video_decode_threads = video_threads.IsOn();
		save();
	});

	ListFolders(state);
	page.FindName(L"AddFolder")
.as<xaml::Controls::Button>().Click(
	    [state](auto const&, auto const&) { AddFolder(state); });

	state->user_data_path   = page.FindName(L"UserDataPath").as<xaml::Controls::TextBlock>();
	state->user_data_status = page.FindName(L"UserDataStatus").as<xaml::Controls::TextBlock>();
	ShowUserData(state);
	page.FindName(L"ChooseUserData").as<xaml::Controls::Button>().Click([state](auto const&, auto const&) { ChooseUserData(state); });
	page.FindName(L"ResetUserData").as<xaml::Controls::Button>().Click([state](auto const&, auto const&) {
		state->settings.user_data_path.clear();
		SaveSettings(state->settings);
		ShowUserData(state);
	});

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
	auto gpu_memory_log = page.FindName(L"GpuMemoryLog").as<xaml::Controls::ToggleSwitch>();
	gpu_memory_log.IsOn(settings.gpu_memory_log);
	gpu_memory_log.Toggled([state, gpu_memory_log, save](auto const&, auto const&) {
		state->settings.gpu_memory_log = gpu_memory_log.IsOn();
		save();
	});
	auto gpu_crash_diagnostics = page.FindName(L"GpuCrashDiagnostics").as<xaml::Controls::ToggleSwitch>();
	gpu_crash_diagnostics.IsOn(settings.gpu_crash_diagnostics);
	gpu_crash_diagnostics.Toggled([state, gpu_crash_diagnostics, save](auto const&, auto const&) {
		state->settings.gpu_crash_diagnostics = gpu_crash_diagnostics.IsOn();
		save();
	});
	auto shader_cache_any_build = page.FindName(L"ShaderCacheAnyBuild").as<xaml::Controls::ToggleSwitch>();
	shader_cache_any_build.IsOn(settings.shader_cache_any_build);
	shader_cache_any_build.Toggled([state, shader_cache_any_build, save](auto const&, auto const&) {
		state->settings.shader_cache_any_build = shader_cache_any_build.IsOn();
		save();
	});
	// The games' compiled shaders (LocalState\_PipelineCache, a file per game): each game compiles them afresh on its next start.
	auto remove_shaders_status = page.FindName(L"RemoveShadersStatus").as<xaml::Controls::TextBlock>();
	page.FindName(L"RemoveShaders").as<xaml::Controls::Button>().Click([remove_shaders_status](auto const&, auto const&) {
		std::error_code error;
		uint64_t        files = 0;
		uint64_t        bytes = 0;
		for (const auto& entry: std::filesystem::directory_iterator(LocalStateFolder() / L"_PipelineCache", error)) {
			const auto size = entry.is_regular_file(error) ? entry.file_size(error) : 0;
			if (std::filesystem::remove(entry.path(), error)) {
				files++;
				bytes += size;
			}
		}
		remove_shaders_status.Text(files == 0 ? std::wstring(L"No compiled shaders to remove")
		                                      : L"Removed " + std::to_wstring(files) + (files == 1 ? L" file (" : L" files (") +
		                                            std::to_wstring((bytes + (1u << 20u) - 1) >> 20u) + L" MB)");
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
