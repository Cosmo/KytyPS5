#include "trophiesPage.h"

#include "gameImages.h"
#include "gameLibrary.h"
#include "gameSource.h"
#include "settings.h"

#include "common/archive.h"
#include "common/trophies.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml     = winrt::Windows::UI::Xaml;
namespace Trophies = Common::Trophies;

// The emulator's user (Config's default; the app sets no other), whose unlocks are shown.
constexpr int USER_ID = 1000;
// Trophy packages looked for per game: trophy00.ucp to trophy09.ucp (one per service label).
constexpr uint32_t SERVICE_LABELS = 10;

// A trophy cup in a 24 x 24 box, filled: the grade marks.
constexpr wchar_t CUP[] = L"M7,3 H17 V9 C17,12.3 14.8,14.5 12,14.5 C9.2,14.5 7,12.3 7,9 Z "
                          L"M7,4.5 H4 V6.5 C4,8.7 5.3,10.3 7.2,10.7 L7.4,9.2 C6.2,8.9 5.5,7.9 5.5,6.5 V6 H7 Z "
                          L"M17,4.5 H20 V6.5 C20,8.7 18.7,10.3 16.8,10.7 L16.6,9.2 C17.8,8.9 18.5,7.9 18.5,6.5 V6 H17 Z "
                          L"M11,14 H13 V18 H11 Z M8,18 H16 V20.5 H8 Z";

// Sizes in effective pixels, as the library's home screen: its margins, its header (Subtitle over Body) and its controller
// hints; the lists use Body text only.
constexpr double COVER_SIZE = 56;
constexpr double ICON_SIZE  = 56;
constexpr double CUP_SIZE   = 16;
// The grade counts are cells of one width, so that they line up from row to row and with the header's.
constexpr double GRADE_CELL_WIDTH = 88;

constexpr wchar_t PageMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      Background="{ThemeResource ApplicationPageBackgroundThemeBrush}">
  <Grid.Resources>
    <!-- The home screen's controller hints. -->
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

  <Grid Padding="0,40,0,48">
    <Grid.RowDefinitions>
      <RowDefinition Height="Auto" />
      <RowDefinition Height="*" />
      <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>

    <!-- The header: "Trophies" or the open game, a line under it, and the grade counts at the right. -->
    <Grid Margin="48,0" ColumnSpacing="16">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="Auto" />
        <ColumnDefinition Width="*" />
        <ColumnDefinition Width="Auto" />
      </Grid.ColumnDefinitions>
      <Border x:Name="HeaderCover" Width="48" Height="48" CornerRadius="4" Background="#1C2330" Visibility="Collapsed" />
      <StackPanel Grid.Column="1" Spacing="2" VerticalAlignment="Center">
        <TextBlock x:Name="HeaderTitle" Text="Trophies" Style="{StaticResource SubtitleTextBlockStyle}"
                   TextTrimming="CharacterEllipsis" TextWrapping="NoWrap" />
        <TextBlock x:Name="HeaderSummary" Style="{StaticResource BodyTextBlockStyle}"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </StackPanel>
      <StackPanel x:Name="HeaderGrades" Grid.Column="2" Orientation="Horizontal" Spacing="16" VerticalAlignment="Center" />
    </Grid>

    <StackPanel x:Name="Status" Grid.Row="1" Margin="48,32,48,0" Orientation="Horizontal" Spacing="12" VerticalAlignment="Top">
      <ProgressRing x:Name="Loading" IsActive="True" Width="20" Height="20" />
      <TextBlock x:Name="StatusText" Text="Reading the games' trophies..." Style="{StaticResource BodyTextBlockStyle}"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
    </StackPanel>
    <!-- The lists' items have 12 of padding: 36 more puts their content on the header's 48. -->
    <ListView x:Name="Games" Grid.Row="1" Margin="0,24,0,0" Padding="36,0,36,16" SelectionMode="None" IsItemClickEnabled="True"
              XYFocusKeyboardNavigation="Enabled" Visibility="Collapsed" />
    <ListView x:Name="TrophyList" Grid.Row="1" Margin="0,24,0,0" Padding="36,0,36,16" SelectionMode="None" IsItemClickEnabled="True"
              XYFocusKeyboardNavigation="Enabled" Visibility="Collapsed" />

    <Border Grid.Row="2" HorizontalAlignment="Right" Margin="48,16,48,0" Padding="12,8"
            Background="{ThemeResource SystemControlBackgroundChromeMediumLowBrush}" CornerRadius="4">
      <StackPanel Orientation="Horizontal" Spacing="24">
        <StackPanel Orientation="Horizontal">
          <Grid>
            <Ellipse Style="{StaticResource HintRingStyle}" />
            <TextBlock Text="A" Foreground="#7CC24D" Style="{StaticResource HintLetterStyle}" />
          </Grid>
          <TextBlock x:Name="SelectHint" Text="Open" Style="{StaticResource HintTextStyle}" />
        </StackPanel>
        <StackPanel Orientation="Horizontal">
          <Grid>
            <Ellipse Style="{StaticResource HintRingStyle}" />
            <TextBlock Text="B" Foreground="#E5553F" Style="{StaticResource HintLetterStyle}" />
          </Grid>
          <TextBlock Text="Back" Style="{StaticResource HintTextStyle}" />
        </StackPanel>
      </StackPanel>
    </Border>
  </Grid>
