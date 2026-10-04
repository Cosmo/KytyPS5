#ifndef EMULATOR_SRC_UWP_GAMEIMAGES_H_
#define EMULATOR_SRC_UWP_GAMEIMAGES_H_

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <filesystem>

// A game's artwork from its sce_sys folder: icon0.png (the 512x512 cover) and pic0.png (the
// 3840x2160 background).
namespace Kyty::Uwp {

[[nodiscard]] std::filesystem::path CoverImage(const std::filesystem::path& game);
[[nodiscard]] std::filesystem::path BackdropImage(const std::filesystem::path& game);

// UI thread: the image file at `path` decoded `decode_width` pixels wide, or null when it can't
// be read. The file is read like the emulator reads games, so it needs no other access. Completes
// on the UI thread (`dispatcher`'s).
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage>
LoadImageAsync(winrt::Windows::UI::Core::CoreDispatcher dispatcher, std::filesystem::path path,
               int decode_width);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_GAMEIMAGES_H_
