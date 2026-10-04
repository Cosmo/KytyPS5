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

uint32_t ListError(const std::filesystem::path& folder) {
	WIN32_FIND_DATAW data {};
	HANDLE find = FindFirstFileExFromAppW((folder / L"*").c_str(), FindExInfoBasic, &data,
	                                      FindExSearchNameMatch, nullptr, 0);
	if (find == INVALID_HANDLE_VALUE) {
		const auto error = GetLastError();
		Log("cannot list %ls (error %lu)\n", folder.c_str(), error);
		return error;
	}
	FindClose(find);
	return 0;
}

void WaitForGameFolders() {
	const auto start = GetTickCount64();
	for (const auto& folder: GameFolders()) {
		while (ListError(folder) == ERROR_ACCESS_DENIED && GetTickCount64() - start < 2000) {
			Sleep(100);
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
