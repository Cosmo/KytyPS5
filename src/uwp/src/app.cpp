#include "app.h"

#include "cheatsPage.h"
#include "emulatorHost.h"
#include "fileAccess.h"
#include "gameImages.h"
#include "gameLibrary.h"
#include "gameSource.h"
#include "input.h"
#include "keyboard.h"
#include "launcher.h"
#include "log.h"
#include "graphics/host_gpu/d3d12/presentationStubs.h"
#include "graphics/host_gpu/frameDump.h"
#include "overlay.h"
#include "settingsPage.h"
#include "trophiesPage.h"
#include "trophyToast.h"
#include "controlsPage.h"
#include "settings.h"
#include "xboxLook.h"

#include <windows.h>
#include <fileapifromapp.h>

#include <nlohmann/json.hpp>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Kyty::Uwp {

namespace xaml       = winrt::Windows::UI::Xaml;
namespace activation = winrt::Windows::ApplicationModel::Activation;

namespace {

// The game's page: the panel the game presents to; in front of it the launch screen until the game is ready to be seen, and the game menu. The launch
// screen is the game's start screen as the PS5 shows it (its pic1 and pic2), or, when the game has none, a card with its background, name and progress.
constexpr wchar_t GameMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Background="Black">
  <SwapChainPanel x:Name="Panel" HorizontalAlignment="Center" VerticalAlignment="Center" />
  <Grid x:Name="Launch" Background="Black" IsHitTestVisible="False">
    <Grid.OpacityTransition><ScalarTransition Duration="0:0:0.3" /></Grid.OpacityTransition>
    <Grid x:Name="Splash" Visibility="Collapsed">
      <Image x:Name="SplashBackground" Stretch="Uniform" />
      <Image x:Name="SplashLogo" Stretch="Uniform" Opacity="0">
        <Image.OpacityTransition><ScalarTransition Duration="0:0:2.2" /></Image.OpacityTransition>
      </Image>
    </Grid>
    <Grid x:Name="LaunchCard" Background="#0B0E14" Visibility="Collapsed">
      <Image x:Name="Backdrop" Stretch="UniformToFill" HorizontalAlignment="Center" VerticalAlignment="Center" Opacity="0">
        <Image.OpacityTransition><ScalarTransition Duration="0:0:0.4" /></Image.OpacityTransition>
      </Image>
      <Rectangle>
        <Rectangle.Fill>
          <LinearGradientBrush StartPoint="0,0" EndPoint="0,1">
            <GradientStop Color="#800B0E14" Offset="0" />
            <GradientStop Color="#990B0E14" Offset="0.45" />
            <GradientStop Color="#F00B0E14" Offset="1" />
          </LinearGradientBrush>
        </Rectangle.Fill>
      </Rectangle>
      <StackPanel VerticalAlignment="Bottom" Margin="48,0,48,48" Spacing="12">
        <TextBlock x:Name="Title" Style="{StaticResource TitleTextBlockStyle}" TextTrimming="CharacterEllipsis"
                   TextWrapping="NoWrap" />
        <StackPanel Orientation="Horizontal" Spacing="12">
          <ProgressRing IsActive="True" Width="16" Height="16" />
          <TextBlock Text="Starting" Style="{StaticResource BodyTextBlockStyle}" VerticalAlignment="Center"
                     Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        </StackPanel>
      </StackPanel>
    </Grid>
  </Grid>

  <!-- The game menu (View + Menu, or Escape), in the system's look: the game keeps running behind
       it. -->
  <Grid x:Name="Menu" Visibility="Collapsed">
    <Rectangle x:Name="MenuDim" Fill="{ThemeResource SystemControlPageBackgroundMediumAltMediumBrush}" />
    <!-- On the left; Settings on the right and without the dimming, so that the overlay at the top left shows each change. -->
    <Grid x:Name="MenuPanel" Width="320" HorizontalAlignment="Left" Padding="24,32,24,32"
          Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="*" />
        <RowDefinition Height="Auto" />
      </Grid.RowDefinitions>
      <TextBlock x:Name="MenuCaption" Text="Game menu" Style="{StaticResource CaptionTextBlockStyle}"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <TextBlock x:Name="MenuTitle" Grid.Row="1" Style="{StaticResource SubtitleTextBlockStyle}"
                 MaxLines="3" TextTrimming="CharacterEllipsis" Margin="0,4,0,24" />
      <!-- Game menu › Settings: tiles as on the settings page, and under them the explanation of the one with the focus. -->
      <Grid x:Name="MenuSettings" Grid.Row="2" RowSpacing="16" Margin="0,0,0,16" Visibility="Collapsed">
        <Grid.RowDefinitions>
          <RowDefinition Height="*" />
          <RowDefinition Height="Auto" />
        </Grid.RowDefinitions>
        <ScrollViewer VerticalScrollBarVisibility="Hidden">
          <StackPanel x:Name="MenuTiles" Spacing="8" />
        </ScrollViewer>
        <StackPanel x:Name="MenuExplanation" Grid.Row="1" Spacing="12" />
      </Grid>
      <!-- Game menu › Trophies: the running game's progress and trophies (ShowGameTrophies). -->
      <Grid x:Name="MenuTrophies" Grid.Row="2" Margin="0,0,0,16" Visibility="Collapsed" />
      <StackPanel x:Name="MenuList" Grid.Row="2">
        <ListView x:Name="MenuItems" SelectionMode="None" IsItemClickEnabled="True" Margin="-12,0">
          <ListView.ItemContainerStyle>
            <Style TargetType="ListViewItem">
              <Setter Property="CornerRadius" Value="0" />
            </Style>
          </ListView.ItemContainerStyle>
        </ListView>
        <TextBlock x:Name="MenuStatus" Style="{StaticResource CaptionTextBlockStyle}" Margin="0,12,0,0"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" Visibility="Collapsed"
                   Text="Saving, then back to the library..." />
      </StackPanel>
      <Border x:Name="MenuHints" Grid.Row="3" Style="{StaticResource HintBarStyle}" />
    </Grid>
  </Grid>
</Grid>)xaml";

