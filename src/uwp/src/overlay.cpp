#include "overlay.h"

#include "emulatorHost.h"
#include "fileAccess.h"
#include "input.h"
#include "processorLoad.h"
#include "settingTiles.h"
#include "settings.h"

#include "common/compileActivity.h"
#include "common/emulatorConfig.h"
#include "common/pipelinePrewarm.h"
#include "graphics/host_gpu/frameResolution.h"
#include "kytyGitVersion.h"

#include <windows.h>
#include <fileapifromapp.h>
// windows.h's FindText macro breaks Windows.UI.Text.h.
#undef FindText

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

// The statistics are updated every UPDATE_INTERVAL (from values a thread of their own fetches); the run time, the frame rate and the compile
// notice every FRAME_INTERVAL; the graph with every frame the app shows.
constexpr auto   UPDATE_INTERVAL = std::chrono::milliseconds(250);
constexpr auto   FRAME_INTERVAL  = std::chrono::milliseconds(100);
// The frames Detailed's 1% low and worst frame are over.
constexpr size_t GRAPH_FRAMES    = 120;
// The graph's time span; a game that presents no frame for STALL_MS stalls (the overlay shows it in red).
constexpr double GRAPH_WINDOW_MS = 10000.0;
constexpr double GRAPH_SLOT_MS   = 50.0;
constexpr double STALL_MS        = 250.0;
// Upstream's hitches (performanceOverlay.cpp): a frame of more than HITCH_MS that took at least twice the average of the frames before it (an
// average that follows each frame by 5%, from HITCH_WARMUP_MS before the graph's time span).
constexpr double HITCH_MS        = 50.0;
constexpr double HITCH_WARMUP_MS = 2000.0;
// The swap file's stripe at the bottom of the graph (Full): its height; the traffic at which its white is brightest; the share of a stretch
// spent waiting at which its black is darkest.
constexpr double SWAP_LANE_HEIGHT     = 4.0;
constexpr double SWAP_FULL_BYTES_PER_S = 32.0 * 1048576.0;
constexpr double SWAP_FULL_WAIT_SHARE  = 0.25;
// The graph's range: a frame at 60 fps sits at a third of its height.
constexpr double GRAPH_MAX_MS = 50.0;
// The log lines kept; the settings choose how many of them show (overlay_log_lines, at least 2).
constexpr size_t LOG_LINES    = 20;
// The rates and the values per frame are over at least this many seconds.
constexpr double RATE_SECONDS = 1.0;
// The CPU panel: a core's box and the bars in it, up to 8 boxes a row; the busiest threads it lists, and their bars' width.
constexpr double CPU_CORE_HEIGHT    = 32.0;
constexpr double CPU_CORE_PADDING   = 2.0;
constexpr int32_t CPU_CORES_PER_ROW = 8;
constexpr size_t CPU_THREADS        = 10;
constexpr double CPU_THREAD_BAR     = 80.0;
// The hitches panel's bars: narrower, so its labels fit beside values with decimals ("Showing frame", "38.7 ms").
constexpr double HITCH_BAR          = 64.0;
// The rows of the swap file in the statistics (OverlayMarkup): its group and its row in Waiting, and the gap above the group.
constexpr std::array<int32_t, 5> SWAP_ROWS {18, 19, 20, 21, 32};
constexpr uint32_t               SWAP_GAP_ROW = 17;
// The compile notice: how long the emulator has to compile (with gaps shorter than the grace) before it shows, and how long it stays after the
// last compile.
constexpr uint64_t NOTICE_DELAY_MS = 500;
constexpr uint64_t NOTICE_GRACE_MS = 500;

// The compile notice, part of the overlay: at the bottom right over the game (above the overlay's log when that shows). Its width stays, so
// changing text doesn't move it.
constexpr wchar_t CompileNoticeMarkup[] = LR"xaml(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
      IsHitTestVisible="False" Padding="24">
  <Border x:Name="Notice" Width="260" HorizontalAlignment="Right" VerticalAlignment="Bottom" Padding="12,10" Background="#D91F1F1F"
          Visibility="Collapsed">
    <Grid ColumnSpacing="12">
      <Grid.ColumnDefinitions>
        <ColumnDefinition Width="Auto" />
        <ColumnDefinition Width="*" />
      </Grid.ColumnDefinitions>
      <ProgressRing x:Name="NoticeRing" Width="24" Height="24" VerticalAlignment="Center"
                    Foreground="{ThemeResource SystemControlForegroundBaseHighBrush}" />
      <StackPanel Grid.Column="1" VerticalAlignment="Center">
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="NoticeTitle" Style="{StaticResource BodyTextBlockStyle}" TextWrapping="NoWrap" TextTrimming="CharacterEllipsis" />
        <TextBlock x:Name="NoticeDetail" Style="{StaticResource CaptionTextBlockStyle}" TextWrapping="NoWrap" TextTrimming="CharacterEllipsis"
                   Typography.NumeralAlignment="Tabular" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </StackPanel>
    </Grid>
  </Border>
