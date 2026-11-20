#include "cheats.h"

#include "fileAccess.h"
#include "log.h"
#include "settings.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <utility>

namespace Kyty::Uwp::Cheats {

namespace {

namespace http = winrt::Windows::Web::Http;

constexpr uint64_t INDEX_LIMIT = 2u << 20u;
constexpr uint64_t FILE_LIMIT  = 8u << 20u;

constexpr wchar_t BASE_URL[]    = L"https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/";
constexpr wchar_t MODULES_URL[] = L"https://kytyps5.github.io/KytyPS5-Modules/";

// A file name of the collection: the title ID, the game version, a name and the format.
bool MatchName(const std::string& name, std::smatch& match) {
	static const std::regex pattern(R"(^([A-Z]{4}[0-9]{5})_([0-9]+(?:\.[0-9]+)+)_[A-Za-z0-9_.-]+\.(json|mc4|shn)$)");
	return std::regex_match(name, match, pattern);
}

std::string Trim(std::string_view text) {
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos) {
		return {};
	}
	return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

std::string Upper(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return text;
}

bool EqualsIgnoringCase(const std::string& a, const std::string& b) {
	return Upper(a) == Upper(b);
}

std::wstring Wide(const std::string& text) {
	return std::wstring(winrt::to_hstring(text));
}

// A member as a string; empty when it is missing or something else (as QJsonValue::toString).
std::string StringOf(const Json& object, const char* key) {
	const auto it = object.find(key);
	return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// One request with the desktop launcher's limits: `limit` bytes and 30 s in all (whatever step it waits for then is cancelled).
Download Request(const std::wstring& url, uint64_t limit) {
	struct Timeout {
		std::mutex                               mutex;
		winrt::Windows::Foundation::IAsyncInfo   waiting {nullptr};
		bool                                     expired = false;

		void Wait(winrt::Windows::Foundation::IAsyncInfo const& step) {
			std::scoped_lock lock(mutex);
			waiting = step;
			if (expired) {
				waiting.Cancel();
			}
		}
	};
	auto     timeout = std::make_shared<Timeout>();
	auto     timer   = winrt::Windows::System::Threading::ThreadPoolTimer::CreateTimer(
        [timeout](auto const&) {
            std::scoped_lock lock(timeout->mutex);
            timeout->expired = true;
            if (timeout->waiting != nullptr) {
                timeout->waiting.Cancel();
            }
        },
        std::chrono::seconds(30));
	Download result;
	try {
		// The newest file, not a cached one (the collection changes).
		http::Filters::HttpBaseProtocolFilter filter;
		filter.CacheControl().ReadBehavior(http::Filters::HttpCacheReadBehavior::MostRecent);
		http::HttpClient client(filter);
		client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"Kyty-Launcher");
		auto get = client.GetAsync(winrt::Windows::Foundation::Uri(url), http::HttpCompletionOption::ResponseHeadersRead);
		timeout->Wait(get);
		const auto response = get.get();
		const auto length   = response.Content().Headers().ContentLength();
		if (response.StatusCode() != http::HttpStatusCode::Ok) {
			result.error = L"Unexpected response from the cheat collection (HTTP " + std::to_wstring(static_cast<int>(response.StatusCode())) + L").";
		} else if (length != nullptr && length.Value() > limit) {
			result.error = L"The download is bigger than allowed.";
		} else {
			auto read = response.Content().ReadAsBufferAsync();
			timeout->Wait(read);
			const auto buffer = read.get();
			if (buffer.Length() > limit) {
				result.error = L"The download is bigger than allowed.";
			} else {
				result.data.assign(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
			}
		}
	} catch (const winrt::hresult_error& error) {
		std::scoped_lock lock(timeout->mutex);
		result.error = timeout->expired ? L"The download timed out." : L"Could not download: " + std::wstring(error.message());
	}
	timer.Cancel();
	if (!result.error.empty()) {
		Log("cheats: %ls: %ls\n", url.c_str(), result.error.c_str());
	}
	return result;
}

// The files of one index (lines of name=title) for `title_id`; `invalid` counts the lines that don't fit.
std::vector<RemoteFile> ParseIndex(std::string_view data, const std::string& format, const std::string& title_id, size_t& invalid) {
	std::vector<RemoteFile> files;
	std::set<std::string>   seen;
	if (data.starts_with("\xEF\xBB\xBF")) {
		data.remove_prefix(3);
	}
	while (!data.empty()) {
		const auto end  = data.find('\n');
		const auto line = Trim(data.substr(0, end));
		data.remove_prefix(end == std::string_view::npos ? data.size() : end + 1);
		if (line.empty()) {
			continue;
		}
		const auto  separator = line.find('=');
		const auto  name      = line.substr(0, separator);
		std::smatch match;
		if (separator == std::string::npos || !MatchName(name, match) || match[3].str() != format) {
			invalid++;
			continue;
		}
		if (match[1].str() != title_id || !seen.insert(name).second) {
			continue;
		}
		files.push_back({name, Trim(std::string_view(line).substr(separator + 1)), match[2].str(), format, RemoteFile::Kind::Cheat,
		                 std::wstring(BASE_URL) + Wide(format) + L"/" + Wide(name)});
	}
	return files;
}

// A path of KytyPS5-Modules' catalog as a URL within its site; empty when it leads elsewhere.
std::wstring ModuleUrl(const std::string& path) {
	try {
		const winrt::Windows::Foundation::Uri base(MODULES_URL);
		std::wstring                          url(base.CombineUri(winrt::to_hstring(path)).AbsoluteUri());
		if (url.size() > std::size(MODULES_URL) - 1 && url.starts_with(MODULES_URL)) {
			return url;
		}
	} catch (const winrt::hresult_error&) {
	}
	return {};
}

// The patches and cheats of KytyPS5-Modules' catalog for `title_id`; `invalid` counts the entries whose path leads elsewhere. An error when the
// catalog isn't one.
std::wstring ParseModules(std::string_view data, const std::string& title_id, std::vector<RemoteFile>& files, size_t& invalid) {
	const auto root = Json::parse(data, nullptr, false);
	if (!root.is_object() || root.empty()) {
		return L"Invalid module catalog.";
	}
	for (const auto& [category, kind]: {std::pair {"patches", RemoteFile::Kind::Patch}, std::pair {"cheats", RemoteFile::Kind::Cheat}}) {
		const auto group = root.find(category);
		if (group == root.end() || !group->is_object()) {
			continue;
		}
		const auto entries = group->find(title_id);
		if (entries == group->end() || !entries->is_array()) {
			continue;
		}
		for (const auto& entry: *entries) {
			if (!entry.is_object()) {
				continue;
			}
			auto url = ModuleUrl(StringOf(entry, "path"));
			if (url.empty()) {
				invalid++;
				continue;
			}
			files.push_back({StringOf(entry, "title"), StringOf(entry, "gameName"), StringOf(entry, "version"), "json", kind, std::move(url)});
		}
	}
	return {};
}

// A cheat file in itself: a "mods" array of named mods, whose "enabled" is a bool if it is there.
std::wstring ValidateDocument(const Json& document) {
	if (!document.is_object()) {
		return L"This isn't a cheat file.";
	}
	const auto mods = document.find("mods");
	if (mods == document.end() || !mods->is_array()) {
		return L"The cheat file has no \"mods\" array.";
	}
	for (const auto& mod: *mods) {
		if (!mod.is_object() || StringOf(mod, "name").empty() || (mod.contains("enabled") && !mod["enabled"].is_boolean())) {
			return L"The cheat file has an entry that isn't a cheat.";
		}
	}
	return {};
}

} // namespace

bool RemoteFile::IsSupported() const {
	return format == "json" && !url.empty();
}

Catalog LoadCatalog(const std::string& title_id) {
	Catalog    catalog;
	const auto id = Upper(Trim(title_id));
	{
		const auto   index = Request(std::wstring(MODULES_URL) + L"index.min.json", INDEX_LIMIT);
		std::wstring error = index.error;
		if (error.empty()) {
			size_t invalid = 0;
			error          = ParseModules(index.data, id, catalog.files, invalid);
			if (error.empty() && invalid != 0) {
				error = L"Skipped " + std::to_wstring(invalid) + L" invalid module catalog entries.";
			}
		}
		if (!error.empty()) {
			catalog.error = L"Modules: " + error;
		}
	}
	for (const std::string format: {"json", "mc4", "shn"}) {
		const auto   index = Request(std::wstring(BASE_URL) + Wide(format) + L".txt", INDEX_LIMIT);
		std::wstring error = index.error;
		if (error.empty()) {
			size_t invalid = 0;
			auto   files   = ParseIndex(index.data, format, id, invalid);
			catalog.files.insert(catalog.files.end(), files.begin(), files.end());
			if (index.data.empty()) {
				error = L"The index is empty.";
			} else if (invalid != 0) {
				error = L"Skipped " + std::to_wstring(invalid) + L" entries of the index that don't fit.";
			}
		}
		if (!error.empty()) {
			catalog.error += (catalog.error.empty() ? L"" : L"\n") + Wide(Upper(format)) + L": " + error;
		}
	}
	std::sort(catalog.files.begin(), catalog.files.end(), [](const RemoteFile& a, const RemoteFile& b) {
		if (a.version != b.version) {
			return a.version > b.version;
		}
		if (a.format != b.format) {
			return a.format < b.format;
		}
		return a.name < b.name;
	});
	Log("cheats: %zu files in the sources for %s\n", catalog.files.size(), id.c_str());
	return catalog;
}

Download Fetch(const RemoteFile& file) {
	if (!file.IsSupported()) {
		return {{}, L"Only JSON cheat files can be used."};
	}
	return Request(file.url, FILE_LIMIT);
}

bool IsSupportedTitleId(const std::string& title_id) {
	static const std::regex pattern(R"(^PPSA[0-9]{5}$)");
	return std::regex_match(Upper(Trim(title_id)), pattern);
}

std::filesystem::path PatchFolder() {
	const auto settings  = LoadSettings();
	auto       user_data = UserDataFolder(settings);
	if (!settings.user_data_path.empty() && WriteError(user_data) != 0) {
		user_data = LocalStateFolder();
	}
	return user_data / L"_Patches";
}

std::wstring Parse(std::string_view data, Json& document) {
	auto parsed = Json::parse(data, nullptr, false);
	if (parsed.is_discarded()) {
		document = Json();
		return L"This isn't a cheat file.";
	}
	auto error = ValidateDocument(parsed);
	document   = error.empty() ? std::move(parsed) : Json();
	return error;
}

std::wstring ValidateImport(const Json& document, const std::string& title_id, const std::string& version, const std::string& process) {
	if (auto error = ValidateDocument(document); !error.empty()) {
		return error;
	}
	if (!IsSupportedTitleId(title_id) || !EqualsIgnoringCase(StringOf(document, "id"), Trim(title_id))) {
		return L"The cheat file is for another game.";
	}
	if (version.empty() || StringOf(document, "version") != version) {
		return L"The cheat file is for another version of the game (this one is " + (version.empty() ? L"unknown" : Wide(version)) + L").";
	}
	// A file without a process is for the game's executable (KytyPS5-Modules' cheats name none).
	const auto file_process = document.find("process");
	if (process.empty() ||
	    (file_process != document.end() && !(file_process->is_string() && EqualsIgnoringCase(file_process->get<std::string>(), process)))) {
		return L"The cheat file is for another program of the game than " + Wide(process) + L".";
	}
	static const std::regex offset_pattern(R"(^(?:0[xX])?[0-9a-fA-F]{1,16}$)");
	static const std::regex bytes_pattern(R"(^(?:[0-9a-fA-F]{2})+$)");
	const auto&             mods = document["mods"];
	if (mods.empty()) {
		return L"The cheat file has no cheats.";
	}
	if (document.contains("master")) {
		return L"The cheat file uses master codes, which the emulator can't apply yet.";
	}
	for (const auto& mod: mods) {
		const auto memory = mod.find("memory");
		if (mod.contains("module_name")) {
			return L"The cheat file has cheats for single modules, which the emulator can't apply yet.";
		}
		const auto unsupported = L"The emulator can't apply what the cheat \"" + Wide(StringOf(mod, "name")) + L"\" writes.";
		if (memory == mod.end() || !memory->is_array() || memory->empty()) {
			return unsupported;
		}
		for (const auto& write: *memory) {
			if (!write.is_object()) {
				return unsupported;
			}
			const auto offset   = StringOf(write, "offset");
			const auto off      = StringOf(write, "off");
			const auto on       = StringOf(write, "on");
			const auto absolute = write.find("absolute");
			const auto expected = write.find("expected");
			if ((absolute != write.end() && (!absolute->is_boolean() || absolute->get<bool>())) ||
			    (expected != write.end() && (!expected->is_string() || !EqualsIgnoringCase(expected->get<std::string>(), off))) ||
			    write.contains("section") || write.contains("sectionName") || write.contains("sectionProtection") ||
			    write.contains("logicalOffset") || write.contains("sectionOffset")) {
				return L"The cheat file finds what it changes in ways the emulator can't apply yet.";
			}
			if (!std::regex_match(offset, offset_pattern) || !std::regex_match(off, bytes_pattern) ||
			    !std::regex_match(on, bytes_pattern) || off.size() != on.size()) {
				return unsupported;
			}
		}
	}
	return {};
}

std::vector<Cheat> CheatsOf(const Json& document) {
	std::vector<Cheat> cheats;
	if (const auto mods = document.find("mods"); mods != document.end() && mods->is_array()) {
		for (const auto& mod: *mods) {
			const auto enabled = mod.find("enabled");
			cheats.push_back({StringOf(mod, "name"), enabled == mod.end() || !enabled->is_boolean() || enabled->get<bool>()});
		}
	}
	return cheats;
}

Json WithSelection(const Json& document, const std::vector<bool>& enabled) {
	auto result = document;
	auto mods   = result.find("mods");
	if (mods == result.end() || !mods->is_array() || mods->size() != enabled.size()) {
		return {};
	}
	for (size_t index = 0; index < enabled.size(); index++) {
		(*mods)[index]["enabled"] = static_cast<bool>(enabled[index]);
	}
	return result;
}

Json Merge(const Json& local, const Json& imported, const std::string& process) {
	Json                  root = imported.is_object() ? imported : Json::object();
	Json                  mods = Json::array();
	std::set<std::string> names;
	if (const auto list = imported.find("mods"); list != imported.end() && list->is_array()) {
		for (auto mod: *list) {
			names.insert(StringOf(mod, "name"));
			if (mod.is_object()) {
				mod["enabled"] = false;
			}
			mods.push_back(std::move(mod));
		}
	}
	if (ValidateImport(local, StringOf(imported, "id"), StringOf(imported, "version"), process).empty()) {
		Json kept = Json::array();
		for (const auto& mod: local["mods"]) {
			if (!names.contains(StringOf(mod, "name"))) {
				kept.push_back(mod);
			}
		}
		for (auto& mod: mods) {
			kept.push_back(std::move(mod));
		}
		mods = std::move(kept);
	}
	root["mods"]    = std::move(mods);
	root["process"] = process;
	return root;
}

std::string Text(const Json& document) {
	return document.dump(4, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::wstring Save(const std::filesystem::path& path, const Json& document, const std::string& expected) {
	if (path.empty() || !document.is_object()) {
		return L"This isn't a cheat file.";
	}
	// Changed since it was read (another copy of the page, or a file copied over it): saving now would lose that.
	if (IsFile(path)) {
		const auto current = ReadTextFile(path);
		if (!current) {
			return L"The cheat file can't be read.";
		}
		if (*current != expected) {
			return L"The cheat file changed meanwhile. Press Y to read it again.";
		}
	} else if (!expected.empty()) {
		return L"The cheat file changed meanwhile. Press Y to read it again.";
	}
	if (const auto error = WriteFileReplacing(path, Text(document)); error != 0) {
		Log("cheats: cannot save %ls (error %lu)\n", path.c_str(), error);
		return L"The cheat file can't be saved (error " + std::to_wstring(error) + L").";
	}
	Log("cheats: saved %ls\n", path.c_str());
	return {};
}

} // namespace Kyty::Uwp::Cheats
