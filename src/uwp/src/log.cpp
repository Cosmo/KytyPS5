#include "log.h"

#include <windows.h>

#include <winrt/Windows.Storage.h>

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

namespace Kyty::Uwp {

namespace {

// The time since the app started, at the start of every line.
std::string ElapsedTime() {
	static const auto start = std::chrono::steady_clock::now();
	const double      seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	char              text[32];
	snprintf(text, sizeof(text), "[%8.3f] ", seconds);
	return text;
}

FILE* OpenLog() {
	std::wstring folder;
	try {
		folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
	} catch (const winrt::hresult_error&) {
		// The package's temporary folder, which the app can always write.
		wchar_t temp[MAX_PATH] {};
		GetTempPathW(MAX_PATH, temp);
		folder = temp;
	}
	return _wfsopen((folder + L"\\kyty-uwp.txt").c_str(), L"w", _SH_DENYWR);
}

} // namespace

void Log(const char* format, ...) {
	static std::mutex mutex;
	static FILE*      file = OpenLog();

	char    line[1024];
	va_list args;
	va_start(args, format);
	const int length = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	// A line cut short still ends its line (the next one starts with its time).
	if (length >= static_cast<int>(sizeof(line)) && std::strchr(format, '\n') != nullptr) {
		line[sizeof(line) - 2] = '\n';
	}

	std::lock_guard lock(mutex);
	// Each line starts with the time since the app started, as the emulator's log's do.
	static bool line_start = true;
	std::string timed;
	for (const char* c = line; *c != '\0'; c++) {
		if (line_start) {
			timed += ElapsedTime();
			line_start = false;
		}
		timed += *c;
		line_start = *c == '\n';
	}
	// OutputDebugString raises an exception, which the system cannot dispatch on a guest thread (its stack is not the thread's own): only with a
	// debugger attached, which handles it first.
	if (IsDebuggerPresent()) {
		OutputDebugStringA(timed.c_str());
	}
	if (file != nullptr) {
		fputs(timed.c_str(), file);
		fflush(file);
	}
}

} // namespace Kyty::Uwp
