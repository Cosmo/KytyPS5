#include "cheatsPage.h"

#include "cheats.h"
#include "fileAccess.h"
#include "gameImages.h"
#include "log.h"
#include "settingTiles.h"
#include "settings.h"
#include "xboxLook.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// The executable the emulator runs (EmulatorHost: /app0/eboot.bin), which a cheat file names.
constexpr char PROCESS[] = "eboot.bin";

// The categories, in the list's order.
enum Category : int32_t { LOCAL = 0, REMOTE = 1, INFO = 2 };

// As the settings page: the header (with the game's cover, as the trophies page), the categories on the left in the Xbox's green, the chosen
// one's tiles, the explanation of the one with the focus, the hints. A remote file opens in a panel at the right, as a game's options do in
// the library.
constexpr wchar_t PageMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Background="{ThemeResource ApplicationPageBackgroundThemeBrush}">
  <Grid Padding="0,40,0,32">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto" />
      <RowDefinition Height="*" />
      <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>
    <Grid Margin="48,0,48,20">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="Auto" />
        <ColumnDefinition Width="*" />
      </Grid.ColumnDefinitions>
      <Border x:Name="HeaderCover" Width="48" Height="48" Margin="0,0,16,0" CornerRadius="4" Background="#1C2330" />
      <StackPanel Grid.Column="1" Spacing="2" VerticalAlignment="Center">
        <TextBlock Text="Cheats" Style="{StaticResource PageTitleTextBlockStyle}" />
        <TextBlock x:Name="GameName" Style="{StaticResource BodyTextBlockStyle}"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </StackPanel>
    </Grid>

    <Grid Grid.Row="1" ColumnSpacing="24">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="220" />
        <ColumnDefinition Width="*" />
      </Grid.ColumnDefinitions>
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
        <x:String>Local</x:String>
        <x:String>Remote</x:String>
        <x:String>Info</x:String>
      </ListView>

      <Grid Grid.Column="1" Margin="0,0,48,0">
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="*" />
        </Grid.ColumnDefinitions>
        <ScrollViewer x:Name="Panels" VerticalScrollBarVisibility="Hidden" Padding="0,0,0,16">
          <Grid>
            <!-- The cheats saved for the game, a check box each. -->
            <StackPanel x:Name="Local" Width="219" Spacing="8">
              <TextBlock x:Name="LocalCaption" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap"
                         Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
              <StackPanel x:Name="LocalTiles" Spacing="8" />
            </StackPanel>
            <!-- The collection's files for the game. -->
            <StackPanel x:Name="Remote" Width="219" Spacing="8" Visibility="Collapsed">
              <StackPanel Orientation="Horizontal" Spacing="8">
                <ProgressRing x:Name="RemoteLoading" Width="16" Height="16" VerticalAlignment="Top" />
                <TextBlock x:Name="RemoteCaption" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap" MaxWidth="195"
                           Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
              </StackPanel>
              <StackPanel x:Name="RemoteTiles" Spacing="8" />
            </StackPanel>
            <!-- Where the game's cheat file is, from a folder the player knows, and how to reach it from a PC. -->
            <StackPanel x:Name="Info" Width="474" Spacing="8" Visibility="Collapsed">
              <TextBlock Text="File location" Style="{StaticResource BodyTextBlockStyle}" />
              <TextBlock x:Name="InfoLocation" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap" Margin="0,0,0,8" />
              <StackPanel x:Name="InfoText" Spacing="8" />
            </StackPanel>
          </Grid>
        </ScrollViewer>
        <StackPanel x:Name="Explanation" Grid.Column="1" Margin="36,0,0,0" MaxWidth="230" HorizontalAlignment="Left" VerticalAlignment="Top"
                    Spacing="12" />
      </Grid>
    </Grid>
    <Border x:Name="Hints" Grid.Row="2" Style="{StaticResource HintBarStyle}" Margin="48,16,48,0" />
  </Grid>

  <!-- A remote file, in the system's look as a game's options in the library: what it is, its cheats, where it would be saved, Download. -->
  <Grid x:Name="FilePanel" Visibility="Collapsed">
    <Rectangle Fill="{ThemeResource SystemControlPageBackgroundMediumAltMediumBrush}" />
    <Grid Width="360" HorizontalAlignment="Right" Padding="24,32,24,32"
          Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="*" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
      </Grid.RowDefinitions>
      <TextBlock Text="Remote file" Style="{StaticResource CaptionTextBlockStyle}"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <TextBlock x:Name="FileTitle" Grid.Row="1" Style="{StaticResource SubtitleTextBlockStyle}"
                 MaxLines="3" TextTrimming="CharacterEllipsis" Margin="0,4,0,20" />
      <StackPanel x:Name="FileDetails" Grid.Row="2" Spacing="12" Margin="0,0,0,16" />
      <ListView x:Name="FileCheats" Grid.Row="3" Margin="-12,0" SelectionMode="None" IsItemClickEnabled="False">
        <ListView.ItemContainerStyle>
          <Style TargetType="ListViewItem">
            <Setter Property="CornerRadius" Value="0" />
            <Setter Property="MinHeight" Value="32" />
          </Style>
        </ListView.ItemContainerStyle>
      </ListView>
      <StackPanel Grid.Row="4" Orientation="Horizontal" Spacing="8" Margin="0,12,0,12">
        <ProgressRing x:Name="FileLoading" Width="16" Height="16" VerticalAlignment="Top" />
        <TextBlock x:Name="FileStatus" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap" MaxWidth="288" />
      </StackPanel>
      <ListView x:Name="FileActions" Grid.Row="5" Margin="-12,0,-12,16" SelectionMode="None" IsItemClickEnabled="True">
        <ListView.ItemContainerStyle>
          <Style TargetType="ListViewItem">
            <Setter Property="CornerRadius" Value="0" />
          </Style>
        </ListView.ItemContainerStyle>
      </ListView>
      <Border x:Name="FileHints" Grid.Row="6" Style="{StaticResource HintBarStyle}" />
    </Grid>
  </Grid>
