#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

#include "common/assert.h"
#include "common/platform/sysWindowsVirtual.h"
#include "common/virtualMemory.h"

#if defined(KYTY_PLATFORM_UWP)
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#endif

// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <memoryapi.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>
// IWYU pragma: no_include <winerror.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::VirtualMemory {

static DWORD GetProtectionFlag(Mode mode) {
	DWORD protect = PAGE_NOACCESS;
	switch (mode) {
		case Mode::Read: protect = PAGE_READONLY; break;

		case Mode::Write:
		case Mode::ReadWrite: protect = PAGE_READWRITE; break;

		case Mode::Execute: protect = PAGE_EXECUTE; break;

		case Mode::ExecuteRead: protect = PAGE_EXECUTE_READ; break;

		case Mode::ExecuteWrite:
		case Mode::ExecuteReadWrite:
#if defined(KYTY_PLATFORM_UWP)
			// Never both in the app container: writable until executed (HandleWriteExecuteFault).
			protect = PAGE_READWRITE;
#else
			protect = PAGE_EXECUTE_READWRITE;
#endif
			break;

		case Mode::NoAccess:
		default: protect = PAGE_NOACCESS; break;
	}
	return protect;
}

static bool IsWriteExecute(Mode mode) {
	return mode == Mode::ExecuteWrite || mode == Mode::ExecuteReadWrite;
}

#if defined(KYTY_PLATFORM_UWP)

// Ranges meant to be writable and executable. Their pages are writable until executed and
// executable until written; faults switch them (HandleWriteExecuteFault).
class WriteExecuteRanges {
public:
	void Add(uint64_t address, uint64_t size) {
		Remove(address, size);
		m_ranges.emplace(address, address + size);
	}

	void Remove(uint64_t address, uint64_t size) {
		const auto end = address + size;
		auto       it  = m_ranges.upper_bound(address);
		if (it != m_ranges.begin()) {
			--it;
		}
		while (it != m_ranges.end() && it->first < end) {
			const auto [start, range_end] = *it;
			if (range_end <= address) {
				++it;
				continue;
			}
			it = m_ranges.erase(it);
			if (start < address) {
				m_ranges.emplace(start, address);
			}
			if (range_end > end) {
				m_ranges.emplace(end, range_end);
			}
		}
	}

	[[nodiscard]] bool Contains(uint64_t address) const {
		auto it = m_ranges.upper_bound(address);
		return it != m_ranges.begin() && address < std::prev(it)->second;
	}

	std::mutex mutex;

private:
	std::map<uint64_t, uint64_t> m_ranges; // start -> end
};

static WriteExecuteRanges& WriteExecute() {
	static WriteExecuteRanges ranges;
	return ranges;
}

static void* HostVirtualAlloc(void* address, SIZE_T size, ULONG type, ULONG protect) {
	return VirtualAllocFromApp(address, size, type, protect);
}

static void* HostVirtualAlloc2(void* address, SIZE_T size, ULONG type, ULONG protect,
                               MEM_EXTENDED_PARAMETER* params, ULONG count) {
	return VirtualAlloc2FromApp(nullptr, address, size, type, protect, params, count);
}

static bool HostVirtualProtect(void* address, SIZE_T size, ULONG protect) {
	ULONG old_protect = 0;
	return VirtualProtectFromApp(address, size, protect, &old_protect) != 0;
}

// The app container can't allocate executable memory: it starts writable, and
// FinishAllocation applies the mode.
static DWORD AllocationProtect(Mode mode) {
	return IsExecute(mode) ? PAGE_READWRITE : GetProtectionFlag(mode);
}

// The size of the allocation starting at address.
static uint64_t AllocationSize(uint64_t address) {
	uint64_t current = address;
	for (;;) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) == 0 ||
		    reinterpret_cast<uint64_t>(info.AllocationBase) != address || info.State == MEM_FREE) {
			return current - address;
		}
		current = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
	}
}

#else

using VirtualAlloc2_func_t = /*WINBASEAPI*/ PVOID WINAPI (*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                                             MEM_EXTENDED_PARAMETER*, ULONG);

