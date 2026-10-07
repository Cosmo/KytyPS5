#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#if defined(KYTY_PLATFORM_UWP)
#include <fcntl.h>
#include <fileapifromapp.h>
#include <io.h>
#endif

#include "common/assert.h"
#include "common/platform/sysFileIO.h"
#include "common/stringUtils.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_t {
	HANDLE handle;
};

// IWYU pragma: no_include <fileapi.h>
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <winbase.h>

constexpr DWORD FILE_SHARE_POSIX = static_cast<DWORD>(FILE_SHARE_READ) |
                                   static_cast<DWORD>(FILE_SHARE_WRITE) |
                                   static_cast<DWORD>(FILE_SHARE_DELETE);
constexpr DWORD READ_WRITE_DELETE = static_cast<DWORD>(GENERIC_READ) |
                                    static_cast<DWORD>(GENERIC_WRITE) | static_cast<DWORD>(DELETE);

// The UWP app reaches files outside its package only through the *FromApp functions, which apply its file capabilities (removableStorage).
#if defined(KYTY_PLATFORM_UWP)
// Where the app has no direct access (the Xbox's USB drives), the *FromApp functions go through the file broker, over COM and RPC, which needs far more
// stack than a guest thread may have: games give threads small stacks of their own, which the host code they call runs on (a game thread overflowed its
// stack in GetFileAttributesExFromAppW, which ended the process). These calls run on a thread of their own with a large stack; the caller waits, and gets
// the call's last error.
class BrokerThread {
public:
	template <typename Fn>
	auto Run(Fn&& fn) -> decltype(fn()) {
		using Result = decltype(fn());
		if (GetCurrentThreadId() == m_thread_id) {
			return fn();
		}
		std::optional<Result> result;
		DWORD                 error = 0;
		Call([&] {
			result = fn();
			error  = GetLastError();
		});
		SetLastError(error);
		return *result;
	}

private:
	void Call(std::function<void()> job) {
		std::unique_lock lock(m_mutex);
		if (m_thread == nullptr) {
			m_thread = CreateThread(
			    nullptr, StackSize,
			    [](void* parameter) -> DWORD {
				    static_cast<BrokerThread*>(parameter)->Loop();
				    return 0;
			    },
			    this, STACK_SIZE_PARAM_IS_A_RESERVATION, &m_thread_id);
			EXIT_IF(m_thread == nullptr);
		}
		const auto ticket = ++m_submitted;
		m_jobs.push_back(std::move(job));
		m_changed.notify_all();
		m_changed.wait(lock, [&] { return m_finished >= ticket; });
	}

	void Loop() {
		std::unique_lock lock(m_mutex);
		for (;;) {
			m_changed.wait(lock, [&] { return !m_jobs.empty(); });
			auto job = std::move(m_jobs.front());
			m_jobs.pop_front();
			lock.unlock();
			job();
			lock.lock();
			m_finished++;
			m_changed.notify_all();
		}
	}

	static constexpr SIZE_T StackSize = SIZE_T {4} << 20u;

	std::mutex                        m_mutex;
	std::condition_variable           m_changed;
	std::deque<std::function<void()>> m_jobs;
	uint64_t                          m_submitted = 0;
	uint64_t                          m_finished  = 0;
	HANDLE                            m_thread    = nullptr;
	DWORD                             m_thread_id = 0;
};

static BrokerThread& Broker() {
	static BrokerThread thread;
	return thread;
}

// The Xbox's USB drives refuse a UWP app now and then for a moment: a starting app gets "access denied" at first (fileAccess.cpp waits for its
// game folders), and a file's look-up or open can get it later too, or "not ready" or a sharing violation, where asked again a moment later it
// works (a game's video of its microphone prompt was "missing" so once, and the game stopped). Reading a file's attributes and
// opening a file to read are asked again for up to RETRY_LIMIT on those errors; the log says when that was needed and how it ended.
constexpr DWORD RETRY_INTERVAL_MS = 50;
constexpr int   RETRY_LIMIT       = 10;