</Grid>)xaml";

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
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Game" Style="{StaticResource BodyTextBlockStyle}" FontWeight="SemiBold"
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
      <!-- The frame times of the last GRAPH_WINDOW_MS, scrolling with time; stalls as red blocks. -->
      <Grid x:Name="Graph" Height="48" Background="#14FFFFFF">
        <Canvas x:Name="StallMarks" />
        <Canvas x:Name="SwapMarks" />
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
        </Grid.RowDefinitions>
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="0" Text="Frame" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="5" Text="Resolution" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="9" Text="System" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="13" Text="RAM" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="18" Text="Swap file" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="23" Text="Shaders" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="27" Text="Pipelines" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="31" Text="Waiting" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <!-- Draws made per frame; draws skipped per frame as not supported here (with the last reason); the GPU's time per frame. -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="0" Grid.Column="1" Text="Draws" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="1" Grid.Column="1" Text="Skipped" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="2" Grid.Column="1" Text="GPU time" />
        <!-- Frames made by FSR frame generation per second, and the game's own; shown while its switch is on. -->
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="GeneratedLabel" Grid.Row="3" Grid.Column="1" Text="Generated" Visibility="Collapsed" />
        <!-- Internal: the largest viewport the game draws its scene into; Scaled: the picture it hands over each frame, after its own upscaling;
             Display: what the app shows on the screen (the TV's resolution on the Xbox, the window's size on a PC). -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="5" Grid.Column="1" Text="Internal" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="6" Grid.Column="1" Text="Scaled" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="7" Grid.Column="1" Text="Display" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="9" Grid.Column="1" Text="CPU" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="10" Grid.Column="1" Text="Disk" />
        <!-- Per second, the game's writes to memory the GPU keeps a copy of, each caught to mark the copy as outdated (upstream's write-tracking
             faults). -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="11" Grid.Column="1" Text="Write faults" />
        <!-- RAM in use and its parts: the game's memory in RAM, the GPU's memory (in RAM on the Xbox, on the graphics card on a PC) and the rest (the
             emulator and the app). -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="13" Grid.Column="1" Text="Used" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="14" Grid.Column="1" Text="Game" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="15" Grid.Column="1" Text="GPU" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="16" Grid.Column="1" Text="Other" />
        <!-- The game's memory in the swap file (not in RAM), the compressed pool in RAM in front of it (its size and the share of reads it
             served; - without one), and what was written to the file and read back from it. -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="18" Grid.Column="1" Text="Size" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="19" Grid.Column="1" Text="Pool" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="20" Grid.Column="1" Text="Out" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="21" Grid.Column="1" Text="In" />
        <!-- The game's shaders so far, and the shaders translated in this run (not taken from the cache an earlier run saved). -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="23" Grid.Column="1" Text="Drawing" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="24" Grid.Column="1" Text="Compute" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="25" Grid.Column="1" Text="New" />
        <!-- The pipelines this run made, those of them not made from the cache, and the cache file's. -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="27" Grid.Column="1" Text="This run" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="28" Grid.Column="1" Text="New" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="29" Grid.Column="1" Text="In cache" />
        <!-- The time per second the work stood still for compiling shaders and making pipelines, and for the swap file. -->
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="31" Grid.Column="1" Text="Compiling" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="32" Grid.Column="1" Text="Swap file" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Draws" Grid.Row="0" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Skipped" Grid.Row="1" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="GpuTime" Grid.Row="2" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Generated" Grid.Row="3" Grid.Column="2" Visibility="Collapsed" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="RenderResolution" Grid.Row="5" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="OutputResolution" Grid.Row="6" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="DisplayResolution" Grid.Row="7" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Cpu" Grid.Row="9" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Disk" Grid.Row="10" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="WriteFaults" Grid.Row="11" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Memory" Grid.Row="13" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="GameMemory" Grid.Row="14" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="GpuMemory" Grid.Row="15" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="OtherMemory" Grid.Row="16" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="SwappedMemory" Grid.Row="18" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="SwapPool" Grid.Row="19" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="SwapOut" Grid.Row="20" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="SwapIn" Grid.Row="21" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="DrawingShaders" Grid.Row="23" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="ComputeShaders" Grid.Row="24" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="NewShaders" Grid.Row="25" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Pipelines" Grid.Row="27" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="NewPipelines" Grid.Row="28" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="SavedPipelines" Grid.Row="29" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="CompileWait" Grid.Row="31" Grid.Column="2" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="SwapWait" Grid.Row="32" Grid.Column="2" />
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
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="0" Text="GPU time" />
        <TextBlock Typography.NumeralAlignment="Tabular" Grid.Row="1" Text="RAM" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="CompactSwapLabel" Grid.Row="2" Text="Swap file" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="CompactGpuTime" Grid.Row="0" Grid.Column="1" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="CompactMemory" Grid.Row="1" Grid.Column="1" />
        <TextBlock Typography.NumeralAlignment="Tabular" x:Name="CompactSwap" Grid.Row="2" Grid.Column="1" />
      </Grid>
      <!-- The app's version and the commit it was built from, the last line (Full and Detailed). -->
      <TextBlock x:Name="Build" Style="{StaticResource CaptionTextBlockStyle}" Typography.NumeralAlignment="Tabular" Margin="0,4,0,0"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" Visibility="Collapsed" />
    </StackPanel>
  </Border>
  </StackPanel>

  <!-- At the top right: Controllers, and the hitches and CPU panels when the settings show them, which then take the corner, hitches above the
       CPU (its height stays the same; the CPU panel's changes with the threads it lists). -->
  <StackPanel Orientation="Horizontal" Spacing="12" HorizontalAlignment="Right" VerticalAlignment="Top">
  <Border x:Name="ControllersPanel" MinWidth="220" VerticalAlignment="Top" Padding="12"
          Background="{StaticResource PanelBrush}">
    <StackPanel Spacing="4">
      <TextBlock Typography.NumeralAlignment="Tabular" Text="Controllers" Style="{StaticResource CaptionTextBlockStyle}"
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

  <StackPanel Spacing="12" VerticalAlignment="Top">
  <!-- The slow frames (BuildHitchesPanel, UpdateHitchesPanel): upstream's hitches in the last GRAPH_WINDOW_MS (the graph's amber and red
       marks), the last one and what its time went on, then what an average frame's goes on. A row is a label, a value and a bar for its
       share of the frame. -->
  <Border x:Name="HitchesPanel" Width="240" VerticalAlignment="Top" Padding="12" Background="{StaticResource PanelBrush}" Visibility="Collapsed">
    <StackPanel Spacing="4">
      <Grid>
        <TextBlock Text="Hitches" Style="{StaticResource CaptionTextBlockStyle}" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Text="last 10 s" Style="{StaticResource CaptionTextBlockStyle}" HorizontalAlignment="Right"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </Grid>
      <TextBlock x:Name="HitchCount" Typography.NumeralAlignment="Tabular" />
      <TextBlock x:Name="HitchLast" Style="{StaticResource CaptionTextBlockStyle}" Typography.NumeralAlignment="Tabular" TextWrapping="NoWrap"
                 TextTrimming="CharacterEllipsis" />
      <Grid x:Name="HitchCauses" ColumnSpacing="8">
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="*" />
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="64" />
        </Grid.ColumnDefinitions>
      </Grid>
      <TextBlock Text="Per frame" Style="{StaticResource CaptionTextBlockStyle}" Margin="0,4,0,0"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <Grid x:Name="FrameCauses" ColumnSpacing="8">
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="*" />
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="64" />
        </Grid.ColumnDefinitions>
      </Grid>
    </StackPanel>
  </Border>

  <!-- The processor (BuildCpuPanel): the app's share of it and its shape, a box per core with a bar per logical processor (where Windows tells
       each one's load), which colour is the system's cores, and the busiest threads. -->
  <Border x:Name="CpuPanel" Width="240" VerticalAlignment="Top" Padding="12" Background="{StaticResource PanelBrush}" Visibility="Collapsed">
    <StackPanel Spacing="4">
      <TextBlock Text="CPU" Style="{StaticResource CaptionTextBlockStyle}" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <Grid>
        <TextBlock x:Name="CpuTotal" Typography.NumeralAlignment="Tabular" />
        <TextBlock x:Name="CpuShape" Style="{StaticResource CaptionTextBlockStyle}" HorizontalAlignment="Right" VerticalAlignment="Bottom"
                   Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      </Grid>
      <Grid x:Name="CpuCores" ColumnSpacing="4" RowSpacing="4" Margin="0,2,0,0" />
      <StackPanel x:Name="CpuLegend" Orientation="Horizontal" Spacing="12" Visibility="Collapsed">
        <StackPanel Orientation="Horizontal" Spacing="5">
          <Ellipse Width="7" Height="7" Fill="#7CC24D" VerticalAlignment="Center" />
          <TextBlock Text="Game" Style="{StaticResource CaptionTextBlockStyle}" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        </StackPanel>
        <StackPanel Orientation="Horizontal" Spacing="5">
          <Ellipse Width="7" Height="7" Fill="#8C8C8C" VerticalAlignment="Center" />
          <TextBlock Text="System" Style="{StaticResource CaptionTextBlockStyle}" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        </StackPanel>
      </StackPanel>
      <TextBlock Text="Busiest threads" Style="{StaticResource CaptionTextBlockStyle}" Margin="0,4,0,0"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <!-- A row per thread: its name, its share of one core and a bar for it. -->
      <Grid x:Name="CpuThreads" ColumnSpacing="8">
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="*" />
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="80" />
        </Grid.ColumnDefinitions>
      </Grid>
    </StackPanel>
  </Border>
  </StackPanel>
  </StackPanel>

  <Border x:Name="LogPanel" Grid.Row="2" Padding="12,8" Background="{StaticResource PanelBrush}">
    <TextBlock Typography.NumeralAlignment="Tabular" x:Name="Log" FontFamily="Consolas" FontSize="12" LineHeight="16" TextWrapping="NoWrap"
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
		// A new log may be in another folder than the last (Settings › Storage › Logs and dumps): it's opened there.
		if (m_file != INVALID_HANDLE_VALUE && EmulatorHost::Get().LogGeneration() != m_generation && EmulatorHost::Get().LogFile() != m_path) {
			CloseHandle(m_file);
			m_file = INVALID_HANDLE_VALUE;
		}
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
		m_path = EmulatorHost::Get().LogFile();
		m_file = CreateFile2FromAppW(m_path.c_str(), GENERIC_READ,
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
	std::filesystem::path   m_path;
	uint32_t                m_generation = EmulatorHost::Get().LogGeneration();
	Escape                  m_escape     = Escape::None;
	std::deque<std::string> m_lines;
	std::string             m_partial;
	bool                    m_skip_partial_line = false;
};

// The marks of the user's region for numbers: what separates thousands (0: nothing) and the decimal point, as the system formats them.
struct NumberMarks {
	wchar_t group   = 0;
	wchar_t decimal = L'.';
};

const NumberMarks& RegionNumberMarks() {
	static const NumberMarks marks = [] {
		NumberMarks                                                    found;
		winrt::Windows::Globalization::NumberFormatting::DecimalFormatter formatter;
		formatter.NumeralSystem(L"Latn");
		formatter.IsGrouped(true);
		formatter.FractionDigits(1);
		const std::wstring text(formatter.FormatDouble(1000.5));
		const auto         digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
		const auto         first = std::find_if_not(text.begin(), text.end(), digit); // "1" then a mark
		const auto         last  = std::find_if_not(text.rbegin(), text.rend(), digit); // "5" then a mark
		if (last != text.rend()) {
			found.decimal = *last;
		}
		if (first != text.end() && first != last.base() - 1) {
			found.group = *first;
		}
		return found;
	}();
	return marks;
}

// The numbers of a text in the region's marks: 12345.6 becomes 12,345.6 or 12.345,6.
std::wstring RegionNumbers(const std::wstring& text) {
	const auto&  marks = RegionNumberMarks();
	const auto   digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
	std::wstring result;
	result.reserve(text.size() + 4);
	for (size_t i = 0; i < text.size();) {
		if (!digit(text[i])) {
			result += text[i++];
			continue;
		}
		size_t end = i;
		while (end < text.size() && digit(text[end])) {
			end++;
		}
		for (size_t c = i; c < end; c++) {
			result += text[c];
			const auto left = end - c - 1;
			if (marks.group != 0 && left != 0 && left % 3 == 0) {
				result += marks.group;
			}
		}
		i = end;
		if (i + 1 < text.size() && text[i] == L'.' && digit(text[i + 1])) {
			result += marks.decimal;
			i++;
			while (i < text.size() && digit(text[i])) {
				result += text[i++];
			}
		}
	}
	return result;
}

template <typename Number>
std::wstring Count(Number value) {
	return RegionNumbers(std::to_wstring(value));
}

std::wstring Megabytes(uint64_t bytes) {
	return Count((bytes + (1u << 19)) >> 20) + L" MB";
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

// The overlay's last line: the app's version and the commit it was built from, "1.1.26317.620 · 668bd1d" (KYTY_GIT_HASH: "-dirty" when built from
// uncommitted changes, "unknown" without git).
winrt::hstring BuildLine() {
	constexpr wchar_t SEPARATOR[] = {L' ', 0x00B7, L' ', 0};
	std::wstring      version     = L"unknown";
	try {
		const auto number = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
		version = std::to_wstring(number.Major) + L"." + std::to_wstring(number.Minor) + L"." + std::to_wstring(number.Build) + L"." +
		          std::to_wstring(number.Revision);
	} catch (winrt::hresult_error const&) {
	}
	return winrt::hstring(version + SEPARATOR + std::wstring(winrt::to_hstring(KYTY_GIT_HASH)));
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
	// color metadata (DCC/CMASK) the CPU waited to read back, and the checks of it the GPU made instead (upstream's rows);
	// draws skipped as not supported here (with the last reason); the game's writes into memory the GPU tracks (each one a fault); the shaders and
	// pipelines so far (as Full has them); the shaders translated and the pipelines built since the overlay showed, with their average and longest
	// times (D3D12's translation to DXIL and linking count to them).
	STAT_DRAWS,
	STAT_INSTANCES,
	STAT_DISPATCHES,
	STAT_SUBMITS,
	STAT_GPU_WAIT,
	STAT_COLOR_READBACKS,
	STAT_COLOR_CHECKS,
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
	STAT_SWAP_POOL, // the compressed pool in RAM and the share of reads it served
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
	       {L"CPU waiting on GPU", STAT_GPU_WAIT}, {L"DCC/CMASK readbacks", STAT_COLOR_READBACKS},
	       {L"DCC/CMASK GPU checks", STAT_COLOR_CHECKS}, {L"GPU time", STAT_GPU_TIME}, {L"Skipped draws", STAT_SKIPPED},
	       {L"Write-tracking faults", STAT_FAULTS}, {L"Shaders", STAT_SHADERS}, {L"Pipelines", STAT_PIPELINES},
	       {L"Shader recompiler", STAT_SHADER_COMPILE}, {L"Pipeline compile", STAT_PIPELINE_COMPILE}}},
	     {L"GPU memory",
	      {{L"GPU memory", STAT_GPU_MEMORY}, {L"Texture transfers", STAT_TEXTURE_TRANSFERS}, {L"Texture evictions", STAT_TEXTURE_EVICTIONS},
	       {L"Buffer transfers", STAT_BUFFER_TRANSFERS}, {L"Buffer evictions", STAT_BUFFER_EVICTIONS}}}},
	    {{L"Guest memory",
	      {{L"Direct", STAT_DIRECT}, {L"  Mapped", STAT_MAPPED}, {L"  Pool", STAT_POOL}, {L"Flexible", STAT_FLEXIBLE}, {L"Extended", STAT_EXTENDED},
	       {L"Page table (2 MB)", STAT_PAGE_TABLE}}},
	     {L"App memory", {{L"Used", STAT_USED}, {L"Game in RAM", STAT_GAME}, {L"Other", STAT_OTHER}}},
	     {L"Swap file", {{L"Size", STAT_SWAP_SIZE}, {L"Pool", STAT_SWAP_POOL}, {L"Out / in", STAT_SWAP_TRAFFIC}, {L"Waits", STAT_SWAP_WAIT}}},
	     {L"I/O & audio", {{L"File read / write", STAT_FILES}, {L"Audio underruns", STAT_AUDIO}}}},
	}};
	return panels;
}

