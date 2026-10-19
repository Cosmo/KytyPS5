#include "overlay.h"

#include "emulatorHost.h"
#include "fileAccess.h"
#include "input.h"
#include "settingTiles.h"
#include "settings.h"

#include <windows.h>
#include <fileapifromapp.h>
// windows.h's FindText macro breaks Windows.UI.Text.h.
#undef FindText

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
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
constexpr std::array<int32_t, 4> SWAP_ROWS {16, 17, 18, 29};
constexpr uint32_t               SWAP_GAP_ROW = 15;

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
       the graph and Statistics (Full); with the game and the graph, and the Detailed panels beside it (BuildDetailed). -->
  <StackPanel x:Name="TopRow" Orientation="Horizontal" Spacing="12" HorizontalAlignment="Left" VerticalAlignment="Top">
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
        <TextBlock Grid.Row="8" Text="System" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="11" Text="RAM" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="16" Text="Swap file" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="20" Text="Shaders" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="24" Text="Pipelines" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="28" Text="Waiting" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <!-- Draws made per frame; draws skipped per frame as not supported here (with the last reason); the GPU's time per frame. -->
        <TextBlock Grid.Row="0" Grid.Column="1" Text="Draws" />
        <TextBlock Grid.Row="1" Grid.Column="1" Text="Skipped" />
        <TextBlock Grid.Row="2" Grid.Column="1" Text="GPU time" />
        <!-- Internal: the largest viewport the game draws its scene into; Scaled: the picture it hands over each frame, after its own upscaling;
             Display: what the app shows on the screen (the TV's resolution on the Xbox, the window's size on a PC). -->
        <TextBlock Grid.Row="4" Grid.Column="1" Text="Internal" />
        <TextBlock Grid.Row="5" Grid.Column="1" Text="Scaled" />
        <TextBlock Grid.Row="6" Grid.Column="1" Text="Display" />
        <TextBlock Grid.Row="8" Grid.Column="1" Text="CPU" />
        <TextBlock Grid.Row="9" Grid.Column="1" Text="Disk" />
        <!-- RAM in use and its parts: the game's memory in RAM, the GPU's memory (in RAM on the Xbox, on the graphics card on a PC) and the rest (the
             emulator and the app). -->
        <TextBlock Grid.Row="11" Grid.Column="1" Text="Used" />
        <TextBlock Grid.Row="12" Grid.Column="1" Text="Game" />
        <TextBlock Grid.Row="13" Grid.Column="1" Text="GPU" />
        <TextBlock Grid.Row="14" Grid.Column="1" Text="Other" />
        <!-- The game's memory in the swap file (not in RAM), written to it and read back from it. -->
        <TextBlock Grid.Row="16" Grid.Column="1" Text="Size" />
        <TextBlock Grid.Row="17" Grid.Column="1" Text="Out" />
        <TextBlock Grid.Row="18" Grid.Column="1" Text="In" />
        <!-- The game's shaders so far, and the shaders translated in this run (not taken from the cache an earlier run saved). -->
        <TextBlock Grid.Row="20" Grid.Column="1" Text="Drawing" />
        <TextBlock Grid.Row="21" Grid.Column="1" Text="Compute" />
        <TextBlock Grid.Row="22" Grid.Column="1" Text="New" />
        <!-- The pipelines this run made, those of them not made from the cache, and the cache file's. -->
        <TextBlock Grid.Row="24" Grid.Column="1" Text="This run" />
        <TextBlock Grid.Row="25" Grid.Column="1" Text="New" />
        <TextBlock Grid.Row="26" Grid.Column="1" Text="In cache" />
        <!-- The time per second the work stood still for compiling shaders and making pipelines, and for the swap file. -->
        <TextBlock Grid.Row="28" Grid.Column="1" Text="Compiling" />
        <TextBlock Grid.Row="29" Grid.Column="1" Text="Swap file" />
        <TextBlock x:Name="Draws" Grid.Row="0" Grid.Column="2" />
        <TextBlock x:Name="Skipped" Grid.Row="1" Grid.Column="2" />
        <TextBlock x:Name="GpuTime" Grid.Row="2" Grid.Column="2" />
        <TextBlock x:Name="RenderResolution" Grid.Row="4" Grid.Column="2" Typography.NumeralAlignment="Tabular" />
        <TextBlock x:Name="OutputResolution" Grid.Row="5" Grid.Column="2" Typography.NumeralAlignment="Tabular" />
        <TextBlock x:Name="DisplayResolution" Grid.Row="6" Grid.Column="2" Typography.NumeralAlignment="Tabular" />
        <TextBlock x:Name="Cpu" Grid.Row="8" Grid.Column="2" />
        <TextBlock x:Name="Disk" Grid.Row="9" Grid.Column="2" />
        <TextBlock x:Name="Memory" Grid.Row="11" Grid.Column="2" />
        <TextBlock x:Name="GameMemory" Grid.Row="12" Grid.Column="2" />
        <TextBlock x:Name="GpuMemory" Grid.Row="13" Grid.Column="2" />
        <TextBlock x:Name="OtherMemory" Grid.Row="14" Grid.Column="2" />
        <TextBlock x:Name="SwappedMemory" Grid.Row="16" Grid.Column="2" />
        <TextBlock x:Name="SwapOut" Grid.Row="17" Grid.Column="2" />
        <TextBlock x:Name="SwapIn" Grid.Row="18" Grid.Column="2" />
        <TextBlock x:Name="DrawingShaders" Grid.Row="20" Grid.Column="2" />
        <TextBlock x:Name="ComputeShaders" Grid.Row="21" Grid.Column="2" />
        <TextBlock x:Name="NewShaders" Grid.Row="22" Grid.Column="2" />
        <TextBlock x:Name="Pipelines" Grid.Row="24" Grid.Column="2" />
        <TextBlock x:Name="NewPipelines" Grid.Row="25" Grid.Column="2" />
        <TextBlock x:Name="SavedPipelines" Grid.Row="26" Grid.Column="2" />
        <TextBlock x:Name="CompileWait" Grid.Row="28" Grid.Column="2" />
        <TextBlock x:Name="SwapWait" Grid.Row="29" Grid.Column="2" />
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
  </StackPanel>

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

