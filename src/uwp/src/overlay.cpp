#include "overlay.h"

#include "emulatorHost.h"
#include "input.h"
#include "settings.h"

#include <windows.h>
#include <fileapifromapp.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace xaml = winrt::Windows::UI::Xaml;

constexpr auto   UPDATE_INTERVAL = std::chrono::milliseconds(250);
constexpr size_t GRAPH_FRAMES    = 120;
// The graph's range: a frame at 60 fps sits at a third of its height.
constexpr double GRAPH_MAX_MS = 50.0;
constexpr size_t LOG_LINES    = 20;

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

  <Border Width="300" HorizontalAlignment="Left" VerticalAlignment="Top" Padding="12"
          Background="{StaticResource PanelBrush}">
    <StackPanel Spacing="6">
      <StackPanel>
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
      <!-- Groups, labels, values; a gap between the groups. -->
      <Grid ColumnSpacing="12">
        <Grid.Resources>
          <Style TargetType="TextBlock" BasedOn="{StaticResource CaptionTextBlockStyle}" />
        </Grid.Resources>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="Auto" />
          <ColumnDefinition Width="*" />
        </Grid.ColumnDefinitions>
        <Grid.RowDefinitions>
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="8" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
          <RowDefinition Height="Auto" />
        </Grid.RowDefinitions>
        <TextBlock Grid.Row="0" Text="System" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="3" Text="Shaders" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="6" Text="Pipelines" Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
        <TextBlock Grid.Row="0" Grid.Column="1" Text="CPU" />
        <TextBlock Grid.Row="1" Grid.Column="1" Text="RAM" />
        <TextBlock Grid.Row="3" Grid.Column="1" Text="Drawing" />
        <TextBlock Grid.Row="4" Grid.Column="1" Text="Compute" />
        <TextBlock Grid.Row="6" Grid.Column="1" Text="Built" />
        <TextBlock Grid.Row="7" Grid.Column="1" Text="From cache" />
        <TextBlock Grid.Row="8" Grid.Column="1" Text="In cache" />
        <TextBlock x:Name="Cpu" Grid.Row="0" Grid.Column="2" />
        <TextBlock x:Name="Memory" Grid.Row="1" Grid.Column="2" />
        <TextBlock x:Name="DrawingShaders" Grid.Row="3" Grid.Column="2" />
        <TextBlock x:Name="ComputeShaders" Grid.Row="4" Grid.Column="2" />
        <TextBlock x:Name="Pipelines" Grid.Row="6" Grid.Column="2" />
        <TextBlock x:Name="CachedPipelines" Grid.Row="7" Grid.Column="2" />
        <TextBlock x:Name="SavedPipelines" Grid.Row="8" Grid.Column="2" />
      </Grid>
    </StackPanel>
  </Border>

  <Border MinWidth="220" HorizontalAlignment="Right" VerticalAlignment="Top" Padding="12"
          Background="{StaticResource PanelBrush}">
    <StackPanel Spacing="4">
      <TextBlock Text="Controllers" Style="{StaticResource CaptionTextBlockStyle}"
                 Foreground="{ThemeResource SystemControlForegroundBaseMediumBrush}" />
      <StackPanel x:Name="Controllers" Spacing="2" />
    </StackPanel>
  </Border>

  <Border Grid.Row="2" Padding="12,8" Background="{StaticResource PanelBrush}">
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
						m_lines.push_back(std::move(m_partial));
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

} // namespace

struct GameOverlay::State {
	xaml::Controls::Grid       root {nullptr};
	xaml::Controls::TextBlock  run_time {nullptr};
	xaml::Controls::TextBlock  fps {nullptr};
	xaml::Controls::TextBlock  frame_time {nullptr};
	xaml::Controls::Grid       graph {nullptr};
	xaml::Shapes::Line         budget {nullptr};
	xaml::Shapes::Polyline     frames {nullptr};
	xaml::Controls::TextBlock  memory {nullptr};
	xaml::Controls::TextBlock  cpu {nullptr};
	xaml::Controls::TextBlock  drawing_shaders {nullptr};
	xaml::Controls::TextBlock  compute_shaders {nullptr};
	xaml::Controls::TextBlock  pipelines {nullptr};
	xaml::Controls::TextBlock  cached_pipelines {nullptr};
	xaml::Controls::TextBlock  saved_pipelines {nullptr};
	xaml::Controls::StackPanel controllers {nullptr};
	xaml::Controls::TextBlock  log {nullptr};
	xaml::DispatcherTimer      timer {nullptr};
	std::unique_ptr<LogTail>   log_tail;
	// The process's CPU time (100 ns units) and the time, at the last update.
	uint64_t                              cpu_time = 0;
	std::chrono::steady_clock::time_point cpu_time_at;

