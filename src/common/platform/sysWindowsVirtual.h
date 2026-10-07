#ifndef KYTY_COMMON_PLATFORM_SYSWINDOWSVIRTUAL_H_
#define KYTY_COMMON_PLATFORM_SYSWINDOWSVIRTUAL_H_

#include "common/common.h"
#include "common/virtualMemory.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

using HANDLE = void*;

// Windows placeholder and section-view operations for the guest address space
// (kernel/memoryAddressSpace.inc).
//
// Desktop builds use the regular APIs. The UWP app (KYTY_PLATFORM_UWP) uses the *FromApp APIs of
// the app container, which never give memory write and execute access at once:
// - memory asked to be writable and executable is writable; it becomes executable on its first
//   execute fault and writable again on a write fault (HandleWriteExecuteFault);
// - a view's write or execute access is fixed when it is mapped;
namespace Common::VirtualMemory::Windows {

// Reserves [address, address + size) as a placeholder.
[[nodiscard]] bool ReservePlaceholder(uint64_t address, uint64_t size);
// Commits private memory in place of the placeholder [address, address + size).
[[nodiscard]] bool CommitPlaceholder(uint64_t address, uint64_t size, Mode mode);

// The section backing guest direct memory; nullptr on failure.
[[nodiscard]] HANDLE CreateGuestSection(uint64_t size);
// Maps the whole section writable at a system-chosen address.
[[nodiscard]] void* MapSectionAlias(HANDLE section, uint64_t size);
// Maps [offset, offset + size) of the section in place of the placeholder at address.
[[nodiscard]] void* MapPlaceholderView(HANDLE section, uint64_t address, uint64_t offset,
                                       uint64_t size, Mode mode);

// Called first for every access violation: makes a page that is meant to be writable and
// executable accessible for the faulting access. False when the fault is not such a page.
[[nodiscard]] bool HandleWriteExecuteFault(uint64_t address, bool execute);

} // namespace Common::VirtualMemory::Windows

#endif

#endif /* KYTY_COMMON_PLATFORM_SYSWINDOWSVIRTUAL_H_ */