// ---- Detailed: panels beside the frame rate's box, grouped as the desktop's performance panel (upstream) groups them.

// The busiest threads Detailed shows.
constexpr size_t DETAILED_THREADS = 4;
// Upstream's panel's colours: frame times up to these are green and yellow, longer ones red.
constexpr double FRAME_TIME_GOOD   = 17.5;
constexpr double FRAME_TIME_MEDIUM = 34.0;
// Each panel's label and value columns have fixed widths (the panels keep their size whatever the values say), the gap between them, and the
// panels' padding: with four panels and their gaps the row fits the TV at the medium overlay size.
constexpr double DETAILED_LABEL_WIDTH = 105.0;
constexpr double DETAILED_VALUE_WIDTH = 131.0;
constexpr double DETAILED_CELL_GAP    = 8.0;

enum Stat : size_t {
	// Frames: the 1% low (the 99th percentile of the graph's frame times) and the worst frame of the last second; Internal, Scaled and Display as
	// Full has them; per second, the game's flips, the flips it asked for while the flip queue was full, and the vblanks the present thread was
	// late for.
	STAT_LOW,
	STAT_WORST,
	STAT_INTERNAL,
	STAT_SCALED,
	STAT_DISPLAY,
	STAT_FLIPS,
	STAT_QUEUE_FULL,
	STAT_LATE_VBLANKS,
	// CPU: the app's share of all processor cores; the busiest threads, each with its share of one core (the labels are their names); per second,
	// the instructions the host CPU lacks that the emulator does instead, and the game's calls into functions the emulator does not have.
	STAT_CPU,
	STAT_THREAD_1,
	STAT_THREAD_2,
	STAT_THREAD_3,
	STAT_THREAD_4,
	STAT_EMULATED,
	STAT_MISSING_CALLS,
	// GPU: draws, instances, dispatches and command lists handed to the GPU; the time the emulator waited for the GPU; the GPU's time per frame;
	// draws skipped as not supported here (with the last reason); the game's writes into memory the GPU tracks (each one a fault); the shaders and
	// pipelines so far (as Full has them); the shaders translated and the pipelines built since the overlay showed, with their average and longest
	// times (D3D12's translation to DXIL and linking count to them).
	STAT_DRAWS,
	STAT_INSTANCES,
	STAT_DISPATCHES,
	STAT_SUBMITS,
	STAT_GPU_WAIT,
	STAT_GPU_TIME,
	STAT_SKIPPED,
	STAT_FAULTS,
	STAT_SHADERS,
	STAT_PIPELINES,
	STAT_SHADER_COMPILE,
	STAT_PIPELINE_COMPILE,
	// GPU memory: in use of the GPU's budget; per second, textures and buffers written to the GPU and read back, and the ones the caches let go.
	STAT_GPU_MEMORY,
	STAT_TEXTURE_TRANSFERS,
	STAT_TEXTURE_EVICTIONS,
	STAT_BUFFER_TRANSFERS,
	STAT_BUFFER_EVICTIONS,
	// Guest memory, the game's memory as the PS5 hands it out: direct memory taken of all there is, the part of it mapped, the pool's part (and
	// what of the pool is committed), flexible memory, extended memory, and the page-table entries in use per side (red at the side's total: the game
	// then sees none left).
	STAT_DIRECT,
	STAT_MAPPED,
	STAT_POOL,
	STAT_FLEXIBLE,
	STAT_EXTENDED,
	STAT_PAGE_TABLE,
	// App memory: RAM in use (of the app's limit on the Xbox), the game's memory in RAM, and the rest (the emulator and the app; the GPU's memory
	// is in the GPU panel).
	STAT_USED,
	STAT_GAME,
	STAT_OTHER,
	// The game's memory in the swap file (not in RAM), written to it and read back from it per second, and the time the game waited for it.
	STAT_SWAP_SIZE,
	STAT_SWAP_TRAFFIC,
	STAT_SWAP_WAIT,
	// I/O and audio: the game's file reads and writes per second, and the times its sound ran out before it gave more.
	STAT_FILES,
	STAT_AUDIO,
	STAT_COUNT
};