</Grid>)xaml";

// One trophy package of a game (trophyNN.ucp) with the unlocks of it.
struct GamePackage {
	uint32_t             label = 0;
	Trophies::Package    package;
	Trophies::UnlockData unlocks;
	Trophies::Progress   progress;
};

// A game with trophies: its packages and their progress together.
struct GameTrophies {
	GameEntry                game;
	std::vector<GamePackage> packages;
	Trophies::Progress       progress;
};

// Grades as the PS5 numbers them.
const wchar_t* GradeName(int grade) {
	switch (grade) {
		case 1: return L"Platinum";
		case 2: return L"Gold";
		case 3: return L"Silver";
		case 4: return L"Bronze";
		default: return L"Trophy";
	}
}

// The desktop launcher's grade colours.
winrt::Windows::UI::Color GradeColor(int grade) {
	switch (grade) {
		case 1: return {255, 0x7f, 0xc8, 0xff};
		case 2: return {255, 0xf5, 0xc5, 0x42};
		case 3: return {255, 0xc4, 0xca, 0xd2};
		case 4: return {255, 0xcd, 0x7f, 0x4f};
		default: return {255, 0x80, 0x80, 0x80};
	}
}

// The games of the last library scan that have trophy packages, by name (a background thread: it reads the games and LocalState).
std::vector<GameTrophies> LoadAllTrophies(int language) {
	std::vector<GameTrophies> games;
	for (auto& game: LoadGameLibrary()) {
		GameTrophies entry;
		entry.game = std::move(game);
		for (uint32_t label = 0; label < SERVICE_LABELS; label++) {
			const auto inner = Trophies::PackagePath(label);
			if (!GameFileExists(entry.game.folder / inner)) {
				continue;
			}
			// Common::File reads a member of an archive by its virtual path.
			const auto  path = IsArchivePath(entry.game.folder) ? Common::MakeArchivePath(entry.game.folder, inner) : entry.game.folder / inner;
			GamePackage package;
			package.label   = label;
			package.package = Trophies::LoadPackage(path, language);
			if (package.package.trophies.empty()) {
				continue;
			}
			package.unlocks  = Trophies::LoadUnlockData(Trophies::UnlocksPath(LocalStateFolder(), entry.game.title_id, USER_ID, label));
			package.progress = Trophies::GetProgress(package.package, package.unlocks);
			for (size_t grade = 0; grade < entry.progress.total_grade.size(); grade++) {
				entry.progress.total_grade[grade] += package.progress.total_grade[grade];
				entry.progress.earned_grade[grade] += package.progress.earned_grade[grade];
			}
			entry.progress.total += package.progress.total;
			entry.progress.earned += package.progress.earned;
			entry.packages.push_back(std::move(package));
		}
		if (!entry.packages.empty()) {
			games.push_back(std::move(entry));
		}
	}
	std::sort(games.begin(), games.end(), [](const GameTrophies& a, const GameTrophies& b) { return a.game.name < b.game.name; });
	return games;
}

