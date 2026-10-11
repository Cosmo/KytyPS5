#include "overlay.h"

#include "emulatorHost.h"
#include "fileAccess.h"
#include "input.h"
#include "settingTiles.h"
#include "settings.h"

#include <windows.h>
#include <fileapifromapp.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

constexpr auto   UPDATE_INTERVAL = std::chrono::milliseconds(250);
constexpr size_t GRAPH_FRAMES    = 120;
// The graph's range: a frame at 60 fps sits at a third of its height.
constexpr double GRAPH_MAX_MS = 50.0;
// The log lines kept; the settings choose how many of them show (overlay_log_lines, at least 2).
constexpr size_t LOG_LINES    = 20;
// The rates and the values per frame are over at least this many seconds.
constexpr double RATE_SECONDS = 1.0;
// The rows of the swap file in the statistics (OverlayMarkup): its group and its row in Waiting, and the gap above the group.
constexpr std::array<int32_t, 4> SWAP_ROWS {15, 16, 17, 28};
constexpr uint32_t               SWAP_GAP_ROW = 14;

constexpr wchar_t OverlayMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      IsHitTestVisible="False" Visibility="Collapsed" Padding="24">
  <Grid.Resources>
    <SolidColorBrush x:Key="PanelBrush" Color="#D91F1F1F" />
  </Grid.Resources>
  <Grid.RowDefinitions>
    <RowDefinition Height="Auto" />
    <RowDefinition Height="*" />
    <RowDefinition Height="Auto" />
  </Grid.RowDefinitions>

  <!-- Its parts by AppSettings::overlay_detail: the frame rate alone (in a box that fits it); with the graph and Compact; with the game,
       the graph and Statistics (Full). -->
  <Border x:Name="StatsPanel" Width="300" HorizontalAlignment="Left" VerticalAlignment="Top" Padding="12"
          Background="{StaticResource PanelBrush}">
    <StackPanel Spacing="6">
      <StackPanel x:Name="Header">
        <TextBlock x:Name="Game" Style="{StaticResource BodyTextBlockStyle}" FontWeight="SemiBold"
                   TextWrapping="NoWrap" TextTrimming="CharacterEllipsis" />
        <TextBlock x:Name="RunTime" Style="{StaticResource CaptionTextBlockStyle}"
                   Typography.NumeralAlignment="Tabular"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </StackPanel>
      <StackPanel Orientation="Horizontal" Spacing="10">
        <TextBlock x:Name="Fps" Style="{StaticResource SubtitleTextBlockStyle}" Typography.NumeralAlignment="Tabular" />
        <TextBlock x:Name="FrameTime" Style="{StaticResource BodyTextBlockStyle}" VerticalAlignment="Bottom"
                   Typography.NumeralAlignment="Tabular"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </StackPanel>
      <Grid x:Name="Graph" Height="48" Background="#14FFFFFF">
        <Line x:Name="Budget" Stroke="#40FFFFFF" StrokeThickness="1" StrokeDashArray="3,3" />
        <Polyline x:Name="Frames" Stroke="#7CC24D" StrokeThickness="1.5" />
      </Grid>
      <!-- Groups, labels, values; a gap between the groups. The rates and the values per frame are over the last second. -->
      <Grid x:Name="Statistics" ColumnSpacing="12">
        <Grid.Resources>
          <Style TargetType="TextBlock" BasedOn="{StaticResource CaptionTextBlockStyle}">
            <Setter Property="TextWrapping" Value="NoWrap" />
            <Setter Property="TextTrimming" Value="CharacterEllipsis" />
          </Style>
        </Grid.Resources>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="*" />
        </Grid.ColumnDefinitions>
        <Grid.RowDefinitions>
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
        </Grid.RowDefinitions>
        <TextBlock Grid.Row="0" Text="Frame" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="4" Text="Resolution" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="7" Text="System" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="10" Text="RAM" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="15" Text="Swap file" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="19" Text="Shaders" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="23" Text="Pipelines" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="27" Text="Waiting" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <!-- Draws made per frame; draws skipped per frame as not supported here (with the last reason); the GPU's time per frame. -->
        <TextBlock Grid.Row="0" Grid.Column="1" Text="Draws" />
        <TextBlock Grid.Row="1" Grid.Column="1" Text="Skipped" />
        <TextBlock Grid.Row="2" Grid.Column="1" Text="GPU time" />
        <!-- Output: the picture the game hands over each frame; Render: the largest viewport it draws its scene into. -->
        <TextBlock Grid.Row="4" Grid.Column="1" Text="Output" />
        <TextBlock Grid.Row="5" Grid.Column="1" Text="Render" />
        <TextBlock Grid.Row="7" Grid.Column="1" Text="CPU" />
        <TextBlock Grid.Row="8" Grid.Column="1" Text="Disk" />
        <!-- RAM in use and its parts: the game's memory in RAM, the GPU's memory (in RAM on the Xbox, on the graphics card on a PC) and the rest (the
             emulator and the app). -->
        <TextBlock Grid.Row="10" Grid.Column="1" Text="Used" />
        <TextBlock Grid.Row="11" Grid.Column="1" Text="Game" />
        <TextBlock Grid.Row="12" Grid.Column="1" Text="GPU" />
        <TextBlock Grid.Row="13" Grid.Column="1" Text="Other" />
        <!-- The game's memory in the swap file (not in RAM), written to it and read back from it. -->
        <TextBlock Grid.Row="15" Grid.Column="1" Text="Size" />
        <TextBlock Grid.Row="16" Grid.Column="1" Text="Out" />
        <TextBlock Grid.Row="17" Grid.Column="1" Text="In" />
        <!-- The game's shaders so far, and the shaders translated in this run (not taken from the cache an earlier run saved). -->
        <TextBlock Grid.Row="19" Grid.Column="1" Text="Drawing" />
        <TextBlock Grid.Row="20" Grid.Column="1" Text="Compute" />
        <TextBlock Grid.Row="21" Grid.Column="1" Text="New" />
        <!-- The pipelines this run made, those of them not made from the cache, and the cache file's. -->
        <TextBlock Grid.Row="23" Grid.Column="1" Text="This run" />
        <TextBlock Grid.Row="24" Grid.Column="1" Text="New" />
        <TextBlock Grid.Row="25" Grid.Column="1" Text="In cache" />
        <!-- The time per second the work stood still for compiling shaders and making pipelines, and for the swap file. -->
        <TextBlock Grid.Row="27" Grid.Column="1" Text="Compiling" />
        <TextBlock Grid.Row="28" Grid.Column="1" Text="Swap file" />
        <TextBlock x:Name="Draws" Grid.Row="0" Grid.Column="2" />
        <TextBlock x:Name="Skipped" Grid.Row="1" Grid.Column="2" />
        <TextBlock x:Name="GpuTime" Grid.Row="2" Grid.Column="2" />
        <TextBlock x:Name="OutputResolution" Grid.Row="4" Grid.Column="2" Typography.NumeralAlignment="Tabular" />
        <TextBlock x:Name="RenderResolution" Grid.Row="5" Grid.Column="2" Typography.NumeralAlignment="Tabular" />
        <TextBlock x:Name="Cpu" Grid.Row="7" Grid.Column="2" />
        <TextBlock x:Name="Disk" Grid.Row="8" Grid.Column="2" />
        <TextBlock x:Name="Memory" Grid.Row="10" Grid.Column="2" />
        <TextBlock x:Name="GameMemory" Grid.Row="11" Grid.Column="2" />
        <TextBlock x:Name="GpuMemory" Grid.Row="12" Grid.Column="2" />
        <TextBlock x:Name="OtherMemory" Grid.Row="13" Grid.Column="2" />
        <TextBlock x:Name="SwappedMemory" Grid.Row="15" Grid.Column="2" />
        <TextBlock x:Name="SwapOut" Grid.Row="16" Grid.Column="2" />
        <TextBlock x:Name="SwapIn" Grid.Row="17" Grid.Column="2" />
        <TextBlock x:Name="DrawingShaders" Grid.Row="19" Grid.Column="2" />
        <TextBlock x:Name="ComputeShaders" Grid.Row="20" Grid.Column="2" />
        <TextBlock x:Name="NewShaders" Grid.Row="21" Grid.Column="2" />
        <TextBlock x:Name="Pipelines" Grid.Row="23" Grid.Column="2" />
        <TextBlock x:Name="NewPipelines" Grid.Row="24" Grid.Column="2" />
        <TextBlock x:Name="SavedPipelines" Grid.Row="25" Grid.Column="2" />
        <TextBlock x:Name="CompileWait" Grid.Row="27" Grid.Column="2" />
        <TextBlock x:Name="SwapWait" Grid.Row="28" Grid.Column="2" />
      </Grid>
      <!-- Compact: the GPU's time per frame, RAM in use and the game's memory in the swap file (where there is one), as in Statistics. -->
      <Grid x:Name="Compact" ColumnSpacing="12" Visibility="Collapsed">
        <Grid.Resources>
          <Style TargetType="TextBlock" BasedOn="{StaticResource CaptionTextBlockStyle}">
            <Setter Property="TextWrapping" Value="NoWrap" />
            <Setter Property="TextTrimming" Value="CharacterEllipsis" />
          </Style>
        </Grid.Resources>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="*" />
        </Grid.ColumnDefinitions>
        <Grid.RowDefinitions>
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
        </Grid.RowDefinitions>
        <TextBlock Grid.Row="0" Text="GPU time" />
        <TextBlock Grid.Row="1" Text="RAM" />
        <TextBlock x:Name="CompactSwapLabel" Grid.Row="2" Text="Swap file" />
        <TextBlock x:Name="CompactGpuTime" Grid.Row="0" Grid.Column="1" />
        <TextBlock x:Name="CompactMemory" Grid.Row="1" Grid.Column="1" />
        <TextBlock x:Name="CompactSwap" Grid.Row="2" Grid.Column="1" />
      </Grid>
    </StackPanel>
  </Border>

  <Border x:Name="ControllersPanel" MinWidth="220" HorizontalAlignment="Right" VerticalAlignment="Top" Padding="12"
          Background="{StaticResource PanelBrush}">
    <StackPanel Spacing="4">
      <TextBlock Text="Controllers" Style="{StaticResource CaptionTextBlockStyle}"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <!-- One row per controller: its number, the controller symbol and its name, each in a column. -->
      <Grid x:Name="Controllers" ColumnSpacing="8" RowSpacing="2">
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="Auto" />
        </Grid.ColumnDefinitions>
      </Grid>
    </StackPanel>
  </Border>

  <Border x:Name="LogPanel" Grid.Row="2" Padding="12,8" Background="{StaticResource PanelBrush}">
    <TextBlock x:Name="Log" FontFamily="Consolas" FontSize="12" LineHeight="16" TextWrapping="NoWrap"
               TextTrimming="CharacterEllipsis" Foreground="#D8DCE3" />
  </Border>
