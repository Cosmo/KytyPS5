#include "fileAccess.h"

#include "log.h"
#include "settings.h"

#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/Windows.System.Profile.h>

namespace Kyty::Uwp {

bool IsFile(const std::filesystem::path& path) {
	WIN32_FILE_ATTRIBUTE_DATA attributes {};
	return GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes) != 0 &&
	       (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

namespace {

// The whole file into `Container` (bytes or chars), if it's smaller than `max_size`.
template <typename Container>
std::optional<Container> ReadWholeFile(const std::filesystem::path& path, int64_t max_size) {
	HANDLE file = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
	                                  nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return std::nullopt;
	}
	Container     data;
	LARGE_INTEGER size {};
	if (GetFileSizeEx(file, &size) && size.QuadPart < max_size) {
		data.resize(static_cast<size_t>(size.QuadPart));
		DWORD read = 0;
		if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) {
			read = 0;
		}
		data.resize(read);
	}
	CloseHandle(file);
	return data;
}

} // namespace

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
	return ReadWholeFile<std::string>(path, int64_t {16} << 20);
}

std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path) {
	return ReadWholeFile<std::vector<uint8_t>>(path, int64_t {32} << 20);
}

namespace {

// ListError without the log line.
uint32_t AccessError(const std::filesystem::path& folder) {
	WIN32_FIND_DATAW data {};
	HANDLE find = FindFirstFileExFromAppW((folder / L"*").c_str(), FindExInfoBasic, &data,
	                                      FindExSearchNameMatch, nullptr, 0);
	if (find == INVALID_HANDLE_VALUE) {
		return GetLastError();
	}
	FindClose(find);
	return 0;
}

} // namespace

uint32_t ListError(const std::filesystem::path& folder) {
	const auto error = AccessError(folder);
	if (error != 0) {
		Log("cannot list %ls (error %lu)\n", folder.c_str(), error);
	}
	return error;
}

void WaitForGameFolders() {
	const auto start = GetTickCount64();
	for (const auto& folder: GameFolders()) {
		auto error = AccessError(folder);
		if (error == ERROR_ACCESS_DENIED && GetTickCount64() - start < 2000) {
			const auto waiting = GetTickCount64();
			Log("waiting for access to %ls (the Xbox lets a starting app onto its drives a moment late)\n", folder.c_str());
			while (error == ERROR_ACCESS_DENIED && GetTickCount64() - start < 2000) {
				Sleep(100);
				error = AccessError(folder);
			}
			if (error == 0) {
				Log("access to %ls after %llu ms\n", folder.c_str(), GetTickCount64() - waiting);
			}
		}
		if (error != 0) {
			Log("cannot open game folder %ls (error %lu): its games cannot be started\n", folder.c_str(), error);
		}
	}
	Log("game folders checked after %llu ms\n", GetTickCount64() - start);
}

bool IsXbox() {
	static const bool xbox =
	    winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
	return xbox;
}

std::wstring GrantCommand(std::filesystem::path folder) {
	folder.make_preferred();
	if (IsXbox() && folder.has_root_name()) {
		folder = std::filesystem::path(L"X:\\") / folder.relative_path();
	}
	// S-1-15-2-1 is ALL APPLICATION PACKAGES, on every Windows language.
	return L"icacls \"" + folder.wstring() + L"\" /grant \"*S-1-15-2-1:(OI)(CI)RX\"";
}

std::vector<std::filesystem::path> Subfolders(const std::filesystem::path& folder) {
	std::vector<std::filesystem::path> result;
	WIN32_FIND_DATAW                   data {};
	HANDLE find = FindFirstFileExFromAppW((folder / L"*").c_str(), FindExInfoBasic, &data,
	                                      FindExSearchLimitToDirectories, nullptr, 0);
	if (find == INVALID_HANDLE_VALUE) {
		return result;
	}
	do {
		const std::wstring name = data.cFileName;
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && name != L"." &&
		    name != L"..") {
			result.push_back(folder / name);
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	return result;
}

} // namespace Kyty::Uwp