xaml::Style TextStyle(const wchar_t* name) {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(name)).as<xaml::Style>();
}

xaml::Media::Brush MediumBrush() {
	return xaml::Application::Current().Resources().Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>();
}

// A cup in a grade's colour, `size` pixels high. (A geometry can only be used once: each cup parses its own.)
xaml::Shapes::Path Cup(int grade, double size) {
	xaml::Shapes::Path cup;
	cup.Data(xaml::Markup::XamlBindingHelper::ConvertValue(winrt::xaml_typename<xaml::Media::Geometry>(), winrt::box_value(CUP))
	             .as<xaml::Media::Geometry>());
	cup.Fill(xaml::Media::SolidColorBrush(GradeColor(grade)));
	cup.Stretch(xaml::Media::Stretch::Uniform);
	cup.Width(size);
	cup.Height(size);
	cup.VerticalAlignment(xaml::VerticalAlignment::Center);
	return cup;
}

xaml::Controls::TextBlock Text(const std::wstring& text, const wchar_t* style = L"BodyTextBlockStyle") {
	xaml::Controls::TextBlock block;
	block.Style(TextStyle(style));
	block.Text(text);
	return block;
}

// The earned and total count per grade, one cell each; a grade without trophies (often platinum) leaves its cell empty.
void FillGrades(xaml::Controls::StackPanel const& panel, const Trophies::Progress& progress) {
	panel.Children().Clear();
	for (int grade = 1; grade <= 4; grade++) {
		xaml::Controls::StackPanel cell;
		cell.Orientation(xaml::Controls::Orientation::Horizontal);
		cell.Spacing(6);
		cell.Width(GRADE_CELL_WIDTH);
		cell.VerticalAlignment(xaml::VerticalAlignment::Center);
		if (progress.total_grade[grade] != 0) {
			cell.Children().Append(Cup(grade, CUP_SIZE));
			cell.Children().Append(Text(std::to_wstring(progress.earned_grade[grade]) + L" / " + std::to_wstring(progress.total_grade[grade])));
		}
		panel.Children().Append(cell);
	}
}

std::wstring Summary(const Trophies::Progress& progress) {
	return std::to_wstring(progress.earned) + L" of " + std::to_wstring(progress.total) + L" earned  ·  " + std::to_wstring(progress.Percentage()) +
	       L" %";
}

// "Earned 2026-10-08 18:30" in local time, or "Earned" without a time.
std::wstring EarnedText(const Trophies::UnlockData& unlocks, int id) {
	const auto found = unlocks.timestamps.find(id);
	if (found == unlocks.timestamps.end() || found->second <= Trophies::UnixEpochTick) {
		return L"Earned";
	}
	const auto seconds = static_cast<std::time_t>((found->second - Trophies::UnixEpochTick) / 1000000);
	std::tm    local {};
	if (localtime_s(&local, &seconds) != 0) {
		return L"Earned";
	}
	wchar_t text[64];
	std::wcsftime(text, std::size(text), L"Earned %Y-%m-%d %H:%M", &local);
	return text;
}

// A trophy's name as shown: hidden ones stay hidden until earned (or revealed in the details).
std::wstring TrophyName(const Trophies::Trophy& trophy, bool masked) {
	if (masked) {
		return L"Hidden trophy";
	}
	return trophy.name.empty() ? L"Trophy " + std::to_wstring(trophy.id) : std::wstring(winrt::to_hstring(trophy.name));
}