bool IsSwapStat(Stat stat) {
	return stat == STAT_SWAP_SIZE || stat == STAT_SWAP_POOL || stat == STAT_SWAP_TRAFFIC || stat == STAT_SWAP_WAIT;
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
	return RegionNumbers(text);
}

std::wstring Formatted(const wchar_t* pattern, ...) {
	wchar_t text[96];
	va_list arguments;
	va_start(arguments, pattern);
	vswprintf_s(text, pattern, arguments);
	va_end(arguments);
	return RegionNumbers(text);
}

// The compressed swap pool: its size (as `size`) and the share of reads it served so far; - without one.
std::wstring SwapPoolText(const MemoryTotals& totals, std::wstring size) {
	const auto reads = totals.pool_hits + totals.pool_misses;
	if (totals.pool_bytes == 0 && reads == 0) {
		return L"-";
	}
	return size + Formatted(L", %.0f%% hits", reads != 0 ? static_cast<double>(totals.pool_hits) * 100.0 / static_cast<double>(reads) : 0.0);
}

// A thread's name as the CPU panel shows it: the app's window thread, which Windows names "ApplicationView" and more, as "App UI".
std::wstring ThreadDisplayName(const std::wstring& name) {
	return name.rfind(L"ApplicationView", 0) == 0 ? std::wstring(L"App UI") : name;
}

// The CPU panel's colours: the game's bars (as the frame graph), the system's, a core's box and an empty bar.
xaml::Media::SolidColorBrush CpuBrush(uint8_t alpha, uint8_t red, uint8_t green, uint8_t blue) {
	return xaml::Media::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(alpha, red, green, blue));
}

// Which of `frames` (oldest first) are hitches (HITCH_MS).
std::vector<bool> Hitches(const std::vector<EmulatorHost::RecentFrame>& frames) {
	std::vector<bool> hitches(frames.size(), false);
	double            average = 0.0;
	for (size_t i = 0; i < frames.size(); i++) {
		const double ms = frames[i].time_ms;
		hitches[i]      = average > 0.0 && ms > HITCH_MS && ms >= 2.0 * average;
		average         = average > 0.0 ? average * 0.95 + ms * 0.05 : ms;
	}
	return hitches;
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
	// While the game stalls: the red blocks in the graph, and the frame rate's brushes.
	xaml::Controls::Canvas     stall_marks {nullptr};
	xaml::Media::Brush         fps_brush {nullptr};
	xaml::Media::Brush         stall_brush {nullptr};
	xaml::Media::Brush         stall_fill_brush {nullptr};
	bool                       stalled = false;
	// The graph's red blocks (made once, then moved) and its size when it was last drawn.
	std::vector<xaml::Shapes::Rectangle> stall_blocks;
	// The swap file's stripe (Full): the traffic to and from it and the game's waits for it between fetches, over the graph's time span; the
	// totals at the last fetch (ClockMs, bytes moved, ns waited); and its marks (made once, then moved): a white block per stretch of traffic,
	// a black one per wait.
	struct SwapSample {
		double   from_ms   = 0.0;
		double   to_ms     = 0.0;
		uint64_t moved     = 0;
		uint64_t waited_ns = 0;
	};
	std::deque<SwapSample>                                               swap_samples;
	std::optional<SwapSample>                                            swap_last;
	xaml::Controls::Canvas                                               swap_marks {nullptr};
	bool                                                                 swap_shown_in_graph = false;
	std::vector<std::pair<xaml::Shapes::Rectangle, xaml::Shapes::Rectangle>> swap_blocks;
	double                               graph_width  = 0.0;
	double                               graph_height = 0.0;
	xaml::DispatcherTimer      frame_timer {nullptr};
	xaml::Controls::Grid       statistics {nullptr};
	xaml::Controls::TextBlock  draws {nullptr};
	xaml::Controls::TextBlock  skipped {nullptr};
	xaml::Controls::TextBlock  gpu_time {nullptr};
	xaml::Controls::TextBlock  generated_label {nullptr};
	xaml::Controls::TextBlock  generated {nullptr};
	xaml::Controls::TextBlock  output_resolution {nullptr};
	xaml::Controls::TextBlock  render_resolution {nullptr};
	xaml::Controls::TextBlock  display_resolution {nullptr};
	xaml::Controls::TextBlock  build {nullptr};
	xaml::Controls::TextBlock  cpu {nullptr};
	xaml::Controls::TextBlock  disk {nullptr};
	xaml::Controls::TextBlock  write_faults {nullptr};
	xaml::Controls::TextBlock  memory {nullptr};
	xaml::Controls::TextBlock  game_memory {nullptr};
	xaml::Controls::TextBlock  gpu_memory {nullptr};
	xaml::Controls::TextBlock  other_memory {nullptr};
	xaml::Controls::TextBlock  swapped_memory {nullptr};
	xaml::Controls::TextBlock  swap_pool {nullptr};
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
		uint64_t                              generated = 0; // frames made by FSR frame generation and shown, so far
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
	// The compile notice (CompileNoticeMarkup): its layer and parts; whether the settings show it; when the emulator's current stretch of
	// compiling began and last compiled (0: none), the compiles started before that stretch and at the last look.
	xaml::Controls::Grid         notice_layer {nullptr};
	xaml::Controls::Border       notice {nullptr};
	xaml::Controls::ProgressRing notice_ring {nullptr};
	xaml::Controls::TextBlock    notice_title {nullptr};
	xaml::Controls::TextBlock    notice_detail {nullptr};
	bool                         notice_enabled = true;
	bool                         notice_shown   = false;
	uint64_t                     busy_since_ms  = 0;
	uint64_t                     last_busy_ms   = 0;
	uint64_t                     started_before = 0;
	uint64_t                     last_started   = 0;

	// The CPU panel (BuildCpuPanel): whether the settings show it, its parts; a box per core with a bar per logical processor (with the
	// processor's number), which cores are the system's (ShowSystemCores), each processor's load, and the rows of the busiest threads.
	struct CoreBox {
		xaml::Controls::Border                                    box {nullptr};
		std::vector<std::pair<uint32_t, xaml::Shapes::Rectangle>> bars;
	};
	struct ThreadRow {
		xaml::Controls::TextBlock name {nullptr};
		xaml::Controls::TextBlock percent {nullptr};
		xaml::Controls::Grid      track {nullptr};
		xaml::Shapes::Rectangle   bar {nullptr};
	};
	bool                      cpu_shown = false;
	xaml::UIElement           cpu_panel {nullptr};
	xaml::Controls::TextBlock cpu_total {nullptr};
	xaml::UIElement           cpu_legend {nullptr};
	std::vector<CoreBox>      cpu_cores;
	std::vector<bool>         cpu_system;
	ProcessorLoad             processor_load;
	std::vector<ThreadRow>    cpu_threads;

	// The hitches panel (BuildHitchesPanel): whether the settings show it, its parts, and its rows' values and bars, the last hitch's causes and
	// an average frame's; the graph's amber marks for hitches shorter than a stall (made once, then moved).
	struct CauseRow {
		xaml::Controls::TextBlock value {nullptr};
		xaml::Shapes::Rectangle   bar {nullptr};
	};
	bool                                 hitches_shown = false;
	xaml::UIElement                      hitches_panel {nullptr};
	xaml::Controls::TextBlock            hitch_count {nullptr};
	xaml::Controls::TextBlock            hitch_last {nullptr};
	xaml::Media::Brush                   hitch_last_brush {nullptr};
	std::vector<CauseRow>                hitch_rows;
	std::vector<CauseRow>                frame_rows;
	xaml::Media::Brush                   hitch_fill_brush {nullptr};
	std::vector<xaml::Shapes::Rectangle> hitch_blocks;

	void BuildCpuPanel();
	void UpdateCpuPanel();
	void BuildHitchesPanel();
	void UpdateHitchesPanel();
	void ShowSystemCores(std::vector<bool> system);
	void BuildDetailed(xaml::Controls::Panel const& row, xaml::Media::Brush const& background);
	void Set(Stat stat, std::wstring const& text, Level level = Level::None);
	void UpdateDetailed(const std::vector<double>& times, uint64_t used, uint64_t limit, const MemoryTotals& totals);
	void UpdateDetailedRates(const Sample& current, const Sample& before, double seconds);
	void Update();
	void UpdateRates(const MemoryTotals& totals);
	void UpdateControllers();
	void UpdateNotice();
	void UpdateFrame();
	void DrawGraph();
	void RecordSwap(const MemoryTotals& totals, double at_ms);
	void DrawSwap(double width, double height, const std::function<double(double)>& x_of);
	void DrawGraphWhileShown();
	void Fetch(std::stop_token stop);
	void ShowSwapFile(bool shown);
	void SetScale(double value);
	void Fit(double width, double height);

	// What the overlay shows that the emulator or the driver has to be asked for, fetched by `fetcher` while the overlay shows: a call that
	// waits (the driver's memory figures while it builds a pipeline, say) then never stops the app's UI. The UI shows the last values it got.
	struct Fetched {
		MemoryTotals                    memory;
		std::optional<ShaderStatistics> shaders;
		std::optional<GameMemoryTotals> game_memory;
		double                          at_ms = 0.0; // when (ClockMs)
	};
	std::mutex                  fetched_mutex;
	std::optional<Fetched>      fetched;        // the fetcher's latest, under fetched_mutex
	Fetched                     shown_values;   // the UI's copy
	std::atomic_bool            fetching {false};
	std::atomic_bool            fetch_game_memory {false};
	std::condition_variable_any fetch_wake;
	// Draws the graph with every frame the app shows (DrawGraphWhileShown); after the parts it draws, so it is revoked before they go.
	xaml::Media::CompositionTarget::Rendering_revoker graph_rendering;
	// Last, so it stops before the rest goes.
	std::jthread fetcher;

	// UI thread: the fetcher's latest values when it isn't storing them, else the last ones.
	const Fetched& Latest() {
		std::unique_lock lock(fetched_mutex, std::try_to_lock);
		if (lock.owns_lock() && fetched) {
			shown_values = *fetched;
		}
		return shown_values;
	}
};