struct StatRow {
	const wchar_t* label;
	Stat           stat;
};
struct StatGroup {
	const wchar_t*       name;
	std::vector<StatRow> rows;
};
// The panels, each a list of groups; the swap file's group shows only where there is a swap file.
const std::array<std::vector<StatGroup>, 3>& DetailedPanels() {
	static const std::array<std::vector<StatGroup>, 3> panels {{
	    {{L"Frames",
	      {{L"1% low", STAT_LOW}, {L"Worst frame", STAT_WORST}, {L"Internal", STAT_INTERNAL}, {L"Scaled", STAT_SCALED}, {L"Display", STAT_DISPLAY},
	       {L"Flips", STAT_FLIPS}, {L"Flip queue full", STAT_QUEUE_FULL}, {L"Late vblanks", STAT_LATE_VBLANKS}}},
	     {L"CPU",
	      {{L"Emulator", STAT_CPU}, {L"", STAT_THREAD_1}, {L"", STAT_THREAD_2}, {L"", STAT_THREAD_3}, {L"", STAT_THREAD_4},
	       {L"Emulated instrs", STAT_EMULATED}, {L"Missing HLE calls", STAT_MISSING_CALLS}}}},
	    {{L"GPU",
	      {{L"Draw calls", STAT_DRAWS}, {L"Instances", STAT_INSTANCES}, {L"Dispatches", STAT_DISPATCHES}, {L"Queue submits", STAT_SUBMITS},
	       {L"CPU waiting on GPU", STAT_GPU_WAIT}, {L"GPU time", STAT_GPU_TIME}, {L"Skipped draws", STAT_SKIPPED},
	       {L"Write-tracking faults", STAT_FAULTS}, {L"Shaders", STAT_SHADERS}, {L"Pipelines", STAT_PIPELINES},
	       {L"Shader recompiler", STAT_SHADER_COMPILE}, {L"Pipeline compile", STAT_PIPELINE_COMPILE}}},
	     {L"GPU memory",
	      {{L"GPU memory", STAT_GPU_MEMORY}, {L"Texture transfers", STAT_TEXTURE_TRANSFERS}, {L"Texture evictions", STAT_TEXTURE_EVICTIONS},
	       {L"Buffer transfers", STAT_BUFFER_TRANSFERS}, {L"Buffer evictions", STAT_BUFFER_EVICTIONS}}}},
	    {{L"Guest memory",
	      {{L"Direct", STAT_DIRECT}, {L"  Mapped", STAT_MAPPED}, {L"  Pool", STAT_POOL}, {L"Flexible", STAT_FLEXIBLE}, {L"Extended", STAT_EXTENDED},
	       {L"Page table (2 MB)", STAT_PAGE_TABLE}}},
	     {L"App memory", {{L"Used", STAT_USED}, {L"Game in RAM", STAT_GAME}, {L"Other", STAT_OTHER}}},
	     {L"Swap file", {{L"Size", STAT_SWAP_SIZE}, {L"Out / in", STAT_SWAP_TRAFFIC}, {L"Waits", STAT_SWAP_WAIT}}},
	     {L"I/O & audio", {{L"File read / write", STAT_FILES}, {L"Audio underruns", STAT_AUDIO}}}},
	}};
	return panels;
}

bool IsSwapStat(Stat stat) {
	return stat == STAT_SWAP_SIZE || stat == STAT_SWAP_TRAFFIC || stat == STAT_SWAP_WAIT;
}