</Grid>)xaml";

// The end of the emulator's log, read as it grows.
class LogTail {
public:
	LogTail() { Open(); }
	~LogTail() {
		if (m_file != INVALID_HANDLE_VALUE) {
			CloseHandle(m_file);
		}
	}
	LogTail(const LogTail&)            = delete;
	LogTail& operator=(const LogTail&) = delete;

	// The last lines, after reading what was added.
	const std::deque<std::string>& Update() {
		// The log may not exist yet (the first run, or deleted): tried again until it does.
		if (m_file == INVALID_HANDLE_VALUE && !Open()) {
			return m_lines;
		}
		// The overlay opens the log before the emulator starts it anew (the last run's log until
		// then): a new log (the host says so, or the file got shorter) is read from its end. When
		// much was added, only its end matters (the emulator may log thousands of lines a second).
		LARGE_INTEGER  size {};
		LARGE_INTEGER  position {};
		const uint32_t generation = EmulatorHost::Get().LogGeneration();
		const bool     new_log    = generation != m_generation;
		if (GetFileSizeEx(m_file, &size) &&
		    SetFilePointerEx(m_file, LARGE_INTEGER {}, &position, FILE_CURRENT) &&
		    (new_log || size.QuadPart < position.QuadPart ||
		     size.QuadPart - position.QuadPart > MAX_UNREAD)) {
			if (new_log || size.QuadPart < position.QuadPart) {
				m_generation = generation;
				m_lines.clear();
			}
			LARGE_INTEGER start {};
			start.QuadPart = std::max<int64_t>(0, size.QuadPart - 4096);
			SetFilePointerEx(m_file, start, nullptr, FILE_BEGIN);
			m_partial.clear();
			m_escape            = Escape::None;
			m_skip_partial_line = start.QuadPart != 0;
		}
		char  buffer[16384];
		DWORD read = 0;
		while (ReadFile(m_file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
			for (DWORD i = 0; i < read; i++) {
				const char c = buffer[i];
				// Without the terminal colours (ESC [ ... letter).
				if (c == '\x1b') {
					m_escape = Escape::Start;
					continue;
				}
				if (m_escape == Escape::Start) {
					m_escape = c == '[' ? Escape::Sequence : Escape::None;
					continue;
				}
				if (m_escape == Escape::Sequence) {
					if (c >= '@' && c <= '~') {
						m_escape = Escape::None;
					}
					continue;
				}
				if (c == '\n') {
					if (!m_skip_partial_line && !m_partial.empty()) {
						// The game's running time when the line was read, so that one can tell how recent it is.
						const auto seconds = EmulatorHost::Get().RunTimeMs() / 1000;
						char       stamp[24];
						std::snprintf(stamp, sizeof(stamp), "%llu:%02llu  ", static_cast<unsigned long long>(seconds / 60),
						              static_cast<unsigned long long>(seconds % 60));
						m_lines.push_back(stamp + std::move(m_partial));
						if (m_lines.size() > LOG_LINES) {
							m_lines.pop_front();
						}
					}
					m_partial.clear();
					m_skip_partial_line = false;
				} else if (c != '\r' && m_partial.size() < 512) {
					m_partial += c;
				}
			}
		}
		return m_lines;
	}

private:
	bool Open() {
		m_file = CreateFile2FromAppW((LocalStateFolder() / L"kyty-emulator.txt").c_str(), GENERIC_READ,
		                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                             OPEN_EXISTING, nullptr);
		if (m_file == INVALID_HANDLE_VALUE) {
			return false;
		}
		// Start with the last few kilobytes rather than the whole log.
		LARGE_INTEGER size {};
		GetFileSizeEx(m_file, &size);
		LARGE_INTEGER start {};
		start.QuadPart = std::max<int64_t>(0, size.QuadPart - 4096);
		SetFilePointerEx(m_file, start, nullptr, FILE_BEGIN);
		m_skip_partial_line = start.QuadPart != 0;
		return true;
	}

	static constexpr int64_t MAX_UNREAD = 64 * 1024;

	enum class Escape { None, Start, Sequence };

	HANDLE                  m_file       = INVALID_HANDLE_VALUE;
	uint32_t                m_generation = EmulatorHost::Get().LogGeneration();
	Escape                  m_escape     = Escape::None;
	std::deque<std::string> m_lines;
	std::string             m_partial;
	bool                    m_skip_partial_line = false;
};

std::wstring Megabytes(uint64_t bytes) {
	return std::to_wstring((bytes + (1u << 19)) >> 20) + L" MB";
}

// "M:SS", or "H:MM:SS" after an hour.
std::wstring Duration(uint64_t ms) {
	const auto seconds = ms / 1000;
	wchar_t    text[32];
	if (seconds >= 3600) {
		swprintf_s(text, L"%llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60);
	} else {
		swprintf_s(text, L"%llu:%02llu", seconds / 60, seconds % 60);
	}
	return text;
}

// The overlay's scale for AppSettings::overlay_size (small, medium, large). Large is the system's
// size, which the Xbox doubles for the TV.
constexpr std::array<double, 3> OverlaySizes {0.6, 0.8, 1.0};
// The width of the performance box (StatsPanel) when it shows more than the frame rate.
constexpr double STATS_WIDTH = 300.0;

xaml::Visibility VisibleIf(bool shown) {
	return shown ? xaml::Visibility::Visible : xaml::Visibility::Collapsed;
}

} // namespace

struct GameOverlay::State {
	xaml::Controls::Grid       page {nullptr};
	xaml::Controls::Grid       root {nullptr};
	xaml::Controls::Border     stats_panel {nullptr};
	xaml::UIElement            header {nullptr};
	xaml::Controls::Grid       compact {nullptr};
	xaml::Controls::TextBlock  compact_gpu_time {nullptr};
	xaml::Controls::TextBlock  compact_memory {nullptr};
	xaml::UIElement            compact_swap_label {nullptr};
	xaml::Controls::TextBlock  compact_swap {nullptr};
	xaml::UIElement            controllers_panel {nullptr};
	xaml::UIElement            log_panel {nullptr};
	uint32_t                   detail            = OVERLAY_FULL;
	bool                       log_shown         = true;
	bool                       controllers_shown = true;
	double                     scale             = 1.0;
	xaml::Controls::TextBlock  run_time {nullptr};
	xaml::Controls::TextBlock  fps {nullptr};
	xaml::Controls::TextBlock  frame_time {nullptr};
	xaml::Controls::Grid       graph {nullptr};
	xaml::Shapes::Line         budget {nullptr};
	xaml::Shapes::Polyline     frames {nullptr};
	xaml::Controls::Grid       statistics {nullptr};
	xaml::Controls::TextBlock  draws {nullptr};
	xaml::Controls::TextBlock  skipped {nullptr};
	xaml::Controls::TextBlock  gpu_time {nullptr};
	xaml::Controls::TextBlock  output_resolution {nullptr};
	xaml::Controls::TextBlock  render_resolution {nullptr};
	xaml::Controls::TextBlock  cpu {nullptr};
	xaml::Controls::TextBlock  disk {nullptr};
	xaml::Controls::TextBlock  memory {nullptr};
	xaml::Controls::TextBlock  game_memory {nullptr};
	xaml::Controls::TextBlock  gpu_memory {nullptr};
	xaml::Controls::TextBlock  other_memory {nullptr};
	xaml::Controls::TextBlock  swapped_memory {nullptr};
	xaml::Controls::TextBlock  swap_out {nullptr};
	xaml::Controls::TextBlock  swap_in {nullptr};
	xaml::Controls::TextBlock  drawing_shaders {nullptr};
	xaml::Controls::TextBlock  compute_shaders {nullptr};
	xaml::Controls::TextBlock  new_shaders {nullptr};
	xaml::Controls::TextBlock  pipelines {nullptr};
	xaml::Controls::TextBlock  new_pipelines {nullptr};
	xaml::Controls::TextBlock  saved_pipelines {nullptr};
	xaml::Controls::TextBlock  compile_wait {nullptr};
	xaml::Controls::TextBlock  swap_wait {nullptr};
	xaml::Controls::Grid       controllers {nullptr};
	xaml::Controls::TextBlock  log {nullptr};
	size_t                     log_lines = LOG_LINES;
	bool                       headset_mic_option = false; // the settings page's: the headset microphone as the controller's
	xaml::DispatcherTimer      timer {nullptr};
	std::unique_ptr<LogTail>   log_tail;
	// The process's CPU time (100 ns units) and the time, at the last update.
	uint64_t                              cpu_time = 0;
	std::chrono::steady_clock::time_point cpu_time_at;
	// The totals at the start of the second the rates and the values per frame are over.
	struct Sample {
		std::chrono::steady_clock::time_point time;
		uint64_t                              frames = 0;
		WorkTotals                            work;
		MemoryTotals                          memory;
	};
	std::optional<Sample> sample;
	bool                  swap_shown = true;