// Shown instead of the game when it can't start.
constexpr wchar_t ProblemMarkup[] = LR"(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      xmlns:muxc="using:Microsoft.UI.Xaml.Controls" Padding="48">
  <StackPanel MaxWidth="640" VerticalAlignment="Center" Spacing="16">
    <TextBlock Text="The game can't start" Style="{StaticResource TitleTextBlockStyle}" />
    <TextBlock x:Name="Game" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap"
               Opacity="0.7" />
    <muxc:InfoBar x:Name="Problem" IsOpen="True" IsClosable="False" Severity="Error" />
    <TextBox x:Name="Command" IsReadOnly="True" FontFamily="Consolas" TextWrapping="Wrap" />
    <StackPanel Orientation="Horizontal" Spacing="8">
      <Button x:Name="CopyCommand" Content="Copy command"
              Style="{StaticResource AccentButtonStyle}" />
      <Button x:Name="Retry" Content="Retry" />
    </StackPanel>
  </StackPanel>
</Grid>)";

// A kyty://run activation: a game folder (?game=<folder>) or a title ID (?title=<ID>, looked
// up in the game library).
struct RunRequest {
	std::filesystem::path folder;
	std::string           title_id;
	// How many times the app restarted for this game (retry=<n>, see EmulatorHost::RestartForAddressCollision).
	uint32_t retry = 0;
};

// The arguments as a kyty:// link to `host`, or nothing. The scheme is KYTY_UWP_PROTOCOL of the build ("kyty", or another name for a build
// installed next to it).
std::optional<winrt::Windows::Foundation::Uri> KytyLink(winrt::hstring const& arguments, std::wstring_view host) {
	if (arguments.empty()) {
		return std::nullopt;
	}
	try {
		winrt::Windows::Foundation::Uri uri(arguments);
		if (std::wstring_view(uri.SchemeName()).substr(0, 4) != L"kyty" || uri.Host() != host) {
			return std::nullopt;
		}
		return uri;
	} catch (const winrt::hresult_error&) {
	}
	return std::nullopt;
}

// A trophy notification's link: kyty://trophy?title=<title ID>&name=<the trophy's name>. It comes back as the notification's activation
// argument, so the scheme is never resolved and any build's name works.
struct TrophyLink {
	std::string title_id;
	std::string name;
};

winrt::hstring TrophyLinkArguments(const std::string& title_id, const std::string& name) {
	using winrt::Windows::Foundation::Uri;
	return L"kyty://trophy?title=" + Uri::EscapeComponent(winrt::to_hstring(title_id)) + L"&name=" + Uri::EscapeComponent(winrt::to_hstring(name));
}

std::optional<TrophyLink> TrophyLinkFromArguments(winrt::hstring const& arguments) {
	const auto uri = KytyLink(arguments, L"trophy");
	if (!uri) {
		return std::nullopt;
	}
	try {
		TrophyLink link;
		for (const auto& entry: uri->QueryParsed()) {
			if (entry.Name() == L"title") {
				link.title_id = winrt::to_string(entry.Value());
			} else if (entry.Name() == L"name") {
				link.name = winrt::to_string(entry.Value());
			}
		}
		if (!link.title_id.empty()) {
			return link;
		}
	} catch (const winrt::hresult_error&) {
	}
	return std::nullopt;
}

// While a game runs: shows a trophy's details over it (SetUpGameMenu).
std::function<void(const TrophyLink&)> g_trophy_over_game;

