#include "gameSource.h"

#include "fileAccess.h"
#include "log.h"

#include <windows.h>
#include <fileapifromapp.h>
#include <fcntl.h>
#include <io.h>

#include <winrt/base.h>

#include <zarchive/zarchivereader.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <memory>
#include <mutex>

namespace Kyty::Uwp {

namespace {

constexpr size_t   MAX_OPEN_ARCHIVES = 4;
constexpr uint64_t MAX_FILE_SIZE     = uint64_t {32} << 20;

struct OpenArchive {
	std::wstring                    key;
	std::shared_ptr<ZArchiveReader> reader;
};

std::mutex               g_mutex;
std::vector<OpenArchive> g_open; // most recently used last

// Opens the archive's file with the *FromApp functions (the Xbox lets the app read a USB drive only that way; a plain C++ stream cannot open the
// file there) and gives the reader a stream on it. The stream does not own the C file, so the reader's deleter closes it.
std::shared_ptr<ZArchiveReader> OpenReader(const std::filesystem::path& path) {
	HANDLE handle = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		Log("cannot open archive %ls (error %lu)\n", path.c_str(), GetLastError());
		return nullptr;
	}
	const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
	if (descriptor < 0) {
		CloseHandle(handle);
		Log("cannot open archive %ls (no C file descriptor)\n", path.c_str());
		return nullptr;
	}
	FILE* file = _fdopen(descriptor, "rb");
	if (file == nullptr) {
		_close(descriptor);
		Log("cannot open archive %ls (no C file)\n", path.c_str());
		return nullptr;
	}
	std::ifstream   stream(file);
	ZArchiveReader* reader = ZArchiveReader::OpenFromStream(std::move(stream));
	if (reader == nullptr) {
		fclose(file);
		Log("archive %ls is not a readable .zar\n", path.c_str());
		return nullptr;
	}
	return std::shared_ptr<ZArchiveReader>(reader, [file](ZArchiveReader* archive) {
		delete archive;
		fclose(file);
	});
}

// The archive at `path`, kept open for the next read (opening reads its index).
std::shared_ptr<ZArchiveReader> Open(const std::filesystem::path& path) {
	std::lock_guard lock(g_mutex);
	const auto      key = path.wstring();
	for (auto it = g_open.begin(); it != g_open.end(); ++it) {
		if (it->key == key) {
			std::rotate(it, it + 1, g_open.end());
			return g_open.back().reader;
		}
	}
	auto reader = OpenReader(path);
	if (!reader) {
		return nullptr;
	}
	if (g_open.size() >= MAX_OPEN_ARCHIVES) {
		g_open.erase(g_open.begin());
	}
	g_open.push_back({key, reader});
	return reader;
}

// Splits `path` at an archive that is a file: the archive and the path inside it, with forward slashes.
bool SplitArchivePath(const std::filesystem::path& path, std::filesystem::path& archive, std::string& inner) {
	std::filesystem::path walked;
	bool                  found = false;
	for (const auto& part: path) {
		if (found) {
			inner += inner.empty() ? "" : "/";
			inner += winrt::to_string(part.wstring());
			continue;
		}
		walked /= part;
		if (IsArchivePath(walked) && IsFile(walked)) {
			archive = walked;
			found   = true;
		}
	}
	return found;
}

} // namespace

bool IsArchivePath(const std::filesystem::path& path) {
	auto extension = path.extension().wstring();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
	return extension == L".zar";
}

bool GameFileExists(const std::filesystem::path& path) {
	std::filesystem::path archive;
	std::string           inner;
	if (!SplitArchivePath(path, archive, inner)) {
		return IsFile(path);
	}
	const auto reader = Open(archive);
	return reader && reader->LookUp(inner, true, false) != ZARCHIVE_INVALID_NODE;
}

std::optional<std::vector<uint8_t>> ReadGameBinary(const std::filesystem::path& path) {
	std::filesystem::path archive;
	std::string           inner;
	if (!SplitArchivePath(path, archive, inner)) {
		return ReadBinaryFile(path);
	}
	const auto reader = Open(archive);
	if (!reader) {
		return std::nullopt;
	}
	const auto node = reader->LookUp(inner, true, false);
	if (node == ZARCHIVE_INVALID_NODE) {
		return std::nullopt;
	}
	const auto size = reader->GetFileSize(node);
	if (size > MAX_FILE_SIZE) {
		return std::nullopt;
	}
	std::vector<uint8_t> data(static_cast<size_t>(size));
	if (reader->ReadFromFile(node, 0, size, data.data()) != size) {
		return std::nullopt;
	}
	return data;
}

std::optional<std::string> ReadGameText(const std::filesystem::path& path) {
	auto data = ReadGameBinary(path);
	if (!data) {
		return std::nullopt;
	}
	return std::string(data->begin(), data->end());
}

std::vector<std::filesystem::path> ArchivesIn(const std::filesystem::path& folder) {
	std::vector<std::filesystem::path> result;
	WIN32_FIND_DATAW                   data {};
	HANDLE find = FindFirstFileExFromAppW((folder / L"*.zar").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
	if (find == INVALID_HANDLE_VALUE) {
		return result;
	}
	do {
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			result.push_back(folder / data.cFileName);
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	std::sort(result.begin(), result.end());
	return result;
}

} // namespace Kyty::Uwp
