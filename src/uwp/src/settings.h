#ifndef EMULATOR_SRC_UWP_SETTINGS_H_
#define EMULATOR_SRC_UWP_SETTINGS_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Kyty::Uwp {

// The app's own data folder (LocalState); it is always readable and writable, on any drive.
[[nodiscard]] std::filesystem::path LocalStateFolder();

// The user's settings, in LocalState\kyty-uwp.json (the settings page and `uwp.ps1 folders` edit
// it). They apply when a game starts.
struct AppSettings {
	// Searched for games besides LocalState\Games, which needs no setup.
	std::vector<std::filesystem::path> game_folders;
	// The settings page, User Data Path: where saves (_SaveData), patches (_Patches) and trophies (_Trophies) are kept, for one on a USB
	// drive. Empty: LocalState.
	std::filesystem::path user_data_path;
	uint32_t                           screen_width  = 1280; // the games' output resolution
	uint32_t                           screen_height = 720;
	bool                               vsync         = false;
	std::string                        user_name     = "Kyty";
	uint32_t                           console_language = 1; // English (United States)
	bool                               debug_layer      = false; // the D3D12 debug layer
	bool                               game_output      = false; // the game's printf in the log
	bool                               d3d12_selftest   = false; // translate and sign built-in shaders at start (not on the settings page)
	// The overlay's size: 0 small, 1 medium, 2 large (OverlaySizes in overlay.cpp).
	uint32_t overlay_size = 1;
	// How many of the log's last lines the overlay shows: 2 to 20 (OverlayLogLines in overlay.cpp).
	uint32_t overlay_log_lines = 20;
	// The settings page, Controller (ControllerOptions in input.h): the games' adaptive trigger effects rumble the triggers' motors. On.
	bool impulse_triggers = true;
	// The settings page, Controller: the DualSense's haptic feedback as gentle rumble. On.
	bool haptic_rumble = true;
	// The settings page, Controller: the DualSense's speaker on a headset plugged into the controller. On.
	bool headset_speaker = true;
	// The settings page, Controller: the microphone of that headset as the DualSense's microphone. Off (Windows asks for the microphone when it
	// is turned on).
	bool headset_mic = false;
	// Debugging, only in the file: the Xbox's shader limits on any GPU (Config::XboxGpuLimitsEnabled).
	bool xbox_gpu_limits = false;
	// Debugging, only in the file: which limits of the DXIL compiler xbox_gpu_limits applies (bit 0 shader model 6.4, bit 1 16-bit ops lowered; default 3).
	uint32_t xbox_limits_dxil_parts = 3;
	// Debugging, only in the file: save the shaders of the pipeline with this number when it is created (0: none).
	uint32_t dump_pipeline = 0;
	// Debugging, only in the file: a folder (in LocalState, or a full path) with DXIL files that replace shaders: <size>.ps.dxil and <size>.vs.dxil for the
	// pipelines whose pixel program's SPIR-V has that many bytes (Config::ShaderOverrideFolder). Empty: off.
	std::string shader_override;
	// Debugging, only in the file: end the emulator with the hang report once the GPU device is removed or its oldest work hangs for this many seconds
	// (Config::GpuHangExitSeconds). 0: off.
	uint32_t gpu_hang_exit_seconds = 0;
	// Debugging, only in the file: merged geometry programs run as compute shaders even where the device has mesh shaders (Config::MeshComputeForced).
	bool mesh_compute = false;
	// Only in the file: guest memory above this many MB is kept in a file, the coldest first (Config::GuestRamBudgetMb). 0: off. Not set (0xFFFFFFFF):
	// on where the app's memory limit is small (the Series X), with the limit as the cap; off on a PC.
	uint32_t guest_ram_budget_mb = 0xFFFFFFFFu;
	// Debugging the pager, only in the file: 0 normal, 1 sweep only (nothing leaves memory), 2 take a window out and page it straight back in (Config::GuestRamPagerMode).
	uint32_t guest_ram_pager_mode = 0;
	// Debugging the pager, only in the file: a commit of guest memory fails above this many MB of guest memory (Config::GuestCommitLimitMb). 0: none.
	uint32_t guest_commit_limit_mb = 0;
	// Low quality, only in the file: sampled block-compressed textures from texture_mip_skip_min_size texels up lose their texture_mip_skip finest mip levels on the GPU
	// (Config::TextureMipSkip). 0: off.
	uint32_t texture_mip_skip          = 1;
	uint32_t texture_mip_skip_min_size = 2048;
	// Debugging, only in the file: ask for a frame dump this many seconds after the game started (0: never; the game menu has Save frame dump).
	uint32_t frame_dump_after_s = 0;
	// Debugging, only in the file: 1 adds the average color of the large floating-point render targets after each draw to the frame dump's draw log (slow).
	uint32_t frame_dump_snapshots = 0;
	// The settings page, for testing: the GPU memory savings (Config::GpuMemorySavings) and the video decoder's threads (Config::VideoDecodeThreads). On.
	bool gpu_memory_savings   = true;
	bool video_decode_threads = true;
	// The settings page, Developer: where the GPU memory is in the log every 10 seconds (Config::GpuMemoryLog). Off.
	bool gpu_memory_log = false;
	// The settings page, Developer: DRED and the GPU trace, so a GPU hang or crash names the draw (Config::GpuCrashDiagnostics). Off.
	bool gpu_crash_diagnostics = false;
	// The settings page, Developer: the games' compiled shaders are kept across builds of the app (Config::PipelineCacheAnyBuild). Off.
	bool shader_cache_any_build = false;
	// Debugging the pager, only in the file: the limit of the app's memory in MB for the pager instead of the OS's (a PC has none to speak of). 0: the OS's.
	uint32_t app_memory_limit_mb = 0;
	// Testing, only in the file: press Cross (A on an Xbox controller) every this many milliseconds, so that nobody has to click through the menus
	// (Pad::StartAutoPress). 0: off.
	uint32_t auto_press_ms = 0;
};
[[nodiscard]] AppSettings LoadSettings();
void                      SaveSettings(const AppSettings& settings);

// The folder of the user's data: AppSettings::user_data_path, or LocalState when none is set.
[[nodiscard]] std::filesystem::path UserDataFolder(const AppSettings& settings);

// The folders games are searched in: LocalState\Games, then AppSettings::game_folders.
[[nodiscard]] std::vector<std::filesystem::path> GameFolders();

// The app's own state, kept in LocalStatepp-state.json (kyty-uwp.json is the user's):
// the title ID of the game started last, for the launcher to start on (empty when there is none),
// and whether the overlay shows over games.
[[nodiscard]] std::string LastPlayed();
void                      SetLastPlayed(const std::string& title_id);
[[nodiscard]] bool        OverlayShown();
void                      SetOverlayShown(bool shown);

// Games hidden from the launcher (by folder) until the next scan.
[[nodiscard]] std::vector<std::filesystem::path> HiddenGames();
void                                             HideGame(const std::filesystem::path& folder);
void                                             ShowAllGames();

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_SETTINGS_H_