// Upstream's panel's colours: good, look at it, bad.
enum class Level { None, Good, Medium, Bad };
Level FrameTimeLevel(double ms) {
	return ms <= FRAME_TIME_GOOD ? Level::Good : ms <= FRAME_TIME_MEDIUM ? Level::Medium : Level::Bad;
}
// Good below `warn`, medium below `bad`, bad above.
Level LevelOf(double value, double warn, double bad) {
	return value < warn ? Level::Good : value < bad ? Level::Medium : Level::Bad;
}

// "1.23 GB" from a GiB on, else "456 MB" (upstream's panel's sizes).
std::wstring Bytes(uint64_t bytes) {
	wchar_t text[32];
	if (bytes >= (uint64_t {1} << 30u)) {
		swprintf_s(text, L"%.2f GB", static_cast<double>(bytes) / static_cast<double>(uint64_t {1} << 30u));
	} else {
		swprintf_s(text, L"%.0f MB", static_cast<double>(bytes) / static_cast<double>(1u << 20u));
	}
	return text;
}

std::wstring Formatted(const wchar_t* pattern, ...) {
	wchar_t text[96];
	va_list arguments;
	va_start(arguments, pattern);
	vswprintf_s(text, pattern, arguments);
	va_end(arguments);
	return text;
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
	xaml::Controls::TextBlock  display_resolution {nullptr};
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
	double                                cpu_share = -1.0; // of all cores, at the last update (-1: none yet)
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
	// Detailed (BuildDetailed): its panels, their values and labels, the swap file's group, and the values' colours (the theme's, and upstream's
	// green, yellow and red).
	std::vector<xaml::UIElement>                      detailed_panels;
	std::array<xaml::Controls::TextBlock, STAT_COUNT> detailed_values {};
	std::array<xaml::Controls::TextBlock, STAT_COUNT> detailed_labels {};
	std::vector<xaml::UIElement>                      detailed_swap;
	xaml::Media::Brush                                value_brush {nullptr};
	xaml::Media::Brush                                good_brush {nullptr};
	xaml::Media::Brush                                medium_brush {nullptr};
	xaml::Media::Brush                                bad_brush {nullptr};

	void BuildDetailed(xaml::Controls::Panel const& row, xaml::Media::Brush const& background);
	void Set(Stat stat, std::wstring const& text, Level level = Level::None);
	void UpdateDetailed(const std::vector<double>& times, uint64_t used, uint64_t limit, const MemoryTotals& totals);
	void UpdateDetailedRates(const Sample& current, const Sample& before, double seconds);
	void Update();
	void UpdateRates(const MemoryTotals& totals);
	void UpdateControllers();
	void ShowSwapFile(bool shown);
	void SetScale(double value);
	void Fit(double width, double height);
};