static bool IsTransientFileError(DWORD error) {
	return error == ERROR_ACCESS_DENIED || error == ERROR_NOT_READY || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

// Calls `call` again while it fails with a transient error; `failed` tells a failure from a result.
template <typename Call, typename Failed>
static auto Retried(const wchar_t* name, const char* what, Call&& call, Failed&& failed) -> decltype(call()) {
	auto result = call();
	if (!failed(result) || !IsTransientFileError(GetLastError())) {
		return result;
	}
	const DWORD first = GetLastError();
	DWORD       error = first;
	for (int attempt = 1; attempt <= RETRY_LIMIT; attempt++) {
		Sleep(RETRY_INTERVAL_MS);
		result = call();
		if (!failed(result)) {
			std::printf("File: %s %ls worked when asked again (%d times; error %lu at first)\n", what, name, attempt, first);
			std::fflush(stdout);
			return result;
		}
		error = GetLastError();
		if (!IsTransientFileError(error)) {
			break;
		}
	}
	std::printf("File: %s %ls failed, also when asked again (error %lu; %lu at first)\n", what, name, error, first);
	std::fflush(stdout);
	SetLastError(error);
	return result;
}

static HANDLE HostCreateFile(const wchar_t* name, DWORD access, DWORD disposition, DWORD flags) {
	const auto open = [&] {
		return Broker().Run([&] { return CreateFileFromAppW(name, access, FILE_SHARE_POSIX, nullptr, disposition, flags, nullptr); });
	};
	// Only opens to read: a refused write can be meant (a read-only file).
	if (access != GENERIC_READ || disposition != OPEN_EXISTING) {
		return open();
	}
	return Retried(name, "opening", open, [](HANDLE handle) { return handle == INVALID_HANDLE_VALUE; });
}

static BOOL HostGetFileAttributesEx(const wchar_t* name, WIN32_FILE_ATTRIBUTE_DATA* data) {
	return Retried(
	    name, "looking up",
	    [&] { return Broker().Run([&] { return GetFileAttributesExFromAppW(name, GetFileExInfoStandard, data); }); },
	    [](BOOL result) { return result == 0; });
}

static BOOL HostCreateDirectory(const wchar_t* name) {
	return Broker().Run([&] { return CreateDirectoryFromAppW(name, nullptr); });
}

static BOOL HostRemoveDirectory(const wchar_t* name) {
	return Broker().Run([&] { return RemoveDirectoryFromAppW(name); });
}

static BOOL HostDeleteFile(const wchar_t* name) {
	return Broker().Run([&] { return DeleteFileFromAppW(name); });
}

static HANDLE HostFindFirstFile(const wchar_t* pattern, WIN32_FIND_DATAW* data) {
	return Broker().Run([&] { return FindFirstFileExFromAppW(pattern, FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0); });
}

static BOOL HostFindNextFile(HANDLE find, WIN32_FIND_DATAW* data) {
	return Broker().Run([&] { return FindNextFileW(find, data); });
}

static BOOL HostMoveFile(const wchar_t* source, const wchar_t* destination) {
	return Broker().Run([&] { return MoveFileFromAppW(source, destination); });
}
#else
static HANDLE HostCreateFile(const wchar_t* name, DWORD access, DWORD disposition, DWORD flags) {
	return CreateFileW(name, access, FILE_SHARE_POSIX, nullptr, disposition, flags, nullptr);
}

static BOOL HostGetFileAttributesEx(const wchar_t* name, WIN32_FILE_ATTRIBUTE_DATA* data) {
	return GetFileAttributesExW(name, GetFileExInfoStandard, data);
}

static BOOL HostCreateDirectory(const wchar_t* name) {
	return CreateDirectoryW(name, nullptr);
}

static BOOL HostRemoveDirectory(const wchar_t* name) {
	return RemoveDirectoryW(name);
}

static BOOL HostDeleteFile(const wchar_t* name) {
	return DeleteFileW(name);
}

static HANDLE HostFindFirstFile(const wchar_t* pattern, WIN32_FIND_DATAW* data) {
	return FindFirstFileW(pattern, data);
}

static BOOL HostFindNextFile(HANDLE find, WIN32_FIND_DATAW* data) {
	return FindNextFileW(find, data);
}

static BOOL HostMoveFile(const wchar_t* source, const wchar_t* destination) {
	return MoveFileW(source, destination);
}
#endif

void SysFileRead(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read) {
	DWORD r = 0;
	ReadFile(f.handle, data, size, &r, nullptr);
	if (bytes_read != nullptr) {
		*bytes_read = r;
	}
}

void SysFileWrite(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written) {
	DWORD w = 0;
	WriteFile(f.handle, data, size, &w, nullptr);
	if (bytes_written != nullptr) {
		*bytes_written = w;
	}
}

static sys_file_t* OpenHostFile(const std::filesystem::path& file_name, DWORD access,
                                DWORD disposition) {
	HANDLE handle = HostCreateFile(file_name.c_str(), access, disposition, FILE_ATTRIBUTE_NORMAL);
	if (handle == INVALID_HANDLE_VALUE) {
		return nullptr;
	}
	return new sys_file_t {handle};
}

sys_file_t* SysFileCreate(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, READ_WRITE_DELETE, CREATE_ALWAYS);
}

sys_file_t* SysFileOpenR(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, GENERIC_READ, OPEN_EXISTING);
}

sys_file_t* SysFileOpenW(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, static_cast<DWORD>(GENERIC_WRITE) | static_cast<DWORD>(DELETE),
	                    OPEN_EXISTING);
}

sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name) {
	return OpenHostFile(file_name, READ_WRITE_DELETE, OPEN_EXISTING);
}

