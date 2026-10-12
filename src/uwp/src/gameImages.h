#ifndef EMULATOR_SRC_UWP_GAMEIMAGES_H_
#define EMULATOR_SRC_UWP_GAMEIMAGES_H_

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <array>
#include <cstdint>
#include <filesystem>

// A game's artwork from its sce_sys folder: icon0.png (the 512x512 cover) and pic0.png (the
// 3840x2160 background).
namespace Kyty::Uwp {

[[nodiscard]] std::filesystem::path CoverImage(const std::filesystem::path& game);
[[nodiscard]] std::filesystem::path BackdropImage(const std::filesystem::path& game);
// The start screen's images, as the PS5 shows them while a game starts (its splash screen): pic1.dds, the background, and pic2.dds, the logo over
// it, in the console language where the game has them (pic1_NN.dds). Any thread.
[[nodiscard]] std::array<std::filesystem::path, 2> SplashImages(const std::filesystem::path& game, uint32_t console_language);

// UI thread: the image file at `path` decoded `decode_width` pixels wide, or null when it can't
// be read. The file is read like the emulator reads games, so it needs no other access. Completes
// on the UI thread (`dispatcher`'s).
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage>
LoadImageAsync(winrt::Windows::UI::Core::CoreDispatcher dispatcher, std::filesystem::path path,
               int decode_width);

// UI thread: the DDS image at `path` (BC7, BC3 or BC1, as the start screen's are) decoded at half its size, or null when it can't be read or has
// another format. Completes on the UI thread (`dispatcher`'s).
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Media::Imaging::SoftwareBitmapSource>
LoadDdsImageAsync(winrt::Windows::UI::Core::CoreDispatcher dispatcher, std::filesystem::path path);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_GAMEIMAGES_H_