	void Update();
	void UpdateRates(const MemoryTotals& totals);
	void UpdateControllers();
	void ShowSwapFile(bool shown);
	void SetScale(double value);
	void Fit(double width, double height);
};

void GameOverlay::State::Update() {
	run_time.Text(L"Running  " + Duration(EmulatorHost::Get().RunTimeMs()));

	// Frame rate and frame times, from the frames presented.
	const auto times = EmulatorHost::Get().RecentFrameTimes(GRAPH_FRAMES);
	if (!times.empty()) {
		double second = 0.0;
		size_t count  = 0;
		for (auto it = times.rbegin(); it != times.rend() && second < 1000.0; ++it, count++) {
			second += *it;
		}
		const double average = second / static_cast<double>(count);
		fps.Text(std::to_wstring(std::lround(1000.0 / average)) + L" fps");
		wchar_t text[32];
		swprintf_s(text, L"%.1f", std::min(average, 999.9));
		frame_time.Text(std::wstring(text) + L" ms");
	}
	const double width  = graph.ActualWidth();
	const double height = graph.ActualHeight();
	const double budget_y = height - height * (1000.0 / 60.0) / GRAPH_MAX_MS;
	budget.X1(0);
	budget.X2(width);
	budget.Y1(budget_y);
	budget.Y2(budget_y);
	winrt::Windows::UI::Xaml::Media::PointCollection points;
	for (size_t i = 0; i < times.size(); i++) {
		const double x = width * static_cast<double>(i + GRAPH_FRAMES - times.size()) / (GRAPH_FRAMES - 1);
		const double y = height - height * std::min(times[i], GRAPH_MAX_MS) / GRAPH_MAX_MS;
		points.Append({static_cast<float>(x), static_cast<float>(y)});
	}
	frames.Points(points);

	// Resolutions of the last frame: "-" before the first flip, and for render when the frame drew no scene (a menu).
	const auto resolutions = CurrentFrameResolutions();
	const auto size        = [](uint32_t width, uint32_t height) {
		return width == 0 || height == 0 ? std::wstring(L"\u2013") : std::to_wstring(width) + L" \u00d7 " + std::to_wstring(height);
	};
	output_resolution.Text(size(resolutions.output_width, resolutions.output_height));
	render_resolution.Text(size(resolutions.render_width, resolutions.render_height));

	// Memory: on Xbox the limit is the app's budget; on a PC it's no real limit.
	const uint64_t used  = winrt::Windows::System::MemoryManager::AppMemoryUsage();
	const uint64_t limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
	memory.Text(Megabytes(used) +
	            (limit < (uint64_t {64} << 30) ? L" of " + Megabytes(limit) : std::wstring()));

	// Its parts. The GPU's memory is part of RAM on the Xbox; on a PC it is on the graphics card, so the rest is RAM minus the game's memory there.
	const auto totals = CurrentMemoryTotals();
	game_memory.Text(Megabytes(totals.game));
	gpu_memory.Text(Megabytes(totals.gpu) + (IsXbox() ? std::wstring() : std::wstring(L" (graphics card)")));
	const uint64_t parts = totals.game + (IsXbox() ? totals.gpu : 0);
	other_memory.Text(Megabytes(used > parts ? used - parts : 0));
	// The swap file, where there is one (the pager: by default on the Xbox only).
	ShowSwapFile(totals.pager);
	swapped_memory.Text(Megabytes(totals.swapped));
	UpdateRates(totals);

	// CPU: the process's share of all processor cores since the last update.
	FILETIME created {}, exited {}, kernel {}, user {};
	if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0) {
		const auto time = [](FILETIME t) {
			return (static_cast<uint64_t>(t.dwHighDateTime) << 32u) | t.dwLowDateTime;
		};
		const uint64_t now_cpu = time(kernel) + time(user);
		const auto     now     = std::chrono::steady_clock::now();
		if (cpu_time != 0) {
			const double wall = std::chrono::duration<double>(now - cpu_time_at).count() * 1e7 *
			                    std::max(1u, std::thread::hardware_concurrency());
			const double share = wall > 0.0 ? static_cast<double>(now_cpu - cpu_time) / wall : 0.0;
			const auto   percent = static_cast<uint64_t>(std::lround(std::min(share, 1.0) * 100.0));
			cpu.Text(std::to_wstring(percent) + L" %");
		}
		cpu_time    = now_cpu;
		cpu_time_at = now;
	}

