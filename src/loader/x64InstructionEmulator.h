#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstddef>
#include <cstdint>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
// Makes the faulting store itself: works out what it writes, hands that to `write` (address, bytes, size), sets the flags and resumes
// after it. Plain stores of a general, XMM or immediate operand, and INC, DEC, ADD, SUB, AND, OR, XOR on memory; false for others. (The
// UWP app's write-or-execute pages: an instruction that writes into the page it runs from could otherwise never finish.)
using StoreWriter = bool (*)(uint64_t address, const void* data, size_t size);
[[nodiscard]] bool EmulateStore(void* native_context, StoreWriter write);
// Completes a faulting load into EAX as a zero read and resumes after it.
void SkipLoad(void* native_context, size_t length);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
