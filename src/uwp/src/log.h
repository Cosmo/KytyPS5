#ifndef EMULATOR_SRC_UWP_LOG_H_
#define EMULATOR_SRC_UWP_LOG_H_

namespace Kyty::Uwp {

// Appends a line to kyty-uwp.txt (and the debugger output), in the logs and dumps folder chosen when the app started, else in LocalState.
// UWP apps have no console; uwp.ps1 prints this file.
void Log(const char* format, ...);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_LOG_H_