// Shows a PNG (a trophy's icon) in `image`, decoded `width` pixels wide (UI thread).
winrt::fire_and_forget ShowPng(xaml::Controls::Image image, std::vector<std::byte> png, int width) {
	namespace streams = winrt::Windows::Storage::Streams;
	if (png.empty()) {
		co_return;
	}
	streams::InMemoryRandomAccessStream stream;
	streams::DataWriter                 writer(stream);
	const auto*                         bytes = reinterpret_cast<const uint8_t*>(png.data());
	writer.WriteBytes(winrt::array_view<const uint8_t>(bytes, bytes + png.size()));
	co_await writer.StoreAsync();
	writer.DetachStream();
	stream.Seek(0);
	xaml::Media::Imaging::BitmapImage bitmap;
	bitmap.DecodePixelWidth(width);
	try {
		co_await bitmap.SetSourceAsync(stream);
	} catch (const winrt::hresult_error&) {
		co_return; // not a readable PNG
	}
	image.Source(bitmap);
}

// The home screen's colour of a cover until its image loads.
xaml::Media::Brush TileBrush() {
	return xaml::Media::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x1C, 0x23, 0x30));
}

// A game's cover as the home screen shows it: in a rounded tile.
xaml::Controls::Border CoverTile(double size) {
	xaml::Controls::Border tile;
	tile.Width(size);
	tile.Height(size);
	tile.CornerRadius({4, 4, 4, 4});
	tile.Background(TileBrush());
	tile.VerticalAlignment(xaml::VerticalAlignment::Center);
	return tile;
}

winrt::fire_and_forget ShowCover(xaml::Controls::Border tile, std::filesystem::path game, int width) {
	auto bitmap = co_await LoadImageAsync(tile.Dispatcher(), CoverImage(game), width);
	if (bitmap != nullptr) {
		xaml::Media::ImageBrush brush;
		brush.ImageSource(bitmap);
		brush.Stretch(xaml::Media::Stretch::UniformToFill);
		tile.Background(brush);
	}
}

// A column of a list row: a fixed width in pixels, the rest (star) or as wide as its content (auto).
xaml::Controls::ColumnDefinition Column(xaml::GridLength width) {
	xaml::Controls::ColumnDefinition column;
	column.Width(width);
	return column;
}

struct Page {
	std::function<void()>        back;
	bool                         left = false;
	xaml::Controls::Grid         root {nullptr};
	xaml::Controls::Border       header_cover {nullptr};
	xaml::Controls::TextBlock    header_title {nullptr};
	xaml::Controls::TextBlock    header_summary {nullptr};
	xaml::Controls::StackPanel   header_grades {nullptr};
	xaml::Controls::StackPanel   status {nullptr};
	xaml::Controls::ProgressRing loading {nullptr};
	xaml::Controls::TextBlock    status_text {nullptr};
	xaml::Controls::ListView     games_list {nullptr};
	xaml::Controls::ListView     trophy_list {nullptr};
	xaml::Controls::TextBlock    select_hint {nullptr};
	std::vector<GameTrophies>    games;
	size_t                       open_game = 0;
	Trophies::Progress           all; // of all games
	// The Unloaded handler holds the page while it is shown and lets it go then.
	winrt::event_token          unloaded;

	void ShowOverview();
	void ShowOverviewHeader();
	void ShowGame(size_t index);
	void FocusGame(size_t index) const;
	void Back();
};

void Page::ShowOverviewHeader() {
	header_cover.Visibility(xaml::Visibility::Collapsed);
	header_title.Text(L"Trophies");
	header_summary.Text(std::to_wstring(all.earned) + L" of " + std::to_wstring(all.total) + L" earned  ·  " + std::to_wstring(games.size()) +
	                    (games.size() == 1 ? L" game" : L" games"));
	FillGrades(header_grades, all);
	select_hint.Text(L"Open");
}