	if (const auto statistics = CurrentShaderStatistics()) {
		drawing_shaders.Text(std::to_wstring(statistics->drawing_shaders));
		compute_shaders.Text(std::to_wstring(statistics->compute_shaders));
		new_shaders.Text(std::to_wstring(statistics->new_shaders));
		pipelines.Text(std::to_wstring(statistics->pipelines));
		new_pipelines.Text(std::to_wstring(statistics->pipelines - std::min(statistics->cached_pipelines, statistics->pipelines)));
		saved_pipelines.Text(std::to_wstring(statistics->saved_pipelines));
	}
	if (detail == OVERLAY_COMPACT) {
		compact_gpu_time.Text(gpu_time.Text());
		compact_memory.Text(memory.Text());
		compact_swap.Text(swapped_memory.Text());
	}

	// The parts the settings hide are not filled; the log is read again from its end when it shows again.
	if (controllers_shown) {
		UpdateControllers();
	}
	if (log_shown) {
		std::wstring text;
		const auto&  lines = log_tail->Update();
		for (size_t i = lines.size() > log_lines ? lines.size() - log_lines : 0; i < lines.size(); i++) {
			text += (text.empty() ? L"" : L"\n") + std::wstring(winrt::to_hstring(lines[i]));
		}
		log.Text(text);
	}
}