std::optional<RunRequest> RunRequestFromArguments(winrt::hstring const& arguments) {
	const auto uri = KytyLink(arguments, L"run");
	if (!uri) {
		return std::nullopt;
	}
	try {
		RunRequest request;
		bool       found = false;
		for (const auto& entry: uri->QueryParsed()) {
			if (entry.Name() == L"game") {
				request.folder = std::filesystem::path(std::wstring(entry.Value()));
				found          = true;
			} else if (entry.Name() == L"title") {
				request.title_id = winrt::to_string(entry.Value());
				found            = true;
			} else if (entry.Name() == L"retry") {
				request.retry = static_cast<uint32_t>(std::wcstoul(std::wstring(entry.Value()).c_str(), nullptr, 10));
			}
		}
		if (found) {
			return request;
		}
	} catch (const winrt::hresult_error&) {
	}
	return std::nullopt;
}

struct GameProblem {
	winrt::hstring title;
	winrt::hstring message;
	// The command that fixes it, if one does.
	winrt::hstring command;
};

// Why the game in `game` can't start, if it can't. Game folders have to grant UWP apps read
// access ("ALL APPLICATION PACKAGES", see docs/uwp.md); files are then read directly, as fast
// as on the desktop.
std::optional<GameProblem> CheckGame(const std::filesystem::path& game) {
	const auto                eboot = game / L"eboot.bin";
	WIN32_FILE_ATTRIBUTE_DATA attributes {};
	if (IsArchivePath(game)) {
		// the archive itself must be readable and hold the game
		if (GetFileAttributesExFromAppW(game.c_str(), GetFileExInfoStandard, &attributes) == 0) {
			Log("cannot read %ls (error %lu)\n", game.c_str(), GetLastError());
			return GameProblem {L"The archive can't be read", winrt::hstring(game.wstring())};
		}
		if (GameFileExists(eboot)) {
			return std::nullopt;
		}
		return GameProblem {L"No game in this archive", L"The archive has no eboot.bin at its root."};
	}
	if (GetFileAttributesExFromAppW(eboot.c_str(), GetFileExInfoStandard, &attributes)) {
		return std::nullopt;
	}
	const auto error = GetLastError();
	Log("cannot read %ls (error %lu)\n", eboot.c_str(), error);
	if (error == ERROR_ACCESS_DENIED) {
		const auto games = game.has_parent_path() ? game.parent_path() : game;
		return GameProblem {
		    L"KytyPS5 may not read this folder",
		    IsXbox() ? L"Apps need read access to your games folder, which a PC gives: plug the "
		               L"drive (NTFS) into a PC, open the folder's Properties > Security > Edit > "
		               L"Add, enter ALL APPLICATION PACKAGES and keep Read & execute, or run this "
		               L"command there, with the drive's letter for X:. Then plug it back in and retry."
		             : L"Give apps read access to your games folder, then retry: in Explorer, open "
		               L"the folder's Properties > Security > Edit > Add, enter ALL APPLICATION "
		               L"PACKAGES and keep Read & execute. Or run this command:",
		    winrt::hstring(GrantCommand(games))};
	}
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
		return GameProblem {L"No game in this folder", L"The folder has no eboot.bin."};
	}
	return GameProblem {L"The game can't be read",
	                    winrt::hstring(L"Error code ") +
	                        winrt::to_hstring(static_cast<uint32_t>(error))};
}

xaml::UIElement CreateProblemPage(winrt::hstring const& subject, GameProblem const& problem,
                                  std::function<xaml::UIElement()> retry) {
	auto page = xaml::Markup::XamlReader::Load(ProblemMarkup).as<xaml::FrameworkElement>();
	page.FindName(L"Game").as<xaml::Controls::TextBlock>().Text(subject);
	auto info = page.FindName(L"Problem").as<winrt::Microsoft::UI::Xaml::Controls::InfoBar>();
	info.Title(problem.title);
	info.Message(problem.message);
	auto       command     = page.FindName(L"Command").as<xaml::Controls::TextBox>();
	auto       copy        = page.FindName(L"CopyCommand").as<xaml::Controls::Button>();
	const auto has_command = problem.command.empty() ? xaml::Visibility::Collapsed
	                                                 : xaml::Visibility::Visible;
	command.Text(problem.command);
	command.Visibility(has_command);
	// On the Xbox the command is for a PC, which the console's clipboard doesn't reach.
	copy.Visibility(IsXbox() ? xaml::Visibility::Collapsed : has_command);
	copy.Click([text = problem.command](auto const&, auto const&) {
		winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
		package.SetText(text);
		winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
	});
	page.FindName(L"Retry").as<xaml::Controls::Button>().Click(
	    [retry = std::move(retry)](auto const&, auto const&) {
		    xaml::Window::Current().Content(retry());
	    });
	return page;
}

winrt::fire_and_forget ShowLaunchBackdrop(xaml::Controls::Image image, std::filesystem::path path) {
	if (auto bitmap = co_await LoadImageAsync(image.Dispatcher(), path, 1920)) {
		image.Source(bitmap);
		image.Opacity(1.0);
	}
}

