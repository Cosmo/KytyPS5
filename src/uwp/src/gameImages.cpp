#include "gameImages.h"

#include "gameSource.h"

#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#include "bcdec.h"

#include <cstring>
#include <cwchar>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace Kyty::Uwp {

std::filesystem::path CoverImage(const std::filesystem::path& game) {
	return game / L"sce_sys" / L"icon0.png";
}

std::filesystem::path BackdropImage(const std::filesystem::path& game) {
	return game / L"sce_sys" / L"pic0.png";
}

std::array<std::filesystem::path, 2> SplashImages(const std::filesystem::path& game, uint32_t console_language) {
	std::array<std::filesystem::path, 2> paths;
	for (size_t i = 0; i < paths.size(); i++) {
		wchar_t name[32];
		std::swprintf(name, std::size(name), L"pic%zu_%02u.dds", i + 1, console_language);
		paths[i] = game / L"sce_sys" / name;
		if (!GameFileExists(paths[i])) {
			std::swprintf(name, std::size(name), L"pic%zu.dds", i + 1);
			paths[i] = game / L"sce_sys" / name;
		}
	}
	return paths;
}

namespace {

// A DDS file's blocks decoded and halved in each direction (2x2 pixels averaged, with premultiplied alpha): BGRA with premultiplied alpha, as XAML
// takes it. Null for another format or a short file.
struct HalfImage {
	uint32_t             width  = 0;
	uint32_t             height = 0;
	std::vector<uint8_t> bgra;
};

std::optional<HalfImage> DecodeDdsHalf(const std::vector<uint8_t>& file) {
	constexpr uint32_t HEADER = 4 + 124;
	if (file.size() < HEADER || std::memcmp(file.data(), "DDS ", 4) != 0) {
		return std::nullopt;
	}
	const auto read32 = [&](size_t offset) {
		uint32_t value = 0;
		std::memcpy(&value, file.data() + offset, sizeof(value));
		return value;
	};
	const uint32_t height = read32(12);
	const uint32_t width  = read32(16);
	const uint32_t fourcc = read32(84);
	size_t         data   = HEADER;
	uint32_t       format = 0; // DXGI_FORMAT
	if (fourcc == 0x30315844u) { // DX10: the header extension names the DXGI format
		if (file.size() < HEADER + 20) {
			return std::nullopt;
		}
		format = read32(HEADER);
		data += 20;
	} else if (fourcc == 0x31545844u) { // DXT1
		format = 71;
	} else if (fourcc == 0x35545844u) { // DXT5
		format = 77;
	}
	void (*decode_block)(const void*, void*, int) = nullptr;
	size_t block_size                             = 0;
	switch (format) {
		case 71: // BC1, UNORM and sRGB
		case 72: decode_block = bcdec_bc1, block_size = BCDEC_BC1_BLOCK_SIZE; break;
		case 77: // BC3
		case 78: decode_block = bcdec_bc3, block_size = BCDEC_BC3_BLOCK_SIZE; break;
		case 98: // BC7
		case 99: decode_block = bcdec_bc7, block_size = BCDEC_BC7_BLOCK_SIZE; break;
		default: return std::nullopt;
	}
	const uint32_t blocks_x = width / 4;
	const uint32_t blocks_y = height / 4;
	if (blocks_x == 0 || blocks_y == 0 || file.size() < data + size_t(blocks_x) * blocks_y * block_size) {
		return std::nullopt;
	}
	HalfImage image;
	image.width  = blocks_x * 2;
	image.height = blocks_y * 2;
	image.bgra.resize(size_t(image.width) * image.height * 4);
	const uint8_t* block = file.data() + data;
	uint8_t        rgba[4 * 4 * 4];
	for (uint32_t by = 0; by < blocks_y; by++) {
		for (uint32_t bx = 0; bx < blocks_x; bx++, block += block_size) {
			decode_block(block, rgba, 4 * 4);
			for (uint32_t y = 0; y < 2; y++) {
				for (uint32_t x = 0; x < 2; x++) {
					uint32_t sum[4] {};
					for (uint32_t sy = 0; sy < 2; sy++) {
						for (uint32_t sx = 0; sx < 2; sx++) {
							const uint8_t* p = rgba + ((y * 2 + sy) * 4 + x * 2 + sx) * 4;
							sum[0] += p[2] * p[3]; // B, premultiplied
							sum[1] += p[1] * p[3]; // G
							sum[2] += p[0] * p[3]; // R
							sum[3] += p[3];
						}
					}
					uint8_t* out = image.bgra.data() + ((size_t(by) * 2 + y) * image.width + bx * 2 + x) * 4;
					for (int c = 0; c < 3; c++) {
						out[c] = static_cast<uint8_t>((sum[c] + 2 * 255) / (4 * 255));
					}
					out[3] = static_cast<uint8_t>((sum[3] + 2) / 4);
				}
			}
		}
	}
	return image;
}

} // namespace

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

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Media::Imaging::SoftwareBitmapSource>
LoadDdsImageAsync(winrt::Windows::UI::Core::CoreDispatcher dispatcher, std::filesystem::path path) {
	namespace imaging = winrt::Windows::Graphics::Imaging;

	co_await winrt::resume_background();
	imaging::SoftwareBitmap bitmap {nullptr};
	if (const auto bytes = ReadGameBinary(path)) {
		if (const auto image = DecodeDdsHalf(*bytes)) {
			winrt::Windows::Storage::Streams::Buffer buffer(static_cast<uint32_t>(image->bgra.size()));
			std::memcpy(buffer.data(), image->bgra.data(), image->bgra.size());
			buffer.Length(static_cast<uint32_t>(image->bgra.size()));
			bitmap = imaging::SoftwareBitmap(imaging::BitmapPixelFormat::Bgra8, static_cast<int32_t>(image->width), static_cast<int32_t>(image->height),
			                                 imaging::BitmapAlphaMode::Premultiplied);
			bitmap.CopyFromBuffer(buffer);
		}
	}
	co_await winrt::resume_foreground(dispatcher);
	if (bitmap == nullptr) {
		co_return nullptr;
	}
	winrt::Windows::UI::Xaml::Media::Imaging::SoftwareBitmapSource source;
	co_await source.SetBitmapAsync(bitmap);
	co_return source;
}

} // namespace Kyty::Uwp