</Grid>)xaml";

constexpr wchar_t ACTION_DOWNLOAD[] = L"Download";
constexpr wchar_t ACTION_CLOSE[]    = L"Close";

struct Page {
	GameEntry             game;
	std::function<void()> back;
	bool                  left        = false; // the page went
	bool                  dialog_open = false;
	bool                  in_panel    = false; // the focus is in the tiles (B goes to the categories), not in the categories

	xaml::Controls::Grid         root {nullptr};
	xaml::Controls::ListView     categories {nullptr};
	xaml::Controls::ScrollViewer panels {nullptr};
	xaml::UIElement              category_panels[3] {nullptr, nullptr, nullptr};
	xaml::Controls::StackPanel   explanation {nullptr};
	xaml::Controls::TextBlock    local_caption {nullptr};
	xaml::Controls::StackPanel   local_tiles {nullptr};
	xaml::Controls::ProgressRing remote_loading {nullptr};
	xaml::Controls::TextBlock    remote_caption {nullptr};
	xaml::Controls::StackPanel   remote_tiles {nullptr};
	xaml::Controls::TextBlock    info_location {nullptr};
	xaml::Controls::StackPanel   info_text {nullptr};
	// The remote file's panel.
	xaml::FrameworkElement       file_panel {nullptr};
	xaml::Controls::TextBlock    file_title {nullptr};
	xaml::Controls::StackPanel   file_details {nullptr};
	xaml::Controls::ListView     file_cheats {nullptr};
	xaml::Controls::ProgressRing file_loading {nullptr};
	xaml::Controls::TextBlock    file_status {nullptr};
	xaml::Controls::ListView     file_actions {nullptr};
	size_t                       file_index = 0; // the tile it opened from
	winrt::event_token           unloaded;

	// The file on this console: where it is (once known), what it held when it was read or saved (empty: there is none), and its parsed form.
	std::filesystem::path path;
	bool                  data_folder_set = false; // a data folder is set (the file is in LocalState when it can't be written)
	bool                  local_readable  = false;
	std::string           local_data;
	Cheats::Json          local_document;
	// The collection's files, and the one in the panel when it can be downloaded.
	std::vector<Cheats::RemoteFile> files;
	Cheats::Json                    remote_document;
	bool                            remote_valid = false;
	// Results that come back after a newer request are dropped.
	uint32_t local_generation   = 0;
	uint32_t catalog_generation = 0;
	uint32_t preview_generation = 0;