// Whether the game asks for its start screen's logo at once (param.json's attribute3 bit 0x1000, as upstream's splash screen reads it).
bool StartsFast(const std::filesystem::path& game) {
	const auto text = ReadGameText(game / L"sce_sys" / L"param.json");
	if (!text) {
		return false;
	}
	const auto json = nlohmann::json::parse(*text, nullptr, false);
	return json.is_object() && json.contains("attribute3") && json["attribute3"].is_number_integer() &&
	       (json["attribute3"].get<int64_t>() & 0x1000) != 0;
}

// The launch screen: the game's start screen as the PS5 shows it while the game starts (its pic1, with pic2's logo fading in over it after 1.4 s,
// at once for a game that starts fast; upstream's splash screen does the same), or the card with its background, name and progress when it has none.
// `own` becomes true with the game's start screen.
winrt::fire_and_forget ShowStartScreen(xaml::Controls::Grid page, std::filesystem::path game, std::shared_ptr<bool> own) {
	const auto dispatcher = page.Dispatcher();
	co_await winrt::resume_background();
	const auto paths = SplashImages(game, LoadSettings().console_language);
	const bool fast  = StartsFast(game);
	// Back on the UI thread before awaiting: an awaited operation resumes on the thread it was awaited from, and the XAML below needs the UI thread.
	co_await winrt::resume_foreground(dispatcher);
	auto background = co_await LoadDdsImageAsync(dispatcher, paths[0]);
	auto logo       = background != nullptr ? co_await LoadDdsImageAsync(dispatcher, paths[1]) : nullptr;
	if (background == nullptr || logo == nullptr) {
		page.FindName(L"LaunchCard").as<xaml::UIElement>().Visibility(xaml::Visibility::Visible);
		ShowLaunchBackdrop(page.FindName(L"Backdrop").as<xaml::Controls::Image>(), BackdropImage(game));
		co_return;
	}
	*own = true;
	page.FindName(L"SplashBackground").as<xaml::Controls::Image>().Source(background);
	auto logo_image = page.FindName(L"SplashLogo").as<xaml::Controls::Image>();
	logo_image.Source(logo);
	page.FindName(L"Splash").as<xaml::UIElement>().Visibility(xaml::Visibility::Visible);
	if (fast) {
		logo_image.OpacityTransition(nullptr);
	} else {
		co_await winrt::resume_after(std::chrono::milliseconds(1400));
		co_await winrt::resume_foreground(dispatcher);
	}
	logo_image.Opacity(1.0);
}

// In Segoe MDL2 Assets.
constexpr wchar_t CHEVRON_RIGHT[] = {0xE76C, 0};

// An entry of the game menu; `more` adds the chevron of an entry that opens a submenu.
xaml::Controls::ListViewItem MenuEntry(const wchar_t* text, bool more = false) {
	xaml::Controls::ListViewItem entry;
	entry.HorizontalContentAlignment(xaml::HorizontalAlignment::Stretch);
	xaml::Controls::Grid content;
	content.ColumnDefinitions().Append(xaml::Controls::ColumnDefinition());
	xaml::Controls::ColumnDefinition chevron_column;
	chevron_column.Width(xaml::GridLengthHelper::Auto());
	content.ColumnDefinitions().Append(chevron_column);
	xaml::Controls::TextBlock label;
	label.Text(text);
	content.Children().Append(label);
	if (more) {
		xaml::Controls::FontIcon chevron;
		chevron.FontFamily(xaml::Media::FontFamily(L"Segoe MDL2 Assets"));
		chevron.Glyph(CHEVRON_RIGHT);
		chevron.FontSize(12);
		chevron.VerticalAlignment(xaml::VerticalAlignment::Center);
		xaml::Controls::Grid::SetColumn(chevron, 1);
		content.Children().Append(chevron);
	}
	entry.Content(content);
	return entry;
}

void FocusControl(xaml::DependencyObject const& target) {
	if (auto control = target.try_as<xaml::Controls::Control>()) {
		control.Focus(xaml::FocusState::Keyboard);
	}
}

// The game menu's views: its entries, Game menu › Settings and Game menu › Trophies.
enum class MenuView { Entries, Settings, Trophies };