static VirtualAlloc2_func_t ResolveVirtualAlloc2() {
	HMODULE h = GetModuleHandle("KernelBase"); // @suppress("Invalid arguments")
	if (h != nullptr) {
		return reinterpret_cast<VirtualAlloc2_func_t>(GetProcAddress(h, "VirtualAlloc2"));
	}
	return nullptr;
}

static void* HostVirtualAlloc(void* address, SIZE_T size, ULONG type, ULONG protect) {
	return VirtualAlloc(address, size, type, protect);
}

static void* HostVirtualAlloc2(void* address, SIZE_T size, ULONG type, ULONG protect,
                               MEM_EXTENDED_PARAMETER* params, ULONG count) {
	static auto virtual_alloc2 = ResolveVirtualAlloc2();
	EXIT_NOT_IMPLEMENTED(virtual_alloc2 == nullptr);
	return virtual_alloc2(GetCurrentProcess(), address, size, type, protect, params, count);
}

static bool HostVirtualProtect(void* address, SIZE_T size, ULONG protect) {
	DWORD old_protect = 0;
	return VirtualProtect(address, size, protect, &old_protect) != 0;
}

static DWORD AllocationProtect(Mode mode) {
	return GetProtectionFlag(mode);
}

#endif

// Gives newly allocated memory its mode where it had to be allocated writable instead.
static bool FinishAllocation(uint64_t address, uint64_t size, Mode mode) {
#if defined(KYTY_PLATFORM_UWP)
	return !IsExecute(mode) || Protect(address, size, mode);
#else
	(void)address;
	(void)size;
	(void)mode;
	return true;
#endif
}

void Init() {}

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode) {
	if (address == 0) {
		return AllocAligned(address, size, mode, 1);
	}
	auto ptr = reinterpret_cast<uintptr_t>(HostVirtualAlloc(
	    reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	    static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE), AllocationProtect(mode)));
	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());

		if (err != ERROR_INVALID_ADDRESS) {
			printf("VirtualAlloc() failed: 0x%08" PRIx32 "\n", err);
		} else {
			return AllocAligned(address, size, mode, 1);
		}
	} else if (!FinishAllocation(ptr, size, mode)) {
		Free(ptr);
		return 0;
	}
	return ptr;
}

static uint64_t AlignUp(uint64_t addr, uint64_t alignment) {
	return (addr + alignment - 1) & ~(alignment - 1);
}

uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment) {
	if (alignment == 0) {
		printf("VirtualAlloc2 failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return 0;
	}

	static constexpr uint64_t SYSTEM_MANAGED_MIN = 0x0000040000u;
	static constexpr uint64_t SYSTEM_MANAGED_MAX = 0x07FFFFBFFFu;
	static constexpr uint64_t USER_MIN           = 0x1000000000u;
	static constexpr uint64_t USER_MAX           = 0xFBFFFFFFFFu;

	MEM_ADDRESS_REQUIREMENTS req {};
	MEM_EXTENDED_PARAMETER   param {};
	req.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req.HighestEndingAddress = (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MAX)
	                                         : reinterpret_cast<PVOID>(USER_MAX));
	req.Alignment            = alignment;
	param.Type               = MemExtendedParameterAddressRequirements;
	param.Pointer            = &req;

	MEM_ADDRESS_REQUIREMENTS req2 {};
	MEM_EXTENDED_PARAMETER   param2 {};
	req2.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(USER_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req2.HighestEndingAddress = reinterpret_cast<PVOID>(USER_MAX);
	req2.Alignment            = alignment;
	param2.Type               = MemExtendedParameterAddressRequirements;
	param2.Pointer            = &req2;

	auto ptr = reinterpret_cast<uintptr_t>(
	    HostVirtualAlloc2(nullptr, size,
	                      static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
	                      AllocationProtect(mode), &param, 1));

	if (ptr == 0) {
		ptr = reinterpret_cast<uintptr_t>(
		    HostVirtualAlloc2(nullptr, size,
		                      static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE),
		                      AllocationProtect(mode), &param2, 1));
	}

	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());
		if (err != ERROR_INVALID_PARAMETER) {
			printf("VirtualAlloc2(alignment = 0x%016" PRIx64 ") failed: 0x%08" PRIx32 "\n",
			       alignment, err);
		} else {
			return AllocAligned(address, size, mode, alignment << 1u);
		}
	} else if (!FinishAllocation(ptr, size, mode)) {
		Free(ptr);
		return 0;
	}
	return ptr;
}

