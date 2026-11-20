#ifndef EMULATOR_SRC_UWP_CHEATS_H_
#define EMULATOR_SRC_UWP_CHEATS_H_

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The desktop launcher's cheats (launcher/src/cheatFile.cpp, cheatRepository.cpp) without Qt: a game's cheats are a GoldHEN-style JSON file,
// <user data>\_Patches\<title ID>.json, whose mods the emulator applies when the game starts (loader/gamePatch.cpp); such files come from
// KytyPS5-Modules (patches and cheats) and the HEN Cheats Collection on GitHub, as on the desktop.
namespace Kyty::Uwp::Cheats {

// The file's own key order is kept when it is saved again.
using Json = nlohmann::ordered_json;

// The sources' credit line, as the desktop launcher shows it.
inline constexpr wchar_t CREDITS[] = L"Patches and cheats by KytyPS5-Modules and TeeKay87 / HEN Cheats Collection and their contributors.";

// A file of a source: its name (KytyPS5-Modules: its title; the HEN Cheats Collection: PPSA00000_01.000_name.json), the game's name in the
// index, the game version it is for, its format (json, mc4 or shn; only JSON files can be used), whether it is a patch or a cheat (as its
// source lists it; the files are alike), and where it is downloaded from (empty when its entry doesn't fit).
struct RemoteFile {
	enum class Kind { Patch, Cheat };

	std::string  name;
	std::string  title;
	std::string  version;
	std::string  format;
	Kind         kind = Kind::Cheat;
	std::wstring url;

	[[nodiscard]] bool IsSupported() const;
};

// The sources' files for a game, the newest version first, and what went wrong reading their indexes (empty when nothing did).
struct Catalog {
	std::vector<RemoteFile> files;
	std::wstring            error;
};

// A downloaded file, or why there is none.
struct Download {
	std::string  data;
	std::wstring error;
};

// Reads the indexes for `title_id`: KytyPS5-Modules' catalog (index.min.json) and the collection's three (json.txt, mc4.txt, shn.txt). Blocks:
// a background thread.
[[nodiscard]] Catalog LoadCatalog(const std::string& title_id);
// Downloads a JSON file of a source (up to 8 MB, 30 s). Blocks: a background thread.
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
// Whether a parsed file fits the game (title ID, version, executable where it names one) and uses only what the emulator can apply; an error
// when not.
[[nodiscard]] std::wstring ValidateImport(const Json& document, const std::string& title_id, const std::string& version,
                                          const std::string& process);
[[nodiscard]] std::vector<Cheat> CheatsOf(const Json& document);
// The document with each mod's "enabled" set from `enabled` (one per mod).
[[nodiscard]] Json WithSelection(const Json& document, const std::vector<bool>& enabled);
// The imported document, its mods switched off, with the game's executable as its process; the local file's other mods (other names) come
// first when it is for the same game (passes ValidateImport for the imported file's ID and version). With a null `local`: the imported one only.
[[nodiscard]] Json Merge(const Json& local, const Json& imported, const std::string& process);
// The document as the file holds it.
[[nodiscard]] std::string Text(const Json& document);
// Saves `document` to `path`, unless the file changed since it was read as `expected` (empty: there was none). An error, or empty.
[[nodiscard]] std::wstring Save(const std::filesystem::path& path, const Json& document, const std::string& expected);

} // namespace Kyty::Uwp::Cheats

#endif // EMULATOR_SRC_UWP_CHEATS_H_
