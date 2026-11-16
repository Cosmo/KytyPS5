#include "folderPanel.h"

#include "fileAccess.h"
#include "gameLibrary.h"
#include "xboxLook.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

constexpr wchar_t DEVELOPMENT_FILES[] = L"D:\\DevelopmentFiles";

// As the library's game options (launcher.cpp): the page dimmed, the panel on the right. "Use this folder" above the line, the folders under
// it in the row that takes the rest of the height, so that their list scrolls inside the panel and the hints stay at its foot.
constexpr wchar_t PanelMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Rectangle Fill="{ThemeResource SystemControlPageBackgroundMediumAltMediumBrush}" />
  <Grid Width="360" HorizontalAlignment="Right" Padding="24,32,24,32"
        Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
      <RowDefinition Height="Auto" />
      <RowDefinition Height="*" />
      <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>
    <TextBlock x:Name="Caption" Style="{StaticResource CaptionTextBlockStyle}"
               Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
    <TextBlock x:Name="Title" Grid.Row="1" Style="{StaticResource SubtitleTextBlockStyle}"
               MaxLines="2" TextTrimming="CharacterEllipsis" Margin="0,4,0,4" />
    <TextBlock x:Name="Path" Grid.Row="2" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="Wrap" Margin="0,0,0,16"
               Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
    <ListView x:Name="Top" Grid.Row="3" SelectionMode="None" IsItemClickEnabled="True" Margin="-12,0">
      <ListView.ItemContainerStyle>
        <Style TargetType="ListViewItem">
          <Setter Property="CornerRadius" Value="0" />
          <Setter Property="HorizontalContentAlignment" Value="Stretch" />
        </Style>
      </ListView.ItemContainerStyle>
    </ListView>
    <Rectangle x:Name="Divider" Grid.Row="4" Height="1" Margin="0,8"
               Fill="{ThemeResource SystemControlForegroundBaseLowBrush}" />
    <Grid Grid.Row="5">
      <ListView x:Name="Bottom" SelectionMode="None" IsItemClickEnabled="True" Margin="-12,0">
        <ListView.ItemContainerStyle>
          <Style TargetType="ListViewItem">
            <Setter Property="CornerRadius" Value="0" />
            <Setter Property="HorizontalContentAlignment" Value="Stretch" />
          </Style>
        </ListView.ItemContainerStyle>
      </ListView>
      <!-- Takes the focus when nothing else in the panel can (a folder that can't be opened), so that B still comes here. -->
      <ContentControl x:Name="Note" IsTabStop="True" UseSystemFocusVisuals="False" VerticalAlignment="Top"
                      HorizontalContentAlignment="Stretch">
        <TextBlock x:Name="NoteText" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="Wrap"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </ContentControl>
    </Grid>
    <Border x:Name="Hints" Grid.Row="6" Margin="0,16,0,0" Style="{StaticResource HintBarStyle}" />
  </Grid>
</Grid>)xaml";

enum class EntryKind { Use, Folder };

struct Entry {
	EntryKind             kind;
	std::filesystem::path path;
};

struct Panel {
	FolderPanelOptions                     options;
	winrt::weak_ref<xaml::Controls::Grid>  page;
	xaml::Controls::Grid                   root {nullptr};
	xaml::Controls::TextBlock              caption {nullptr};
	xaml::Controls::TextBlock              title {nullptr};
	xaml::Controls::TextBlock              path {nullptr};
	xaml::Controls::TextBlock              note_text {nullptr};
	xaml::UIElement                        divider {nullptr};
	xaml::Controls::Control                note {nullptr};
	xaml::Controls::ListView               top {nullptr};
	xaml::Controls::ListView               bottom {nullptr};
	xaml::Controls::Border                 hints {nullptr};
	std::vector<Entry>                     entries;
	// The game counts, for the folders in `entries` (filled in the background).
	std::vector<std::pair<std::filesystem::path, xaml::Controls::TextBlock>> counts;
	// Development Files and the folders opened in it, the shown one last.
	std::vector<std::filesystem::path> trail;
	uint64_t                           shown = 0; // renewed with each level, so that late counts of an old one are dropped
	bool                               open  = true;
	winrt::event_token                 getting_focus {};
};

using PanelPtr = std::shared_ptr<Panel>;

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
	return _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}

std::wstring FolderName(const std::filesystem::path& folder) {
	return SamePath(folder, DEVELOPMENT_FILES) ? std::wstring(L"Development Files") : folder.filename().wstring();
}

std::wstring GamesText(size_t count, bool none_too) {
	if (count == 0) {
		return none_too ? L"No games" : L"";
	}
	return count == 1 ? std::wstring(L"1 game") : std::to_wstring(count) + L" games";
}

xaml::Media::Brush Resource(const wchar_t* name) {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(name)).as<xaml::Media::Brush>();
}

