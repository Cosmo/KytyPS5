#include "settingsPage.h"

#include "fileAccess.h"
#include "input.h"
#include "overlay.h"
#include "settingTiles.h"
#include "settings.h"
#include "xboxLook.h"

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
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>

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
// The TV resolution the games are told (VideoOutGetOutputStatus): 1080p below 3840 × 2160, 4K from it. A game that doesn't ask always hears 4K, and
// the picture is scaled to the screen either way, so these two are all there is to choose.
constexpr std::array RESOLUTIONS = {
    Resolution {1920, 1080, L"1080p"},
    Resolution {3840, 2160, L"4K"},
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
      Background="{ThemeResource ApplicationPageBackgroundThemeBrush}" Padding="0,40,0,32">
  <Grid.RowDefinitions>
    <RowDefinition Height="Auto" />
    <RowDefinition Height="*" />
    <RowDefinition Height="Auto" />
  </Grid.RowDefinitions>
  <TextBlock Text="Settings" Style="{StaticResource PageTitleTextBlockStyle}" Margin="48,0,48,20" />
  <!-- As the Xbox's Settings: the categories on the left (the chosen one in its green), the settings of the chosen one in columns of tiles
       (SettingTile, 219 wide, 36 between columns), and next to them the explanation of the one with the focus. -->
  <Grid Grid.Row="1" ColumnSpacing="24">
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="220" />
      <ColumnDefinition Width="*" />
    </Grid.ColumnDefinitions>
    <Grid>
      <Grid.RowDefinitions>
        <RowDefinition Height="*" />
        <RowDefinition Height="Auto" />
      </Grid.RowDefinitions>
      <ListView x:Name="Categories" SelectionMode="Single" Padding="0">
        <ListView.Resources>
          <SolidColorBrush x:Key="ListViewItemBackgroundSelected" Color="#54A81B" />
          <SolidColorBrush x:Key="ListViewItemBackgroundSelectedPointerOver" Color="#5FB526" />
          <SolidColorBrush x:Key="ListViewItemBackgroundSelectedPressed" Color="#4A9518" />
          <SolidColorBrush x:Key="ListViewItemForegroundSelected" Color="White" />
        </ListView.Resources>
        <ListView.ItemContainerStyle>
          <Style TargetType="ListViewItem">
            <Setter Property="CornerRadius" Value="0" />
            <Setter Property="Padding" Value="48,0,12,0" />
            <Setter Property="MinHeight" Value="34" />
          </Style>
        </ListView.ItemContainerStyle>
        <x:String>Display</x:String>
        <x:String>User</x:String>
        <x:String>Controller</x:String>
        <x:String>Game folders</x:String>
        <x:String>User Data Path</x:String>
        <x:String>Memory and quality</x:String>
        <x:String>Developer</x:String>
      </ListView>
      <TextBlock Grid.Row="1" Margin="48,12,12,0" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}"
                 Text="Game settings apply to the next game you start." />
    </Grid>

    <Grid Grid.Column="1" Margin="0,0,48,0">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="Auto" />
        <ColumnDefinition Width="*" />
      </Grid.ColumnDefinitions>
      <ScrollViewer x:Name="Panels" VerticalScrollBarVisibility="Hidden" Padding="0,0,0,16">
        <Grid>
          <!-- In two groups, as the Xbox groups a page's settings in columns. -->
          <StackPanel x:Name="Display" Orientation="Horizontal" Spacing="36">
            <StackPanel x:Name="PictureGroup" Width="219" Spacing="8" />
            <StackPanel x:Name="OverlayGroup" Width="219" Spacing="8" />
          </StackPanel>

          <StackPanel x:Name="User" Width="219" Spacing="8" Visibility="Collapsed">
            <TextBox x:Name="UserName" Header="User name" MaxLength="16" />
          </StackPanel>

          <StackPanel x:Name="Controller" Width="219" Spacing="8" Visibility="Collapsed" />

          <StackPanel x:Name="GameFolders" Width="474" Spacing="12" Visibility="Collapsed">
            <StackPanel x:Name="Folders" Spacing="8" />
            <Button x:Name="AddFolder" Content="Add folder" />
          </StackPanel>

          <StackPanel x:Name="UserData" Width="474" Spacing="12" Visibility="Collapsed">
            <TextBlock x:Name="UserDataPath" TextWrapping="Wrap" />
            <TextBlock x:Name="UserDataStatus" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap" IsTextSelectionEnabled="True" />
            <StackPanel Orientation="Horizontal" Spacing="8">
              <Button x:Name="ChooseUserData" Content="Choose folder" />
              <Button x:Name="ResetUserData" Content="Use the app's folder" />
            </StackPanel>
          </StackPanel>

          <!-- In two groups, as the Xbox groups a page's settings in columns. -->
          <StackPanel x:Name="Memory" Orientation="Horizontal" Spacing="36" Visibility="Collapsed">
            <StackPanel x:Name="MemoryGroup" Width="219" Spacing="8" />
            <StackPanel x:Name="TextureGroup" Width="219" Spacing="8" />
          </StackPanel>

          <StackPanel x:Name="Developer" Width="219" Spacing="8" Visibility="Collapsed">
            <StackPanel x:Name="DeveloperTiles" Spacing="8" />
            <Button x:Name="RemoveShaders" Content="Remove compiled shaders" HorizontalAlignment="Stretch" Margin="0,8,0,0" />
            <TextBlock x:Name="RemoveShadersStatus" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap" />
          </StackPanel>
        </Grid>
      </ScrollViewer>
      <!-- The explanation of the setting with the focus, or of the category: a paragraph each for what it does, its effect and a warning. -->
      <StackPanel x:Name="Explanation" Grid.Column="1" Margin="36,0,0,0" MaxWidth="230" HorizontalAlignment="Left" VerticalAlignment="Top"
                  Spacing="12" />
    </Grid>
  </Grid>
  <Border x:Name="Hints" Grid.Row="2" Style="{StaticResource HintBarStyle}" Margin="48,16,48,0" />
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
	// Windows' "file not found", "path not found" and "device not ready": the folder or its drive is not there, which no access helps.
	constexpr std::array<uint32_t, 3> MISSING = {2, 3, 21};
	const auto                        error   = WriteError(path);
	if (error == 0) {
		state->user_data_status.Text(L"Writable");
	} else if (std::find(MISSING.begin(), MISSING.end(), error) != MISSING.end()) {
		state->user_data_status.Text(L"Can't be found. Plug in the drive or choose another folder.");
	} else {
		// Only a PC can give apps access to a folder: on the Xbox, the drive goes to a PC for it, where its letter differs (GrantCommand writes X:).
		state->user_data_status.Text(L"Can't be written. Error code " + std::to_wstring(error) + L".\n" +
		                             (IsXbox() ? L"To allow it, plug the drive into a PC and run this there as administrator, with the drive's letter for X:\n"
		                                       : L"To allow it, run this as administrator:\n") +
		                             GrantCommand(path, true));
	}
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
		                       : L"Can't be read. Error code " + winrt::to_hstring(error) + L". The library shows how to allow it.");
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

	SetHints(page.FindName(L"Hints").as<xaml::Controls::Border>(), {{XboxButton::A, L"Select"}, {XboxButton::B, L"Back"}});
	const auto panel       = [&page](const wchar_t* name) { return page.FindName(name).as<xaml::Controls::Panel>(); };
	const auto explanation = page.FindName(L"Explanation").as<xaml::Controls::StackPanel>();

	// Display.
	const auto display       = panel(L"PictureGroup");
	const auto overlay_group = panel(L"OverlayGroup");
	AddHeading(display, L"Picture");
	AddHeading(overlay_group, L"Overlay");
	{
		std::vector<std::wstring> names;
		for (const auto& resolution: RESOLUTIONS) {
			names.emplace_back(resolution.name);
		}
		// Older sizes from the settings file (1280 × 720, 2560 × 1440) are told as 1080p, and show as it.
		const size_t selected = settings.screen_width >= 3840 && settings.screen_height >= 2160 ? 1 : 0;
		AddChoice(
		    display, explanation, L"TV resolution", std::move(names), selected,
		    [state, save](size_t index) {
			    state->settings.screen_width  = RESOLUTIONS[index].width;
			    state->settings.screen_height = RESOLUTIONS[index].height;
			    save();
		    },
		    {L"The TV resolution the games are told.", L"Some games choose their render resolution from it. There, 4K looks sharper and needs more of the GPU.",
		     L"Games that don't ask are always told 4K. The picture is scaled to the screen either way."});
	}
	AddSwitch(
	    display, explanation, L"V-Sync", settings.vsync,
	    [state, save](bool on) {
		    state->settings.vsync = on;
		    save();
	    },
	    {L"On: each frame waits for the display's next refresh. Smooth, with a little more delay.",
	     L"Off: the newest frame shows at the next refresh and older ones are skipped. Less delay.",
	     L"Games run at up to 60 frames a second either way."});
	// The same tiles as in the game menu's Settings.
	AddOverlaySettings(overlay_group, explanation, std::shared_ptr<AppSettings>(state, &state->settings), save);

	// Controller.
	const auto controller = panel(L"Controller");
	AddSwitch(
	    controller, explanation, L"Adaptive triggers as Impulse Triggers", settings.impulse_triggers,
	    [state, save](bool on) {
		    state->settings.impulse_triggers = on;
		    save();
	    },
	    {L"The DualSense's adaptive triggers become rumble in the Xbox controller's triggers.",
	     L"When a trigger resists, it rumbles softly while you press it deeper. When a trigger vibrates, it rumbles softly. When a trigger gives way, it "
	     L"rumbles once.",
	     L"Pressed all the way down, a trigger no longer resists or vibrates."});
	AddSwitch(
	    controller, explanation, L"Haptic feedback as gentle rumble", settings.haptic_rumble,
	    [state, save](bool on) {
		    state->settings.haptic_rumble = on;
		    save();
	    },
	    {L"The DualSense's haptic feedback becomes gentle rumble.",
	     L"A sudden effect, such as a knock or a step, gives a soft bump on the small motor. An effect that goes on, such as rain or an engine, only "
	     L"bumps when it starts.",
	     L"The game's sound is not affected."});
	AddSwitch(
	    controller, explanation, L"Controller speaker on the headset", settings.headset_speaker,
	    [state, save](bool on) {
		    state->settings.headset_speaker = on;
		    save();
	    },
	    // The Xbox doesn't report a controller's headset (input.h): there it is always the chat headset.
	    {IsXbox() ? L"Sound a game plays on player 1's DualSense speaker plays on the chat headset, such as one plugged into the controller."
	              : L"Sound a game plays on player 1's DualSense speaker plays on a headset plugged into player 1's controller, or else on the "
	                L"communications device of Windows where that is not the default one.",
	     L"Without a headset, it plays with the rest of the sound."});
	AddSwitch(
	    controller, explanation, L"Headset microphone", settings.headset_mic,
	    [state, save](bool on) {
		    state->settings.headset_mic = on;
		    save();
		    if (on) {
			    RequestMicrophoneAccess();
		    }
	    },
	    {IsXbox() ? L"Player 1's DualSense microphone uses the chat microphone, such as the one of a headset plugged into the controller."
	              : L"Player 1's DualSense microphone uses the microphone of a headset plugged into player 1's controller, or else the "
	                L"communications microphone of Windows.",
	     L"Games use it for example to blow into. Off: games find no microphone.",
	     L"You are asked once whether KytyPS5 may use the microphone. If you say no, allow it later in the privacy settings."});

	auto user_name = page.FindName(L"UserName").as<xaml::Controls::TextBox>();
	user_name.Text(winrt::to_hstring(settings.user_name));
	// The games take up to 16 bytes of UTF-8 (a longer or empty name keeps the default, Kyty): the box takes no more, so that letters outside
	// ASCII, which take 2 to 4 bytes, can't make the name silently fall back.
	user_name.BeforeTextChanging([](auto const&, xaml::Controls::TextBoxBeforeTextChangingEventArgs const& args) {
		if (winrt::to_string(args.NewText()).size() > 16) {
			args.Cancel(true);
		}
	});
	user_name.TextChanged([state, user_name, save](auto const&, auto const&) {
		state->settings.user_name = winrt::to_string(user_name.Text());
		save();
	});
	Explain(user_name, explanation,
	        {L"The name the games show for player 1.", L"Players 2 to 4 are called Player 2, Player 3 and Player 4.",
	         L"It fits 16 letters. Accented and non-Latin letters take more room. An empty name shows as Kyty."});

	{
		std::vector<std::wstring> languages(CONSOLE_LANGUAGES.begin(), CONSOLE_LANGUAGES.end());
		AddChoice(
		    panel(L"User"), explanation, L"Console language", std::move(languages),
		    std::min<size_t>(settings.console_language, CONSOLE_LANGUAGES.size() - 1),
		    [state, save](size_t index) {
			    state->settings.console_language = static_cast<uint32_t>(index);
			    save();
		    },
		    {L"The system language the games use for text and speech, where a game offers it.", L"Trophy names follow it too."});
	}

	// Memory and quality. Game memory paging: Automatic is "not set" (on where the app's memory limit is small), the others are the cap in MB.
	const auto memory   = panel(L"MemoryGroup");
	const auto textures = panel(L"TextureGroup");
	AddHeading(memory, L"Memory and video");
	AddHeading(textures, L"Textures");
	{
		std::vector<std::pair<uint32_t, std::wstring>> choices = {{0xFFFFFFFFu, L"Automatic"},         {0, L"Off: all game memory stays in RAM"},
		                                                          {1500, L"Up to 1500 MB in RAM"},  {2000, L"Up to 2000 MB in RAM"},
		                                                          {3000, L"Up to 3000 MB in RAM"},  {4000, L"Up to 4000 MB in RAM"},
		                                                          {5120, L"Up to 5120 MB in RAM"}};
		if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.first == settings.guest_ram_budget_mb; })) {
			choices.emplace_back(settings.guest_ram_budget_mb, L"Up to " + std::to_wstring(settings.guest_ram_budget_mb) + L" MB in RAM, from the settings file");
		}
		std::vector<std::wstring> names;
		size_t                    selected = choices.size();
		for (size_t index = 0; index < choices.size(); index++) {
			names.push_back(choices[index].second);
			if (choices[index].first == settings.guest_ram_budget_mb) {
				selected = index;
			}
		}
		AddChoice(
		    memory, explanation, L"Game memory paging", std::move(names), selected,
		    [state, choices, save](size_t index) {
			    state->settings.guest_ram_budget_mb = choices[index].first;
			    save();
		    },
		    {L"When game memory goes over its limit, the parts the game has not used lately move to the swap file. They load back when the game "
		     L"needs them.",
		     L"Automatic turns this on when the app may use 8 GB or less, as on the Xbox. The MB choices set the limit for the game's memory. It is "
		     L"lower while the GPU needs the room.",
		     L"Off can crash games that need more memory than there is."});
	}
	AddChoice(
	    textures, explanation, L"Texture detail", {L"Full", L"Half resolution", L"Quarter resolution", L"Eighth resolution", L"Sixteenth resolution"},
	    std::min<uint32_t>(settings.texture_mip_skip, 4),
	    [state, save](size_t index) {
		    state->settings.texture_mip_skip = static_cast<uint32_t>(index);
		    save();
	    },
	    {L"Big compressed textures with mipmaps load at a lower resolution to save GPU memory.",
	     L"Surfaces seen up close look blurrier. Distant surfaces look the same."});
	{
		constexpr std::array<uint32_t, 5> TEXTURE_SIZES = {256, 512, 1024, 2048, 4096};
		std::vector<std::wstring>         names;
		size_t                            selected = 3;
		for (size_t index = 0; index < TEXTURE_SIZES.size(); index++) {
			names.push_back(std::to_wstring(TEXTURE_SIZES[index]) + L" px");
			if (TEXTURE_SIZES[index] == settings.texture_mip_skip_min_size) {
				selected = index;
			}
		}
		AddChoice(
		    textures, explanation, L"Textures count as big from", std::move(names), selected,
		    [state, TEXTURE_SIZES, save](size_t index) {
			    state->settings.texture_mip_skip_min_size = TEXTURE_SIZES[index];
			    save();
		    },
		    {L"Only textures at least this large get a lower resolution. The size is the longer side, in pixels.",
		     L"Smaller textures keep their full resolution."});
	}
	AddSwitch(
	    textures, explanation, L"Shrink uncompressed textures too", settings.texture_mip_skip_uncompressed,
	    [state, save](bool on) {
		    state->settings.texture_mip_skip_uncompressed = on;
		    save();
	    },
	    {L"Uncompressed textures with mipmaps get a lower resolution as well.", L"Saves more memory, but some text and menu images can become blurry."});
	AddChoice(
	    textures, explanation, L"Texture sharpness", {L"Full", L"One level softer", L"Two levels softer", L"Three levels softer", L"Four levels softer"},
	    std::min<uint32_t>(settings.texture_lod_bias, 4),
	    [state, save](size_t index) {
		    state->settings.texture_lod_bias = static_cast<uint32_t>(index);
		    save();
	    },
	    {L"Textures are drawn from their smaller versions. This saves no memory.", L"Can be a little faster when the GPU is the limit, but blurrier."});
	AddSwitch(
	    textures, explanation, L"Anisotropic filtering", settings.anisotropic_filtering,
	    [state, save](bool on) {
		    state->settings.anisotropic_filtering = on;
		    save();
	    },
	    {L"Keeps textures sharp on surfaces seen at an angle, where the game asks for it.", L"Off uses plain filtering instead. It can be a little faster."});
	AddSwitch(
	    memory, explanation, L"Compact GPU memory allocation", settings.gpu_memory_savings,
	    [state, save](bool on) {
		    state->settings.gpu_memory_savings = on;
		    save();
	    },
	    {L"Uses smaller blocks of GPU memory and makes some buffers only when they are needed.",
	     L"Saved about 300 MB in a test, with no visible difference.", L"Off can crash games on the Xbox when memory runs out."});
	AddSwitch(
	    memory, explanation, L"Multi-threaded video decoding", settings.video_decode_threads,
	    [state, save](bool on) {
		    state->settings.video_decode_threads = on;
		    save();
	    },
	    {L"Decodes the videos games play through the system video player on all CPU cores.",
	     L"Off decodes them on one thread. Use it to test video problems."});

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

	const auto developer = panel(L"DeveloperTiles");
	AddSwitch(
	    developer, explanation, L"Graphics debug layer", settings.debug_layer,
	    [state, save](bool on) {
		    state->settings.debug_layer = on;
		    save();
	    },
	    {L"Reports graphics errors in the log.", L"Makes games slower.",
	     IsXbox() ? L"Where the console doesn't have it, the log says debug layer off."
	              : L"It needs the Windows feature Graphics Tools. Without it, the log says debug layer off."});
	AddSwitch(
	    developer, explanation, L"Detailed log", settings.game_output,
	    [state, save](bool on) {
		    state->settings.game_output = on;
		    save();
	    },
	    {L"Adds the game's printf output and the emulator's trace to the log. It also shows warnings that are otherwise hidden.",
	     L"Makes games slower. The log can grow by thousands of lines a second."});
	AddSwitch(
	    developer, explanation, L"Detailed memory report", settings.gpu_memory_log,
	    [state, save](bool on) {
		    state->settings.gpu_memory_log = on;
		    save();
	    },
	    {L"Writes the GPU memory to the log every 10 seconds. While the swap file is in use, also the swap file and the game's memory.",
	     L"Each report causes a short pause."});
	AddSwitch(
	    developer, explanation, L"GPU crash diagnostics", settings.gpu_crash_diagnostics,
	    [state, save](bool on) {
		    state->settings.gpu_crash_diagnostics = on;
		    save();
	    },
	    {L"When the GPU hangs or crashes, the log names the shaders it was running and saves them.", L"Makes games slower."});
	AddSwitch(
	    developer, explanation, L"Keep compiled shaders across app builds", settings.shader_cache_any_build,
	    [state, save](bool on) {
		    state->settings.shader_cache_any_build = on;
		    save();
	    },
	    {L"Games start without compiling their shaders again after an app update, unless the update brings a new shader translator.",
	     L"If the update changed how shaders are translated, games can crash or show wrong images.",
	     L"In that case, turn this off or remove the compiled shaders. The game compiles its shaders again on its next start.",
	     L"Turning this on or off also makes each game compile its shaders once more."});
	// The games' compiled shaders (LocalState\_PipelineCache, a file per game): each game compiles them afresh on its next start.
	auto remove_shaders_status = page.FindName(L"RemoveShadersStatus").as<xaml::Controls::TextBlock>();
	auto remove_shaders        = page.FindName(L"RemoveShaders").as<xaml::Controls::Button>();
	Explain(remove_shaders, explanation, {L"Deletes the compiled shaders of all games.", L"Each game compiles them again on its next start."});
	remove_shaders.Click([remove_shaders_status](auto const&, auto const&) {
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

	// The chosen category's settings, in the categories' order (the markup's panels).
	auto categories = page.FindName(L"Categories").as<xaml::Controls::ListView>();
	auto panels     = page.FindName(L"Panels").as<xaml::Controls::ScrollViewer>();
	std::vector<xaml::UIElement> category_panels;
	for (const auto* name: {L"Display", L"User", L"Controller", L"GameFolders", L"UserData", L"Memory", L"Developer"}) {
		category_panels.push_back(page.FindName(name).as<xaml::UIElement>());
	}
	// The explanation while a category is chosen and before a setting of it has the focus.
	const std::vector<Paragraphs> category_explanations = {
	    {},
	    {},
	    {},
	    {L"KytyPS5 looks for games in its own Games folder and in the folders listed here, two folder levels deep.",
	     L"A game is a folder with eboot.bin, or a .zar archive with eboot.bin at its root.",
	     L"Each folder must allow apps to read it. If one does not, the library shows how to allow it."},
	    {L"The folder for saves, patches and trophies, for example on a USB drive.",
	     L"KytyPS5 keeps them in the subfolders _SaveData, _Patches and _Trophies.",
	     L"Saves, patches and trophies in the old folder are not moved. To keep them, copy these subfolders to the new folder yourself.",
	     L"If the folder can't be written when a game starts, for example when the drive is not plugged in, that game uses the app's folder."},
	    {L"On the Xbox, turning a memory option off can crash games that need the memory."},
	    {},
	};
	const auto explain_category = [categories, explanation, category_explanations] {
		if (const auto index = categories.SelectedIndex(); index >= 0 && static_cast<size_t>(index) < category_explanations.size()) {
			ShowExplanation(explanation, category_explanations[static_cast<size_t>(index)]);
		}
	};
	categories.SelectionChanged([categories, panels, category_panels, explain_category](auto const&, auto const&) {
		const auto index = categories.SelectedIndex();
		if (index < 0) {
			return;
		}
		for (size_t panel_index = 0; panel_index < category_panels.size(); panel_index++) {
			category_panels[panel_index].Visibility(panel_index == static_cast<size_t>(index) ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
		}
		panels.ChangeView(nullptr, 0.0, nullptr, true);
		explain_category();
	});
	categories.SelectedIndex(0);
	// Whether the focus is in the settings (B goes back to the categories) or in the categories (B leaves the page).
	auto in_panel = std::make_shared<bool>(false);
	panels.GotFocus([in_panel](auto const&, auto const&) { *in_panel = true; });
	categories.GotFocus([in_panel, explain_category](auto const&, auto const&) {
		*in_panel = false;
		explain_category();
	});

	// Back: B, Escape or the window's back button (shown while the page is open).
	const auto leave = [state, in_panel, categories] {
		if (*in_panel) {
			categories.Focus(xaml::FocusState::Keyboard);
			return;
		}
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
	page.Loaded([categories](auto const&, auto const&) { categories.Focus(xaml::FocusState::Keyboard); });
	page.Unloaded([navigation, back_token](auto const&, auto const&) {
		navigation.BackRequested(back_token);
		navigation.AppViewBackButtonVisibility(
		    winrt::Windows::UI::Core::AppViewBackButtonVisibility::Collapsed);
	});
	return page;
}

} // namespace Kyty::Uwp