void SysFileClose(sys_file_t* f) {
	CloseHandle(f->handle);
	delete f;
}

uint64_t SysFileSize(sys_file_t& f) {
	LARGE_INTEGER s;
	GetFileSizeEx(f.handle, &s);
	return s.QuadPart;
}

bool SysFileGetInfo(const std::filesystem::path& name, bool* is_file, uint64_t* size) {
	const bool directory_path = !name.empty() && !name.has_filename() && name != name.root_path();
	WIN32_FILE_ATTRIBUTE_DATA info {};
	if (HostGetFileAttributesEx(directory_path ? name.parent_path().c_str() : name.c_str(), &info) == 0) {
		return false;
	}
	const bool file = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	if (directory_path && file) {
		return false;
	}
	*is_file = file;
	*size = *is_file ? ((static_cast<uint64_t>(info.nFileSizeHigh) << 32u) | info.nFileSizeLow) : 0;
	return true;
}

bool SysFileTruncate(sys_file_t& f, uint64_t size) {
	LARGE_INTEGER s {};
	LARGE_INTEGER r {};
	s.QuadPart = 0;
	SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
	s.QuadPart = static_cast<LONGLONG>(size);
	const bool ok =
	    (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0 && SetEndOfFile(f.handle) != 0);
	SetFilePointerEx(f.handle, r, nullptr, FILE_BEGIN);
	return ok;
}

bool SysFileUnlink(sys_file_t& f, const std::filesystem::path& name) {
	FILE_DISPOSITION_INFO info {};
	info.DeleteFile = TRUE;
	if (SetFileInformationByHandle(f.handle, FileDispositionInfo, &info, sizeof(info)) != 0) {
		return true;
	}

	return SysFileDeleteFile(name);
}

bool SysFileSeek(sys_file_t& f, uint64_t offset) {
	LARGE_INTEGER s;
	s.QuadPart = static_cast<LONGLONG>(offset);
	return SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0;
}

uint64_t SysFileTell(sys_file_t& f) {
	LARGE_INTEGER s {};
	LARGE_INTEGER r {};
	s.QuadPart = 0;
	SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
	return r.QuadPart;
}

bool SysFileCreateDirectory(const std::filesystem::path& path) {
	auto wide = path.wstring();
	return HostCreateDirectory(wide.c_str()) != 0;
}

bool SysFileDeleteDirectory(const std::filesystem::path& path, std::error_code* error) {
	auto wide = path.wstring();
	if (HostRemoveDirectory(wide.c_str()) != 0) {
		return true;
	}
	if (error != nullptr) {
		*error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
	}
	return false;
}

bool SysFileDeleteFile(const std::filesystem::path& name) {
	auto wide = name.wstring();
	return HostDeleteFile(wide.c_str()) != 0;
}

bool SysFileFlush(sys_file_t& f) {
	return FlushFileBuffers(f.handle) != 0;
}

SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct a {};
	SysFileTimeStruct w {};
	SysFileGetLastAccessAndWriteTimeUtc(name, a, w);
	return w;
}

void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	sys_file_t* f = SysFileOpenR(name);
	if (f == nullptr) {
		a.is_invalid = w.is_invalid = true;
		return;
	}
	SysFileGetLastAccessAndWriteTimeUtc(*f, a, w);
	SysFileClose(f);
}

void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	a.is_invalid = w.is_invalid = (GetFileTime(f.handle, nullptr, &a.time, &w.time) == 0);
}

void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out) {
	const auto pattern = path / L"*";

	WIN32_FIND_DATAW data {};
	HANDLE           h = HostFindFirstFile(pattern.c_str(), &data);

	if (h == INVALID_HANDLE_VALUE) {
		return;
	}

	do {
		std::filesystem::path file_name(data.cFileName);

		sys_dir_entry_t r {};

		r.is_file = ((data.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
		r.name    = Common::PathToString(file_name);

		out.push_back(std::move(r));

	} while (HostFindNextFile(h, &data) != 0);

	FindClose(h);
}

bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = src.wstring();
	auto dst_wide = dst.wstring();
	return HostMoveFile(src_wide.c_str(), dst_wide.c_str()) != 0;
}

#if defined(KYTY_PLATFORM_UWP)
std::FILE* SysFileOpenCStreamR(const std::filesystem::path& file_name) {
	const auto wide   = file_name.wstring();
	HANDLE     handle = HostCreateFile(wide.c_str(), GENERIC_READ, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS);
	if (handle == INVALID_HANDLE_VALUE) {
		return nullptr;
	}
	const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
	if (descriptor < 0) {
		CloseHandle(handle);
		return nullptr;
	}
	std::FILE* file = _fdopen(descriptor, "rb");
	if (file == nullptr) {
		_close(descriptor);
	}
	return file;
}
#endif

#endif