// A list entry, one line high: the name, and on the right its game count in grey once it is known. `count` gets that count's text.
xaml::Controls::Grid MakeItem(const std::wstring& name, size_t index, xaml::Controls::TextBlock* count) {
	xaml::Controls::TextBlock first;
	first.Text(name);
	first.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
	first.VerticalAlignment(xaml::VerticalAlignment::Center);
	xaml::Controls::TextBlock second;
	second.Foreground(Resource(L"SystemControlForegroundBaseMediumBrush"));
	second.VerticalAlignment(xaml::VerticalAlignment::Center);
	xaml::Controls::Grid item;
	item.ColumnSpacing(12);
	item.ColumnDefinitions().Append(xaml::Controls::ColumnDefinition());
	xaml::Controls::ColumnDefinition count_column;
	count_column.Width(xaml::GridLengthHelper::Auto());
	item.ColumnDefinitions().Append(count_column);
	item.Children().Append(first);
	item.Children().Append(second);
	xaml::Controls::Grid::SetColumn(second, 1);
	item.Tag(winrt::box_value(static_cast<uint32_t>(index)));
	*count = second;
	return item;
}

void Add(const PanelPtr& panel, xaml::Controls::ListView const& list, EntryKind kind, const std::filesystem::path& path, const std::wstring& name) {
	xaml::Controls::TextBlock count {nullptr};
	list.Items().Append(MakeItem(name, panel->entries.size(), &count));
	panel->entries.push_back({kind, path});
	panel->counts.emplace_back(path, count);
}

void Focus(xaml::Controls::ListView const& list, uint32_t index) {
	if (auto item = list.ContainerFromIndex(index).try_as<xaml::Controls::Control>()) {
		item.Focus(xaml::FocusState::Keyboard);
	}
}

// Counts the games of the shown level's folders, one after the other off the UI thread, and shows each count when it is known.
winrt::fire_and_forget CountGamesLater(PanelPtr panel) {
	const uint64_t shown      = panel->shown;
	const auto     dispatcher = panel->root.Dispatcher();
	std::vector<std::filesystem::path> folders;
	for (const auto& count: panel->counts) {
		folders.push_back(count.first);
	}
	for (size_t index = 0; index < folders.size(); index++) {
		co_await winrt::resume_background();
		const size_t games = CountGames(folders[index]);
		co_await winrt::resume_foreground(dispatcher);
		if (!panel->open || panel->shown != shown || index >= panel->counts.size()) {
			co_return;
		}
		// The first entry is "Use this folder", which also says when there are none.
		panel->counts[index].second.Text(GamesText(games, index == 0));
	}
}

// Whether `element` is `root` or inside it.
bool IsInside(xaml::DependencyObject element, xaml::DependencyObject const& root) {
	while (element != nullptr) {
		if (element == root) {
			return true;
		}
		element = xaml::Media::VisualTreeHelper::GetParent(element);
	}
	return false;
}

void Close(const PanelPtr& panel) {
	if (!panel->open) {
		return;
	}
	panel->open = false;
	panel->shown++;
	if (auto page = panel->page.get()) {
		page.GettingFocus(panel->getting_focus);
		uint32_t index = 0;
		if (page.Children().IndexOf(panel->root, index)) {
			page.Children().RemoveAt(index);
		}
	}
	if (panel->options.closed) {
		panel->options.closed();
	}
}

void ShowLevel(const PanelPtr& panel, const std::filesystem::path& focus = {});

void Back(const PanelPtr& panel) {
	if (panel->trail.size() <= 1) {
		Close(panel);
		return;
	}
	const auto left = panel->trail.back();
	panel->trail.pop_back();
	ShowLevel(panel, left);
}

void Choose(const PanelPtr& panel, size_t index) {
	if (index >= panel->entries.size()) {
		return;
	}
	const auto entry = panel->entries[index];
	if (entry.kind == EntryKind::Folder) {
		panel->trail.push_back(entry.path);
		ShowLevel(panel);
		return;
	}
	auto chosen = panel->options.chosen;
	Close(panel);
	if (chosen) {
		chosen(entry.path);
	}
}

void ShowLevel(const PanelPtr& panel, const std::filesystem::path& focus) {
	panel->shown++;
	panel->entries.clear();
	panel->counts.clear();
	panel->top.Items().Clear();
	panel->bottom.Items().Clear();
	panel->top.Visibility(xaml::Visibility::Visible);
	panel->divider.Visibility(xaml::Visibility::Visible);
	panel->bottom.Visibility(xaml::Visibility::Visible);
	panel->note.Visibility(xaml::Visibility::Collapsed);
	SetHints(panel->hints, {{XboxButton::A, L"Select"}, {XboxButton::B, L"Back"}});

	const auto folder = panel->trail.back();
	panel->title.Text(FolderName(folder));
	auto shown_path = folder;
	panel->path.Text(shown_path.make_preferred().wstring());
	const auto show_note = [&](const std::wstring& text) {
		panel->bottom.Visibility(xaml::Visibility::Collapsed);
		panel->note.Visibility(xaml::Visibility::Visible);
		panel->note_text.Text(text);
	};
	if (const auto error = ListError(folder); error != 0) {
		panel->top.Visibility(xaml::Visibility::Collapsed);
		panel->divider.Visibility(xaml::Visibility::Collapsed);
		show_note(L"Can't be opened. Error code " + std::to_wstring(error) + L".");
		SetHints(panel->hints, {{XboxButton::B, L"Back"}});
		panel->root.UpdateLayout();
		panel->note.Focus(xaml::FocusState::Programmatic);
		return;
	}

	xaml::Controls::ListView focus_list  = panel->top;
	uint32_t                 focus_index = 0;
	Add(panel, panel->top, EntryKind::Use, folder, L"Use this folder");
	auto subfolders = Subfolders(folder);
	std::sort(subfolders.begin(), subfolders.end(),
	          [](const auto& a, const auto& b) { return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0; });
	for (const auto& subfolder: subfolders) {
		if (!focus.empty() && SamePath(subfolder, focus)) {
			focus_list  = panel->bottom;
			focus_index = panel->bottom.Items().Size();
		}
		Add(panel, panel->bottom, EntryKind::Folder, subfolder, subfolder.filename().wstring());
	}
	if (subfolders.empty()) {
		show_note(L"No folders in it.");
	}
	panel->root.UpdateLayout();
	if (!panel->options.count_games) {
		panel->counts.clear();
	}
	if (focus_list == panel->bottom) {
		panel->bottom.ScrollIntoView(panel->bottom.Items().GetAt(focus_index));
		panel->root.UpdateLayout();
	}
	Focus(focus_list, focus_index);
	if (!panel->counts.empty()) {
		CountGamesLater(panel);
	}
}

} // namespace