void Page::ShowOverview() {
	status.Visibility(xaml::Visibility::Collapsed);
	if (games.empty()) {
		loading.IsActive(false);
		loading.Visibility(xaml::Visibility::Collapsed);
		status_text.Text(L"No games with trophies in the game folders.");
		status.Visibility(xaml::Visibility::Visible);
		return;
	}
	for (const auto& entry: games) {
		for (size_t grade = 0; grade < all.total_grade.size(); grade++) {
			all.total_grade[grade] += entry.progress.total_grade[grade];
			all.earned_grade[grade] += entry.progress.earned_grade[grade];
		}
		all.total += entry.progress.total;
		all.earned += entry.progress.earned;
	}
	ShowOverviewHeader();

	// A row per game: cover, name, progress, and the grade counts in the header's columns.
	games_list.Items().Clear();
	for (size_t i = 0; i < games.size(); i++) {
		const auto& entry = games[i];
		xaml::Controls::Grid row;
		row.Padding({0, 10, 0, 10});
		row.ColumnSpacing(16);
		row.ColumnDefinitions().Append(Column(xaml::GridLengthHelper::FromPixels(COVER_SIZE)));
		row.ColumnDefinitions().Append(Column(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star)));
		row.ColumnDefinitions().Append(Column(xaml::GridLengthHelper::Auto()));
		auto cover = CoverTile(COVER_SIZE);
		ShowCover(cover, entry.game.folder, 112);
		row.Children().Append(cover);

		xaml::Controls::StackPanel text;
		text.Spacing(4);
		text.VerticalAlignment(xaml::VerticalAlignment::Center);
		auto name = Text(std::wstring(winrt::to_hstring(entry.game.name)));
		name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
		name.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
		name.TextWrapping(xaml::TextWrapping::NoWrap);
		text.Children().Append(name);
		auto summary = Text(Summary(entry.progress));
		summary.Foreground(MediumBrush());
		text.Children().Append(summary);
		xaml::Controls::ProgressBar bar;
		bar.Maximum(100);
		bar.Value(entry.progress.Percentage());
		bar.Width(240);
		bar.HorizontalAlignment(xaml::HorizontalAlignment::Left);
		bar.Margin({0, 2, 0, 0});
		text.Children().Append(bar);
		xaml::Controls::Grid::SetColumn(text, 1);
		row.Children().Append(text);

		xaml::Controls::StackPanel grades;
		grades.Orientation(xaml::Controls::Orientation::Horizontal);
		grades.Spacing(16);
		grades.VerticalAlignment(xaml::VerticalAlignment::Center);
		FillGrades(grades, entry.progress);
		xaml::Controls::Grid::SetColumn(grades, 2);
		row.Children().Append(grades);

		row.Tag(winrt::box_value(static_cast<uint32_t>(i)));
		games_list.Items().Append(row);
	}
	games_list.Visibility(xaml::Visibility::Visible);
	FocusGame(0);
}

void Page::FocusGame(size_t index) const {
	// After the list has made its items.
	games_list.UpdateLayout();
	if (auto item = games_list.ContainerFromIndex(static_cast<int32_t>(index)).try_as<xaml::Controls::Control>()) {
		item.Focus(xaml::FocusState::Keyboard);
	} else {
		games_list.Focus(xaml::FocusState::Keyboard);
	}
}