	[[nodiscard]] bool FileOpen() const { return file_panel.Visibility() == xaml::Visibility::Visible; }

	void Leave() {
		if (!left) {
			left = true;
			back();
		}
	}
};

std::wstring Wide(const std::string& text) {
	return std::wstring(winrt::to_hstring(text));
}

std::wstring Count(size_t count, const wchar_t* one, const wchar_t* many) {
	return std::to_wstring(count) + L" " + (count == 1 ? one : many);
}

xaml::Style AppStyle(const wchar_t* key) {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(key)).as<xaml::Style>();
}

xaml::Media::Brush MutedBrush() {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>();
}

// The separator of the app's menu paths (Settings › Storage).
constexpr wchar_t SEPARATOR[] = {L' ', 0x203A, L' ', 0};

bool InAppStorage(const std::filesystem::path& path) {
	return path.wstring().starts_with(LocalStateFolder().wstring());
}

// Where the file is, from a folder the player knows (the data folder, or the app's storage) instead of the folder's whole path.
std::wstring Location(const std::filesystem::path& path) {
	return std::wstring(InAppStorage(path) ? L"App storage" : L"Data folder") + SEPARATOR + L"_Patches" + SEPARATOR + path.filename().wstring();
}

// Info: where the file is, whether it exists, and how to reach it from a PC.
void UpdateInfo(const std::shared_ptr<Page>& page) {
	page->info_text.Children().Clear();
	if (page->path.empty()) {
		page->info_location.Text(L"");
		return;
	}
	page->info_location.Text(Location(page->path));
	const auto     storage = std::wstring(L"Settings") + SEPARATOR + L"Storage";
	Paragraphs     paragraphs;
	if (!page->local_readable) {
		paragraphs.push_back(L"The file can't be read.");
	} else if (page->local_data.empty()) {
		paragraphs.push_back(L"The file doesn't exist yet. Downloading a file under Remote creates it.");
	}
	if (!InAppStorage(page->path)) {
		paragraphs.push_back(L"The data folder is the one set under " + storage +
		                     (IsXbox() ? L". To copy or edit the file, connect the drive to a PC." : L". To copy or edit the file, open it in File Explorer."));
	} else if (page->data_folder_set) {
		paragraphs.push_back(L"The data folder set under " + storage + L" can't be written to, so the file is kept in the app's own storage.");
		paragraphs.push_back(IsXbox() ? L"Check that the drive is connected and that the app may write to the folder."
		                              : L"Check that the app may write to the folder.");
	} else {
		paragraphs.push_back(L"No data folder is set, so the file is kept in the app's own storage." +
		                     std::wstring(IsXbox() ? L" From a PC, open it in Device Portal under File explorer" + std::wstring(SEPARATOR) + L"LocalAppData" +
		                                                 SEPARATOR + L"KytyPS5" + SEPARATOR + L"LocalState."
		                                           : L" It's the app's LocalState folder, under %LOCALAPPDATA%\\Packages."));
		paragraphs.push_back(std::wstring(IsXbox() ? L"To keep cheats on a USB drive" : L"To keep cheats in a folder of your choice") +
		                     L", set a data folder under " + storage + L".");
	}
	for (const auto& paragraph: paragraphs) {
		xaml::Controls::TextBlock text;
		text.Style(AppStyle(L"BodyTextBlockStyle"));
		text.Foreground(MutedBrush());
		text.TextWrapping(xaml::TextWrapping::Wrap);
		text.Text(paragraph);
		page->info_text.Children().Append(text);
	}
}

// The explanation of the category with the focus.
Paragraphs CategoryExplanation(int32_t category) {
	switch (category) {
		case LOCAL: return {L"The cheats saved for this game. A switches one on or off.", L"Switched-on cheats apply when the game starts."};
		case REMOTE:
			return {L"Cheat files for this game from the HEN Cheats Collection.", L"A shows a file's cheats. Download saves them here, switched off."};
		default: return {}; // Info has the page's width
	}
}