// One row per connected controller: its player number, the controller symbol, its name and, with a headset, the headset symbol.
void GameOverlay::State::UpdateControllers() {
	controllers.Children().Clear();
	controllers.RowDefinitions().Clear();
	const auto connected = ConnectedControllers();
	const auto add_row   = [this]() {
		xaml::Controls::RowDefinition row;
		row.Height(xaml::GridLengthHelper::Auto());
		controllers.RowDefinitions().Append(row);
		return static_cast<int32_t>(controllers.RowDefinitions().Size()) - 1;
	};
	const auto place = [this](xaml::FrameworkElement const& element, int32_t row, int32_t column) {
		xaml::Controls::Grid::SetRow(element, row);
		xaml::Controls::Grid::SetColumn(element, column);
		controllers.Children().Append(element);
	};
	if (connected.empty()) {
		xaml::Controls::TextBlock none;
		none.Text(L"None connected");
		xaml::Controls::Grid::SetColumnSpan(none, 3);
		place(none, add_row(), 0);
	}
	const auto symbol = GetControllerSymbol();
	for (const auto& controller: connected) {
		const int32_t             row = add_row();
		xaml::Controls::TextBlock number;
		number.Text(L"#" + std::to_wstring(controller.player + 1));
		xaml::Documents::Typography::SetNumeralAlignment(number, xaml::FontNumeralAlignment::Tabular);
		place(number, row, 0);
		xaml::Controls::TextBlock icon;
		icon.FontFamily(xaml::Media::FontFamily(symbol.font));
		icon.Text(symbol.glyph);
		icon.VerticalAlignment(xaml::VerticalAlignment::Center);
		place(icon, row, 1);
		xaml::Controls::TextBlock name;
		name.Text(controller.name);
		place(name, row, 2);
		if (controller.headset_mic) {
			xaml::Controls::TextBlock headset;
			headset.FontFamily(xaml::Media::FontFamily(HEADSET_SYMBOL_FONT));
			headset.Text(HEADSET_SYMBOL);
			headset.VerticalAlignment(xaml::VerticalAlignment::Center);
			headset.Opacity(headset_mic_option ? 1.0 : 0.4);
			place(headset, row, 3);
		}
	}
}

