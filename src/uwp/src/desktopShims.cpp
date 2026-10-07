// Desktop-only Windows functions that libraries in the app reference, defined here so the executable does not import them: the Xbox has none of the
// desktop DLLs, and an import it cannot resolve stops the app from starting (Windows still runs it). Defining a function's import pointer
// (__imp_<name>) satisfies the libraries' dllimport calls before the desktop libraries are searched. `uwp.ps1 build` lists what is still imported.
//
// - SDL 3: its Windows core creates a helper window (for joysticks and haptics, which are off) and an icon (for its video and tray code), reads the
//   double-click time (mouse code) and the user folders and opens URLs (filesystem and misc, off). The app uses none of them, so these fail.
// - FFmpeg: the desktop window for a DXVA2 (Direct3D 9) device, which the app never creates.
// - cpuinfo: the processor counts, which it does use.

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

namespace {

UINT WINAPI DoubleClickTime() {
	return 500;
}

HWND WINAPI NoDesktopWindow() {
	return nullptr;
}

// One processor group: the Xbox and UWP apps see at most 64 logical processors.
DWORD WINAPI MaximumProcessorCount(WORD group) {
	if (group != 0 && group != ALL_PROCESSOR_GROUPS) {
		return 0;
	}
	SYSTEM_INFO info {};
	GetNativeSystemInfo(&info);
	return info.dwNumberOfProcessors;
}

WORD WINAPI MaximumProcessorGroupCount() {
	return 1;
}

// SDL 3, unused (see above): each fails.
HWND WINAPI NoCreateWindow(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID) {
	SetLastError(ERROR_NOT_SUPPORTED);
	return nullptr;
}
ATOM WINAPI NoRegisterClass(const WNDCLASSW*) {
	SetLastError(ERROR_NOT_SUPPORTED);
	return 0;
}
BOOL WINAPI NoUnregisterClass(LPCWSTR, HINSTANCE) {
	return FALSE;
}
LRESULT WINAPI NoWindowProc(HWND, UINT, WPARAM, LPARAM) {
	return 0;
}
BOOL WINAPI NoDestroyWindow(HWND) {
	return FALSE;
}
BOOL WINAPI NoSetProp(HWND, LPCWSTR, HANDLE) {
	return FALSE;
}
HDC WINAPI NoGetDC(HWND) {
	return nullptr;
}
int WINAPI NoReleaseDC(HWND, HDC) {
	return 0;
}
HICON WINAPI NoCreateIcon(PICONINFO) {
	return nullptr;
}
HBITMAP WINAPI NoCreateBitmap(int, int, UINT, UINT, const VOID*) {
	return nullptr;
}
HDC WINAPI NoCreateCompatibleDC(HDC) {
	return nullptr;
}
HBITMAP WINAPI NoCreateDIBSection(HDC, const BITMAPINFO*, UINT, VOID**, HANDLE, DWORD) {
	return nullptr;
}
BOOL WINAPI NoDeleteDC(HDC) {
	return FALSE;
}
BOOL WINAPI NoDeleteObject(HGDIOBJ) {
	return FALSE;
}
HGDIOBJ WINAPI NoSelectObject(HDC, HGDIOBJ) {
	return nullptr;
}
COLORREF WINAPI NoSetPixel(HDC, int, int, COLORREF) {
	return CLR_INVALID;
}
HRESULT WINAPI NoFolderPath(HWND, int, HANDLE, DWORD, LPWSTR) {
	return E_NOTIMPL;
}
HINSTANCE WINAPI NoShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) {
	return reinterpret_cast<HINSTANCE>(static_cast<INT_PTR>(SE_ERR_NOASSOC));
}

} // namespace

extern "C" {
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
decltype(&GetDoubleClickTime)            __imp_GetDoubleClickTime            = DoubleClickTime;
decltype(&GetDesktopWindow)              __imp_GetDesktopWindow              = NoDesktopWindow;
decltype(&GetMaximumProcessorCount)      __imp_GetMaximumProcessorCount      = MaximumProcessorCount;
decltype(&GetMaximumProcessorGroupCount) __imp_GetMaximumProcessorGroupCount = MaximumProcessorGroupCount;
decltype(&CreateWindowExW)               __imp_CreateWindowExW               = NoCreateWindow;
decltype(&RegisterClassW)                __imp_RegisterClassW                = NoRegisterClass;
decltype(&UnregisterClassW)              __imp_UnregisterClassW              = NoUnregisterClass;
decltype(&DefWindowProcW)                __imp_DefWindowProcW                = NoWindowProc;
decltype(&DestroyWindow)                 __imp_DestroyWindow                 = NoDestroyWindow;
decltype(&SetPropW)                      __imp_SetPropW                      = NoSetProp;
decltype(&GetDC)                         __imp_GetDC                         = NoGetDC;
decltype(&ReleaseDC)                     __imp_ReleaseDC                     = NoReleaseDC;
decltype(&CreateIconIndirect)            __imp_CreateIconIndirect            = NoCreateIcon;
decltype(&CreateBitmap)                  __imp_CreateBitmap                  = NoCreateBitmap;
decltype(&CreateCompatibleDC)            __imp_CreateCompatibleDC            = NoCreateCompatibleDC;
decltype(&CreateDIBSection)              __imp_CreateDIBSection              = NoCreateDIBSection;
decltype(&DeleteDC)                      __imp_DeleteDC                      = NoDeleteDC;
decltype(&DeleteObject)                  __imp_DeleteObject                  = NoDeleteObject;
decltype(&SelectObject)                  __imp_SelectObject                  = NoSelectObject;
decltype(&SetPixel)                      __imp_SetPixel                      = NoSetPixel;
decltype(&SHGetFolderPathW)              __imp_SHGetFolderPathW              = NoFolderPath;
decltype(&ShellExecuteW)                 __imp_ShellExecuteW                 = NoShellExecute;
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
}
