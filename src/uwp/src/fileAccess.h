#ifndef EMULATOR_SRC_UWP_FILEACCESS_H_
#define EMULATOR_SRC_UWP_FILEACCESS_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Small file helpers for the app's own code, on the *FromApp functions like the emulator's file
// layer (common/platform/sysWindowsFileIO.cpp).
namespace Kyty::Uwp {

[[nodiscard]] bool                       IsFile(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string> ReadTextFile(const std::filesystem::path& path);
// A file of up to 32 MB (a game's images), or nothing when it can't be read.
[[nodiscard]] std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path);
// Why the app may not list `folder` (a Windows error, also logged), or 0 when it may: it exists
// and grants the app read access.
[[nodiscard]] uint32_t ListError(const std::filesystem::path& folder);
// Waits (up to 2 seconds) while game folders deny access: the Xbox gives a starting app access
// to its USB drives a few hundred milliseconds late.
void WaitForGameFolders();
// Whether the app runs on an Xbox, where game folders are on a drive set up on a PC.
[[nodiscard]] bool IsXbox();
// The command that gives UWP apps (ALL APPLICATION PACKAGES) read access to `folder`. On the
// Xbox it is for a PC with the drive plugged in, where the drive's letter is unknown: X:.
[[nodiscard]] std::wstring GrantCommand(std::filesystem::path folder);
// The subfolders of `folder`; empty when it can't be listed.
[[nodiscard]] std::vector<std::filesystem::path> Subfolders(const std::filesystem::path& folder);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_FILEACCESS_H_
