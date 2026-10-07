#ifndef EMULATOR_SRC_UWP_GAMESOURCE_H_
#define EMULATOR_SRC_UWP_GAMESOURCE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// A game is a folder (with an eboot.bin) or a .zar archive (a compressed dump the emulator reads in place). These helpers read the files of
// either: a path below a game folder, or a path inside an archive (<archive>.zar\sce_sys\param.json).
namespace Kyty::Uwp {

// A .zar file (by its name).
[[nodiscard]] bool IsArchivePath(const std::filesystem::path& path);

// Whether the file exists.
[[nodiscard]] bool GameFileExists(const std::filesystem::path& path);
// The file, up to 32 MB, or nothing when it can't be read.
[[nodiscard]] std::optional<std::vector<uint8_t>> ReadGameBinary(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string>          ReadGameText(const std::filesystem::path& path);

// The .zar files directly in `folder`.
[[nodiscard]] std::vector<std::filesystem::path> ArchivesIn(const std::filesystem::path& folder);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_GAMESOURCE_H_
