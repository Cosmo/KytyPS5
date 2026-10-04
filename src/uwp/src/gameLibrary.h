#ifndef EMULATOR_SRC_UWP_GAMELIBRARY_H_
#define EMULATOR_SRC_UWP_GAMELIBRARY_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Kyty::Uwp {

struct GameEntry {
	std::string           title_id; // e.g. PPSA00000; empty when param.json has none
	std::string           name;
	std::string           version;
	std::filesystem::path folder;
};

// The game in `folder` (which has an eboot.bin), described by its sce_sys\param.json.
[[nodiscard]] GameEntry ReadGame(const std::filesystem::path& folder);

// Finds the games in the game folders (GameFolders): folders with an eboot.bin, the game folder
// itself or up to two levels below it, described by sce_sys\param.json. Saves the result to
// LocalState\library.json.
std::vector<GameEntry> ScanGames();

// The games of the last scan.
[[nodiscard]] std::vector<GameEntry> LoadGameLibrary();

// The game with `title_id`, from the last scan or, if it isn't there, a new one.
[[nodiscard]] std::optional<GameEntry> FindGame(const std::string& title_id);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_GAMELIBRARY_H_