// The rates and the values per frame, over the last second (they stay until the next second is over).
void GameOverlay::State::UpdateRates(const MemoryTotals& totals) {
	const Sample current {std::chrono::steady_clock::now(), EmulatorHost::Get().FramesPresented(), CurrentWorkTotals(), totals};
	if (!sample) {
		sample = current;
		return;
	}
	const double seconds = std::chrono::duration<double>(current.time - sample->time).count();
	if (seconds < RATE_SECONDS) {
		return;
	}
	const auto& before        = *sample;
	const auto  delta         = [](uint64_t now, uint64_t then) { return now > then ? now - then : 0; };
	const auto  mb_per_second = [seconds](uint64_t bytes) {
		return std::to_wstring(std::lround(static_cast<double>(bytes) / 1048576.0 / seconds)) + L" MB/s";
	};
	const auto ms_per_second = [seconds](uint64_t ns) { return std::to_wstring(std::lround(static_cast<double>(ns) / 1e6 / seconds)) + L" ms/s"; };

	// Per frame: none when no frame came in that second (the game loads, or waits).
	const uint64_t frames = delta(current.frames, before.frames);
	if (frames == 0) {
		draws.Text(L"–");
		skipped.Text(L"–");
		gpu_time.Text(L"–");
	} else {
		const auto per_frame = [frames](uint64_t value) { return static_cast<double>(value) / static_cast<double>(frames); };
		draws.Text(std::to_wstring(std::lround(per_frame(delta(current.work.draws, before.work.draws)))));
		const uint64_t skips = delta(current.work.skipped_draws, before.work.skipped_draws);
		std::wstring   skip_text = skips == 0 ? L"0" : per_frame(skips) < 0.5 ? L"<1" : std::to_wstring(std::lround(per_frame(skips)));
		if (skips != 0 && current.work.skip_reason != nullptr) {
			const std::string_view reason = current.work.skip_reason;
			skip_text += L"  (" + std::wstring(reason.begin(), reason.end()) + L")";
		}
		skipped.Text(skip_text);
		wchar_t text[32];
		swprintf_s(text, L"%.1f ms", per_frame(delta(current.work.gpu_ns, before.work.gpu_ns)) / 1e6);
		gpu_time.Text(text);
	}
	disk.Text(mb_per_second(delta(current.work.bytes_read, before.work.bytes_read)) + L" read");
	swap_out.Text(mb_per_second(delta(totals.swapped_out, before.memory.swapped_out)));
	swap_in.Text(mb_per_second(delta(totals.swapped_in, before.memory.swapped_in)));
	compile_wait.Text(ms_per_second(delta(current.work.compile_ns, before.work.compile_ns)));
	swap_wait.Text(ms_per_second(delta(totals.swap_wait_ns, before.memory.swap_wait_ns)));
	sample = current;
}

// The swap file's group and its row in Waiting, shown only where there is a swap file.
void GameOverlay::State::ShowSwapFile(bool shown) {
	if (shown == swap_shown) {
		return;
	}
	swap_shown            = shown;
	const auto visibility = shown ? xaml::Visibility::Visible : xaml::Visibility::Collapsed;
	for (const auto& child: statistics.Children()) {
		const auto row = xaml::Controls::Grid::GetRow(child.as<xaml::FrameworkElement>());
		if (std::find(SWAP_ROWS.begin(), SWAP_ROWS.end(), row) != SWAP_ROWS.end()) {
			child.Visibility(visibility);
		}
	}
	statistics.RowDefinitions().GetAt(SWAP_GAP_ROW).Height(xaml::GridLengthHelper::FromPixels(shown ? 8.0 : 0.0));
	compact_swap_label.Visibility(visibility);
	compact_swap.Visibility(visibility);
}