void GameOverlay::State::Update() {
	// Upstream's counters, which the draws, the compile time, the disk and Detailed come from, count while the overlay shows (again each time: the
	// emulator sets them from its own setting when a game starts).
	CountersOn(true);
	const auto& values = Latest();

	// Detailed's 1% low and worst frame: the last frames presented, and a frame that takes longer than the last one while it lasts.
	auto times = EmulatorHost::Get().RecentFrameTimes(GRAPH_FRAMES);
	if (const double waiting = EmulatorHost::Get().MsSinceLastPresent(); !times.empty() && waiting > times.back()) {
		if (times.size() == GRAPH_FRAMES) {
			times.erase(times.begin());
		}
		times.push_back(waiting);
	}

	// Resolutions of the last frame: "-" before the first flip, and for render when the frame drew no scene (a menu).
	const auto resolutions = CurrentFrameResolutions();
	const auto size        = [](uint32_t width, uint32_t height) {
		return width == 0 || height == 0 ? std::wstring(L"\u2013") : std::to_wstring(width) + L" \u00d7 " + std::to_wstring(height);
	};
	output_resolution.Text(size(resolutions.output_width, resolutions.output_height) + (resolutions.fsr ? L" FSR" : L""));
	render_resolution.Text(size(resolutions.render_width, resolutions.render_height));
	display_resolution.Text(size(resolutions.display_width, resolutions.display_height) + (resolutions.fsr1 ? L" FSR 1" : L""));

	// Memory: on Xbox the limit is the app's budget; on a PC it's no real limit.
	const uint64_t used  = winrt::Windows::System::MemoryManager::AppMemoryUsage();
	const uint64_t limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
	memory.Text(Megabytes(used) +
	            (limit < (uint64_t {64} << 30) ? L" of " + Megabytes(limit) : std::wstring()));

	// Its parts. The GPU's memory is part of RAM on the Xbox; on a PC it is on the graphics card, so the rest is RAM minus the game's memory there.
	const auto& totals = values.memory;
	RecordSwap(totals, values.at_ms);
	game_memory.Text(Megabytes(totals.game));
	gpu_memory.Text(Megabytes(totals.gpu) + (IsXbox() ? std::wstring() : std::wstring(L" (graphics card)")));
	const uint64_t parts = totals.game + (IsXbox() ? totals.gpu : 0);
	other_memory.Text(Megabytes(used > parts ? used - parts : 0));
	// The swap file, where there is one (the pager: by default on the Xbox only).
	ShowSwapFile(totals.pager);
	swapped_memory.Text(Megabytes(totals.swapped));
	swap_pool.Text(SwapPoolText(totals, Megabytes(totals.pool_bytes)));
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

	if (const auto& statistics = values.shaders) {
		drawing_shaders.Text(Count(statistics->drawing_shaders));
		compute_shaders.Text(Count(statistics->compute_shaders));
		new_shaders.Text(Count(statistics->new_shaders));
		// Pipelines the game used, and the ones built ahead for it.
		const auto ready = Common::PipelinePrewarm::Detail::built.load();
		pipelines.Text(Count(statistics->pipelines) + (ready != 0 ? L" (" + Count(ready) + L" ready)" : std::wstring()));
		new_pipelines.Text(Count(statistics->pipelines - std::min(statistics->cached_pipelines, statistics->pipelines)));
		saved_pipelines.Text(Count(statistics->saved_pipelines));
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

// One row per connected controller, by player (ConnectedControllers): its player, the controller symbol, its name and, with a headset, the
// headset symbol.
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
		number.Text(controller.player < 0 ? std::wstring(L"–") : L"P" + std::to_wstring(controller.player + 1));
		xaml::Documents::Typography::SetNumeralAlignment(number, xaml::FontNumeralAlignment::Tabular);
		place(number, row, 0);
		// A DSU controller: the network symbol of Segoe MDL2 Assets.
		xaml::Controls::TextBlock icon;
		icon.FontFamily(xaml::Media::FontFamily(controller.network ? L"Segoe MDL2 Assets" : symbol.font));
		icon.Text(controller.network ? L"" : symbol.glyph);
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

// The run time, the frame rate and the compile notice, from values that never wait for the emulator, so the overlay keeps moving and shows at
// once when the game stalls (no frame for STALL_MS) and for how long: the frame rate turns red and the frame time counts the stall.
void GameOverlay::State::UpdateFrame() {
	run_time.Text(L"Running  " + Duration(EmulatorHost::Get().RunTimeMs()));

	const double waiting = EmulatorHost::Get().MsSinceLastPresent();
	const bool   stall   = waiting >= STALL_MS;
	// The frame rate over the last second: while the game stalls the frames presented in it, else from their average time.
	const auto second = EmulatorHost::Get().FramesSince(EmulatorHost::ClockMs() - 1000.0);
	if (stall) {
		fps.Text(std::to_wstring(second.size()) + L" fps");
		frame_time.Text(Formatted(L"%.1f ms", std::min(waiting, 99999.9)));
	} else if (!second.empty()) {
		double total = 0.0;
		for (const auto& frame: second) {
			total += frame.time_ms;
		}
		const double average = total / static_cast<double>(second.size());
		fps.Text(std::to_wstring(std::lround(1000.0 / average)) + L" fps");
		frame_time.Text(Formatted(L"%.1f ms", std::min(average, 999.9)));
	}
	if (stall != stalled) {
		stalled = stall;
		fps.Foreground(stall ? stall_brush : fps_brush);
	}
	UpdateNotice();
}

// The graph, drawn with every frame the app shows while the overlay does (DrawGraphWhileShown): the last GRAPH_WINDOW_MS up to now at the
// right, scrolling with time. Time goes into slots of GRAPH_SLOT_MS fixed to the clock, so a slot keeps its value as it moves; each slot is
// drawn at the longest frame that took any of its time, so a long frame keeps the line up for as long as it lasted (drawn only in the slot it
// ended in, the line ran slanted across the slots before it). Stalls (frames of STALL_MS or more, and the one going on) are red blocks over the
// time they took, hitches shorter than that (HITCH_MS) amber ones.
void GameOverlay::State::DrawGraph() {
	const double width  = graph.ActualWidth();
	const double height = graph.ActualHeight();
	if (width <= 0.0 || height <= 0.0) {
		return;
	}
	if (width != graph_width || height != graph_height) {
		graph_width  = width;
		graph_height = height;
		xaml::Media::RectangleGeometry clip;
		clip.Rect({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});
		graph.Clip(clip);
		const double budget_y = height - height * (1000.0 / 60.0) / GRAPH_MAX_MS;
		budget.X1(0);
		budget.X2(width);
		budget.Y1(budget_y);
		budget.Y2(budget_y);
	}
	const double now     = EmulatorHost::ClockMs();
	const double waiting = EmulatorHost::Get().MsSinceLastPresent();
	const double start   = now - GRAPH_WINDOW_MS - GRAPH_SLOT_MS;
	const auto   recent  = EmulatorHost::Get().FramesSince(start - HITCH_WARMUP_MS);
	const auto   hitches = Hitches(recent);
	const auto   x_of    = [&](double ms) { return width * (1.0 - (now - ms) / GRAPH_WINDOW_MS); };
	const auto   y_of    = [&](double time_ms) { return height - height * std::min(time_ms, GRAPH_MAX_MS) / GRAPH_MAX_MS; };

	const auto first_slot = static_cast<int64_t>(std::floor(start / GRAPH_SLOT_MS));
	const auto last_slot  = static_cast<int64_t>(std::floor(now / GRAPH_SLOT_MS));
	std::vector<double> longest(static_cast<size_t>(last_slot - first_slot + 1), -1.0); // -1: no frame took any of the slot's time
	std::vector<std::pair<double, double>> stalls;    // from, to (ClockMs)
	std::vector<std::pair<double, double>> hitch_marks;
	for (size_t i = 0; i < recent.size(); i++) {
		const auto& frame = recent[i];
		if (frame.presented_ms < start) {
			continue;
		}
		const auto from = std::max(first_slot, static_cast<int64_t>(std::floor((frame.presented_ms - frame.time_ms) / GRAPH_SLOT_MS)));
		const auto to   = std::min(last_slot, static_cast<int64_t>(std::floor(frame.presented_ms / GRAPH_SLOT_MS)));
		for (auto slot = from; slot <= to; slot++) {
			auto& value = longest[static_cast<size_t>(slot - first_slot)];
			value       = std::max(value, frame.time_ms);
		}
		if (frame.time_ms >= STALL_MS) {
			stalls.emplace_back(frame.presented_ms - frame.time_ms, frame.presented_ms);
		} else if (hitches[i]) {
			hitch_marks.emplace_back(frame.presented_ms - frame.time_ms, frame.presented_ms);
		}
	}
	xaml::Media::PointCollection points;
	for (size_t i = 0; i < longest.size(); i++) {
		if (longest[i] >= 0.0) {
			const double middle = (static_cast<double>(first_slot + static_cast<int64_t>(i)) + 0.5) * GRAPH_SLOT_MS;
			points.Append({static_cast<float>(x_of(middle)), static_cast<float>(y_of(longest[i]))});
		}
	}
	if (waiting >= STALL_MS) {
		const double last = now - waiting;
		points.Append({static_cast<float>(x_of(last)), 0.0f});
		points.Append({static_cast<float>(width), 0.0f});
		stalls.emplace_back(last, now);
	}
	frames.Points(points);

	// The red and amber blocks are made once and moved (a hitch's at least 2 pixels wide, so a short one shows).
	const auto place = [&](const std::vector<std::pair<double, double>>& spans, std::vector<xaml::Shapes::Rectangle>& blocks,
	                       const xaml::Media::Brush& fill, double min_width) {
		for (size_t i = 0; i < spans.size(); i++) {
			if (i == blocks.size()) {
				xaml::Shapes::Rectangle block;
				block.Fill(fill);
				stall_marks.Children().Append(block);
				blocks.push_back(block);
			}
			const double right = std::clamp(x_of(spans[i].second), 0.0, width);
			const double left  = std::clamp(std::min(x_of(spans[i].first), right - min_width), 0.0, width);
			auto&        block = blocks[i];
			block.Width(right - left);
			block.Height(height);
			xaml::Controls::Canvas::SetLeft(block, left);
			block.Visibility(xaml::Visibility::Visible);
		}
		for (size_t i = spans.size(); i < blocks.size(); i++) {
			blocks[i].Visibility(xaml::Visibility::Collapsed);
		}
	};
	place(stalls, stall_blocks, stall_fill_brush, 0.0);
	place(hitch_marks, hitch_blocks, hitch_fill_brush, 2.0);
	DrawSwap(width, height, x_of);
}

// The swap file's traffic and the game's waits for it between fetches (the pager's totals, fetched at `at_ms`), kept for the graph's time
// span. A fetch seen before adds nothing; after a gap (the overlay hidden), counting starts anew.
void GameOverlay::State::RecordSwap(const MemoryTotals& totals, double at_ms) {
	if (!totals.pager || at_ms <= 0.0 || (swap_last && at_ms <= swap_last->to_ms)) {
		return;
	}
	const uint64_t moved = totals.swapped_out + totals.swapped_in;
	if (swap_last && at_ms - swap_last->to_ms <= 4.0 * static_cast<double>(UPDATE_INTERVAL.count()) && moved >= swap_last->moved &&
	    totals.swap_wait_ns >= swap_last->waited_ns && (moved != swap_last->moved || totals.swap_wait_ns != swap_last->waited_ns)) {
		swap_samples.push_back({swap_last->to_ms, at_ms, moved - swap_last->moved, totals.swap_wait_ns - swap_last->waited_ns});
	}
	swap_last = SwapSample {0.0, at_ms, moved, totals.swap_wait_ns};
	while (!swap_samples.empty() && swap_samples.front().to_ms < EmulatorHost::ClockMs() - GRAPH_WINDOW_MS) {
		swap_samples.pop_front();
	}
}

// The stripe at the bottom of the graph, Full only: white over each stretch with traffic (brighter with more), and a black notch for the time
// the game waited in it (as long as the wait on the graph's time axis, darker the larger its share of the stretch).
void GameOverlay::State::DrawSwap(double width, double height, const std::function<double(double)>& x_of) {
	size_t used = 0;
	if (detail == OVERLAY_FULL && swap_shown_in_graph) {
		const double top = height - SWAP_LANE_HEIGHT;
		for (const auto& sample: swap_samples) {
			const double left  = std::clamp(x_of(sample.from_ms), 0.0, width);
			const double right = std::clamp(x_of(sample.to_ms), 0.0, width);
			const double span  = sample.to_ms - sample.from_ms;
			if (right <= left || span <= 0.0) {
				continue;
			}
			if (used == swap_blocks.size()) {
				xaml::Shapes::Rectangle traffic;
				traffic.Fill(xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::White()));
				traffic.Height(SWAP_LANE_HEIGHT);
				xaml::Shapes::Rectangle wait;
				wait.Fill(xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::Black()));
				wait.Height(SWAP_LANE_HEIGHT);
				swap_marks.Children().Append(traffic);
				swap_marks.Children().Append(wait);
				swap_blocks.emplace_back(traffic, wait);
			}
			const auto& [traffic, wait] = swap_blocks[used++];
			traffic.Visibility(VisibleIf(sample.moved > 0));
			if (sample.moved > 0) {
				const double rate = static_cast<double>(sample.moved) / (span / 1000.0);
				traffic.Opacity(0.35 + 0.55 * std::min(rate / SWAP_FULL_BYTES_PER_S, 1.0));
				traffic.Width(right - left);
				xaml::Controls::Canvas::SetLeft(traffic, left);
				xaml::Controls::Canvas::SetTop(traffic, top);
			}
			wait.Visibility(VisibleIf(sample.waited_ns > 0));
			if (sample.waited_ns > 0) {
				const double waited_ms = static_cast<double>(sample.waited_ns) / 1e6;
				const double notch     = std::clamp(width * waited_ms / GRAPH_WINDOW_MS, 3.0, right - left);
				wait.Opacity(0.15 + 0.85 * std::min(waited_ms / span / SWAP_FULL_WAIT_SHARE, 1.0));
				wait.Width(notch);
				xaml::Controls::Canvas::SetLeft(wait, (left + right - notch) / 2.0);
				xaml::Controls::Canvas::SetTop(wait, top);
			}
		}
	}
	for (size_t i = used; i < swap_blocks.size(); i++) {
		swap_blocks[i].first.Visibility(xaml::Visibility::Collapsed);
		swap_blocks[i].second.Visibility(xaml::Visibility::Collapsed);
	}
}