// The details of a trophy (A on it); a hidden one that is not earned is masked until "Show hidden trophy".
winrt::fire_and_forget ShowDetails(Trophies::Trophy trophy, bool earned, std::wstring earned_text) {
	xaml::Controls::ContentDialog dialog;
	auto                          content = std::make_shared<xaml::Controls::StackPanel>();
	content->Spacing(16);
	const auto fill = [content, trophy, earned, earned_text](bool masked) {
		content->Children().Clear();
		xaml::Controls::Grid header;
		header.ColumnSpacing(20);
		xaml::Controls::ColumnDefinition icon_column;
		icon_column.Width(xaml::GridLengthHelper::FromPixels(128));
		header.ColumnDefinitions().Append(icon_column);
		header.ColumnDefinitions().Append(xaml::Controls::ColumnDefinition {});
		xaml::Controls::Image icon;
		icon.Width(128);
		icon.Height(128);
		icon.Opacity(earned ? 1.0 : 0.4);
		if (!masked) {
			ShowPng(icon, trophy.icon_png, 256);
			header.Children().Append(icon);
		} else {
			auto cup = Cup(0, 96);
			cup.Opacity(0.5);
			header.Children().Append(cup);
		}
		xaml::Controls::StackPanel title;
		title.Spacing(6);
		title.VerticalAlignment(xaml::VerticalAlignment::Center);
		auto name = Text(TrophyName(trophy, masked), L"SubtitleTextBlockStyle");
		name.TextWrapping(xaml::TextWrapping::Wrap);
		title.Children().Append(name);
		if (!masked) {
			xaml::Controls::StackPanel grade;
			grade.Orientation(xaml::Controls::Orientation::Horizontal);
			grade.Spacing(6);
			grade.Children().Append(Cup(trophy.grade, CUP_SIZE));
			grade.Children().Append(Text(GradeName(trophy.grade)));
			title.Children().Append(grade);
		}
		auto status = Text(earned ? earned_text : std::wstring(L"Not earned"));
		status.Foreground(MediumBrush());
		title.Children().Append(status);
		xaml::Controls::Grid::SetColumn(title, 1);
		header.Children().Append(title);
		content->Children().Append(header);
		if (masked) {
			auto note = Text(L"This trophy is hidden until it is earned.");
			note.Foreground(MediumBrush());
			content->Children().Append(note);
			return;
		}
		if (!trophy.description.empty()) {
			auto details = Text(std::wstring(winrt::to_hstring(trophy.description)));
			details.TextWrapping(xaml::TextWrapping::Wrap);
			content->Children().Append(details);
		}
		if (trophy.has_reward && !trophy.reward.empty()) {
			auto reward = Text(L"Reward: " + std::wstring(winrt::to_hstring(trophy.reward)));
			reward.TextWrapping(xaml::TextWrapping::Wrap);
			reward.Foreground(MediumBrush());
			content->Children().Append(reward);
		}
	};
	const bool masked = trophy.hidden && !earned;
	fill(masked);
	dialog.Content(*content);
	dialog.CloseButtonText(L"Close");
	if (masked) {
		dialog.PrimaryButtonText(L"Show hidden trophy");
		dialog.PrimaryButtonClick([fill, weak = winrt::make_weak(dialog)](auto const&, xaml::Controls::ContentDialogButtonClickEventArgs const& args) {
			args.Cancel(true); // stays open, now unmasked
			fill(false);
			if (auto shown = weak.get()) {
				shown.PrimaryButtonText(L"");
			}
		});
	}
	co_await dialog.ShowAsync();
}

