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
	// Debugging, only in the file: the Xbox's shader limits on any GPU (Config::XboxGpuLimitsEnabled).
	bool xbox_gpu_limits = false;
};
[[nodiscard]] AppSettings LoadSettings();
void                      SaveSettings(const AppSettings& settings);

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
