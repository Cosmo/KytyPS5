#include "gameLibrary.h"

#include "fileAccess.h"
#include "log.h"
#include "settings.h"

#include <windows.h>

#include <winrt/base.h>

#include <algorithm>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace Kyty::Uwp {

namespace {

std::string JsonString(const nlohmann::json& object, const char* key) {
	const auto it = object.find(key);
	return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The title in the default language, else in English, else in any language (as the desktop
// launcher shows it).
std::string TitleName(const nlohmann::json& param) {
	const auto localized = param.find("localizedParameters");
	if (localized == param.end() || !localized->is_object()) {
		return {};
	}
	for (const auto& language: {JsonString(*localized, "defaultLanguage"), std::string("en-US")}) {
		const auto entry = localized->find(language);
		if (!language.empty() && entry != localized->end() && entry->is_object()) {
			if (auto name = JsonString(*entry, "titleName"); !name.empty()) {
				return name;
			}
		}
	}
	for (const auto& entry: *localized) {
		if (entry.is_object()) {
			if (auto name = JsonString(entry, "titleName"); !name.empty()) {
				return name;
			}
		}
	}
	return {};
}

void ScanFolder(const std::filesystem::path& folder, int depth, std::vector<GameEntry>& games) {
	if (IsFile(folder / L"eboot.bin")) {
		games.push_back(ReadGame(folder));
		return;
	}
	if (depth == 0) {
		return;
	}
	for (const auto& subfolder: Subfolders(folder)) {
		ScanFolder(subfolder, depth - 1, games);
	}
}

std::filesystem::path LibraryPath() {
	return LocalStateFolder() / L"library.json";
}

void SaveLibrary(const std::vector<GameEntry>& games) {
	auto list = nlohmann::json::array();
	for (const auto& game: games) {
		list.push_back({{"title_id", game.title_id},
		                {"name", game.name},
		                {"version", game.version},
		                {"folder", winrt::to_string(game.folder.wstring())}});
	}
	const auto text = nlohmann::json {{"games", list}}.dump(2);
	if (FILE* file = _wfopen(LibraryPath().c_str(), L"wb"); file != nullptr) {
		fwrite(text.data(), 1, text.size(), file);
		fclose(file);
	}
}

} // namespace

GameEntry ReadGame(const std::filesystem::path& folder) {
	GameEntry game;
	game.folder = folder;
	game.name   = winrt::to_string(folder.filename().wstring());
	if (const auto text = ReadTextFile(folder / L"sce_sys" / L"param.json")) {
		const auto param = nlohmann::json::parse(*text, nullptr, false);
		if (!param.is_discarded() && param.is_object()) {
			game.title_id = JsonString(param, "titleId");
			game.version  = JsonString(param, "appVersion");
			if (game.version.empty()) {
				game.version = JsonString(param, "contentVersion");
			}
			if (auto name = TitleName(param); !name.empty()) {
				game.name = std::move(name);
			}
		}
	}
	return game;
}

std::vector<GameEntry> ScanGames() {
	std::vector<GameEntry> games;
	for (const auto& folder: GameFolders()) {
		const auto before = games.size();
		ScanFolder(folder, 2, games);
		Log("game folder %ls: %zu games\n", folder.c_str(), games.size() - before);
	}
	std::sort(games.begin(), games.end(),
	          [](const GameEntry& a, const GameEntry& b) { return a.name < b.name; });
	SaveLibrary(games);
	return games;
}

std::vector<GameEntry> LoadGameLibrary() {
	std::vector<GameEntry> games;
	const auto             text = ReadTextFile(LibraryPath());
	if (!text) {
		return games;
	}
	const auto library = nlohmann::json::parse(*text, nullptr, false);
	if (library.is_discarded() || !library.contains("games") || !library["games"].is_array()) {
		return games;
	}
	for (const auto& entry: library["games"]) {
		if (!entry.is_object()) {
			continue;
		}
		GameEntry game;
		game.title_id = JsonString(entry, "title_id");
		game.name     = JsonString(entry, "name");
		game.version  = JsonString(entry, "version");
		game.folder   = std::filesystem::path(winrt::to_hstring(JsonString(entry, "folder")).c_str());
		games.push_back(std::move(game));
	}
	return games;
}

std::optional<GameEntry> FindGame(const std::string& title_id) {
	const auto find = [&](const std::vector<GameEntry>& games) -> std::optional<GameEntry> {
		const auto it = std::find_if(games.begin(), games.end(), [&](const GameEntry& game) {
			return _stricmp(game.title_id.c_str(), title_id.c_str()) == 0;
		});
		return it != games.end() ? std::optional(*it) : std::nullopt;
	};
	if (auto game = find(LoadGameLibrary())) {
		return game;
	}
	return find(ScanGames());
}

} // namespace Kyty::Uwp