void Page::ShowGame(size_t index) {
	open_game        = index;
	const auto& entry = games[index];
	header_cover.Background(TileBrush());
	header_cover.Visibility(xaml::Visibility::Visible);
	ShowCover(header_cover, entry.game.folder, 96);
	header_title.Text(winrt::to_hstring(entry.game.name));
	header_summary.Text(Summary(entry.progress));
	FillGrades(header_grades, entry.progress);
	select_hint.Text(L"Details");

	// A row per trophy: icon, name and description, then its grade and whether it is earned in columns of their own.
	trophy_list.Items().Clear();
	for (size_t p = 0; p < entry.packages.size(); p++) {
		const auto& package = entry.packages[p];
		if (entry.packages.size() > 1) {
			// Several packages (service labels): a heading for each.
			auto heading = Text(package.package.title.empty() ? L"Trophy package " + std::to_wstring(package.label)
			                                                  : std::wstring(winrt::to_hstring(package.package.title)));
			heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
			heading.Foreground(MediumBrush());
			heading.Margin({0, p == 0 ? 0.0 : 16.0, 0, 4});
			xaml::Controls::ListViewItem item;
			item.Content(heading);
			item.IsEnabled(false);
			trophy_list.Items().Append(item);
		}
		for (const auto& [id, trophy]: package.package.trophies) {
			const bool earned = package.unlocks.unlocked.contains(id);
			const bool masked = trophy.hidden && !earned;
			xaml::Controls::Grid row;
			row.Padding({0, 10, 0, 10});
			row.ColumnSpacing(16);
			row.ColumnDefinitions().Append(Column(xaml::GridLengthHelper::FromPixels(ICON_SIZE)));
			row.ColumnDefinitions().Append(Column(xaml::GridLengthHelper::FromValueAndType(1, xaml::GridUnitType::Star)));
			row.ColumnDefinitions().Append(Column(xaml::GridLengthHelper::FromPixels(112)));
			row.ColumnDefinitions().Append(Column(xaml::GridLengthHelper::FromPixels(176)));

			if (masked) {
				auto cup = Cup(0, 32);
				cup.Opacity(0.5);
				row.Children().Append(cup);
			} else {
				xaml::Controls::Image icon;
				icon.Width(ICON_SIZE);
				icon.Height(ICON_SIZE);
				icon.Opacity(earned ? 1.0 : 0.4); // locked trophies dimmed (the launcher greys them)
				icon.VerticalAlignment(xaml::VerticalAlignment::Center);
				ShowPng(icon, trophy.icon_png, 112);
				row.Children().Append(icon);
			}

			xaml::Controls::StackPanel text;
			text.Spacing(4);
			text.VerticalAlignment(xaml::VerticalAlignment::Center);
			auto name = Text(TrophyName(trophy, masked));
			name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
			name.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
			name.TextWrapping(xaml::TextWrapping::NoWrap);
			text.Children().Append(name);
			const auto description = masked ? std::wstring(L"Hidden until it is earned.") : std::wstring(winrt::to_hstring(trophy.description));
			if (!description.empty()) {
				auto line = Text(description);
				line.TextWrapping(xaml::TextWrapping::Wrap);
				line.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
				line.MaxLines(2);
				line.Foreground(MediumBrush());
				text.Children().Append(line);
			}
			xaml::Controls::Grid::SetColumn(text, 1);
			row.Children().Append(text);

			if (!masked) {
				xaml::Controls::StackPanel grade;
				grade.Orientation(xaml::Controls::Orientation::Horizontal);
				grade.Spacing(6);
				grade.VerticalAlignment(xaml::VerticalAlignment::Center);
				grade.Children().Append(Cup(trophy.grade, CUP_SIZE));
				grade.Children().Append(Text(GradeName(trophy.grade)));
				xaml::Controls::Grid::SetColumn(grade, 2);
				row.Children().Append(grade);
			}

			auto status = Text(earned ? EarnedText(package.unlocks, id) : std::wstring(L"Not earned"));
			status.Foreground(MediumBrush());
			status.TextAlignment(xaml::TextAlignment::Right);
			status.VerticalAlignment(xaml::VerticalAlignment::Center);
			xaml::Controls::Grid::SetColumn(status, 3);
			row.Children().Append(status);

			// Which trophy: the package and the trophy id.
			row.Tag(winrt::box_value((static_cast<uint32_t>(p) << 16u) | static_cast<uint32_t>(id)));
			trophy_list.Items().Append(row);
		}
	}

	games_list.Visibility(xaml::Visibility::Collapsed);
	trophy_list.Visibility(xaml::Visibility::Visible);
	trophy_list.UpdateLayout();
	trophy_list.Focus(xaml::FocusState::Keyboard);
}

void Page::Back() {
	if (trophy_list.Visibility() == xaml::Visibility::Visible) {
		trophy_list.Visibility(xaml::Visibility::Collapsed);
		trophy_list.Items().Clear();
		games_list.Visibility(xaml::Visibility::Visible);
		ShowOverviewHeader();
		FocusGame(open_game);
		return;
	}
	if (!left) {
		left = true;
		back();
	}
}


winrt::fire_and_forget LoadTrophies(std::shared_ptr<Page> page) {
	const auto dispatcher = page->root.Dispatcher();
	const int  language   = static_cast<int>(LoadSettings().console_language);
	co_await winrt::resume_background();
	auto games = LoadAllTrophies(language);
	co_await winrt::resume_foreground(dispatcher);
	if (page->left) {
		co_return;
	}
	page->games = std::move(games);
	page->ShowOverview();
}

} // namespace