// The overlay's size: below the system's size, laid out over the page enlarged by 1 / scale, then scaled down to fit.
void GameOverlay::State::SetScale(double value) {
	if (value == scale) {
		return;
	}
	scale = value;
	if (scale == 1.0) {
		root.RenderTransform(nullptr);
		root.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
		root.VerticalAlignment(xaml::VerticalAlignment::Stretch);
		root.ClearValue(xaml::FrameworkElement::WidthProperty());
		root.ClearValue(xaml::FrameworkElement::HeightProperty());
		return;
	}
	xaml::Media::ScaleTransform transform;
	transform.ScaleX(scale);
	transform.ScaleY(scale);
	root.RenderTransform(transform);
	root.HorizontalAlignment(xaml::HorizontalAlignment::Left);
	root.VerticalAlignment(xaml::VerticalAlignment::Top);
	Fit(page.ActualWidth(), page.ActualHeight());
}

void GameOverlay::State::Fit(double width, double height) {
	if (scale != 1.0 && width > 0.0 && height > 0.0) {
		root.Width(width / scale);
		root.Height(height / scale);
	}
}

GameOverlay::GameOverlay(xaml::Controls::Grid const& page, uint32_t index,
                         winrt::hstring const& game_name)
    : m_state(std::make_shared<State>()) {
	auto& state = *m_state;
	state.page  = page;
	state.root  = xaml::Markup::XamlReader::Load(OverlayMarkup).as<xaml::Controls::Grid>();
	const auto find = [&state](const wchar_t* name) { return state.root.FindName(name); };
	find(L"Game").as<xaml::Controls::TextBlock>().Text(game_name);
	state.stats_panel        = find(L"StatsPanel").as<xaml::Controls::Border>();
	state.header             = find(L"Header").as<xaml::UIElement>();
	state.compact            = find(L"Compact").as<xaml::Controls::Grid>();
	state.compact_gpu_time   = find(L"CompactGpuTime").as<xaml::Controls::TextBlock>();
	state.compact_memory     = find(L"CompactMemory").as<xaml::Controls::TextBlock>();
	state.compact_swap_label = find(L"CompactSwapLabel").as<xaml::UIElement>();
	state.compact_swap       = find(L"CompactSwap").as<xaml::Controls::TextBlock>();
	state.controllers_panel  = find(L"ControllersPanel").as<xaml::UIElement>();
	state.log_panel          = find(L"LogPanel").as<xaml::UIElement>();
	state.run_time         = find(L"RunTime").as<xaml::Controls::TextBlock>();
	state.fps              = find(L"Fps").as<xaml::Controls::TextBlock>();
	state.frame_time       = find(L"FrameTime").as<xaml::Controls::TextBlock>();
	state.graph            = find(L"Graph").as<xaml::Controls::Grid>();
	state.budget           = find(L"Budget").as<xaml::Shapes::Line>();
	state.frames           = find(L"Frames").as<xaml::Shapes::Polyline>();
	state.statistics        = find(L"Statistics").as<xaml::Controls::Grid>();
	state.draws             = find(L"Draws").as<xaml::Controls::TextBlock>();
	state.skipped           = find(L"Skipped").as<xaml::Controls::TextBlock>();
	state.gpu_time          = find(L"GpuTime").as<xaml::Controls::TextBlock>();
	state.output_resolution = find(L"OutputResolution").as<xaml::Controls::TextBlock>();
	state.render_resolution = find(L"RenderResolution").as<xaml::Controls::TextBlock>();
	state.cpu               = find(L"Cpu").as<xaml::Controls::TextBlock>();
	state.disk              = find(L"Disk").as<xaml::Controls::TextBlock>();
	state.memory            = find(L"Memory").as<xaml::Controls::TextBlock>();
	state.game_memory       = find(L"GameMemory").as<xaml::Controls::TextBlock>();
	state.gpu_memory        = find(L"GpuMemory").as<xaml::Controls::TextBlock>();
	state.other_memory      = find(L"OtherMemory").as<xaml::Controls::TextBlock>();
	state.swapped_memory    = find(L"SwappedMemory").as<xaml::Controls::TextBlock>();
	state.swap_out          = find(L"SwapOut").as<xaml::Controls::TextBlock>();
	state.swap_in           = find(L"SwapIn").as<xaml::Controls::TextBlock>();
	state.drawing_shaders   = find(L"DrawingShaders").as<xaml::Controls::TextBlock>();
	state.compute_shaders   = find(L"ComputeShaders").as<xaml::Controls::TextBlock>();
	state.new_shaders       = find(L"NewShaders").as<xaml::Controls::TextBlock>();
	state.pipelines         = find(L"Pipelines").as<xaml::Controls::TextBlock>();
	state.new_pipelines     = find(L"NewPipelines").as<xaml::Controls::TextBlock>();
	state.saved_pipelines   = find(L"SavedPipelines").as<xaml::Controls::TextBlock>();
	state.compile_wait      = find(L"CompileWait").as<xaml::Controls::TextBlock>();
	state.swap_wait         = find(L"SwapWait").as<xaml::Controls::TextBlock>();
	state.controllers      = find(L"Controllers").as<xaml::Controls::Grid>();
	state.log              = find(L"Log").as<xaml::Controls::TextBlock>();
	page.Children().InsertAt(index, state.root);
	page.SizeChanged([weak = std::weak_ptr<State>(m_state)](auto const&, xaml::SizeChangedEventArgs const& args) {
		if (auto shared = weak.lock()) {
			shared->Fit(args.NewSize().Width, args.NewSize().Height);
		}
	});

	state.timer = xaml::DispatcherTimer();
	state.timer.Interval(UPDATE_INTERVAL);
	state.timer.Tick([weak = std::weak_ptr<State>(m_state)](auto const&, auto const&) {
		if (auto shared = weak.lock()) {
			shared->Update();
		}
	});
	Apply(LoadSettings());
}

