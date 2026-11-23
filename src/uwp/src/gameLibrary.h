#ifndef EMULATOR_SRC_UWP_GAMELIBRARY_H_
#define EMULATOR_SRC_UWP_GAMELIBRARY_H_

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Kyty::Uwp {

struct GameEntry {
	std::string           title_id; // e.g. PPSA00000; empty when param.json has none
	std::string           name;
	std::string           version;
	std::filesystem::path folder; // the game's folder, or its .zar archive
	uint32_t              attribute3 = 0; // param.json's feature flags (PS5 Pro, PS VR2, ...: GameFeature)
};

// Bits of GameEntry::attribute3 the library shows as badges (param.json; the meanings as psdevwiki and comparisons of games' files give
// them).
namespace GameFeature {
constexpr uint32_t Ps5Pro      = 1u << 22u;
constexpr uint32_t PsVr2       = 1u << 10u;
constexpr uint32_t PsVr2Needed = 1u << 11u;
} // namespace GameFeature

// The game in `folder` (a folder with an eboot.bin, or a .zar archive), described by its sce_sys\param.json.
[[nodiscard]] GameEntry ReadGame(const std::filesystem::path& folder);

// Finds the games in the game folders (GameFolders): folders with an eboot.bin, the game folder
// itself or up to two levels below it, and .zar archives in those folders, described by sce_sys\param.json. Saves the result to
// LocalState\library.json.
std::vector<GameEntry> ScanGames();

// How many games a scan finds with `folder` as a game folder (their param.json is not read).
[[nodiscard]] size_t CountGames(const std::filesystem::path& folder);

// The games of the last scan.
[[nodiscard]] std::vector<GameEntry> LoadGameLibrary();

// The game with `title_id`, from the last scan or, if it isn't there, a new one.
[[nodiscard]] std::optional<GameEntry> FindGame(const std::string& title_id);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_GAMELIBRARY_H_
