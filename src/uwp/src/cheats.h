#ifndef EMULATOR_SRC_UWP_CHEATS_H_
#define EMULATOR_SRC_UWP_CHEATS_H_

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The desktop launcher's cheats (launcher/src/cheatFile.cpp, cheatRepository.cpp) without Qt: a game's cheats are a GoldHEN-style JSON file,
// <user data>\_Patches\<title ID>.json, whose mods the emulator applies when the game starts (loader/gamePatch.cpp); such files come from the
// HEN Cheats Collection on GitHub, as on the desktop.
namespace Kyty::Uwp::Cheats {

// The file's own key order is kept when it is saved again.
using Json = nlohmann::ordered_json;

// The collection's credit line, as the desktop launcher shows it.
inline constexpr wchar_t CREDITS[] = L"Cheats by TeeKay87 / HEN Cheats Collection and its contributors.";

// A file of the collection: its name there (PPSA00000_01.000_name.json), its title in the index, the game version it is for, and its format
// (json, mc4 or shn; only JSON files can be used).
struct RemoteFile {
	std::string name;
	std::string title;
	std::string version;
	std::string format;

	[[nodiscard]] bool         IsSupported() const;
	[[nodiscard]] std::wstring Url() const; // empty when the name doesn't fit its format
};

// The collection's files for a game, the newest version first, and what went wrong reading its indexes (empty when nothing did).
struct Catalog {
	std::vector<RemoteFile> files;
	std::wstring            error;
};

// A downloaded file, or why there is none.
struct Download {
	std::string  data;
	std::wstring error;
};

// Reads the collection's three indexes (json.txt, mc4.txt, shn.txt) for `title_id`. Blocks: a background thread.
[[nodiscard]] Catalog LoadCatalog(const std::string& title_id);
// Downloads a JSON file of the collection (up to 8 MB, 30 s). Blocks: a background thread.
[[nodiscard]] Download Fetch(const RemoteFile& file);

// A cheat of a file: its name and whether it is switched on (a mod without "enabled" is).
struct Cheat {
	std::string name;
	bool        enabled = true;
};

// Whether the emulator takes cheats for this title ID: PPSA and five digits, as on the desktop.
[[nodiscard]] bool IsSupportedTitleId(const std::string& title_id);
// The folder the emulator reads cheats from: <user data>\_Patches, LocalState\_Patches when the user data folder can't be written (as
// EmulatorHost decides). Tests writing: a background thread.
[[nodiscard]] std::filesystem::path PatchFolder();

// Parses a cheat file into `document`: an error when it isn't one (a "mods" array of named mods).
[[nodiscard]] std::wstring Parse(std::string_view data, Json& document);
// Whether a parsed file fits the game (title ID, version, executable) and uses only what the emulator can apply; an error when not.
[[nodiscard]] std::wstring ValidateImport(const Json& document, const std::string& title_id, const std::string& version,
                                          const std::string& process);
[[nodiscard]] std::vector<Cheat> CheatsOf(const Json& document);
// The document with each mod's "enabled" set from `enabled` (one per mod).
[[nodiscard]] Json WithSelection(const Json& document, const std::vector<bool>& enabled);
// The document as the file holds it.
[[nodiscard]] std::string Text(const Json& document);
// Saves `document` to `path`, unless the file changed since it was read as `expected` (empty: there was none). An error, or empty.
[[nodiscard]] std::wstring Save(const std::filesystem::path& path, const Json& document, const std::string& expected);

} // namespace Kyty::Uwp::Cheats

#endif // EMULATOR_SRC_UWP_CHEATS_H_