void GameOverlay::State::Update() {
	// Upstream's counters, which the draws, the compile time, the disk and Detailed come from, count while the overlay shows (again each time: the
	// emulator sets them from its own setting when a game starts).
	CountersOn(true);
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
	display_resolution.Text(size(resolutions.display_width, resolutions.display_height));

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
			cpu_share = std::min(share, 1.0);
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
	if (detail == OVERLAY_DETAILED) {
		UpdateDetailed(times, used, limit, totals);
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
	using Counter            = Common::DebugCounters::Counter;
	const auto counted       = [&](Counter counter) { return delta(current.work.Get(counter), before.work.Get(counter)); };

	// Per frame: none when no frame came in that second (the game loads, or waits).
	const uint64_t frames = delta(current.frames, before.frames);
	if (frames == 0) {
		draws.Text(L"–");
		skipped.Text(L"–");
		gpu_time.Text(L"–");
	} else {
		const auto per_frame = [frames](uint64_t value) { return static_cast<double>(value) / static_cast<double>(frames); };
		draws.Text(std::to_wstring(std::lround(per_frame(counted(Counter::Draws)))));
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
	disk.Text(mb_per_second(counted(Counter::GuestFileReadBytes)) + L" read");
	swap_out.Text(mb_per_second(delta(totals.swapped_out, before.memory.swapped_out)));
	swap_in.Text(mb_per_second(delta(totals.swapped_in, before.memory.swapped_in)));
	compile_wait.Text(ms_per_second(counted(Counter::ShaderCompileNs) + counted(Counter::PipelineCreateNs)));
	swap_wait.Text(ms_per_second(delta(totals.swap_wait_ns, before.memory.swap_wait_ns)));
	if (detail == OVERLAY_DETAILED) {
		UpdateDetailedRates(current, before, seconds);
	}
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
	for (const auto& element: detailed_swap) {
		element.Visibility(visibility);
	}
}

// ---- Detailed

void GameOverlay::State::Set(Stat stat, std::wstring const& text, Level level) {
	auto& block = detailed_values[stat];
	block.Text(text);
	switch (level) {
		case Level::Good: block.Foreground(good_brush); break;
		case Level::Medium: block.Foreground(medium_brush); break;
		case Level::Bad: block.Foreground(bad_brush); break;
		default: block.Foreground(value_brush); break;
	}
}

// A panel per group list, beside the frame rate's box: each group a heading in bold (with a gap above it), over rows of a label and a value of
// fixed widths.
void GameOverlay::State::BuildDetailed(xaml::Controls::Panel const& row, xaml::Media::Brush const& background) {
	const auto resources = xaml::Application::Current().Resources();
	const auto caption   = resources.Lookup(winrt::box_value(L"CaptionTextBlockStyle")).as<xaml::Style>();
	const auto muted     = resources.Lookup(winrt::box_value(L"SystemControlForegroundBaseMediumBrush")).as<xaml::Media::Brush>();
	const auto color     = [](uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
		return xaml::Media::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b));
	};
	good_brush         = color(255, 96, 220, 120);
	medium_brush       = color(255, 230, 195, 60);
	bad_brush          = color(255, 235, 80, 70);
	const auto text    = [&caption](std::wstring_view content, double width) {
		xaml::Controls::TextBlock block;
		block.Style(caption);
		block.TextWrapping(xaml::TextWrapping::NoWrap);
		block.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
		block.Width(width);
		block.Text(content);
		xaml::Documents::Typography::SetNumeralAlignment(block, xaml::FontNumeralAlignment::Tabular);
		return block;
	};
	for (const auto& groups: DetailedPanels()) {
		xaml::Controls::Grid grid;
		grid.ColumnSpacing(DETAILED_CELL_GAP);
		for (const double width: {DETAILED_LABEL_WIDTH, DETAILED_VALUE_WIDTH}) {
			xaml::Controls::ColumnDefinition definition;
			definition.Width(xaml::GridLengthHelper::FromPixels(width));
			grid.ColumnDefinitions().Append(definition);
		}
		const auto add_row = [&grid]() {
			xaml::Controls::RowDefinition definition;
			definition.Height(xaml::GridLengthHelper::Auto());
			grid.RowDefinitions().Append(definition);
			return static_cast<int32_t>(grid.RowDefinitions().Size()) - 1;
		};
		const auto place = [&grid](xaml::FrameworkElement const& element, int32_t at_row, int32_t at_column) {
			xaml::Controls::Grid::SetRow(element, at_row);
			xaml::Controls::Grid::SetColumn(element, at_column);
			grid.Children().Append(element);
		};
		for (const auto& group: groups) {
			const bool swap_group = std::all_of(group.rows.begin(), group.rows.end(), [](const StatRow& r) { return IsSwapStat(r.stat); });
			// The heading: the group's name in bold, as a title (with a gap above it, but for the first).
			xaml::Controls::TextBlock heading;
			heading.Style(caption);
			heading.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
			heading.Text(group.name);
			heading.Margin({0, grid.RowDefinitions().Size() != 0 ? 8.0 : 0.0, 0, 2});
			xaml::Controls::Grid::SetColumnSpan(heading, 2);
			place(heading, add_row(), 0);
			if (swap_group) {
				detailed_swap.push_back(heading);
			}
			for (const auto& entry: group.rows) {
				const int32_t at_row = add_row();
				detailed_labels[entry.stat] = text(entry.label, DETAILED_LABEL_WIDTH);
				detailed_labels[entry.stat].Foreground(muted);
				detailed_values[entry.stat] = text(L"–", DETAILED_VALUE_WIDTH);
				place(detailed_labels[entry.stat], at_row, 0);
				place(detailed_values[entry.stat], at_row, 1);
				if (IsSwapStat(entry.stat)) {
					detailed_swap.push_back(detailed_labels[entry.stat]);
					detailed_swap.push_back(detailed_values[entry.stat]);
				}
			}
		}
		xaml::Controls::Border panel;
		panel.Background(background);
		panel.Padding({12, 12, 12, 12});
		panel.VerticalAlignment(xaml::VerticalAlignment::Top);
		panel.Child(grid);
		panel.Visibility(xaml::Visibility::Collapsed);
		row.Children().Append(panel);
		detailed_panels.push_back(panel);
	}
	value_brush = detailed_values[STAT_LOW].Foreground();
}