bool AllocFixed(uint64_t address, uint64_t size, Mode mode) {
	auto ptr = reinterpret_cast<uintptr_t>(HostVirtualAlloc(
	    reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	    static_cast<DWORD>(MEM_COMMIT) | static_cast<DWORD>(MEM_RESERVE), AllocationProtect(mode)));
	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());

		printf("VirtualAlloc() failed: 0x%08" PRIx32 "\n", err);
		return false;
	}

	if (ptr != address) {
		printf("VirtualAlloc() failed: wrong address\n");
		VirtualFree(reinterpret_cast<LPVOID>(ptr), 0, MEM_RELEASE);
		return false;
	}

	if (!FinishAllocation(ptr, size, mode)) {
		Free(ptr);
		return false;
	}
	return true;
}

bool Commit(uint64_t address, uint64_t size, Mode mode) {
	auto ptr = reinterpret_cast<uintptr_t>(
	    HostVirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                     static_cast<DWORD>(MEM_COMMIT), AllocationProtect(mode)));
	if (ptr == 0) {
		printf("VirtualAlloc(MEM_COMMIT) failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return false;
	}

	if (ptr != address) {
		printf("VirtualAlloc(MEM_COMMIT) failed: wrong address\n");
		return false;
	}

	return FinishAllocation(ptr, size, mode);
}

uint64_t Reserve(uint64_t address, uint64_t size) {
	auto ptr = (address == 0 ? ReserveAligned(address, size, 1)
	                         : reinterpret_cast<uintptr_t>(HostVirtualAlloc(
	                               reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                               static_cast<DWORD>(MEM_RESERVE), PAGE_NOACCESS)));
	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());

		if (err != ERROR_INVALID_ADDRESS) {
			printf("VirtualAlloc(MEM_RESERVE) failed: 0x%08" PRIx32 "\n", err);
		} else {
			return ReserveAligned(address, size, 1);
		}
	}
	return ptr;
}