void ShowCategory(const std::shared_ptr<Page>& page, int32_t category) {
	for (int32_t index = 0; index < 3; index++) {
		page->category_panels[index].Visibility(index == category ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
	}
	page->panels.ChangeView(nullptr, 0.0, nullptr, true);
	ShowExplanation(page->explanation, CategoryExplanation(category));
}

void FocusFirst(const xaml::Controls::Panel& tiles) {
	if (tiles.Children().Size() != 0) {
		tiles.Children().GetAt(0).as<xaml::Controls::Control>().Focus(xaml::FocusState::Keyboard);
	}
}

winrt::fire_and_forget ShowCover(xaml::Controls::Border tile, std::filesystem::path game) {
	auto bitmap = co_await LoadImageAsync(tile.Dispatcher(), CoverImage(game), 96);
	if (bitmap != nullptr) {
		xaml::Media::ImageBrush brush;
		brush.ImageSource(bitmap);
		brush.Stretch(xaml::Media::Stretch::UniformToFill);
		tile.Background(brush);
	}
}

winrt::fire_and_forget LoadLocal(std::shared_ptr<Page> page);

// Switches the cheat at `index` on or off and saves the file; when that fails, the tiles show the file as it is.
void Switch(const std::shared_ptr<Page>& page, size_t index, bool on) {
	auto cheats = Cheats::CheatsOf(page->local_document);
	if (index >= cheats.size() || page->path.empty()) {
		return;
	}
	std::vector<bool> enabled;
	for (const auto& cheat: cheats) {
		enabled.push_back(cheat.enabled);
	}
	enabled[index]      = on;
	const auto document = Cheats::WithSelection(page->local_document, enabled);
	if (const auto error = Cheats::Save(page->path, document, page->local_data); !error.empty()) {
		ShowExplanation(page->explanation, {error});
		LoadLocal(page);
		return;
	}
	page->local_document = document;
	page->local_data     = Cheats::Text(document);
}

// Takes what the file on this console holds now: `data` (empty when there is none).
void TakeLocal(const std::shared_ptr<Page>& page, std::string data) {
	page->local_data     = std::move(data);
	page->local_document = Cheats::Json();
	page->local_readable = true;
	page->local_tiles.Children().Clear();
	UpdateInfo(page);
	if (page->local_data.empty()) {
		page->local_caption.Text(L"No cheats for this game yet. Download a file under Remote.");
		return;
	}
	if (const auto error = Cheats::Parse(page->local_data, page->local_document); !error.empty()) {
		page->local_caption.Text(error);
		return;
	}
	const auto cheats = Cheats::CheatsOf(page->local_document);
	page->local_caption.Text(Count(cheats.size(), L"cheat", L"cheats") + L". Switched-on ones apply when the game starts.");
	for (size_t index = 0; index < cheats.size(); index++) {
		AddSwitch(page->local_tiles, page->explanation, Wide(cheats[index].name).c_str(), cheats[index].enabled,
		          [weak = std::weak_ptr<Page>(page), index](bool on) {
			          if (auto shared = weak.lock()) {
				          Switch(shared, index, on);
			          }
		          },
		          {L"A switches it on or off. It applies the next time the game starts.",
		           L"Cheats change the game's code: one that doesn't fit can crash the game."});
	}
}

winrt::fire_and_forget LoadLocal(std::shared_ptr<Page> page) {
	const auto generation = ++page->local_generation;
	const auto dispatcher = page->root.Dispatcher();
	page->local_readable  = false;
	page->local_tiles.Children().Clear();
	page->local_caption.Text(L"Reading...");
	const auto title_id = page->game.title_id;
	co_await winrt::resume_background();
	const bool                 data_folder_set = !LoadSettings().user_data_path.empty();
	const auto                 path            = Cheats::PatchFolder() / Wide(title_id + ".json");
	std::optional<std::string> data            = IsFile(path) ? ReadTextFile(path) : std::optional<std::string>(std::string());
	co_await winrt::resume_foreground(dispatcher);
	if (page->left || generation != page->local_generation) {
		co_return;
	}
	page->path            = path;
	page->data_folder_set = data_folder_set;
	if (!data) {
		page->local_caption.Text(L"The cheat file can't be read. Info says where it is.");
		UpdateInfo(page);
		co_return;
	}
	TakeLocal(page, std::move(*data));
}

winrt::fire_and_forget OpenFile(std::shared_ptr<Page> page, size_t index);

winrt::fire_and_forget LoadRemote(std::shared_ptr<Page> page) {
	const auto generation = ++page->catalog_generation;
	const auto dispatcher = page->root.Dispatcher();
	page->files.clear();
	page->remote_tiles.Children().Clear();
	page->remote_loading.IsActive(true);
	page->remote_loading.Visibility(xaml::Visibility::Visible);
	page->remote_caption.Text(L"Loading the collection...");
	const auto title_id = page->game.title_id;
	co_await winrt::resume_background();
	auto catalog = Cheats::LoadCatalog(title_id);
	co_await winrt::resume_foreground(dispatcher);
	if (page->left || generation != page->catalog_generation) {
		co_return;
	}
	page->remote_loading.IsActive(false);
	page->remote_loading.Visibility(xaml::Visibility::Collapsed);
	// The installed version's files first, as on the desktop.
	const auto& version = page->game.version;
	std::stable_sort(catalog.files.begin(), catalog.files.end(),
	                 [&version](const auto& a, const auto& b) { return (a.version == version) > (b.version == version); });
	page->files = std::move(catalog.files);
	for (size_t index = 0; index < page->files.size(); index++) {
		const auto& file      = page->files[index];
		const bool  usable    = file.IsSupported();
		const bool  installed = file.version == version;
		std::wstring detail   = !usable ? L"Can't be used (" + Wide(file.format) + L")" : installed ? L"Installed version" : L"Other version";
		Paragraphs   help;
		if (!usable) {
			help = {L"Only JSON files can be used. This one is " + Wide(file.format) + L"."};
		} else if (installed) {
			help = {L"A file for the version you have.", L"A shows its cheats."};
		} else {
			help = {L"A file for version " + Wide(file.version) + L". You have " + (version.empty() ? L"an unknown one" : Wide(version)) + L".",
			        L"Its cheats can't be used with your version."};
		}
		auto tile = AddAction(page->remote_tiles, page->explanation, Wide(file.version).c_str(), detail.c_str(),
		                      [weak = std::weak_ptr<Page>(page), index, usable] {
			                      if (auto shared = weak.lock(); shared != nullptr && usable && !shared->FileOpen()) {
				                      OpenFile(shared, index);
			                      }
		                      },
		                      std::move(help));
		if (!usable) {
			tile.Opacity(0.5);
		}
	}
	if (!catalog.error.empty()) {
		page->remote_caption.Text(L"The collection couldn't be read in full. Y tries again.");
	} else {
		page->remote_caption.Text(page->files.empty() ? std::wstring(L"The collection has no cheats for this game.")
		                                              : Count(page->files.size(), L"file", L"files") + L" for this game.");
	}
}

xaml::Controls::StackPanel Detail(const std::wstring& label, const std::wstring& value) {
	xaml::Controls::StackPanel block;
	xaml::Controls::TextBlock  caption;
	caption.Style(AppStyle(L"CaptionTextBlockStyle"));
	caption.Foreground(MutedBrush());
	caption.Text(label);
	xaml::Controls::TextBlock text;
	text.Style(AppStyle(L"BodyTextBlockStyle"));
	text.TextWrapping(xaml::TextWrapping::Wrap);
	text.Text(value);
	block.Children().Append(caption);
	block.Children().Append(text);
	return block;
}

// While the file's panel is open, the categories and tiles behind it can't take the focus.
void EnableBehindPanel(const std::shared_ptr<Page>& page, bool enabled) {
	page->categories.IsEnabled(enabled);
	for (const auto& tiles: {page->local_tiles, page->remote_tiles}) {
		for (const auto& tile: tiles.Children()) {
			tile.as<xaml::Controls::Control>().IsEnabled(enabled);
		}
	}
}

void FocusFirstAction(const std::shared_ptr<Page>& page) {
	page->file_actions.UpdateLayout();
	if (auto first = page->file_actions.ContainerFromIndex(0).try_as<xaml::Controls::Control>()) {
		first.Focus(xaml::FocusState::Keyboard);
	}
}

void CloseFile(const std::shared_ptr<Page>& page) {
	page->preview_generation++;
	page->file_panel.Visibility(xaml::Visibility::Collapsed);
	EnableBehindPanel(page, true);
	if (page->file_index < page->remote_tiles.Children().Size()) {
		page->remote_tiles.Children().GetAt(static_cast<uint32_t>(page->file_index)).as<xaml::Controls::Control>().Focus(xaml::FocusState::Keyboard);
	}
}

// Opens the panel of the file at `index`: its name, who made it, its cheats, where it would be saved, and Download when it fits the game.
winrt::fire_and_forget OpenFile(std::shared_ptr<Page> page, size_t index) {
	const auto generation = ++page->preview_generation;
	const auto dispatcher = page->root.Dispatcher();
	const auto file       = page->files[index];
	page->file_index      = index;
	page->remote_document = Cheats::Json();
	page->remote_valid    = false;
	page->file_title.Text(Wide(page->game.name) + L" " + Wide(file.version));
	page->file_details.Children().Clear();
	page->file_cheats.Items().Clear();
	page->file_actions.Items().Clear();
	page->file_loading.IsActive(true);
	page->file_loading.Visibility(xaml::Visibility::Visible);
	page->file_status.Text(L"Loading the file...");
	page->file_actions.Items().Append(winrt::box_value(ACTION_CLOSE));
	page->file_panel.Visibility(xaml::Visibility::Visible);
	FocusFirstAction(page);
	EnableBehindPanel(page, false);
	co_await winrt::resume_background();
	auto download = Cheats::Fetch(file);
	co_await winrt::resume_foreground(dispatcher);
	if (page->left || generation != page->preview_generation) {
		co_return;
	}
	page->file_loading.IsActive(false);
	page->file_loading.Visibility(xaml::Visibility::Collapsed);
	Cheats::Json document;
	if (download.error.empty()) {
		download.error = Cheats::Parse(download.data, document);
	}
	std::wstring status;
	if (!download.error.empty()) {
		status = L"This file can't be shown. " + download.error;
	} else {
		std::string credits;
		if (const auto list = document.find("credits"); list != document.end() && list->is_array()) {
			for (const auto& name: *list) {
				if (name.is_string()) {
					credits += (credits.empty() ? "" : ", ") + name.get<std::string>();
				}
			}
		}
		if (credits.empty() && document.contains("author") && document["author"].is_string()) {
			credits = document["author"].get<std::string>();
		}
		const auto cheats = Cheats::CheatsOf(document);
		page->file_details.Children().Append(Detail(L"By", credits.empty() ? L"Unknown" : Wide(credits)));
		page->file_details.Children().Append(Detail(L"Cheats", std::to_wstring(cheats.size())));
		for (const auto& cheat: cheats) {
			xaml::Controls::TextBlock name;
			name.Style(AppStyle(L"BodyTextBlockStyle"));
			name.Foreground(MutedBrush());
			name.TextWrapping(xaml::TextWrapping::Wrap);
			name.Text(Wide(cheat.name));
			page->file_cheats.Items().Append(name);
		}
		const auto error   = Cheats::ValidateImport(document, page->game.title_id, page->game.version, PROCESS);
		page->remote_valid = error.empty();
		if (page->remote_valid) {
			page->file_details.Children().Append(Detail(L"Saves to", Location(page->path)));
		}
		page->remote_document = std::move(document);
		status                = error.empty() ? L"Downloaded cheats start switched off." : error;
	}
	page->file_status.Text(status);
	if (page->remote_valid) {
		page->file_actions.Items().InsertAt(0, winrt::box_value(ACTION_DOWNLOAD));
		FocusFirstAction(page);
	}
}

// Saves the file of the panel on this console, all its cheats switched off (after asking, when the game has cheats already); then Local shows
// them.
winrt::fire_and_forget Download(std::shared_ptr<Page> page) {
	if (!page->remote_valid || page->path.empty()) {
		co_return;
	}
	if (!page->local_readable) {
		page->file_status.Text(L"The cheats saved for this game can't be read, so they aren't replaced.");
		co_return;
	}
	if (!page->local_data.empty()) {
		xaml::Controls::ContentDialog dialog;
		dialog.Title(winrt::box_value(L"Replace this game's cheats?"));
		dialog.Content(winrt::box_value(L"The cheats saved for this game are replaced. The downloaded ones start switched off."));
		dialog.PrimaryButtonText(L"Replace");
		dialog.CloseButtonText(L"Cancel");
		dialog.DefaultButton(xaml::Controls::ContentDialogButton::Close);
		page->dialog_open = true;
		const auto answer = co_await dialog.ShowAsync();
		page->dialog_open = false;
		if (page->left || answer != xaml::Controls::ContentDialogResult::Primary) {
			co_return;
		}
	}
	const auto cheats   = Cheats::CheatsOf(page->remote_document);
	const auto document = Cheats::WithSelection(page->remote_document, std::vector<bool>(cheats.size(), false));
	if (const auto error = Cheats::Save(page->path, document, page->local_data); !error.empty()) {
		page->file_status.Text(error);
		co_return;
	}
	page->local_generation++; // a read still on its way is older
	TakeLocal(page, Cheats::Text(document));
	page->preview_generation++;
	page->file_panel.Visibility(xaml::Visibility::Collapsed);
	EnableBehindPanel(page, true);
	page->categories.SelectedIndex(LOCAL);
	page->local_tiles.UpdateLayout();
	FocusFirst(page->local_tiles);
}

void Reload(const std::shared_ptr<Page>& page) {
	LoadLocal(page);
	LoadRemote(page);
}

} // namespace