// What Detailed shows that changes each update: the frames, the resolutions, the memory, the CPU, the shaders and the pipelines.
void GameOverlay::State::UpdateDetailed(const std::vector<double>& times, uint64_t used, uint64_t limit, const MemoryTotals& totals) {
	if (!times.empty()) {
		double second = 0.0;
		double worst  = 0.0;
		for (auto it = times.rbegin(); it != times.rend() && second < 1000.0; ++it) {
			second += *it;
			worst = std::max(worst, *it);
		}
		// The 1% low as upstream's panel has it: the 99th percentile of the frame times in the graph.
		auto       sorted     = times;
		const auto percentile = sorted.begin() + static_cast<ptrdiff_t>(sorted.size() * 99 / 100);
		std::nth_element(sorted.begin(), percentile, sorted.end());
		if (*percentile > 0.0) {
			Set(STAT_LOW, Formatted(L"%.1f fps", 1000.0 / *percentile), FrameTimeLevel(*percentile));
		}
		Set(STAT_WORST, Formatted(L"%.1f ms", std::min(worst, 9999.9)), FrameTimeLevel(worst));
	}
	Set(STAT_INTERNAL, render_resolution.Text().c_str());
	Set(STAT_SCALED, output_resolution.Text().c_str());
	Set(STAT_DISPLAY, display_resolution.Text().c_str());
	const bool limited = limit != 0 && limit < (uint64_t {64} << 30);
	Set(STAT_USED, limited ? Bytes(used) + L" / " + Bytes(limit) : Bytes(used),
	    limited ? LevelOf(static_cast<double>(used) * 100.0 / static_cast<double>(limit), 85, 95) : Level::None);
	Set(STAT_GAME, Bytes(totals.game));
	const uint64_t parts = totals.game + (IsXbox() ? totals.gpu : 0);
	Set(STAT_OTHER, Bytes(used > parts ? used - parts : 0));
	if (totals.gpu_budget != 0) {
		Set(STAT_GPU_MEMORY, Bytes(totals.gpu) + L" / " + Bytes(totals.gpu_budget),
		    LevelOf(static_cast<double>(totals.gpu) * 100.0 / static_cast<double>(totals.gpu_budget), 80, 95));
	}
	Set(STAT_SWAP_SIZE, Bytes(totals.swapped));
	// The CPU as Full has it, with the cores it makes.
	if (cpu_share >= 0.0) {
		const auto cores = std::max(1u, std::thread::hardware_concurrency());
		Set(STAT_CPU, Formatted(L"%.0f%%  (%.1f of %u cores)", cpu_share * 100.0, cpu_share * cores, cores));
	}
	if (const auto statistics = CurrentShaderStatistics()) {
		Set(STAT_SHADERS, Formatted(L"%llu  (%llu new)", statistics->drawing_shaders + statistics->compute_shaders, statistics->new_shaders));
		Set(STAT_PIPELINES, Formatted(L"%llu  (%llu new, %llu cached)", statistics->pipelines,
		                              statistics->pipelines - std::min(statistics->cached_pipelines, statistics->pipelines),
		                              statistics->saved_pipelines));
	}
}