// Sets up the game menu and the overlay of the page of `game`.
void SetUpGameMenu(xaml::Controls::Grid const& page, GameEntry const& game, winrt::hstring const& title) {
	page.FindName(L"MenuTitle").as<xaml::Controls::TextBlock>().Text(title);
	auto menu          = page.FindName(L"Menu").as<xaml::UIElement>();
	auto dim           = page.FindName(L"MenuDim").as<xaml::UIElement>();
	auto panel         = page.FindName(L"MenuPanel").as<xaml::FrameworkElement>();
	auto caption       = page.FindName(L"MenuCaption").as<xaml::Controls::TextBlock>();
	auto list          = page.FindName(L"MenuList").as<xaml::UIElement>();
	auto items         = page.FindName(L"MenuItems").as<xaml::Controls::ListView>();
	auto status        = page.FindName(L"MenuStatus").as<xaml::UIElement>();
	auto settings_view = page.FindName(L"MenuSettings").as<xaml::UIElement>();
	auto tiles         = page.FindName(L"MenuTiles").as<xaml::Controls::StackPanel>();
	auto explanation   = page.FindName(L"MenuExplanation").as<xaml::Controls::StackPanel>();
	auto trophies_view = page.FindName(L"MenuTrophies").as<xaml::Controls::Grid>();
	SetHints(page.FindName(L"MenuHints").as<xaml::Controls::Border>(), {{XboxButton::A, L"Select"}, {XboxButton::B, L"Back"}});

	// The overlay goes under the menu, over the launch screen: its log shows the start too.
	uint32_t menu_index = 0;
	page.Children().IndexOf(menu, menu_index);
	auto overlay = std::make_shared<GameOverlay>(page, menu_index, title);
	overlay->Show(OverlayShown());
	const auto toggle_overlay = [overlay] {
		const bool shown = !overlay->Shown();
		overlay->Show(shown);
		SetOverlayShown(shown);
	};

	const auto resume   = MenuEntry(L"Resume");
	const auto settings = MenuEntry(L"Settings", true);
	const auto trophies = MenuEntry(L"Trophies", true);
	const auto dump     = MenuEntry(L"Save frame dump");
	const auto quit     = MenuEntry(L"Quit to library");
	for (const auto& entry: {resume, settings, trophies, dump, quit}) {
		items.Items().Append(entry);
	}

	// Settings show on the right and without the dimming, so that the overlay at the top left shows each change.
	const auto set_view = [dim, panel, caption, list, settings_view, trophies_view](MenuView view) {
		const auto shown_if = [](bool shown) { return shown ? xaml::Visibility::Visible : xaml::Visibility::Collapsed; };
		dim.Visibility(shown_if(view != MenuView::Settings));
		panel.HorizontalAlignment(view == MenuView::Settings ? xaml::HorizontalAlignment::Right : xaml::HorizontalAlignment::Left);
		list.Visibility(shown_if(view == MenuView::Entries));
		settings_view.Visibility(shown_if(view == MenuView::Settings));
		trophies_view.Visibility(shown_if(view == MenuView::Trophies));
		const std::wstring prefix = std::wstring(L"Game menu ") + wchar_t {0x203A};
		caption.Text(view == MenuView::Settings ? prefix + L" Settings" : view == MenuView::Trophies ? prefix + L" Trophies" : std::wstring(L"Game menu"));
	};
	const auto in_subview = [settings_view, trophies_view] {
		return settings_view.Visibility() == xaml::Visibility::Visible || trophies_view.Visibility() == xaml::Visibility::Visible;
	};
	// The overlay's tiles, made anew each time so that they show the current values; a change shows at once.
	const auto open_settings = [set_view, tiles, explanation, overlay] {
		set_view(MenuView::Settings);
		tiles.Children().Clear();
		explanation.Children().Clear();
		auto values = std::make_shared<AppSettings>(LoadSettings());
		AddOverlaySettings(tiles, explanation, values, [values, overlay] {
			SaveSettings(*values);
			overlay->Apply(*values);
			overlay->Show(OverlayShown());
		});
		tiles.UpdateLayout();
		FocusControl(tiles.Children().GetAt(0));
	};
	// The running game's trophies, read anew each time so that the ones earned in this session show.
	const auto open_trophies = [set_view, trophies_view, game] {
		set_view(MenuView::Trophies);
		ShowGameTrophies(trophies_view, game);
	};
	// Back to the entries, with the focus on the one the view was opened with.
	const auto close_subview = [set_view, items, trophies_view] {
		const int32_t from = trophies_view.Visibility() == xaml::Visibility::Visible ? 2 : 1;
		set_view(MenuView::Entries);
		items.UpdateLayout();
		FocusControl(items.ContainerFromIndex(from));
	};

	const auto show = [menu, items, set_view](bool open) {
		menu.Visibility(open ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
		PauseGameInput(open || GameKeyboardShown());
		if (open) {
			set_view(MenuView::Entries);
			menu.UpdateLayout();
			FocusControl(items.ContainerFromIndex(0));
		}
	};
	const auto is_open = [menu] { return menu.Visibility() == xaml::Visibility::Visible; };

	// A game asking for text: the box for it goes over the game and the overlay, under the menu; the game's input is paused while it shows.
	page.Children().IndexOf(menu, menu_index);
	SetUpGameKeyboard(page, menu_index, [is_open](bool shown) { PauseGameInput(shown || is_open()); });

	// A trophy notification selected during the game: the trophy's details over it, the game's input paused while they show (and after them
	// while the menu is open).
	g_trophy_over_game = [is_open](const TrophyLink& link) {
		PauseGameInput(true);
		ShowTrophyDetails(link.title_id, link.name, [is_open] {
			if (!is_open() && !GameKeyboardShown()) {
				PauseGameInput(false);
			}
		});
	};

	items.ItemClick([show, open_settings, open_trophies, status, items, resume, settings, trophies, dump,
	                 quit](auto const&, xaml::Controls::ItemClickEventArgs const& args) {
		// The entry, or its content: an entry is its own container, and the list may report either.
		const auto clicked = args.ClickedItem();
		const auto is      = [&clicked](xaml::Controls::ListViewItem const& entry) { return clicked == entry || clicked == entry.Content(); };
		if (is(resume)) {
			show(false);
			return;
		}
		if (is(settings)) {
			open_settings();
			return;
		}
		if (is(trophies)) {
			open_trophies();
			return;
		}
		if (is(dump)) {
			// The renderer takes the request at the next flip and writes the render targets as PNG files: the line under the menu follows it.
			Libs::Graphics::FrameDump::Request();
			auto text = status.try_as<xaml::Controls::TextBlock>();
			if (text == nullptr) {
				return;
			}
			text.Text(winrt::to_hstring(Libs::Graphics::FrameDump::GetStatus()));
			status.Visibility(xaml::Visibility::Visible);
			xaml::DispatcherTimer timer;
			timer.Interval(std::chrono::milliseconds(500));
			timer.Tick([timer, text](auto const&, auto const&) {
				const auto line = Libs::Graphics::FrameDump::GetStatus();
				text.Text(winrt::to_hstring(line));
				if (line.rfind("Frame dump", 0) == 0) {
					timer.Stop();
				}
			});
			timer.Start();
			return;
		}
		if (!is(quit)) {
			return;
		}
		items.IsEnabled(false);
		status.Visibility(xaml::Visibility::Visible);
		EmulatorHost::Get().QuitToLibrary();
	});

	// B (or Escape) goes back from Settings or Trophies to the entries and closes the menu from there; View + Menu (or Escape) opens it.
	// View + Menu with both triggers pulled (or F3) shows or hides the overlay.
	menu.KeyDown([show, in_subview, close_subview](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadB ||
		    args.Key() == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			if (in_subview()) {
				close_subview();
			} else {
				show(false);
			}
		}
	});
	winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().KeyDown(
	    [show, is_open, toggle_overlay](auto const&, winrt::Windows::UI::Core::KeyEventArgs const& args) {
		    if (args.VirtualKey() == winrt::Windows::System::VirtualKey::Escape && !is_open() && !GameKeyboardShown()) {
			    args.Handled(true);
			    show(true);
		    } else if (args.VirtualKey() == winrt::Windows::System::VirtualKey::F3) {
			    args.Handled(true);
			    toggle_overlay();
		    }
	    });
	const auto dispatcher = page.Dispatcher();
	OnChords(
	    [dispatcher, show, is_open] {
		    (void)dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
		                              [show, is_open] { show(!is_open()); });
	    },
	    [dispatcher, toggle_overlay] {
		    (void)dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
		                              [toggle_overlay] { toggle_overlay(); });
	    });
}

