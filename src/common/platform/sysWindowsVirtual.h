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

// Lazily committed guest memory (T3, src/xbox/lazyRegions.h). Never executable.
// Replaces the placeholder [address, address + size) by reserved, not committed private memory.
[[nodiscard]] bool ReserveLazy(uint64_t address, uint64_t size);
// Commits read-write pages inside such a reservation (fresh pages are zero).
[[nodiscard]] bool CommitLazy(uint64_t address, uint64_t size);
// Returns committed pages of such a reservation to the reserved state.
[[nodiscard]] bool DecommitLazy(uint64_t address, uint64_t size);
// Read-only, no-access or read-write for committed pages of such a reservation.
[[nodiscard]] bool ProtectLazy(uint64_t address, uint64_t size, Mode mode);

// Memory of the emulator itself, committed at an address the OS chooses (Alloc(0, ...) places memory in the guest's address
// ranges, which the guest may have taken all of). Free it with Free; Protect makes it executable.
[[nodiscard]] uint64_t AllocHost(uint64_t size, Mode mode);

// Called first for every access violation: makes a page that is meant to be writable and
// executable accessible for the faulting access. False when the fault is not such a page.
// `native_context` (the faulting thread's CONTEXT): an instruction that writes into the page it runs from is made by the store
// emulator (SetStoreEmulator), as switching that page could never let it finish.
[[nodiscard]] bool HandleWriteExecuteFault(uint64_t address, bool execute, void* native_context = nullptr);

// Makes a faulting store itself and resumes after it (Loader::X64InstructionEmulator::EmulateStore), writing through `write`.
using StoreWriter   = bool (*)(uint64_t address, const void* data, size_t size);
using StoreEmulator = bool (*)(void* native_context, StoreWriter write);
void SetStoreEmulator(StoreEmulator emulator);

// Makes the pages of [address, address + size) that are meant to be writable and executable executable now, instead of on their first execute
// fault (a write still makes a page writable again). Code that runs in the middle of a guest function, like the red-zone trampolines, must not
// fault on its first execution: the exception frame would land on the guest's red zone. Does nothing on the desktop.
bool MakeWriteExecuteExecutable(uint64_t address, uint64_t size);

} // namespace Common::VirtualMemory::Windows

#endif

#endif /* KYTY_COMMON_PLATFORM_SYSWINDOWSVIRTUAL_H_ */