// What Detailed shows per second (and per frame), from the counters over the last second, and what costs more to find out: the game's memory as
// the PS5 hands it out, and the busiest threads.
void GameOverlay::State::UpdateDetailedRates(const Sample& current, const Sample& before, double seconds) {
	using Counter       = Common::DebugCounters::Counter;
	const auto delta    = [](uint64_t now, uint64_t then) { return now > then ? now - then : 0; };
	const auto counted  = [&](Counter counter) { return delta(current.work.Get(counter), before.work.Get(counter)); };
	const auto rate     = [&](Counter counter) { return static_cast<double>(counted(counter)) / seconds; };
	const auto total    = [&](Counter counter) { return static_cast<unsigned long long>(current.work.Get(counter)); };
	const auto mb       = [](double bytes_per_second) { return bytes_per_second / static_cast<double>(1u << 20u); };
	const uint64_t frames    = delta(current.frames, before.frames);
	const double   per_frame = frames != 0 ? seconds / static_cast<double>(frames) : 0.0; // a rate times this is per frame

	Set(STAT_FLIPS, Formatted(L"%.0f/s requested", rate(Counter::FlipsRequested)));
	const double rejected = rate(Counter::FlipsRejected);
	Set(STAT_QUEUE_FULL, Formatted(L"%.0f/s", rejected), rejected > 0.0 ? Level::Bad : Level::Good);
	const double late = rate(Counter::LateVblanks);
	Set(STAT_LATE_VBLANKS, Formatted(L"%.0f/s", late), LevelOf(late, 1.0, 10.0));
	const double emulated = rate(Counter::EmulatedInstructions);
	Set(STAT_EMULATED, Formatted(L"%.0f/s", emulated), LevelOf(emulated, 1000, 100000));
	const double unresolved = rate(Counter::UnresolvedImportCalls);
	Set(STAT_MISSING_CALLS, Formatted(L"%.0f/s  (%llu total)", unresolved, total(Counter::UnresolvedImportCalls)),
	    unresolved > 0.0 ? Level::Medium : Level::Good);

	// Per frame: none when no frame came in that second (the game loads, or waits).
	if (frames == 0) {
		Set(STAT_DRAWS, Formatted(L"%.0f/s", rate(Counter::Draws)));
		Set(STAT_INSTANCES, L"–");
		Set(STAT_DISPATCHES, Formatted(L"%.0f/s", rate(Counter::Dispatches)));
	} else {
		Set(STAT_DRAWS, Formatted(L"%.0f/frame  %.0f/s", rate(Counter::Draws) * per_frame, rate(Counter::Draws)));
		Set(STAT_INSTANCES, Formatted(L"%.0f/frame", rate(Counter::DrawInstances) * per_frame));
		Set(STAT_DISPATCHES, Formatted(L"%.0f/frame  %.0f/s", rate(Counter::Dispatches) * per_frame, rate(Counter::Dispatches)));
	}
	// The GPU's time and the skipped draws as Full has them.
	Set(STAT_GPU_TIME, gpu_time.Text().c_str());
	const uint64_t skips = delta(current.work.skipped_draws, before.work.skipped_draws);
	Set(STAT_SKIPPED, skipped.Text().c_str(), skips != 0 ? Level::Medium : Level::Good);
	Set(STAT_SUBMITS, Formatted(L"%.0f/s", rate(Counter::QueueSubmits)));
	Set(STAT_GPU_WAIT, Formatted(L"%.0f ms/s  (%.0f waits/s)", rate(Counter::GpuWaitNs) / 1e6, rate(Counter::GpuWaits)));
	const double faults = rate(Counter::GpuFaults);
	Set(STAT_FAULTS, Formatted(L"%.0f/s", faults), LevelOf(faults, 20000, 100000));
	const auto compiles = [&](Stat stat, Counter count_counter, Counter duration_counter, uint64_t peak_ns) {
		const auto count = total(count_counter);
		Set(stat,
		    Formatted(L"%llu, avg %.1f, peak %.0f ms", count,
		              count != 0 ? static_cast<double>(total(duration_counter)) / 1e6 / static_cast<double>(count) : 0.0,
		              static_cast<double>(peak_ns) / 1e6),
		    counted(count_counter) != 0 ? Level::Medium : Level::Good);
	};
	compiles(STAT_SHADER_COMPILE, Counter::ShadersCompiled, Counter::ShaderCompileNs, current.work.shader_peak_ns);
	compiles(STAT_PIPELINE_COMPILE, Counter::PipelinesCreated, Counter::PipelineCreateNs, current.work.pipeline_peak_ns);
	const auto transfers = [&](Counter up, Counter down) {
		return Formatted(L"%.1f up / %.1f down MB/s", mb(rate(up)), mb(rate(down)));
	};
	Set(STAT_TEXTURE_TRANSFERS, transfers(Counter::TextureUploadBytes, Counter::TextureDownloadBytes));
	Set(STAT_TEXTURE_EVICTIONS, Formatted(L"%.0f/s", rate(Counter::TextureEvictions)));
	Set(STAT_BUFFER_TRANSFERS, transfers(Counter::BufferUploadBytes, Counter::BufferDownloadBytes));
	Set(STAT_BUFFER_EVICTIONS, Formatted(L"%.0f/s", rate(Counter::BufferEvictions)));
	Set(STAT_SWAP_TRAFFIC, Formatted(L"%.1f / %.1f MB/s", mb(static_cast<double>(delta(current.memory.swapped_out, before.memory.swapped_out)) / seconds),
	                                 mb(static_cast<double>(delta(current.memory.swapped_in, before.memory.swapped_in)) / seconds)));
	Set(STAT_SWAP_WAIT, Formatted(L"%.0f ms/s", static_cast<double>(delta(current.memory.swap_wait_ns, before.memory.swap_wait_ns)) / 1e6 / seconds));
	Set(STAT_FILES, Formatted(L"%.1f / %.1f MB/s", mb(rate(Counter::GuestFileReadBytes)), mb(rate(Counter::GuestFileWriteBytes))));
	const double underruns = rate(Counter::AudioUnderruns);
	Set(STAT_AUDIO, Formatted(L"%.0f/s  (%llu total)", underruns, total(Counter::AudioUnderruns)), underruns > 0.0 ? Level::Bad : Level::Good);

	if (const auto game = CurrentGameMemory()) {
		const uint64_t direct = game->direct_allocated + game->pooled_allocated;
		Set(STAT_DIRECT, Bytes(direct) + L" / " + Bytes(game->direct_total),
		    LevelOf(game->direct_total != 0 ? static_cast<double>(direct) * 100.0 / static_cast<double>(game->direct_total) : 0.0, 85, 97));
		Set(STAT_MAPPED, Bytes(game->direct_mapped));
		Set(STAT_POOL, Bytes(game->pooled_allocated) + L"  (" + Bytes(game->pool_committed) + L" committed)");
		Set(STAT_FLEXIBLE, Bytes(game->flexible_used) + L" / " + Bytes(game->flexible_total));
		Set(STAT_EXTENDED, Bytes(game->automatic_allocated));
		// Past its total a side sees no page-table entries left.
		const uint64_t percent = std::max(game->cpu_page_entries * 100 / std::max<uint64_t>(game->cpu_page_entries_total, 1),
		                                  game->gpu_page_entries * 100 / std::max<uint64_t>(game->gpu_page_entries_total, 1));
		Set(STAT_PAGE_TABLE,
		    Formatted(L"CPU %llu / %llu  GPU %llu / %llu", static_cast<unsigned long long>(game->cpu_page_entries),
		              static_cast<unsigned long long>(game->cpu_page_entries_total), static_cast<unsigned long long>(game->gpu_page_entries),
		              static_cast<unsigned long long>(game->gpu_page_entries_total)),
		    LevelOf(static_cast<double>(percent), 90, 100));
	}
	const auto threads = BusiestThreads(DETAILED_THREADS);
	for (size_t i = 0; i < DETAILED_THREADS; i++) {
		const auto stat = static_cast<Stat>(STAT_THREAD_1 + i);
		detailed_labels[stat].Text(i < threads.size() ? L"  " + threads[i].name : std::wstring());
		if (i < threads.size()) {
			Set(stat, Formatted(L"%.0f%%", threads[i].percent), LevelOf(threads[i].percent, 70, 95));
		} else {
			Set(stat, L"");
		}
	}
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
	state.display_resolution = find(L"DisplayResolution").as<xaml::Controls::TextBlock>();
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
	state.BuildDetailed(find(L"TopRow").as<xaml::Controls::Panel>(),
	                    state.root.Resources().Lookup(winrt::box_value(L"PanelBrush")).as<xaml::Media::Brush>());
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
	// Upstream's counters count while the overlay shows.
	CountersOn(shown);
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
	state.detail             = std::min(settings.overlay_detail, OVERLAY_DETAILED);
	state.log_shown          = settings.overlay_log;
	state.controllers_shown  = settings.overlay_controllers;
	state.log_lines          = std::clamp<size_t>(settings.overlay_log_lines, 2, LOG_LINES);
	state.headset_mic_option = settings.headset_mic;
	// Detailed: the box with the game, the frame rate and the graph, and its panels beside it.
	state.header.Visibility(VisibleIf(state.detail == OVERLAY_FULL || state.detail == OVERLAY_DETAILED));
	state.graph.Visibility(VisibleIf(state.detail != OVERLAY_FRAME_RATE));
	state.statistics.Visibility(VisibleIf(state.detail == OVERLAY_FULL));
	state.compact.Visibility(VisibleIf(state.detail == OVERLAY_COMPACT));
	for (const auto& panel: state.detailed_panels) {
		panel.Visibility(VisibleIf(state.detail == OVERLAY_DETAILED));
	}
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
	    panel, explanation, L"Overlay", {L"Off", L"Frame rate", L"Compact", L"Full", L"Detailed"},
	    OverlayShown() ? 1 + std::min(settings->overlay_detail, OVERLAY_DETAILED) : 0,
	    [settings, changed](size_t index) {
		    SetOverlayShown(index != 0);
		    if (index != 0) {
			    settings->overlay_detail = static_cast<uint32_t>(index - 1);
		    }
		    changed();
	    },
	    {L"What the overlay shows over games.",
	     L"Frame rate, Compact and Full show more with each step. Detailed shows everything, in panels along the top.",
	     L"During a game, both triggers with View and Menu turn it off and on."});
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