bool DevelopmentFilesReadable() {
	return ListError(DEVELOPMENT_FILES) == 0;
}

bool DevelopmentFilesWritable() {
	return WriteError(DEVELOPMENT_FILES) == 0;
}

std::function<bool()> ShowDevelopmentFilesPanel(xaml::Controls::Grid const& page, FolderPanelOptions options) {
	auto panel     = std::make_shared<Panel>();
	panel->options = std::move(options);
	panel->page    = winrt::make_weak(page);
	panel->trail   = {DEVELOPMENT_FILES};
	panel->root    = xaml::Markup::XamlReader::Load(PanelMarkup).as<xaml::Controls::Grid>();
	const auto find = [&](const wchar_t* name) { return panel->root.FindName(name); };
	panel->caption   = find(L"Caption").as<xaml::Controls::TextBlock>();
	panel->title     = find(L"Title").as<xaml::Controls::TextBlock>();
	panel->path      = find(L"Path").as<xaml::Controls::TextBlock>();
	panel->note_text = find(L"NoteText").as<xaml::Controls::TextBlock>();
	panel->divider   = find(L"Divider").as<xaml::UIElement>();
	panel->note      = find(L"Note").as<xaml::Controls::Control>();
	panel->top       = find(L"Top").as<xaml::Controls::ListView>();
	panel->bottom    = find(L"Bottom").as<xaml::Controls::ListView>();
	panel->hints     = find(L"Hints").as<xaml::Controls::Border>();
	panel->caption.Text(panel->options.caption);

	// Over the whole page, its padding too.
	const auto padding = page.Padding();
	panel->root.Margin(xaml::ThicknessHelper::FromLengths(-padding.Left, -padding.Top, -padding.Right, -padding.Bottom));
	xaml::Controls::Grid::SetRowSpan(panel->root, std::max<int32_t>(1, static_cast<int32_t>(page.RowDefinitions().Size())));
	xaml::Controls::Grid::SetColumnSpan(panel->root, std::max<int32_t>(1, static_cast<int32_t>(page.ColumnDefinitions().Size())));

	const std::weak_ptr<Panel> weak = panel;
	const auto clicked = [weak](auto const&, xaml::Controls::ItemClickEventArgs const& args) {
		auto panel = weak.lock();
		auto item  = args.ClickedItem().try_as<xaml::FrameworkElement>();
		if (panel != nullptr && item != nullptr) {
			Choose(panel, winrt::unbox_value<uint32_t>(item.Tag()));
		}
	};
	panel->top.ItemClick(clicked);
	panel->bottom.ItemClick(clicked);
	// B (or Escape) goes back up, and closes the panel at Development Files; the page behind doesn't see it.
	panel->root.KeyDown([weak](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadB || args.Key() == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			if (auto panel = weak.lock()) {
				Back(panel);
			}
		}
	});
	// Clicks on the dimmed page close the panel.
	panel->root.Children().GetAt(0).Tapped([weak](auto const&, auto const&) {
		if (auto panel = weak.lock()) {
			Close(panel);
		}
	});

	// The controller's directions stay in the panel: the page behind it doesn't take the focus while it shows.
	panel->getting_focus = page.GettingFocus([weak](auto const&, xaml::Input::GettingFocusEventArgs const& args) {
		auto panel = weak.lock();
		if (panel != nullptr && panel->open && args.NewFocusedElement() != nullptr && !IsInside(args.NewFocusedElement(), panel->root)) {
			args.Cancel(true);
		}
	});
	page.Children().Append(panel->root);
	ShowLevel(panel);
	// The panel lives as long as it shows: the page holds its root, whose handlers hold only weak references, so this keeps it.
	return [panel] {
		if (!panel->open) {
			return false;
		}
		Back(panel);
		return true;
	};
}

} // namespace Kyty::Uwp