// The emulator's page: a panel the emulator presents to, filling the window, under the game's
// launch screen until the game runs, and the game menu.
xaml::UIElement CreateEmulatorPage(std::filesystem::path game) {
	auto page = xaml::Markup::XamlReader::Load(GameMarkup).as<xaml::Controls::Grid>();
	const auto entry = ReadGame(game);
	const auto title = winrt::to_hstring(entry.name);
	SetUpGameMenu(page, entry, title);
	// Trophies the game unlocks show as system notifications, made on the UI thread from a copy of the name; selecting one opens the trophy.
	Libs::Graphics::SetTrophyUnlockHandler(
	    [dispatcher = page.Dispatcher(), title_id = entry.title_id](std::string_view name, int32_t /*grade*/, std::span<const std::byte> /*icon_png*/) {
		    (void)dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [title_id, name = std::string(name)] {
			    ShowTrophyToast(name, std::wstring(TrophyLinkArguments(title_id, name)));
		    });
	    });
	// The launch screen goes when the game says it is ready to be seen (sceSystemServiceHideSplashScreen), as on the PS5. A game that never says so
	// is shown once it presents steadily: at once over the card, 10 s later over its own start screen.
	const auto launch    = page.FindName(L"Launch").as<xaml::UIElement>();
	const auto own_start = std::make_shared<bool>(false);
	Libs::Graphics::SetSplashHiddenHandler([dispatcher = page.Dispatcher(), launch] {
		(void)dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [launch] {
			if (launch.Opacity() != 0.0) {
				Log("launch screen: the game is ready to be seen after %.1f s\n", EmulatorHost::Get().RunTimeMs() / 1000.0);
				launch.Opacity(0.0);
			}
		});
	});
	EmulatorHost::Get().OnGameVisible([launch, own_start] {
		if (!*own_start) {
			launch.Opacity(0.0);
			return;
		}
		xaml::DispatcherTimer timer;
		timer.Interval(std::chrono::seconds(10));
		timer.Tick([launch](winrt::Windows::Foundation::IInspectable const& sender, auto const&) {
			sender.as<xaml::DispatcherTimer>().Stop();
			if (launch.Opacity() != 0.0) {
				Log("launch screen: closed 10 s after the game presented steadily; it never said it was ready\n");
				launch.Opacity(0.0);
			}
		});
		timer.Start();
	});
	page.FindName(L"Title").as<xaml::Controls::TextBlock>().Text(title);
	ShowStartScreen(page, game, own_start);
	auto panel = page.FindName(L"Panel").as<xaml::Controls::SwapChainPanel>();
	// The game's picture is 16:9: the panel is the largest 16:9 rectangle that fits the window (black bars around it), so the picture is never stretched.
	page.SizeChanged([panel](auto const& sender, auto const&) {
		const auto   area   = sender.template as<xaml::FrameworkElement>();
		const double width  = area.ActualWidth();
		const double height = area.ActualHeight();
		if (width <= 0.0 || height <= 0.0) {
			return;
		}
		double panel_width  = width;
		double panel_height = width * 9.0 / 16.0;
		if (panel_height > height) {
			panel_height = height;
			panel_width  = height * 16.0 / 9.0;
		}
		panel.Width(panel_width);
		panel.Height(panel_height);
	});
	const auto                     report_size = [](xaml::Controls::SwapChainPanel const& panel) {
        EmulatorHost::Get().SetPanelSize(static_cast<float>(panel.ActualWidth()),
		                                                     static_cast<float>(panel.ActualHeight()),
		                                                     panel.CompositionScaleX(),
		                                                     panel.CompositionScaleY());
	};
	panel.SizeChanged([report_size](auto const& sender, auto const&) {
		report_size(sender.template as<xaml::Controls::SwapChainPanel>());
	});
	panel.CompositionScaleChanged(
	    [report_size](xaml::Controls::SwapChainPanel const& sender, auto const&) { report_size(sender); });
	// On Xbox, B navigates back; in a game it is Circle.
	winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
	    [](auto const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
		    args.Handled(true);
	    });
	panel.Loaded([report_size, game = std::move(game)](auto const& sender, auto const&) {
		auto loaded = sender.template as<xaml::Controls::SwapChainPanel>();
		report_size(loaded);
		EmulatorHost::Get().Start(loaded, game);
	});
	return page;
}

