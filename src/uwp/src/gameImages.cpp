#include "gameImages.h"

#include "gameSource.h"

#include <winrt/Windows.Storage.Streams.h>

#include <utility>

namespace Kyty::Uwp {

std::filesystem::path CoverImage(const std::filesystem::path& game) {
	return game / L"sce_sys" / L"icon0.png";
}

std::filesystem::path BackdropImage(const std::filesystem::path& game) {
	return game / L"sce_sys" / L"pic0.png";
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage>
LoadImageAsync(winrt::Windows::UI::Core::CoreDispatcher dispatcher, std::filesystem::path path,
               int decode_width) {
	namespace streams = winrt::Windows::Storage::Streams;

	co_await winrt::resume_background();
	const auto bytes = ReadGameBinary(path);
	co_await winrt::resume_foreground(dispatcher);
	if (!bytes || bytes->empty()) {
		co_return nullptr;
	}

	streams::InMemoryRandomAccessStream stream;
	streams::DataWriter                 writer(stream);
	writer.WriteBytes(*bytes);
	co_await writer.StoreAsync();
	writer.DetachStream();
	stream.Seek(0);

	winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage bitmap;
	bitmap.DecodePixelWidth(decode_width);
	try {
		co_await bitmap.SetSourceAsync(stream);
	} catch (const winrt::hresult_error&) {
		co_return nullptr; // not a readable PNG
	}
	co_return bitmap;
}

} // namespace Kyty::Uwp
