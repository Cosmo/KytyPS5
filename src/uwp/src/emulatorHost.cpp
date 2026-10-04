#include "emulatorHost.h"

#include "common/archive.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "fileAccess.h"
#include "gameLibrary.h"
#include "gameSource.h"
#include "graphics/host_gpu/d3d12/selfTest.h"
#include "log.h"
#include "settings.h"

#include <windows.h>

#include <dxgi1_3.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <future>

#include <io.h>

namespace Kyty::Uwp {

namespace core = winrt::Windows::UI::Core;

namespace {

bool IsAmdCpu() {
	return Common::GetSystemInfo().ProcessorName.find("AMD") != std::string::npos;
}

} // namespace

std::optional<ShaderStatistics> CurrentShaderStatistics() {
	return std::nullopt; // the renderer makes no shaders yet
}

EmulatorHost& EmulatorHost::Get() {
	static EmulatorHost host;
	return host;
}

void EmulatorHost::Start(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel, std::filesystem::path game_dir) {
	if (m_started.exchange(true)) {
		return;
	}
	m_start_ms   = GetTickCount64();
	m_panel      = winrt::make_agile(panel);
	m_dispatcher = core::CoreWindow::GetForCurrentThread().Dispatcher();
	Log("starting %ls\n", game_dir.c_str());

	// The emulator writes its data folders (_SaveData, ...) relative to the working directory, and its messages to stdout and stderr: both go to
	// LocalState.
	const std::wstring local_state(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
	if (!SetCurrentDirectoryW(local_state.c_str())) {
		Log("could not change to LocalState: %lu\n", GetLastError());
	}
	if (_wfreopen((local_state + L"\\kyty-emulator.txt").c_str(), L"w", stdout) != nullptr) {
		setvbuf(stdout, nullptr, _IONBF, 0);
		_dup2(_fileno(stdout), _fileno(stderr));
	}
	m_log_generation++;

	// The game's patches, as with the desktop launcher's _Patches folder: LocalState\_Patches\<title ID>.json.
	std::filesystem::path game_patch;
	if (const auto title_id = ReadGame(game_dir).title_id; !title_id.empty()) {
		const auto patch = std::filesystem::path(local_state) / L"_Patches" / (winrt::to_hstring(title_id) + L".json").c_str();
		if (IsFile(patch)) {
			game_patch = patch;
			Log("patches: %ls\n", patch.c_str());
		}
	}

	// The settings page's choices (LocalState\kyty-uwp.json).
	const auto settings = LoadSettings();

	m_thread = std::thread([this, game_dir = std::move(game_dir), game_patch, settings] {
		// The emulator's main thread: the window logic runs here, not on the UI thread.
		Common::VirtualMemory::Init();
		Common::InitializeThreads();

		if (settings.d3d12_selftest) {
			(void)Libs::Graphics::D3D12::RunShaderSelfTest(settings.debug_layer);
		}

		Emulator::RunOptions options;
		options.app0_dir   = IsArchivePath(game_dir) ? Common::MakeArchivePath(game_dir) : game_dir;
		options.elf        = "/app0/eboot.bin";
		options.game_patch = game_patch;
		auto& config         = options.config;
		config.screen_width  = settings.screen_width;
		config.screen_height = settings.screen_height;
		config.present_mode  = settings.vsync ? Config::PresentMode::Fifo : Config::PresentMode::Mailbox;
		if (!settings.user_name.empty() && settings.user_name.size() <= Config::MAX_USER_NAME_LENGTH) {
			config.user_name = settings.user_name;
		}
		config.console_language          = std::min(settings.console_language, Config::MAX_CONSOLE_LANGUAGE);
		config.vulkan_validation_enabled = settings.debug_layer; // the D3D12 debug layer
		config.printf_direction          = settings.game_output ? Config::LogDirection::Console : Config::LogDirection::Silent;
		// The guest uses instructions only AMD CPUs have: on another CPU the emulator patches them (its --amd-cpu option).
		config.amd_cpu_enabled = !IsAmdCpu();
		Emulator::Run(options);

		Log("emulator stopped\n");
		(void)m_dispatcher.RunAsync(core::CoreDispatcherPriority::Normal, [] { winrt::Windows::UI::Xaml::Application::Current().Exit(); });
	});
	m_thread.detach();
}

void EmulatorHost::SetPanelSize(float width, float height, float scale_x, float scale_y) {
	std::lock_guard lock(m_mutex);
	m_width   = width;
	m_height  = height;
	m_scale_x = scale_x;
	m_scale_y = scale_y;
}

void EmulatorHost::RequestExit() {
	{
		std::lock_guard lock(m_mutex);
		m_exit_requested = true;
	}
	m_exit_changed.notify_all();
}

void EmulatorHost::QuitToLibrary() {
	m_restart = true;
	RequestExit();
}

void EmulatorHost::Finish() {
	if (!m_restart) {
		return;
	}
	// Restarts only while the app is in the foreground, which it is when the player quits.
	const auto reason = winrt::Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(L"").get();
	Log("restart to the launcher failed: %d\n", static_cast<int>(reason));
}

template <typename Task>
void EmulatorHost::RunOnUiThread(Task&& task) {
	std::promise<void> done;
	auto               finished = done.get_future();
	(void)m_dispatcher.RunAsync(core::CoreDispatcherPriority::Normal, [&] {
		try {
			task();
			done.set_value();
		} catch (...) {
			done.set_exception(std::current_exception());
		}
	});
	finished.get();
}

void EmulatorHost::AttachSwapChain(IDXGISwapChain1* swapchain) {
	// The swap chain is sized in physical pixels; the panel lays it out in view pixels.
	float scale_x = 1.0f;
	float scale_y = 1.0f;
	{
		std::lock_guard lock(m_mutex);
		scale_x = m_scale_x;
		scale_y = m_scale_y;
	}
	winrt::com_ptr<IDXGISwapChain2> swapchain2;
	if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(swapchain2.put())))) {
		DXGI_MATRIX_3X2_F inverse_scale {};
		inverse_scale._11 = 1.0f / scale_x;
		inverse_scale._22 = 1.0f / scale_y;
		(void)swapchain2->SetMatrixTransform(&inverse_scale);
	}
	RunOnUiThread([&] {
		auto native = m_panel.get().as<ISwapChainPanelNative>();
		winrt::check_hresult(native->SetSwapChain(swapchain));
	});
}