// The game's page, or why it can't start.
xaml::UIElement CreateGamePage(std::filesystem::path game) {
	if (const auto problem = CheckGame(game)) {
		return CreateProblemPage(game.wstring().c_str(), *problem,
		                         [game] { return CreateGamePage(game); });
	}
	return CreateEmulatorPage(std::move(game));
}

// The page for a game identified by its title ID.
xaml::UIElement CreateTitlePage(std::string title_id) {
	if (const auto game = FindGame(title_id)) {
		return CreateGamePage(game->folder);
	}
	std::wstring folders;
	for (const auto& folder: GameFolders()) {
		folders += L"\n" + folder.wstring();
	}
	return CreateProblemPage(winrt::to_hstring(title_id),
	                         {L"No game with this title ID",
	                          winrt::hstring(L"It isn't in the game folders:") + folders.c_str()},
	                         [title_id] { return CreateTitlePage(title_id); });
}

xaml::UIElement CreateLibraryPage(bool scan = false, const std::filesystem::path& select = {}) {
	LauncherActions actions;
	actions.play = [](const GameEntry& game) {
		SetLastPlayed(game.title_id);
		xaml::Window::Current().Content(CreateGamePage(game.folder));
	};
	actions.open_settings = [] {
		xaml::Window::Current().Content(CreateSettingsPage([](bool folders_changed) {
			xaml::Window::Current().Content(CreateLibraryPage(folders_changed));
		}));
	};
	actions.open_trophies = [] {
		xaml::Window::Current().Content(CreateTrophiesPage([] { xaml::Window::Current().Content(CreateLibraryPage()); }));
	};
	actions.open_controls = [] {
		xaml::Window::Current().Content(CreateControlsPage([] { xaml::Window::Current().Content(CreateLibraryPage()); }));
	};
	actions.open_cheats = [](const GameEntry& game) {
		// Back at the game the cheats are for, not the one played last.
		xaml::Window::Current().Content(
		    CreateCheatsPage(game, [folder = game.folder] { xaml::Window::Current().Content(CreateLibraryPage(false, folder)); }));
	};
	return CreateLauncherPage(std::move(actions), scan, select);
}

} // namespace

