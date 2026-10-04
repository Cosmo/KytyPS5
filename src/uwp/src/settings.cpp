#include "settings.h"

#include "fileAccess.h"
#include "log.h"

#include <winrt/Windows.Storage.h>

#include <fstream>
#include <type_traits>
#include <nlohmann/json.hpp>
#include <system_error>

namespace Kyty::Uwp {

std::filesystem::path LocalStateFolder() {
	return std::filesystem::path(
	    std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
}

namespace {

std::filesystem::path SettingsPath() {
	return LocalStateFolder() / L"kyty-uwp.json";
}

} // namespace

AppSettings LoadSettings() {
	AppSettings settings;
	const auto  text = ReadTextFile(SettingsPath());
	if (!text) {
		return settings;
	}
	const auto json = nlohmann::json::parse(*text, nullptr, false);
	if (json.is_discarded() || !json.is_object()) {
		Log("ignoring %ls: not a JSON object\n", SettingsPath().c_str());
		return settings;
	}
	if (const auto folders = json.find("game_folders"); folders != json.end() && folders->is_array()) {
		for (const auto& folder: *folders) {
			if (folder.is_string()) {
				settings.game_folders.emplace_back(winrt::to_hstring(folder.get<std::string>()).c_str());
			}
		}
	}
	const auto read = [&json](const char* key, auto& value) {
		const auto it = json.find(key);
		if (it != json.end() && !it->is_null()) {
			try {
				value = it->get<std::remove_reference_t<decltype(value)>>();
			} catch (const nlohmann::json::exception&) {
				Log("ignoring setting %s: wrong type\n", key);
			}
		}
	};
	read("screen_width", settings.screen_width);
	read("screen_height", settings.screen_height);
	read("vsync", settings.vsync);
	read("user_name", settings.user_name);
	read("console_language", settings.console_language);
	read("debug_layer", settings.debug_layer);
	read("game_output", settings.game_output);
	read("d3d12_selftest", settings.d3d12_selftest);
	read("overlay_size", settings.overlay_size);
	read("xbox_gpu_limits", settings.xbox_gpu_limits);
	return settings;
}

void SaveSettings(const AppSettings& settings) {
	// Keys this version doesn't know stay as they are.
	const auto text = ReadTextFile(SettingsPath());
	auto       json = nlohmann::json::parse(text.value_or("{}"), nullptr, false);
	if (!json.is_object()) {
		json = nlohmann::json::object();
	}
	auto folders = nlohmann::json::array();
	for (const auto& folder: settings.game_folders) {
		folders.push_back(winrt::to_string(folder.wstring()));
	}
	json["game_folders"]     = folders;
	json["screen_width"]     = settings.screen_width;
	json["screen_height"]    = settings.screen_height;
	json["vsync"]            = settings.vsync;
	json["user_name"]        = settings.user_name;
	json["console_language"] = settings.console_language;
	json["debug_layer"]      = settings.debug_layer;
	json["game_output"]      = settings.game_output;
	json["d3d12_selftest"]   = settings.d3d12_selftest;
	json["overlay_size"]     = settings.overlay_size;
	std::ofstream(SettingsPath(), std::ios::binary | std::ios::trunc) << json.dump(2);
}

std::vector<std::filesystem::path> GameFolders() {
	std::vector<std::filesystem::path> folders;

	const auto      own = LocalStateFolder() / L"Games";
	std::error_code error;
	std::filesystem::create_directories(own, error);
	folders.push_back(own);

	for (auto& folder: LoadSettings().game_folders) {
		folders.push_back(std::move(folder));
	}
	return folders;
}

namespace {

nlohmann::json ReadAppState() {
	const auto text  = ReadTextFile(LocalStateFolder() / L"app-state.json");
	auto       state = nlohmann::json::parse(text.value_or("{}"), nullptr, false);
	return state.is_object() ? state : nlohmann::json::object();
}

void WriteAppState(const nlohmann::json& state) {
	std::ofstream(LocalStateFolder() / L"app-state.json", std::ios::binary | std::ios::trunc)
	    << state.dump(2);
}

} // namespace

std::string LastPlayed() {
	const auto state = ReadAppState();
	const auto value = state.find("last_played");
	return value != state.end() && value->is_string() ? value->get<std::string>() : std::string();
}

void SetLastPlayed(const std::string& title_id) {
	auto state           = ReadAppState();
	state["last_played"] = title_id;
	WriteAppState(state);
}

bool OverlayShown() {
	const auto state = ReadAppState();
	const auto value = state.find("overlay");
	return value != state.end() && value->is_boolean() && value->get<bool>();
}

void SetOverlayShown(bool shown) {
	auto state       = ReadAppState();
	state["overlay"] = shown;
	WriteAppState(state);
}

std::vector<std::filesystem::path> HiddenGames() {
	std::vector<std::filesystem::path> folders;
	const auto                         state  = ReadAppState();
	const auto                         hidden = state.find("hidden_games");
	if (hidden != state.end() && hidden->is_array()) {
		for (const auto& folder: *hidden) {
			if (folder.is_string()) {
				folders.emplace_back(winrt::to_hstring(folder.get<std::string>()).c_str());
			}
		}
	}
	return folders;
}

void HideGame(const std::filesystem::path& folder) {
	auto state = ReadAppState();
	if (!state.contains("hidden_games") || !state["hidden_games"].is_array()) {
		state["hidden_games"] = nlohmann::json::array();
	}
	state["hidden_games"].push_back(winrt::to_string(folder.wstring()));
	WriteAppState(state);
}

void ShowAllGames() {
	auto state = ReadAppState();
	state.erase("hidden_games");
	WriteAppState(state);
}

} // namespace Kyty::Uwp
