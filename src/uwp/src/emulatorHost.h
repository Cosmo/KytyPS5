#ifndef EMULATOR_SRC_UWP_EMULATORHOST_H_
#define EMULATOR_SRC_UWP_EMULATORHOST_H_

// Before C++/WinRT, which then supports classic COM interfaces (ISwapChainPanelNative).
#include <unknwn.h>
// windows.h's GetCurrentTime macro breaks the XAML headers.
#undef GetCurrentTime

#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct IDXGISwapChain1;

namespace Kyty::Uwp {

struct ShaderStatistics {
	uint64_t drawing_shaders  = 0; // vertex, pixel and the other drawing stages
	uint64_t compute_shaders  = 0;
	uint64_t pipelines        = 0;
	uint64_t cached_pipelines = 0; // of `pipelines`, made from the cache an earlier run saved
	uint64_t saved_pipelines  = 0; // in the cache file
};
// The shaders and pipelines the emulator has made so far (window.cpp); none before the game's
// renderer exists.
[[nodiscard]] std::optional<ShaderStatistics> CurrentShaderStatistics();

// Runs the emulator on its own thread inside the app. The XAML page (UI thread) provides the panel
// frames are presented to and reports its size; the emulator's window (window.cpp) presents to it
// and waits here until the app asks it to stop.
class EmulatorHost {
public:
	static EmulatorHost& Get();

	// UI thread: starts the game in `game_dir` presenting to `panel` (once). Exits the app when
	// the emulator returns.
	void Start(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel,
	           std::filesystem::path                                      game_dir);
	// UI thread, before Start: how many times the app has restarted for this game because of an address collision (see
	// RestartForAddressCollision).
	void SetRestartAttempts(uint32_t attempts) { m_restart_attempts = attempts; }
	// Emulator thread: the game asked for the fixed range [start, start + size) and host memory (the process heap, at an address chosen at random
	// when the process started) sits in part of it. A restart gives another layout, so the app restarts with the same game; it comes back only
	// when that fails or the attempts are used up.
	void RestartForAddressCollision(uint64_t start, uint64_t size);
	// UI thread, before Start: `handler` runs on the UI thread once the game presents steadily
	// (30 frames within a second), so the launch screen can give way to it.
	void OnGameVisible(std::function<void()> handler);
	// UI thread: the panel's size in view pixels and its composition scale.
	void SetPanelSize(float width, float height, float scale_x, float scale_y);
	void RequestExit();
	// UI thread: ends the game and restarts the app into the launcher, once the emulator saved its
	// pipeline cache. A game can't be stopped and another started in the same process (the
	// emulator keeps process-wide state), so every game runs in a fresh one.
	void QuitToLibrary();

	// Emulator thread.
	void AttachSwapChain(IDXGISwapChain1* swapchain);
	[[nodiscard]] std::pair<uint32_t, uint32_t> PanelPixelSize() const;
	// Waits up to `milliseconds` for an exit request; true when one came.
	[[nodiscard]] bool WaitForExit(uint32_t milliseconds);
	void               SetTitle(std::string text);
	// Emulator thread, once per presented frame.
	void FramePresented();
	// Any thread: the times between the last presented frames, in milliseconds, oldest first; at
	// most `count` of them.
	[[nodiscard]] std::vector<double> RecentFrameTimes(size_t count) const;
	// Any thread: goes up when the emulator's log (LocalState\kyty-emulator.txt) starts anew.
	[[nodiscard]] uint32_t LogGeneration() const { return m_log_generation.load(); }
	// Any thread: how long the game has been running, in milliseconds (0 before it starts).
	[[nodiscard]] uint64_t RunTimeMs() const;
	// Emulator thread, after the window loop ended and the pipeline cache is saved: restarts the app
	// if QuitToLibrary asked for it. The process ends right after.
	void Finish();

private:
	EmulatorHost() = default;

	// Runs `task` on the UI thread and waits for it.
	template <typename Task>
	void RunOnUiThread(Task&& task);

	winrt::agile_ref<winrt::Windows::UI::Xaml::Controls::SwapChainPanel> m_panel;
	winrt::Windows::UI::Core::CoreDispatcher                             m_dispatcher {nullptr};
	std::thread                                                          m_thread;
	std::atomic_bool                                                     m_started = false;
	std::filesystem::path                                                m_game_dir;
	std::atomic<uint32_t>                                                m_restart_attempts = 0;
	std::atomic_bool                                                     m_restart = false;

	mutable std::mutex      m_mutex;
	std::condition_variable m_exit_changed;
	bool                    m_exit_requested = false;
	float                   m_width          = 0.0f;
	float                   m_height         = 0.0f;
	float                   m_scale_x        = 1.0f;
	float                   m_scale_y        = 1.0f;
	std::atomic<uint64_t>   m_last_title_ms  = 0;
	std::atomic<uint32_t>   m_log_generation = 0;
	std::atomic<uint64_t>   m_start_ms       = 0;

	std::function<void()>    m_game_visible;
	std::array<uint64_t, 30> m_frame_ms {};
	size_t                   m_frames       = 0;
	bool                     m_game_shown   = false;

	mutable std::mutex        m_presents_mutex;
	std::array<int64_t, 241> m_present_counters {}; // QueryPerformanceCounter at each present
	uint64_t                  m_presents = 0;
};

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_EMULATORHOST_H_