	void Update();
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

	// Memory: on Xbox the limit is the app's budget; on a PC it's no real limit.
	const uint64_t used  = winrt::Windows::System::MemoryManager::AppMemoryUsage();
	const uint64_t limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
	memory.Text(Megabytes(used) +
	            (limit < (uint64_t {64} << 30) ? L" of " + Megabytes(limit) : std::wstring()));

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
		pipelines.Text(std::to_wstring(statistics->pipelines));
		cached_pipelines.Text(std::to_wstring(statistics->cached_pipelines));
		saved_pipelines.Text(std::to_wstring(statistics->saved_pipelines));
	}

	controllers.Children().Clear();
	const auto connected = ConnectedControllers();
	if (connected.empty()) {
		xaml::Controls::TextBlock none;
		none.Text(L"None connected");
		controllers.Children().Append(none);
	}
	for (const auto& controller: connected) {
		xaml::Controls::TextBlock line;
		line.Text(L"Player " + std::to_wstring(controller.player + 1) + L"    " + controller.name);
		controllers.Children().Append(line);
	}

	std::wstring text;
	for (const auto& line: log_tail->Update()) {
		text += (text.empty() ? L"" : L"\n") + std::wstring(winrt::to_hstring(line));
	}
	log.Text(text);
}

GameOverlay::GameOverlay(xaml::Controls::Grid const& page, uint32_t index,
                         winrt::hstring const& game_name)
    : m_state(std::make_shared<State>()) {
	auto& state = *m_state;
	state.root  = xaml::Markup::XamlReader::Load(OverlayMarkup).as<xaml::Controls::Grid>();
	const auto find = [&state](const wchar_t* name) { return state.root.FindName(name); };
	find(L"Game").as<xaml::Controls::TextBlock>().Text(game_name);
	state.run_time         = find(L"RunTime").as<xaml::Controls::TextBlock>();
	state.fps              = find(L"Fps").as<xaml::Controls::TextBlock>();
	state.frame_time       = find(L"FrameTime").as<xaml::Controls::TextBlock>();
	state.graph            = find(L"Graph").as<xaml::Controls::Grid>();
	state.budget           = find(L"Budget").as<xaml::Shapes::Line>();
	state.frames           = find(L"Frames").as<xaml::Shapes::Polyline>();
	state.memory           = find(L"Memory").as<xaml::Controls::TextBlock>();
	state.cpu              = find(L"Cpu").as<xaml::Controls::TextBlock>();
	state.drawing_shaders  = find(L"DrawingShaders").as<xaml::Controls::TextBlock>();
	state.compute_shaders  = find(L"ComputeShaders").as<xaml::Controls::TextBlock>();
	state.pipelines        = find(L"Pipelines").as<xaml::Controls::TextBlock>();
	state.cached_pipelines = find(L"CachedPipelines").as<xaml::Controls::TextBlock>();
	state.saved_pipelines  = find(L"SavedPipelines").as<xaml::Controls::TextBlock>();
	state.controllers      = find(L"Controllers").as<xaml::Controls::StackPanel>();
	state.log              = find(L"Log").as<xaml::Controls::TextBlock>();
	page.Children().InsertAt(index, state.root);

	// Its size (Settings): laid out over the page enlarged by 1 / scale, then scaled down to fit.
	const double scale = OverlaySizes[std::min<size_t>(LoadSettings().overlay_size, OverlaySizes.size() - 1)];
	if (scale != 1.0) {
		xaml::Media::ScaleTransform transform;
		transform.ScaleX(scale);
		transform.ScaleY(scale);
		state.root.RenderTransform(transform);
		state.root.HorizontalAlignment(xaml::HorizontalAlignment::Left);
		state.root.VerticalAlignment(xaml::VerticalAlignment::Top);
		const auto fit = [root = state.root, scale](double width, double height) {
			if (width > 0.0 && height > 0.0) {
				root.Width(width / scale);
				root.Height(height / scale);
			}
		};
		fit(page.ActualWidth(), page.ActualHeight());
		page.SizeChanged([fit](auto const&, xaml::SizeChangedEventArgs const& args) {
			fit(args.NewSize().Width, args.NewSize().Height);
		});
	}

	state.timer = xaml::DispatcherTimer();
	state.timer.Interval(UPDATE_INTERVAL);
	state.timer.Tick([weak = std::weak_ptr<State>(m_state)](auto const&, auto const&) {
		if (auto shared = weak.lock()) {
			shared->Update();
		}
	});
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

} // namespace Kyty::Uwp