// The graph is drawn with every frame the app shows (CompositionTarget::Rendering) while the overlay shows it.
void GameOverlay::State::DrawGraphWhileShown() {
	const bool drawn = root.Visibility() == xaml::Visibility::Visible && detail != OVERLAY_FRAME_RATE;
	if (drawn && !graph_rendering) {
		graph_rendering = xaml::Media::CompositionTarget::Rendering(winrt::auto_revoke, [this](auto const&, auto const&) { DrawGraph(); });
	} else if (!drawn && graph_rendering) {
		graph_rendering.revoke();
	}
}

// The fetcher's thread: the values Fetched holds, every UPDATE_INTERVAL while the overlay shows.
void GameOverlay::State::Fetch(std::stop_token stop) {
	std::mutex wait_mutex;
	while (!stop.stop_requested()) {
		if (fetching.load()) {
			Fetched values {CurrentMemoryTotals(), CurrentShaderStatistics(), fetch_game_memory.load() ? CurrentGameMemory() : std::nullopt,
			               EmulatorHost::ClockMs()};
			std::lock_guard lock(fetched_mutex);
			fetched = std::move(values);
		}
		std::unique_lock lock(wait_mutex);
		fetch_wake.wait_for(lock, stop, UPDATE_INTERVAL, [] { return false; });
	}
}

// The compile notice: it shows once the emulator has compiled for NOTICE_DELAY_MS (gaps shorter than NOTICE_GRACE_MS count as compiling),
// with what it compiles, for how long and the compiles of the stretch so far, and goes NOTICE_GRACE_MS after the last compile.
void GameOverlay::State::UpdateNotice() {
	using Common::CompileActivity::Kind;
	const auto     activity = Common::CompileActivity::Current();
	const uint64_t now      = Common::CompileActivity::NowMs();
	// A compile runs, or one ran since the last look.
	if (activity.kind != Kind::None || activity.started != last_started) {
		if (busy_since_ms == 0) {
			busy_since_ms  = activity.kind != Kind::None ? std::min(activity.start_ms, now) : now;
			started_before = last_started;
		}
		last_busy_ms = now;
	} else if (busy_since_ms != 0 && now - last_busy_ms >= NOTICE_GRACE_MS) {
		busy_since_ms = 0;
	}
	last_started     = activity.started;
	// Part of the overlay: only while it shows.
	const bool shown = notice_enabled && root.Visibility() == xaml::Visibility::Visible && busy_since_ms != 0 &&
	                   now - busy_since_ms >= NOTICE_DELAY_MS;
	if (shown) {
		const auto so_far = static_cast<unsigned long long>(activity.started - started_before);
		if (activity.kind != Kind::None) {
			const bool pipeline = activity.kind == Kind::Pipeline;
			notice_title.Text(pipeline ? L"Building pipeline" : L"Translating shaders");
			constexpr wchar_t DOT[] = {L' ', 0x00B7, L' ', 0};
			notice_detail.Text(std::wstring(pipeline ? L"Pipeline" : L"Shader") + DOT +
			                   Formatted(L"%.1f s", static_cast<double>(now - std::min(activity.start_ms, now)) / 1000.0) + DOT +
			                   Formatted(L"%llu so far", so_far));
		} else if (notice_title.Text().empty()) {
			// Only short compiles since the last look.
			notice_title.Text(L"Translating shaders");
			notice_detail.Text(Formatted(L"%llu so far", so_far));
		}
		// Above the overlay's log when that shows.
		const bool over_log = root.Visibility() == xaml::Visibility::Visible && log_shown;
		notice.Margin({0, 0, 0, over_log ? log_panel.as<xaml::FrameworkElement>().ActualHeight() + 12.0 : 0.0});
	}
	if (shown != notice_shown) {
		notice_shown = shown;
		notice.Visibility(VisibleIf(shown));
		notice_ring.IsActive(shown);
		if (!shown) {
			notice_title.Text(L"");
		}
	}
}