GameOverlay::~GameOverlay() {
	m_state->timer.Stop();
}

void GameOverlay::Show(bool shown) {
	auto& state = *m_state;
	state.root.Visibility(shown ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
	if (shown) {
		if (state.log_tail == nullptr) {
			state.log_tail = std::make_unique<LogTail>();
		}
		state.Update();
		state.timer.Start();
	} else {
		state.timer.Stop();
	}
}

bool GameOverlay::Shown() const {
	return m_state->root.Visibility() == xaml::Visibility::Visible;
}

void GameOverlay::Apply(const AppSettings& settings) {
	auto& state              = *m_state;
	state.detail             = std::min(settings.overlay_detail, OVERLAY_FULL);
	state.log_shown          = settings.overlay_log;
	state.controllers_shown  = settings.overlay_controllers;
	state.log_lines          = std::clamp<size_t>(settings.overlay_log_lines, 2, LOG_LINES);
	state.headset_mic_option = settings.headset_mic;
	state.header.Visibility(VisibleIf(state.detail == OVERLAY_FULL));
	state.graph.Visibility(VisibleIf(state.detail != OVERLAY_FRAME_RATE));
	state.statistics.Visibility(VisibleIf(state.detail == OVERLAY_FULL));
	state.compact.Visibility(VisibleIf(state.detail == OVERLAY_COMPACT));
	state.stats_panel.Width(state.detail == OVERLAY_FRAME_RATE ? std::numeric_limits<double>::quiet_NaN() : STATS_WIDTH);
	state.controllers_panel.Visibility(VisibleIf(state.controllers_shown));
	state.log_panel.Visibility(VisibleIf(state.log_shown));
	state.SetScale(OverlaySizes[std::min<size_t>(settings.overlay_size, OverlaySizes.size() - 1)]);
	if (Shown()) {
		state.Update();
	}
}

void AddOverlaySettings(xaml::Controls::Panel const& panel, xaml::Controls::StackPanel const& explanation, std::shared_ptr<AppSettings> settings,
                        std::function<void()> changed) {
	AddChoice(
	    panel, explanation, L"Overlay", {L"Off", L"Frame rate", L"Compact", L"Full"},
	    OverlayShown() ? 1 + std::min(settings->overlay_detail, OVERLAY_FULL) : 0,
	    [settings, changed](size_t index) {
		    SetOverlayShown(index != 0);
		    if (index != 0) {
			    settings->overlay_detail = static_cast<uint32_t>(index - 1);
		    }
		    changed();
	    },
	    {L"What the overlay shows over games.",
	     L"Frame rate: the frames per second and the time per frame. Compact: also a graph of the last frames, the GPU's time per frame, RAM and "
	     L"the swap file. Full: everything, from the draws to the shaders.",
	     L"To turn it off and on during a game, pull both triggers while pressing View and Menu together."});
	// Off, or 2 to 20 lines in steps of 2 (overlay_log and overlay_log_lines; an odd number from the settings file shows as the next even one).
	{
		constexpr auto            MAX_LINES = static_cast<uint32_t>(LOG_LINES);
		std::vector<std::wstring> lines {L"Off"};
		for (uint32_t count = 2; count <= MAX_LINES; count += 2) {
			lines.push_back(std::to_wstring(count) + L" lines");
		}
		const size_t selected = settings->overlay_log ? (std::clamp<uint32_t>(settings->overlay_log_lines, 2, MAX_LINES) + 1) / 2 : 0;
		AddChoice(
		    panel, explanation, L"Log on screen", std::move(lines), selected,
		    [settings, changed](size_t index) {
			    settings->overlay_log = index != 0;
			    if (index != 0) {
				    settings->overlay_log_lines = static_cast<uint32_t>(index) * 2;
			    }
			    changed();
		    },
		    {L"The latest lines of the emulator's log, under the game.", L"The log file is written either way."});
	}
	AddSwitch(
	    panel, explanation, L"Controllers on screen", settings->overlay_controllers,
	    [settings, changed](bool on) {
		    settings->overlay_controllers = on;
		    changed();
	    },
	    {L"The connected controllers and their player numbers, at the top right."});
	AddChoice(
	    panel, explanation, L"Overlay size", {L"Small", L"Medium", L"Large"}, std::min<uint32_t>(settings->overlay_size, 2),
	    [settings, changed](size_t index) {
		    settings->overlay_size = static_cast<uint32_t>(index);
		    changed();
	    },
	    {L"The size of the overlay over games."});
}

} // namespace Kyty::Uwp