xaml::UIElement CreateCheatsPage(GameEntry game, std::function<void()> back) {
	auto page  = std::make_shared<Page>();
	page->game = std::move(game);
	page->back = std::move(back);
	page->root = xaml::Markup::XamlReader::Load(PageMarkup).as<xaml::Controls::Grid>();
	const auto find            = [&page](const wchar_t* name) { return page->root.FindName(name); };
	page->categories           = find(L"Categories").as<xaml::Controls::ListView>();
	page->panels               = find(L"Panels").as<xaml::Controls::ScrollViewer>();
	page->category_panels[LOCAL]  = find(L"Local").as<xaml::UIElement>();
	page->category_panels[REMOTE] = find(L"Remote").as<xaml::UIElement>();
	page->category_panels[INFO]   = find(L"Info").as<xaml::UIElement>();
	page->explanation          = find(L"Explanation").as<xaml::Controls::StackPanel>();
	page->local_caption        = find(L"LocalCaption").as<xaml::Controls::TextBlock>();
	page->local_tiles          = find(L"LocalTiles").as<xaml::Controls::StackPanel>();
	page->remote_loading       = find(L"RemoteLoading").as<xaml::Controls::ProgressRing>();
	page->remote_caption       = find(L"RemoteCaption").as<xaml::Controls::TextBlock>();
	page->remote_tiles         = find(L"RemoteTiles").as<xaml::Controls::StackPanel>();
	page->info_location        = find(L"InfoLocation").as<xaml::Controls::TextBlock>();
	page->info_text            = find(L"InfoText").as<xaml::Controls::StackPanel>();
	page->file_panel           = find(L"FilePanel").as<xaml::FrameworkElement>();
	page->file_title           = find(L"FileTitle").as<xaml::Controls::TextBlock>();
	page->file_details         = find(L"FileDetails").as<xaml::Controls::StackPanel>();
	page->file_cheats          = find(L"FileCheats").as<xaml::Controls::ListView>();
	page->file_loading         = find(L"FileLoading").as<xaml::Controls::ProgressRing>();
	page->file_status          = find(L"FileStatus").as<xaml::Controls::TextBlock>();
	page->file_actions         = find(L"FileActions").as<xaml::Controls::ListView>();
	// The game and its installed version, which the remote files are for.
	find(L"GameName").as<xaml::Controls::TextBlock>().Text(
	    Wide(page->game.name) + (page->game.version.empty() ? std::wstring() : L" · " + Wide(page->game.version)));
	ShowCover(find(L"HeaderCover").as<xaml::Controls::Border>(), page->game.folder);
	SetHints(find(L"Hints").as<xaml::Controls::Border>(), {{XboxButton::A, L"Select"}, {XboxButton::Y, L"Refresh"}, {XboxButton::B, L"Back"}});
	SetHints(find(L"FileHints").as<xaml::Controls::Border>(), {{XboxButton::A, L"Select"}, {XboxButton::B, L"Back"}});

	// The chosen category's tiles; the focus in the tiles or the categories decides what B does.
	page->categories.SelectionChanged([weak = std::weak_ptr<Page>(page)](auto const&, auto const&) {
		if (auto shared = weak.lock(); shared != nullptr && shared->categories.SelectedIndex() >= 0) {
			ShowCategory(shared, shared->categories.SelectedIndex());
		}
	});
	page->categories.GotFocus([weak = std::weak_ptr<Page>(page)](auto const&, auto const&) {
		if (auto shared = weak.lock()) {
			shared->in_panel = false;
			ShowExplanation(shared->explanation, CategoryExplanation(shared->categories.SelectedIndex()));
		}
	});
	page->panels.GotFocus([weak = std::weak_ptr<Page>(page)](auto const&, auto const&) {
		if (auto shared = weak.lock()) {
			shared->in_panel = true;
		}
	});
	page->file_actions.ItemClick([weak = std::weak_ptr<Page>(page)](auto const&, xaml::Controls::ItemClickEventArgs const& args) {
		auto shared = weak.lock();
		if (shared == nullptr) {
			return;
		}
		if (winrt::unbox_value<winrt::hstring>(args.ClickedItem()) == ACTION_DOWNLOAD) {
			Download(shared);
		} else {
			CloseFile(shared);
		}
	});

	// B or Escape: closes the file's panel, leaves the tiles for the categories, or leaves the page (also the window's back button). Y or F5
	// reads both lists again.
	const auto go_back = [weak = std::weak_ptr<Page>(page)] {
		auto shared = weak.lock();
		if (shared == nullptr || shared->dialog_open) {
			return;
		}
		if (shared->FileOpen()) {
			CloseFile(shared);
		} else if (shared->in_panel) {
			shared->categories.Focus(xaml::FocusState::Keyboard);
		} else {
			shared->Leave();
		}
	};
	page->root.KeyDown([weak = std::weak_ptr<Page>(page), go_back](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		auto shared = weak.lock();
		if (shared == nullptr || shared->dialog_open) {
			return;
		}
		const auto key = args.Key();
		if (key == winrt::Windows::System::VirtualKey::GamepadB || key == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			go_back();
		} else if ((key == winrt::Windows::System::VirtualKey::GamepadY || key == winrt::Windows::System::VirtualKey::F5) && !shared->FileOpen()) {
			args.Handled(true);
			if (Cheats::IsSupportedTitleId(shared->game.title_id)) {
				Reload(shared);
			}
		}
	});
	auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	navigation.AppViewBackButtonVisibility(winrt::Windows::UI::Core::AppViewBackButtonVisibility::Visible);
	auto back_token = navigation.BackRequested([go_back](auto const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
		args.Handled(true);
		go_back();
	});
	page->root.Loaded([weak = std::weak_ptr<Page>(page)](auto const&, auto const&) {
		if (auto shared = weak.lock()) {
			shared->categories.Focus(xaml::FocusState::Keyboard);
		}
	});
	// The page's state lives while the page is shown: this handler holds it and removes itself when the page goes.
	page->unloaded = page->root.Unloaded([page, navigation, back_token](auto const&, auto const&) {
		navigation.BackRequested(back_token);
		navigation.AppViewBackButtonVisibility(winrt::Windows::UI::Core::AppViewBackButtonVisibility::Collapsed);
		page->left = true;
		page->root.Unloaded(page->unloaded);
	});

	page->categories.SelectedIndex(LOCAL);
	if (!Cheats::IsSupportedTitleId(page->game.title_id)) {
		page->local_caption.Text(L"Cheats work with PS5 games only (title IDs that start with PPSA).");
		page->remote_loading.Visibility(xaml::Visibility::Collapsed);
		page->remote_caption.Text(L"Cheats work with PS5 games only.");
		page->info_location.Text(L"Cheats work with PS5 games only.");
		return page->root;
	}
	Log("cheats: page for %s, version %s\n", page->game.title_id.c_str(), page->game.version.c_str());
	Reload(page);
	return page->root;
}

} // namespace Kyty::Uwp