uint64_t ReserveAligned(uint64_t address, uint64_t size, uint64_t alignment) {
	if (alignment == 0) {
		printf("VirtualAlloc2(MEM_RESERVE) failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return 0;
	}

	static constexpr uint64_t SYSTEM_MANAGED_MIN = 0x0000040000u;
	static constexpr uint64_t SYSTEM_MANAGED_MAX = 0x07FFFFBFFFu;
	static constexpr uint64_t USER_MIN           = 0x1000000000u;
	static constexpr uint64_t USER_MAX           = 0xFBFFFFFFFFu;

	MEM_ADDRESS_REQUIREMENTS req {};
	MEM_EXTENDED_PARAMETER   param {};
	req.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req.HighestEndingAddress = (address == 0 ? reinterpret_cast<PVOID>(SYSTEM_MANAGED_MAX)
	                                         : reinterpret_cast<PVOID>(USER_MAX));
	req.Alignment            = alignment;
	param.Type               = MemExtendedParameterAddressRequirements;
	param.Pointer            = &req;

	MEM_ADDRESS_REQUIREMENTS req2 {};
	MEM_EXTENDED_PARAMETER   param2 {};
	req2.LowestStartingAddress =
	    (address == 0 ? reinterpret_cast<PVOID>(USER_MIN)
	                  : reinterpret_cast<PVOID>(AlignUp(address, alignment)));
	req2.HighestEndingAddress = reinterpret_cast<PVOID>(USER_MAX);
	req2.Alignment            = alignment;
	param2.Type               = MemExtendedParameterAddressRequirements;
	param2.Pointer            = &req2;

	auto ptr = reinterpret_cast<uintptr_t>(
	    HostVirtualAlloc2(nullptr, size, static_cast<DWORD>(MEM_RESERVE), PAGE_NOACCESS, &param, 1));

	if (ptr == 0) {
		ptr = reinterpret_cast<uintptr_t>(HostVirtualAlloc2(
		    nullptr, size, static_cast<DWORD>(MEM_RESERVE), PAGE_NOACCESS, &param2, 1));
	}

	if (ptr == 0) {
		auto err = static_cast<uint32_t>(GetLastError());
		if (err != ERROR_INVALID_PARAMETER) {
			printf("VirtualAlloc2(MEM_RESERVE, alignment = 0x%016" PRIx64 ") failed: 0x%08" PRIx32
			       "\n",
			       alignment, err);
		} else {
			return ReserveAligned(address, size, alignment << 1u);
		}
	}
	return ptr;
}

bool ReserveFixed(uint64_t address, uint64_t size) {
	auto ptr = reinterpret_cast<uintptr_t>(
	    HostVirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                     static_cast<DWORD>(MEM_RESERVE), PAGE_NOACCESS));
	if (ptr == 0) {
		printf("VirtualAlloc(MEM_RESERVE) failed: address=0x%016" PRIx64 ", size=0x%016" PRIx64
		       ", err=0x%08" PRIx32 "\n",
		       address, size, static_cast<uint32_t>(GetLastError()));
		return false;
	}

	if (ptr != address) {
		printf("VirtualAlloc(MEM_RESERVE) failed: wrong address\n");
		VirtualFree(reinterpret_cast<LPVOID>(ptr), 0, MEM_RELEASE);
		return false;
	}

	return true;
}

bool Decommit(uint64_t address, uint64_t size) {
#if defined(KYTY_PLATFORM_UWP)
	{
		std::lock_guard lock(WriteExecute().mutex);
		WriteExecute().Remove(address, size);
	}
#endif
	if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                MEM_DECOMMIT) == 0) {
		printf("VirtualFree(MEM_DECOMMIT) failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool Free(uint64_t address) {
#if defined(KYTY_PLATFORM_UWP)
	{
		std::lock_guard lock(WriteExecute().mutex);
		WriteExecute().Remove(address, AllocationSize(address));
	}
#endif
	if (VirtualFree(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE) ==
	    0) {
		printf("VirtualFree() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool FreeRange(uint64_t address, uint64_t size) {
	if (address == 0 || size == 0) {
		return false;
	}

	uint64_t current = address;
	while (current - address < size) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) == 0 ||
		    reinterpret_cast<uint64_t>(info.AllocationBase) != address || info.State == MEM_FREE) {
			return false;
		}
		const auto next = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
		if (next <= current || next - address > size) {
			return false;
		}
		current = next;
	}
	return current - address == size && Free(address);
}

bool Protect(uint64_t address, uint64_t size, Mode mode) {
#if defined(KYTY_PLATFORM_UWP)
	std::lock_guard lock(WriteExecute().mutex);
	WriteExecute().Remove(address, size);
	if (IsWriteExecute(mode)) {
		WriteExecute().Add(address, size);
	}
#endif
	if (!HostVirtualProtect(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), size,
	                        GetProtectionFlag(mode))) {
		printf("VirtualProtect() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

bool FlushInstructionCache(uint64_t address, uint64_t size) {
	if (::FlushInstructionCache(GetCurrentProcess(),
	                            reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)),
	                            size) == 0) {
		printf("FlushInstructionCache() failed: 0x%08" PRIx32 "\n",
		       static_cast<uint32_t>(GetLastError()));
		return false;
	}
	return true;
}