// The rates and the values per frame, over the last second (they stay until the next second is over).
void GameOverlay::State::UpdateRates(const MemoryTotals& totals) {
	const Sample current {std::chrono::steady_clock::now(), EmulatorHost::Get().FramesPresented(), CurrentWorkTotals(), totals,
	                      Libs::Graphics::FrameResolution::GeneratedFrames()};
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
		return Count(std::lround(static_cast<double>(bytes) / 1048576.0 / seconds)) + L" MB/s";
	};
	const auto ms_per_second = [seconds](uint64_t ns) { return Count(std::lround(static_cast<double>(ns) / 1e6 / seconds)) + L" ms/s"; };
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
		draws.Text(Count(std::lround(per_frame(counted(Counter::Draws)))));
		const uint64_t skips = delta(current.work.skipped_draws, before.work.skipped_draws);
		std::wstring   skip_text = skips == 0 ? L"0" : per_frame(skips) < 0.5 ? L"<1" : Count(std::lround(per_frame(skips)));
		if (skips != 0 && current.work.skip_reason != nullptr) {
			const std::string_view reason = current.work.skip_reason;
			skip_text += L"  (" + std::wstring(reason.begin(), reason.end()) + L")";
		}
		skipped.Text(skip_text);
		wchar_t text[32];
		swprintf_s(text, L"%.1f ms", per_frame(delta(current.work.gpu_ns, before.work.gpu_ns)) / 1e6);
		gpu_time.Text(RegionNumbers(text));
	}
	// FSR frame generation's frames per second, with the game's own (all frames shown less the made ones), while its switch is on.
	const bool generation = Config::FsrFrameGenerationEnabled();
	generated_label.Visibility(VisibleIf(generation));
	generated.Visibility(VisibleIf(generation));
	if (generation) {
		const uint64_t made = delta(current.generated, before.generated);
		if (made == 0) {
			generated.Text(Libs::Graphics::FrameResolution::LastHadPssr() ? L"–  needs 30 fps" : L"–  needs PSSR");
		} else {
			const auto per_second = [seconds](uint64_t count) { return std::lround(static_cast<double>(count) / seconds); };
			generated.Text(Count(per_second(made)) + L" fps  (game " + Count(per_second(delta(frames, made))) + L")");
		}
	}
	disk.Text(mb_per_second(counted(Counter::GuestFileReadBytes)) + L" read");
	write_faults.Text(Count(std::lround(static_cast<double>(counted(Counter::GpuFaults)) / seconds)) + L"/s");
	swap_out.Text(mb_per_second(delta(totals.swapped_out, before.memory.swapped_out)));
	swap_in.Text(mb_per_second(delta(totals.swapped_in, before.memory.swapped_in)));
	compile_wait.Text(ms_per_second(counted(Counter::ShaderCompileNs) + counted(Counter::PipelineCreateNs)));
	swap_wait.Text(ms_per_second(delta(totals.swap_wait_ns, before.memory.swap_wait_ns)));
	if (detail == OVERLAY_DETAILED) {
		UpdateDetailedRates(current, before, seconds);
	} else if (cpu_shown) {
		UpdateCpuPanel();
	}
	if (hitches_shown) {
		UpdateHitchesPanel();
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

// The CPU panel's parts, laid out from the processor the app sees (ProcessorCores): its shape, a box per core with a bar per logical processor,
// and the rows of the busiest threads. Where Windows doesn't tell each processor's load (ProcessorLoad::Available), there are no boxes.
void GameOverlay::State::BuildCpuPanel() {
	const auto find      = [this](const wchar_t* name) { return root.FindName(name); };
	const auto resources = xaml::Application::Current().Resources();
	const auto caption   = resources.Lookup(winrt::box_value(L"CaptionTextBlockStyle")).as<xaml::Style>();
	cpu_panel            = find(L"CpuPanel").as<xaml::UIElement>();
	cpu_total            = find(L"CpuTotal").as<xaml::Controls::TextBlock>();
	cpu_legend           = find(L"CpuLegend").as<xaml::UIElement>();
	cpu_total.Text(L"–");

	// "4 cores · 8 threads"; with one thread a core, "7 cores".
	const auto& cores      = ProcessorCores();
	size_t      processors = 0;
	for (const auto& core: cores) {
		processors += core.processors.size();
	}
	std::wstring shape;
	if (!cores.empty()) {
		shape = std::to_wstring(cores.size()) + L" cores";
		if (processors > cores.size()) {
			shape += L" · " + std::to_wstring(processors) + L" threads";
		}
	}
	find(L"CpuShape").as<xaml::Controls::TextBlock>().Text(shape);

	auto boxes = find(L"CpuCores").as<xaml::Controls::Grid>();
	if (cores.empty() || !ProcessorLoad::Available()) {
		boxes.Visibility(xaml::Visibility::Collapsed);
	} else {
		const int32_t columns = std::min<int32_t>(static_cast<int32_t>(cores.size()), CPU_CORES_PER_ROW);
		for (int32_t column = 0; column < columns; column++) {
			xaml::Controls::ColumnDefinition definition;
			definition.Width(xaml::GridLengthHelper::FromValueAndType(1.0, xaml::GridUnitType::Star));
			boxes.ColumnDefinitions().Append(definition);
		}
		for (size_t index = 0; index < cores.size(); index++) {
			const auto& core = cores[index];
			if (index % columns == 0) {
				xaml::Controls::RowDefinition definition;
				definition.Height(xaml::GridLengthHelper::Auto());
				boxes.RowDefinitions().Append(definition);
			}
			CoreBox entry;
			xaml::Controls::Grid bars;
			bars.ColumnSpacing(2.0);
			for (size_t slot = 0; slot < core.processors.size(); slot++) {
				xaml::Controls::ColumnDefinition definition;
				definition.Width(xaml::GridLengthHelper::FromValueAndType(1.0, xaml::GridUnitType::Star));
				bars.ColumnDefinitions().Append(definition);
				xaml::Controls::Grid track;
				track.Background(CpuBrush(0x1A, 0xFF, 0xFF, 0xFF));
				xaml::Shapes::Rectangle bar;
				bar.VerticalAlignment(xaml::VerticalAlignment::Bottom);
				bar.Height(0.0);
				track.Children().Append(bar);
				xaml::Controls::Grid::SetColumn(track, static_cast<int32_t>(slot));
				bars.Children().Append(track);
				entry.bars.emplace_back(core.processors[slot], bar);
			}
			entry.box = xaml::Controls::Border();
			entry.box.Height(CPU_CORE_HEIGHT);
			entry.box.Padding(xaml::ThicknessHelper::FromUniformLength(CPU_CORE_PADDING));
			entry.box.CornerRadius(xaml::CornerRadiusHelper::FromUniformRadius(2.0));
			entry.box.Child(bars);
			xaml::Controls::Grid::SetRow(entry.box, static_cast<int32_t>(index) / columns);
			xaml::Controls::Grid::SetColumn(entry.box, static_cast<int32_t>(index) % columns);
			boxes.Children().Append(entry.box);
			cpu_cores.push_back(std::move(entry));
		}
		ShowSystemCores(SystemCores());
	}

	auto list = find(L"CpuThreads").as<xaml::Controls::Grid>();
	for (size_t index = 0; index < CPU_THREADS; index++) {
		xaml::Controls::RowDefinition definition;
		definition.Height(xaml::GridLengthHelper::Auto());
		list.RowDefinitions().Append(definition);
		ThreadRow row;
		row.name = xaml::Controls::TextBlock();
		row.name.Style(caption);
		row.name.TextWrapping(xaml::TextWrapping::NoWrap);
		row.name.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
		row.percent = xaml::Controls::TextBlock();
		row.percent.Style(caption);
		row.percent.HorizontalAlignment(xaml::HorizontalAlignment::Right);
		xaml::Documents::Typography::SetNumeralAlignment(row.percent, xaml::FontNumeralAlignment::Tabular);
		row.track = xaml::Controls::Grid();
		row.track.Height(5.0);
		row.track.VerticalAlignment(xaml::VerticalAlignment::Center);
		row.track.Background(CpuBrush(0x24, 0xFF, 0xFF, 0xFF));
		row.bar = xaml::Shapes::Rectangle();
		row.bar.HorizontalAlignment(xaml::HorizontalAlignment::Left);
		row.bar.Fill(CpuBrush(0xFF, 0x7C, 0xC2, 0x4D));
		row.bar.Width(0.0);
		row.track.Children().Append(row.bar);
		int32_t column = 0;
		for (const auto& element: std::initializer_list<xaml::FrameworkElement> {row.name, row.percent, row.track}) {
			element.Visibility(xaml::Visibility::Collapsed);
			xaml::Controls::Grid::SetRow(element, static_cast<int32_t>(index));
			xaml::Controls::Grid::SetColumn(element, column++);
			list.Children().Append(element);
		}
		cpu_threads.push_back(row);
	}
}

// The system's cores in grey (their boxes fainter) and the game's in green, with the legend while there are any of the system's.
void GameOverlay::State::ShowSystemCores(std::vector<bool> system) {
	const auto green = CpuBrush(0xFF, 0x7C, 0xC2, 0x4D); // as the frame graph
	const auto grey  = CpuBrush(0xFF, 0x8C, 0x8C, 0x8C);
	for (size_t index = 0; index < cpu_cores.size(); index++) {
		const bool theirs = index < system.size() && system[index];
		cpu_cores[index].box.Background(CpuBrush(theirs ? 0x0A : 0x1A, 0xFF, 0xFF, 0xFF));
		for (const auto& [processor, bar]: cpu_cores[index].bars) {
			bar.Fill(theirs ? grey : green);
		}
	}
	cpu_legend.Visibility(VisibleIf(std::find(system.begin(), system.end(), true) != system.end()));
	cpu_system = std::move(system);
}

// Once a second: the app's share of the processor (as the statistics' CPU), which cores are the system's (the Xbox reserves the app's a moment
// after it starts), each logical processor's load and the busiest threads, each as its share of one core. Threads with nothing to do are left
// out.
void GameOverlay::State::UpdateCpuPanel() {
	if (cpu_share >= 0.0) {
		cpu_total.Text(std::to_wstring(std::lround(cpu_share * 100.0)) + L" %");
	}
	if (!cpu_cores.empty()) {
		if (auto system = SystemCores(); system != cpu_system) {
			ShowSystemCores(std::move(system));
		}
	}
	if (const auto load = processor_load.Sample(); !load.empty()) {
		const double height = CPU_CORE_HEIGHT - 2.0 * CPU_CORE_PADDING;
		for (const auto& core: cpu_cores) {
			for (const auto& [processor, bar]: core.bars) {
				bar.Height(processor < load.size() ? load[processor] * height : 0.0);
			}
		}
	}
	const auto threads = BusiestThreads(CPU_THREADS);
	for (size_t index = 0; index < cpu_threads.size(); index++) {
		const auto& row   = cpu_threads[index];
		const bool  shown = index < threads.size() && threads[index].percent >= 0.5;
		for (const auto& element: std::initializer_list<xaml::UIElement> {row.name, row.percent, row.track}) {
			element.Visibility(VisibleIf(shown));
		}
		if (shown) {
			const double percent = std::min(threads[index].percent, 100.0);
			row.name.Text(ThreadDisplayName(threads[index].name));
			row.percent.Text(std::to_wstring(std::lround(percent)) + L" %");
			row.bar.Width(percent / 100.0 * CPU_THREAD_BAR);
		}
	}
}

// The hitches panel's rows: the last hitch's causes and an average frame's, each a label, a value and a bar (as the CPU panel's threads).
void GameOverlay::State::BuildHitchesPanel() {
	const auto find    = [this](const wchar_t* name) { return root.FindName(name); };
	const auto caption = xaml::Application::Current().Resources().Lookup(winrt::box_value(L"CaptionTextBlockStyle")).as<xaml::Style>();
	hitches_panel      = find(L"HitchesPanel").as<xaml::UIElement>();
	hitch_count        = find(L"HitchCount").as<xaml::Controls::TextBlock>();
	hitch_last         = find(L"HitchLast").as<xaml::Controls::TextBlock>();
	hitch_last_brush   = hitch_last.Foreground();
	hitch_count.Text(L"–");
	const auto build = [&](const wchar_t* name, std::initializer_list<const wchar_t*> labels, std::vector<CauseRow>& rows) {
		auto    grid  = find(name).as<xaml::Controls::Grid>();
		int32_t index = 0;
		for (const auto* label: labels) {
			xaml::Controls::RowDefinition definition;
			definition.Height(xaml::GridLengthHelper::Auto());
			grid.RowDefinitions().Append(definition);
			xaml::Controls::TextBlock text;
			text.Style(caption);
			text.Text(label);
			text.TextWrapping(xaml::TextWrapping::NoWrap);
			text.TextTrimming(xaml::TextTrimming::CharacterEllipsis);
			xaml::Controls::TextBlock value;
			value.Style(caption);
			value.Text(L"–");
			value.HorizontalAlignment(xaml::HorizontalAlignment::Right);
			xaml::Documents::Typography::SetNumeralAlignment(value, xaml::FontNumeralAlignment::Tabular);
			xaml::Controls::Grid track;
			track.Height(5.0);
			track.VerticalAlignment(xaml::VerticalAlignment::Center);
			track.Background(CpuBrush(0x24, 0xFF, 0xFF, 0xFF));
			xaml::Shapes::Rectangle bar;
			bar.HorizontalAlignment(xaml::HorizontalAlignment::Left);
			bar.Fill(CpuBrush(0xFF, 0x7C, 0xC2, 0x4D));
			bar.Width(0.0);
			track.Children().Append(bar);
			int32_t column = 0;
			for (const auto& element: std::initializer_list<xaml::FrameworkElement> {text, value, track}) {
				xaml::Controls::Grid::SetRow(element, index);
				xaml::Controls::Grid::SetColumn(element, column++);
				grid.Children().Append(element);
			}
			rows.push_back({value, bar});
			index++;
		}
	};
	build(L"HitchCauses", {L"Compiling", L"GPU wait", L"GPU readback", L"Screen wait", L"Showing frame", L"Game, other"}, hitch_rows);
	build(L"FrameCauses", {L"GPU commands", L"Compiling", L"GPU wait", L"Screen wait", L"Showing frame"}, frame_rows);
}

// Four times a second: the hitches in the graph's time span (its amber and red marks), the last one's causes, or while the game stalls what
// the time without a frame went on so far, and an average frame of the last second.
void GameOverlay::State::UpdateHitchesPanel() {
	using Work         = EmulatorHost::FrameWork;
	auto&        host  = EmulatorHost::Get();
	const double now   = EmulatorHost::ClockMs();
	const auto frames  = host.FramesSince(now - GRAPH_WINDOW_MS - HITCH_WARMUP_MS);
	const auto hitches = Hitches(frames);
	size_t     count   = 0;
	const EmulatorHost::RecentFrame* last = nullptr;
	for (size_t i = 0; i < frames.size(); i++) {
		if (hitches[i] && frames[i].presented_ms >= now - GRAPH_WINDOW_MS) {
			count++;
			last = &frames[i];
		}
	}
	hitch_count.Text(std::to_wstring(count));

	// Each row's time (blank without one) and its share of `total_ms` as its bar.
	const auto show = [](std::vector<CauseRow>& rows, const std::vector<double>& values, double total_ms, const wchar_t* format) {
		for (size_t i = 0; i < rows.size(); i++) {
			const bool known = i < values.size() && total_ms > 0.0;
			rows[i].value.Text(known ? Formatted(format, values[i]) : std::wstring(L"–"));
			rows[i].bar.Width(known ? std::clamp(values[i] / total_ms, 0.0, 1.0) * HITCH_BAR : 0.0);
		}
	};
	const auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
	// The causes, in the rows' order; the game and the rest is the time they leave (they overlap: a readback also waits for the GPU).
	const auto causes = [&](const Work& work, double total_ms) {
		const double present = ms(work.present_ns);
		const double screen  = std::min(ms(work.screen_wait_ns), present);
		const double counted = ms(work.compile_ns) + ms(work.gpu_wait_ns) + ms(work.readback_ns) + present;
		return std::vector<double> {ms(work.compile_ns), ms(work.gpu_wait_ns), ms(work.readback_ns), screen, present - screen,
		                            std::max(total_ms - counted, 0.0)};
	};
	const double waiting = host.MsSinceLastPresent();
	if (host.FramesPresented() > 0 && waiting >= STALL_MS) {
		hitch_last.Text(Formatted(L"No new frame for %.1f s", waiting / 1000.0));
		hitch_last.Foreground(stall_brush);
		show(hitch_rows, causes(host.WorkSinceLastPresent(), waiting), waiting, L"%.0f ms");
	} else if (last != nullptr) {
		hitch_last.Text(Formatted(L"Last %.0f ms · %.0f s ago", last->time_ms, (now - last->presented_ms) / 1000.0));
		hitch_last.Foreground(hitch_last_brush);
		show(hitch_rows, causes(last->work, last->time_ms), last->time_ms, L"%.0f ms");
	} else {
		hitch_last.Text(L"None");
		hitch_last.Foreground(hitch_last_brush);
		show(hitch_rows, {}, 0.0, L"");
	}

	// An average frame of the last second.
	double frame_ms = 0.0;
	Work   work;
	size_t frames_counted = 0;
	for (const auto& frame: frames) {
		if (frame.presented_ms < now - 1000.0 * RATE_SECONDS) {
			continue;
		}
		frame_ms += frame.time_ms;
		work.compile_ns += frame.work.compile_ns;
		work.gpu_wait_ns += frame.work.gpu_wait_ns;
		work.screen_wait_ns += frame.work.screen_wait_ns;
		work.present_ns += frame.work.present_ns;
		work.gpu_thread_ns += frame.work.gpu_thread_ns;
		frames_counted++;
	}
	if (frames_counted == 0) {
		show(frame_rows, {}, 0.0, L"");
		return;
	}
	const double n       = static_cast<double>(frames_counted);
	const double present = ms(work.present_ns) / n;
	const double screen  = std::min(ms(work.screen_wait_ns) / n, present);
	show(frame_rows, {ms(work.gpu_thread_ns) / n, ms(work.compile_ns) / n, ms(work.gpu_wait_ns) / n, screen, present - screen}, frame_ms / n,
	     L"%.1f ms");
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
	Set(STAT_SWAP_POOL, SwapPoolText(totals, Bytes(totals.pool_bytes)));
	// The CPU as Full has it, with the cores it makes.
	if (cpu_share >= 0.0) {
		const auto cores = std::max(1u, std::thread::hardware_concurrency());
		Set(STAT_CPU, Formatted(L"%.0f%%  (%.1f of %u cores)", cpu_share * 100.0, cpu_share * cores, cores));
	}
	if (const auto& statistics = shown_values.shaders) {
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
	Set(STAT_COLOR_READBACKS, Formatted(L"%.0f ms/s  (%.0f/s)", rate(Counter::ColorMetadataReadbackNs) / 1e6,
	                                    rate(Counter::ColorMetadataReadbacks)));
	Set(STAT_COLOR_CHECKS, Formatted(L"%.0f/s", rate(Counter::ColorMetadataChecks)));
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

	if (const auto& game = shown_values.game_memory) {
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
	// The overlay and the compile notice have the same size.
	for (const auto& layer: {root, notice_layer}) {
		if (scale == 1.0) {
			layer.RenderTransform(nullptr);
			layer.HorizontalAlignment(xaml::HorizontalAlignment::Stretch);
			layer.VerticalAlignment(xaml::VerticalAlignment::Stretch);
			layer.ClearValue(xaml::FrameworkElement::WidthProperty());
			layer.ClearValue(xaml::FrameworkElement::HeightProperty());
			continue;
		}
		xaml::Media::ScaleTransform transform;
		transform.ScaleX(scale);
		transform.ScaleY(scale);
		layer.RenderTransform(transform);
		layer.HorizontalAlignment(xaml::HorizontalAlignment::Left);
		layer.VerticalAlignment(xaml::VerticalAlignment::Top);
	}
	if (scale != 1.0) {
		Fit(page.ActualWidth(), page.ActualHeight());
	}
}

void GameOverlay::State::Fit(double width, double height) {
	if (scale != 1.0 && width > 0.0 && height > 0.0) {
		root.Width(width / scale);
		root.Height(height / scale);
		notice_layer.Width(width / scale);
		notice_layer.Height(height / scale);
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
	state.stall_marks      = find(L"StallMarks").as<xaml::Controls::Canvas>();
	state.swap_marks       = find(L"SwapMarks").as<xaml::Controls::Canvas>();
	state.fps_brush        = state.fps.Foreground();
	state.stall_brush      = xaml::Media::SolidColorBrush(winrt::Windows::UI::Color {0xFF, 0xF0, 0x95, 0x95});
	state.stall_fill_brush = xaml::Media::SolidColorBrush(winrt::Windows::UI::Color {0x47, 0xE2, 0x4B, 0x4A});
	state.hitch_fill_brush = xaml::Media::SolidColorBrush(winrt::Windows::UI::Color {0x5C, 0xEF, 0x9F, 0x27});
	state.statistics        = find(L"Statistics").as<xaml::Controls::Grid>();
	state.draws             = find(L"Draws").as<xaml::Controls::TextBlock>();
	state.skipped           = find(L"Skipped").as<xaml::Controls::TextBlock>();
	state.gpu_time          = find(L"GpuTime").as<xaml::Controls::TextBlock>();
	state.generated_label   = find(L"GeneratedLabel").as<xaml::Controls::TextBlock>();
	state.generated         = find(L"Generated").as<xaml::Controls::TextBlock>();
	state.output_resolution = find(L"OutputResolution").as<xaml::Controls::TextBlock>();
	state.render_resolution = find(L"RenderResolution").as<xaml::Controls::TextBlock>();
	state.display_resolution = find(L"DisplayResolution").as<xaml::Controls::TextBlock>();
	state.build              = find(L"Build").as<xaml::Controls::TextBlock>();
	state.build.Text(BuildLine());
	state.cpu               = find(L"Cpu").as<xaml::Controls::TextBlock>();
	state.disk              = find(L"Disk").as<xaml::Controls::TextBlock>();
	state.write_faults      = find(L"WriteFaults").as<xaml::Controls::TextBlock>();
	state.memory            = find(L"Memory").as<xaml::Controls::TextBlock>();
	state.game_memory       = find(L"GameMemory").as<xaml::Controls::TextBlock>();
	state.gpu_memory        = find(L"GpuMemory").as<xaml::Controls::TextBlock>();
	state.other_memory      = find(L"OtherMemory").as<xaml::Controls::TextBlock>();
	state.swapped_memory    = find(L"SwappedMemory").as<xaml::Controls::TextBlock>();
	state.swap_pool         = find(L"SwapPool").as<xaml::Controls::TextBlock>();
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
	state.BuildCpuPanel();
	state.BuildHitchesPanel();
	state.BuildDetailed(find(L"TopRow").as<xaml::Controls::Panel>(),
	                    state.root.Resources().Lookup(winrt::box_value(L"PanelBrush")).as<xaml::Media::Brush>());
	page.Children().InsertAt(index, state.root);
	// The compile notice over the overlay (and under the game menu).
	state.notice_layer  = xaml::Markup::XamlReader::Load(CompileNoticeMarkup).as<xaml::Controls::Grid>();
	state.notice        = state.notice_layer.FindName(L"Notice").as<xaml::Controls::Border>();
	state.notice_ring   = state.notice_layer.FindName(L"NoticeRing").as<xaml::Controls::ProgressRing>();
	state.notice_title  = state.notice_layer.FindName(L"NoticeTitle").as<xaml::Controls::TextBlock>();
	state.notice_detail = state.notice_layer.FindName(L"NoticeDetail").as<xaml::Controls::TextBlock>();
	state.last_started  = Common::CompileActivity::Current().started;
	page.Children().InsertAt(index + 1, state.notice_layer);
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
	state.frame_timer = xaml::DispatcherTimer();
	state.frame_timer.Interval(FRAME_INTERVAL);
	state.frame_timer.Tick([weak = std::weak_ptr<State>(m_state)](auto const&, auto const&) {
		if (auto shared = weak.lock()) {
			shared->UpdateFrame();
		}
	});
	state.fetcher = std::jthread([&state](std::stop_token stop) { state.Fetch(stop); });
	Apply(LoadSettings());
}

GameOverlay::~GameOverlay() {
	m_state->timer.Stop();
	m_state->frame_timer.Stop();
}

void GameOverlay::Show(bool shown) {
	auto& state = *m_state;
	state.root.Visibility(shown ? xaml::Visibility::Visible : xaml::Visibility::Collapsed);
	// Upstream's counters count while the overlay shows, and the fetcher fetches.
	CountersOn(shown);
	state.fetching = shown;
	if (shown) {
		state.fetch_wake.notify_all();
		if (state.log_tail == nullptr) {
			state.log_tail = std::make_unique<LogTail>();
		}
		state.Update();
		state.UpdateFrame();
		state.timer.Start();
		state.frame_timer.Start();
	} else {
		state.timer.Stop();
		state.frame_timer.Stop();
		state.UpdateNotice();
	}
	state.DrawGraphWhileShown();
}

bool GameOverlay::Shown() const {
	return m_state->root.Visibility() == xaml::Visibility::Visible;
}

void GameOverlay::Apply(const AppSettings& settings) {
	auto& state              = *m_state;
	state.detail             = std::min(settings.overlay_detail, OVERLAY_DETAILED);
	state.log_shown          = settings.overlay_log;
	state.controllers_shown  = settings.overlay_controllers;
	state.cpu_shown          = settings.overlay_cpu;
	state.hitches_shown      = settings.overlay_hitches;
	state.swap_shown_in_graph = settings.overlay_swap;
	state.log_lines          = std::clamp<size_t>(settings.overlay_log_lines, 2, LOG_LINES);
	state.headset_mic_option = settings.headset_mic;
	state.notice_enabled     = settings.compile_notice;
	state.fetch_game_memory  = std::min(settings.overlay_detail, OVERLAY_DETAILED) == OVERLAY_DETAILED;
	state.UpdateNotice();
	state.DrawGraphWhileShown();
	// Detailed: the box with the game, the frame rate and the graph, and its panels beside it.
	state.header.Visibility(VisibleIf(state.detail == OVERLAY_FULL || state.detail == OVERLAY_DETAILED));
	state.graph.Visibility(VisibleIf(state.detail != OVERLAY_FRAME_RATE));
	state.statistics.Visibility(VisibleIf(state.detail == OVERLAY_FULL));
	state.compact.Visibility(VisibleIf(state.detail == OVERLAY_COMPACT));
	state.build.Visibility(VisibleIf(state.detail == OVERLAY_FULL || state.detail == OVERLAY_DETAILED));
	for (const auto& panel: state.detailed_panels) {
		panel.Visibility(VisibleIf(state.detail == OVERLAY_DETAILED));
	}
	state.stats_panel.Width(state.detail == OVERLAY_FRAME_RATE ? std::numeric_limits<double>::quiet_NaN() : STATS_WIDTH);
	state.controllers_panel.Visibility(VisibleIf(state.controllers_shown));
	state.cpu_panel.Visibility(VisibleIf(state.cpu_shown && state.detail != OVERLAY_DETAILED));
	state.hitches_panel.Visibility(VisibleIf(state.hitches_shown));
	state.log_panel.Visibility(VisibleIf(state.log_shown));
	state.SetScale(OverlaySizes[std::min<size_t>(settings.overlay_size, OverlaySizes.size() - 1)]);
	if (Shown()) {
		state.Update();
	}
}

void AddOverlaySettings(xaml::Controls::Panel const& overlay, xaml::Controls::Panel const& on_screen, xaml::Controls::StackPanel const& explanation,
                        std::shared_ptr<AppSettings> settings, std::function<void()> changed) {
	// The swap file's stripe in the graph is Full's: its switch shows while Full is chosen.
	const auto swap_tile = std::make_shared<xaml::Controls::Button>(nullptr);
	const auto full      = [](size_t index) { return index == 1 + OVERLAY_FULL; };
	const size_t current = OverlayShown() ? 1 + std::min(settings->overlay_detail, OVERLAY_DETAILED) : 0;
	AddChoice(
	    overlay, explanation, L"Overlay", {L"Off", L"Frame rate", L"Compact", L"Full", L"Detailed"}, current,
	    [settings, changed, swap_tile, full](size_t index) {
		    SetOverlayShown(index != 0);
		    if (index != 0) {
			    settings->overlay_detail = static_cast<uint32_t>(index - 1);
		    }
		    if (*swap_tile != nullptr) {
			    swap_tile->Visibility(VisibleIf(full(index)));
		    }
		    changed();
	    },
	    {L"Each step shows more, from just the frame rate up to details for finding performance problems.",
	     L"During a game, hold both triggers and press [View] + [Menu] to show or hide it."});
	*swap_tile = AddSwitch(
	    overlay, explanation, L"Swap file in the graph", settings->overlay_swap,
	    [settings, changed](bool on) {
		    settings->overlay_swap = on;
		    changed();
	    },
	    {L"Under the frame times: white where data went to or came from the swap file, black where the game waited for it."});
	swap_tile->Visibility(VisibleIf(full(current)));
	AddChoice(overlay, explanation, L"Overlay size", {L"Small", L"Medium", L"Large"}, std::min<uint32_t>(settings->overlay_size, 2),
	          [settings, changed](size_t index) {
		          settings->overlay_size = static_cast<uint32_t>(index);
		          changed();
	          });
	AddSwitch(on_screen, explanation, L"Controllers on screen", settings->overlay_controllers, [settings, changed](bool on) {
		settings->overlay_controllers = on;
		changed();
	});
	AddSwitch(
	    on_screen, explanation, L"CPU on screen", settings->overlay_cpu,
	    [settings, changed](bool on) {
		    settings->overlay_cpu = on;
		    changed();
	    },
	    {L"A panel with the load of each processor core and the busiest threads.", L"Detailed lists the threads itself and doesn't show it."});
	AddSwitch(
	    on_screen, explanation, L"Hitches on screen", settings->overlay_hitches,
	    [settings, changed](bool on) {
		    settings->overlay_hitches = on;
		    changed();
	    },
	    {L"A panel with the slow frames of the last 10 seconds and what the last one's time went on, and where an average frame's goes.",
	     L"The frame graph marks them in amber, stalls in red."});
	// Off, or 2 to 20 lines in steps of 2 (overlay_log and overlay_log_lines; an odd number from the settings file shows as the next even one).
	{
		constexpr auto            MAX_LINES = static_cast<uint32_t>(LOG_LINES);
		std::vector<std::wstring> lines {L"Off"};
		for (uint32_t count = 2; count <= MAX_LINES; count += 2) {
			lines.push_back(std::to_wstring(count) + L" lines");
		}
		const size_t selected = settings->overlay_log ? (std::clamp<uint32_t>(settings->overlay_log_lines, 2, MAX_LINES) + 1) / 2 : 0;
		AddChoice(on_screen, explanation, L"Log on screen", std::move(lines), selected, [settings, changed](size_t index) {
			settings->overlay_log = index != 0;
			if (index != 0) {
				settings->overlay_log_lines = static_cast<uint32_t>(index) * 2;
			}
			changed();
		});
	}
	AddSwitch(
	    on_screen, explanation, L"Compile notice", settings->compile_notice,
	    [settings, changed](bool on) {
		    settings->compile_notice = on;
		    changed();
	    },
	    {L"Shows a notice while a game waits for its graphics to be prepared, so you know it hasn't frozen."});
}

} // namespace Kyty::Uwp