xaml::UIElement CreateTrophiesPage(std::function<void()> back) {
	auto page  = std::make_shared<Page>();
	page->back = std::move(back);
	page->root = xaml::Markup::XamlReader::Load(PageMarkup).as<xaml::Controls::Grid>();
	const auto find   = [&page](const wchar_t* name) { return page->root.FindName(name); };
	page->header_cover   = find(L"HeaderCover").as<xaml::Controls::Border>();
	page->header_title   = find(L"HeaderTitle").as<xaml::Controls::TextBlock>();
	page->header_summary = find(L"HeaderSummary").as<xaml::Controls::TextBlock>();
	page->header_grades  = find(L"HeaderGrades").as<xaml::Controls::StackPanel>();
	page->status         = find(L"Status").as<xaml::Controls::StackPanel>();
	page->loading        = find(L"Loading").as<xaml::Controls::ProgressRing>();
	page->status_text    = find(L"StatusText").as<xaml::Controls::TextBlock>();
	page->games_list     = find(L"Games").as<xaml::Controls::ListView>();
	page->trophy_list    = find(L"TrophyList").as<xaml::Controls::ListView>();
	page->select_hint    = find(L"SelectHint").as<xaml::Controls::TextBlock>();

	page->games_list.ItemClick([weak = std::weak_ptr<Page>(page)](auto const&, xaml::Controls::ItemClickEventArgs const& args) {
		if (auto shared = weak.lock()) {
			const auto row = args.ClickedItem().try_as<xaml::FrameworkElement>();
			if (row != nullptr && row.Tag() != nullptr) {
				shared->ShowGame(winrt::unbox_value<uint32_t>(row.Tag()));
			}
		}
	});
	page->trophy_list.ItemClick([weak = std::weak_ptr<Page>(page)](auto const&, xaml::Controls::ItemClickEventArgs const& args) {
		auto shared = weak.lock();
		const auto card = args.ClickedItem().try_as<xaml::FrameworkElement>();
		if (shared == nullptr || card == nullptr || card.Tag() == nullptr) {
			return;
		}
		const uint32_t tag     = winrt::unbox_value<uint32_t>(card.Tag());
		const auto&    package = shared->games[shared->open_game].packages[tag >> 16u];
		const int      id      = static_cast<int>(tag & 0xffffu);
		const auto     trophy  = package.package.trophies.find(id);
		if (trophy == package.package.trophies.end()) {
			return;
		}
		const bool earned = package.unlocks.unlocked.contains(id);
		ShowDetails(trophy->second, earned, earned ? EarnedText(package.unlocks, id) : std::wstring());
	});

	// Back: B, Escape or the window's back button (shown while the page is open).
	page->root.KeyDown([weak = std::weak_ptr<Page>(page)](auto const&, xaml::Input::KeyRoutedEventArgs const& args) {
		if (args.Key() == winrt::Windows::System::VirtualKey::GamepadB || args.Key() == winrt::Windows::System::VirtualKey::Escape) {
			args.Handled(true);
			if (auto shared = weak.lock()) {
				shared->Back();
			}
		}
	});
	auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
	navigation.AppViewBackButtonVisibility(winrt::Windows::UI::Core::AppViewBackButtonVisibility::Visible);
	auto back_token = navigation.BackRequested(
	    [weak = std::weak_ptr<Page>(page)](auto const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
		    args.Handled(true);
		    if (auto shared = weak.lock()) {
			    shared->Back();
		    }
	    });
	// The page's state lives while the page is shown: this handler holds it and removes itself when the page goes.
	page->unloaded = page->root.Unloaded([page, navigation, back_token](auto const&, auto const&) {
		navigation.BackRequested(back_token);
		navigation.AppViewBackButtonVisibility(winrt::Windows::UI::Core::AppViewBackButtonVisibility::Collapsed);
		page->left = true;
		page->root.Unloaded(page->unloaded);
	});
	LoadTrophies(page);
	return page->root;
}

} // namespace Kyty::Uwp