namespace Windows {

bool ReservePlaceholder(uint64_t address, uint64_t size) {
	auto* ptr = HostVirtualAlloc2(reinterpret_cast<void*>(address), size,
	                              MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
	return reinterpret_cast<uint64_t>(ptr) == address;
}

bool CommitPlaceholder(uint64_t address, uint64_t size, Mode mode) {
	auto* ptr = HostVirtualAlloc2(reinterpret_cast<void*>(address), size,
	                              MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
	                              AllocationProtect(mode), nullptr, 0);
	if (ptr == nullptr) {
		return false;
	}
	if (reinterpret_cast<uint64_t>(ptr) != address || !FinishAllocation(address, size, mode)) {
		VirtualFree(ptr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
		return false;
	}
	return true;
}

HANDLE CreateGuestSection(uint64_t size) {
#if defined(KYTY_PLATFORM_UWP)
	// Backed by the paging file, committed up front, as on the desktop. Execute access is for guest code in direct memory (it is mapped as a writable
	// view and an executable view, never read-write-execute).
	return CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT, size, nullptr);
#else
	return CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
	                          static_cast<DWORD>(size >> 32u),
	                          static_cast<DWORD>(size & 0xffffffffu), nullptr);
#endif
}

void* MapSectionAlias(HANDLE section, uint64_t size) {
#if defined(KYTY_PLATFORM_UWP)
	return MapViewOfFileFromApp(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, size);
#else
	return MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
#endif
}

void* MapPlaceholderView(HANDLE section, uint64_t address, uint64_t offset, uint64_t size,
                         Mode mode) {
#if defined(KYTY_PLATFORM_UWP)
	// A view's write or execute access is fixed when it is mapped. Data views map writable, so
	// write watches can make them read-only and writable again; code views map executable and
	// are written through the section alias.
	if (IsWriteExecute(mode)) {
		static std::atomic_bool warned = false;
		if (!warned.exchange(true)) {
			printf("Warning: guest direct memory mapped writable and executable is only writable "
			       "in the UWP app\n");
		}
	}
	const DWORD protect = IsExecute(mode) && !IsWriteExecute(mode) ? PAGE_EXECUTE_READ
	                                                               : PAGE_READWRITE;
	return MapViewOfFile3FromApp(section, nullptr, reinterpret_cast<void*>(address), offset, size,
	                             MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
#else
	const DWORD protect = mode == Mode::NoAccess ? PAGE_READWRITE : GetProtectionFlag(mode);
	return MapViewOfFile3(section, GetCurrentProcess(), reinterpret_cast<void*>(address), offset,
	                      size, MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
#endif
}

bool ReserveLazy(uint64_t address, uint64_t size) {
	auto* ptr = HostVirtualAlloc2(reinterpret_cast<void*>(address), size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
	if (ptr != nullptr && reinterpret_cast<uint64_t>(ptr) != address) {
		VirtualFree(ptr, 0, MEM_RELEASE);
		return false;
	}
	return ptr != nullptr;
}

bool CommitLazy(uint64_t address, uint64_t size) {
	return HostVirtualAlloc(reinterpret_cast<void*>(address), size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool DecommitLazy(uint64_t address, uint64_t size) {
	return VirtualFree(reinterpret_cast<void*>(address), size, MEM_DECOMMIT) != 0;
}

bool ProtectLazy(uint64_t address, uint64_t size, Mode mode) {
	// Not Protect(): lazy memory is never write-execute, and this runs on every page watcher change.
	return HostVirtualProtect(reinterpret_cast<void*>(address), size, GetProtectionFlag(mode));
}

bool HandleWriteExecuteFault(uint64_t address, bool execute) {
#if defined(KYTY_PLATFORM_UWP)
	constexpr uint64_t PageSize = 0x1000;
	auto&              ranges   = WriteExecute();
	std::lock_guard    lock(ranges.mutex);
	if (!ranges.Contains(address)) {
		return false;
	}
	auto* page = reinterpret_cast<void*>(address & ~(PageSize - 1u));
	if (!HostVirtualProtect(page, PageSize, execute ? PAGE_EXECUTE_READ : PAGE_READWRITE)) {
		return false;
	}
	if (execute) {
		::FlushInstructionCache(GetCurrentProcess(), page, PageSize);
	}
	return true;
#else
	(void)address;
	(void)execute;
	return false;
#endif
}

} // namespace Windows

} // namespace Common::VirtualMemory

#endif