std::pair<uint32_t, uint32_t> EmulatorHost::PanelPixelSize() const {
	std::lock_guard lock(m_mutex);
	const auto      pixels = [](float size, float scale) { return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(size * scale))); };
	return {pixels(m_width, m_scale_x), pixels(m_height, m_scale_y)};
}

bool EmulatorHost::WaitForExit(uint32_t milliseconds) {
	std::unique_lock lock(m_mutex);
	return m_exit_changed.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return m_exit_requested; });
}

void EmulatorHost::OnGameVisible(std::function<void()> handler) {
	m_game_visible = std::move(handler);
}

void EmulatorHost::FramePresented() {
	LARGE_INTEGER counter {};
	QueryPerformanceCounter(&counter);
	{
		std::lock_guard lock(m_presents_mutex);
		m_present_counters[m_presents++ % m_present_counters.size()] = counter.QuadPart;
	}

	if (m_game_shown) {
		return;
	}
	// Games often present a few black frames early, then load: wait for a steady frame rate.
	const uint64_t now                         = GetTickCount64();
	const uint64_t oldest                      = m_frame_ms[m_frames % m_frame_ms.size()];
	m_frame_ms[m_frames++ % m_frame_ms.size()] = now;
	if (m_frames > m_frame_ms.size() && now - oldest < 1000) {
		m_game_shown = true;
		if (m_game_visible) {
			(void)m_dispatcher.RunAsync(core::CoreDispatcherPriority::Normal, [handler = m_game_visible] { handler(); });
		}
	}
}

uint64_t EmulatorHost::RunTimeMs() const {
	const uint64_t start = m_start_ms.load();
	return start == 0 ? 0 : GetTickCount64() - start;
}

std::vector<double> EmulatorHost::RecentFrameTimes(size_t count) const {
	LARGE_INTEGER frequency {};
	QueryPerformanceFrequency(&frequency);
	std::lock_guard lock(m_presents_mutex);
	const size_t    ring = m_present_counters.size();
	count                = std::min({count, ring - 1, m_presents > 0 ? static_cast<size_t>(m_presents - 1) : 0});
	std::vector<double> times;
	times.reserve(count);
	for (size_t age = count; age > 0; age--) {
		const auto newer = m_present_counters[(m_presents - age) % ring];
		const auto older = m_present_counters[(m_presents - age - 1) % ring];
		times.push_back(static_cast<double>(newer - older) * 1000.0 / static_cast<double>(frequency.QuadPart));
	}
	return times;
}

void EmulatorHost::SetTitle(std::string text) {
	// The title changes every frame; the view title is updated a few times a second.
	const uint64_t now  = GetTickCount64();
	uint64_t       last = m_last_title_ms.load(std::memory_order_relaxed);
	if (now - last < 250 || !m_last_title_ms.compare_exchange_strong(last, now)) {
		return;
	}
	(void)m_dispatcher.RunAsync(core::CoreDispatcherPriority::Low, [text = std::move(text)] {
		winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().Title(winrt::to_hstring(text));
	});
}

} // namespace Kyty::Uwp