App::App() {
	RequestedTheme(xaml::ApplicationTheme::Dark);
	// On Xbox, controllers move the focus instead of a mouse cursor.
	RequiresPointerMode(xaml::ApplicationRequiresPointerMode::WhenRequested);
	UnhandledException([](auto const&, xaml::UnhandledExceptionEventArgs const& args) {
		Log("unhandled exception: %ls\n", args.Message().c_str());
	});
}

void App::OnLaunched(activation::LaunchActivatedEventArgs const& args) {
	Show(args.Arguments());
}

void App::OnActivated(activation::IActivatedEventArgs const& args) {
	if (args.Kind() == activation::ActivationKind::Protocol) {
		Show(args.as<activation::ProtocolActivatedEventArgs>().Uri().AbsoluteUri());
	} else if (args.Kind() == activation::ActivationKind::ToastNotification) {
		// A trophy notification selected: its link (TrophyLinkArguments).
		Show(args.as<activation::ToastNotificationActivatedEventArgs>().Argument());
	} else {
		Show(L"");
	}
}

// Arguments left for the next start in LocalState\launch.txt, used once: the Xbox's Device Portal
// starts apps without arguments, so `uwp.ps1 launch` uploads them there first.
static winrt::hstring PendingLaunchArguments() {
	const auto file =
	    std::filesystem::path(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) /
	    L"launch.txt";
	auto text = ReadTextFile(file);
	if (!text) {
		return {};
	}
	DeleteFileFromAppW(file.c_str());
	while (!text->empty() && std::isspace(static_cast<unsigned char>(text->back())) != 0) {
		text->pop_back();
	}
	Log("launch arguments from launch.txt\n");
	return winrt::to_hstring(*text);
}

void App::Show(winrt::hstring const& activation_arguments) {
	const auto arguments = activation_arguments.empty() ? PendingLaunchArguments() : activation_arguments;
	Log("activated, arguments: \"%ls\"\n", arguments.c_str());
	StartInput();
	{
		const auto settings = LoadSettings();
		SetControllerAssignment(settings.controller_auto, settings.controller_players, DsuServersInUse(settings));
	}
	if (!m_resources_loaded) {
		Resources().MergedDictionaries().Append(winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
		Resources().MergedDictionaries().Append(XboxLookResources());
		m_resources_loaded = true;
	}
	auto window = xaml::Window::Current();
	// A trophy notification's link opens the trophy: over the game while one runs, else on the trophies page (back to the library).
	const auto trophy_link    = TrophyLinkFromArguments(arguments);
	const auto open_trophies = [](const TrophyLink& link) {
		return CreateTrophiesPage([] { xaml::Window::Current().Content(CreateLibraryPage()); }, link.title_id, link.name);
	};
	if (window.Content() == nullptr) {
		WaitForGameFolders();
		const auto run_request = RunRequestFromArguments(arguments);
		if (run_request) {
			EmulatorHost::Get().SetRestartAttempts(run_request->retry);
		}
		if (trophy_link) {
			window.Content(open_trophies(*trophy_link));
		} else if (auto request = run_request; !request) {
			window.Content(CreateLibraryPage());
		} else if (!request->title_id.empty()) {
			window.Content(CreateTitlePage(request->title_id));
		} else {
			window.Content(CreateGamePage(request->folder));
		}
	} else if (trophy_link) {
		if (g_trophy_over_game) {
			g_trophy_over_game(*trophy_link);
		} else {
			window.Content(open_trophies(*trophy_link));
		}
	}
	// On a PC the window opens 16:9 (the games' picture is); a console is always full screen. The preferred size applies from the next start of the app,
	// the resize to this one.
	namespace view = winrt::Windows::UI::ViewManagement;
	view::ApplicationView::PreferredLaunchWindowingMode(view::ApplicationViewWindowingMode::PreferredLaunchViewSize);
	view::ApplicationView::PreferredLaunchViewSize(winrt::Windows::Foundation::Size {1280.0f, 720.0f});
	window.Activate();
	(void)view::ApplicationView::GetForCurrentView().TryResizeView(winrt::Windows::Foundation::Size {1280.0f, 720.0f});
}

xaml::Markup::IXamlType App::GetXamlType(xaml::Interop::TypeName const& type) {
	return m_winui_types.GetXamlType(type);
}

xaml::Markup::IXamlType App::GetXamlType(winrt::hstring const& full_name) {
	return m_winui_types.GetXamlType(full_name);
}

winrt::com_array<xaml::Markup::XmlnsDefinition> App::GetXmlnsDefinitions() {
	return m_winui_types.GetXmlnsDefinitions();
}

} // namespace Kyty::Uwp
