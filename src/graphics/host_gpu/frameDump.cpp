#include "graphics/host_gpu/frameDump.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include "common/logging/log.h"
#include <limits>
#include <mutex>
#include <set>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace Libs::Graphics::FrameDump {

namespace {

std::mutex       g_status_mutex;
std::string      g_status;

// The draw log's state. A request waits for a frame to begin (a flip), records the frame up to the next flip, and is then due.
enum class Phase { Idle, Recording, Captured };
std::mutex                                  g_log_mutex;
Phase                                       g_phase = Phase::Idle;
std::atomic_bool                            g_recording {false};
std::chrono::steady_clock::time_point       g_requested_at;
uint32_t                                    g_flips = 0;   // flips since the request
uint32_t                                    g_entries = 0; // numbered log entries of the recorded frame
uint32_t                                    g_draws = 0, g_skipped = 0, g_dispatches = 0;
bool                                        g_partial = false, g_truncated = false;
std::string                                 g_log;
std::set<uint64_t>                          g_written_images;
constexpr size_t                            LogLimit = 24u << 20u;

void AppendEntry(const char* kind, const std::string& text) {
	std::lock_guard lock(g_log_mutex);
	if (!g_recording.load()) {
		return;
	}
	if (g_log.size() > LogLimit) {
		g_truncated = true;
		return;
	}
	g_log += fmt::format("#{} {}{}", ++g_entries, kind, text);
	if (g_log.back() != '\n') {
		g_log += '\n';
	}
}

float HalfToFloat(uint16_t h) {
	const uint32_t sign     = (h >> 15u) & 1u;
	const uint32_t exponent = (h >> 10u) & 0x1Fu;
	const uint32_t mantissa = h & 0x3FFu;
	float          value;
	if (exponent == 0) {
		value = std::ldexp(static_cast<float>(mantissa), -24);
	} else if (exponent == 31) {
		value = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
	} else {
		value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
	}
	return sign != 0 ? -value : value;
}

// An unsigned small float of the 11 and 10 bit formats: exponent of 5 bits, mantissa of `mantissa_bits`.
float SmallFloat(uint32_t bits, uint32_t mantissa_bits) {
	const uint32_t mantissa = bits & ((1u << mantissa_bits) - 1u);
	const uint32_t exponent = bits >> mantissa_bits;
	if (exponent == 0) {
		return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
	}
	if (exponent == 31) {
		return mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
	}
	return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits), static_cast<int>(exponent) - 15);
}

float SrgbToLinear(float c) {
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint8_t ToByte(float value) {
	if (!(value > 0.0f)) { // negative and NaN
		return 0;
	}
	return value >= 1.0f ? 255 : static_cast<uint8_t>(value * 255.0f + 0.5f);
}

struct ChannelStats {
	double min = std::numeric_limits<double>::infinity();
	double max = -std::numeric_limits<double>::infinity();
	double sum = 0.0;
	uint64_t nan = 0;
	void Add(float v) {
		if (std::isnan(v)) {
			++nan;
			return;
		}
		min = std::min(min, static_cast<double>(v));
		max = std::max(max, static_cast<double>(v));
		sum += v;
	}
};

} // namespace

void Request() {
	{
		std::lock_guard lock(g_log_mutex);
		g_phase        = Phase::Recording;
		g_requested_at = std::chrono::steady_clock::now();
		g_flips = g_entries = g_draws = g_skipped = g_dispatches = 0;
		g_partial = g_truncated = false;
		g_log.clear();
		g_written_images.clear();
		// (Recording starts at once: when no flip comes, what was recorded since the request is the partial log. The first flip drops it and starts the whole frame.)
		g_recording.store(true);
	}
	SetStatus("Waiting for the end of the frame...");
	Log::WriteToConsoleAndLog("Frame dump: requested\n");
}

void OnFrameEnd() {
	std::lock_guard lock(g_log_mutex);
	if (g_phase != Phase::Recording) {
		return;
	}
	if (++g_flips == 1) {
		// a frame begins here: what came before is the end of the previous one
		g_log.clear();
		g_written_images.clear();
		g_entries = g_draws = g_skipped = g_dispatches = 0;
		g_truncated = false;
	} else {
		g_phase = Phase::Captured;
		g_recording.store(false);
	}
}

bool TakeRequest(uint32_t max_wait_ms) {
	std::lock_guard lock(g_log_mutex);
	if (g_phase == Phase::Idle) {
		return false;
	}
	if (g_phase == Phase::Recording) {
		const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_requested_at).count();
		if (max_wait_ms == 0 || waited < static_cast<long long>(max_wait_ms)) {
			return false;
		}
		g_partial = true;
		g_recording.store(false);
	}
	g_phase = Phase::Idle;
	return true;
}

bool LogActive() noexcept {
	return g_recording.load(std::memory_order_relaxed);
}

void LogDraw(const std::string& text, bool skipped) {
	AppendEntry(skipped ? "SKIPPED " : "draw ", text);
	std::lock_guard lock(g_log_mutex);
	if (g_recording.load()) {
		(skipped ? g_skipped : g_draws)++;
	}
}

void LogDispatch(const std::string& text) {
	AppendEntry("dispatch ", text);
	std::lock_guard lock(g_log_mutex);
	if (g_recording.load()) {
		g_dispatches++;
	}
}

void LogEvent(const std::string& text) {
	AppendEntry("", text);
}

void NoteWrittenImage(uint64_t guest_address) {
	std::lock_guard lock(g_log_mutex);
	if (g_recording.load()) {
		g_written_images.insert(guest_address);
	}
}

bool IsWrittenImage(uint64_t guest_address) {
	std::lock_guard lock(g_log_mutex);
	return g_written_images.contains(guest_address);
}

std::string TakeLog() {
	std::lock_guard lock(g_log_mutex);
	std::string     text = "# Draw log of one frame (the guest's draws and dispatches in the order the renderer got them).\n";
	text += g_partial ? fmt::format("# PARTIAL: no frame ended after the request ({} flips seen), so this is everything since the request.\n", g_flips)
	                  : "# One whole frame: from one flip to the next.\n";
	text += fmt::format("# draws: {}, skipped draws: {}, dispatches: {}{}\n", g_draws, g_skipped, g_dispatches, g_truncated ? ", the log was cut at its size limit" : "");
	text += "# draw lines: guest primitive/host topology, shader hashes (guest code), host pipeline, targets (guest address, size, format), blend, depth, and the textures each stage reads.\n";
	text += g_log;
	g_log.clear();
	return text;
}

void SetStatus(const std::string& text) {
	std::lock_guard lock(g_status_mutex);
	g_status = text;
}

std::string GetStatus() {
	std::lock_guard lock(g_status_mutex);
	return g_status;
}

bool Decode(PixelKind kind, const uint8_t* rows, uint32_t width, uint32_t height, uint32_t row_bytes, bool srgb_encoded, Decoded& out, uint32_t stats_step) {
	(void)srgb_encoded; // 8-bit color formats are shown as stored
	uint32_t channels = 0, bytes = 0;
	switch (kind) {
		case PixelKind::Rgba8:
		case PixelKind::Bgra8:
		case PixelKind::A2B10G10R10:
		case PixelKind::R11G11B10Float: channels = 4; bytes = 4; break;
		case PixelKind::R8: channels = 1; bytes = 1; break;
		case PixelKind::Rg8: channels = 2; bytes = 2; break;
		case PixelKind::Rgba16Unorm:
		case PixelKind::Rgba16Float: channels = 4; bytes = 8; break;
		case PixelKind::Rg16Float: channels = 2; bytes = 4; break;
		case PixelKind::R16Float: channels = 1; bytes = 2; break;
		case PixelKind::R32Float: channels = 1; bytes = 4; break;
		case PixelKind::Rg32Float: channels = 2; bytes = 8; break;
		case PixelKind::Rgba32Float: channels = 4; bytes = 16; break;
		case PixelKind::Unsupported: return false;
	}
	if (width == 0 || height == 0 || row_bytes < width * bytes) {
		return false;
	}
	// A picture of at most 1280 texels wide: every step-th texel, in both directions
	const uint32_t step = std::max<uint32_t>(1, (width + 1279u) / 1280u);
	out.width           = (width + step - 1) / step;
	out.height          = (height + step - 1) / step;
	out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4u, 255);
	ChannelStats stats[4];
	const auto   read_pixel = [&](const uint8_t* p, float v[4]) {
        v[0] = v[1] = v[2] = 0.0f;
        v[3] = 1.0f;
        switch (kind) {
            case PixelKind::Rgba8: for (int i = 0; i < 4; ++i) v[i] = static_cast<float>(p[i]) / 255.0f; break;
            case PixelKind::Bgra8:
                v[0] = static_cast<float>(p[2]) / 255.0f; v[1] = static_cast<float>(p[1]) / 255.0f; v[2] = static_cast<float>(p[0]) / 255.0f; v[3] = static_cast<float>(p[3]) / 255.0f;
                break;
            case PixelKind::R8: v[0] = static_cast<float>(p[0]) / 255.0f; break;
            case PixelKind::Rg8: v[0] = static_cast<float>(p[0]) / 255.0f; v[1] = static_cast<float>(p[1]) / 255.0f; break;
            case PixelKind::Rgba16Unorm: for (int i = 0; i < 4; ++i) { uint16_t s; std::memcpy(&s, p + i * 2, 2); v[i] = static_cast<float>(s) / 65535.0f; } break;
            case PixelKind::Rgba16Float: for (int i = 0; i < 4; ++i) { uint16_t s; std::memcpy(&s, p + i * 2, 2); v[i] = HalfToFloat(s); } break;
            case PixelKind::Rg16Float: for (int i = 0; i < 2; ++i) { uint16_t s; std::memcpy(&s, p + i * 2, 2); v[i] = HalfToFloat(s); } break;
            case PixelKind::R16Float: { uint16_t s; std::memcpy(&s, p, 2); v[0] = HalfToFloat(s); break; }
            case PixelKind::R32Float: std::memcpy(&v[0], p, 4); break;
            case PixelKind::Rg32Float: std::memcpy(&v[0], p, 4); std::memcpy(&v[1], p + 4, 4); break;
            case PixelKind::Rgba32Float: std::memcpy(v, p, 16); break;
            case PixelKind::R11G11B10Float: {
                uint32_t w; std::memcpy(&w, p, 4);
                v[0] = SmallFloat(w & 0x7FFu, 6); v[1] = SmallFloat((w >> 11u) & 0x7FFu, 6); v[2] = SmallFloat((w >> 22u) & 0x3FFu, 5);
                break;
            }
            case PixelKind::A2B10G10R10: {
                uint32_t w; std::memcpy(&w, p, 4);
                v[0] = static_cast<float>(w & 0x3FFu) / 1023.0f; v[1] = static_cast<float>((w >> 10u) & 0x3FFu) / 1023.0f;
                v[2] = static_cast<float>((w >> 20u) & 0x3FFu) / 1023.0f; v[3] = static_cast<float>(w >> 30u) / 3.0f;
                break;
            }
            case PixelKind::Unsupported: break;
        }
	};
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* row = rows + static_cast<size_t>(y) * row_bytes;
		for (uint32_t x = 0; x < width; ++x) {
			if (stats_step > 1 && (x % stats_step != 0 || y % stats_step != 0)) {
				continue;
			}
			float v[4];
			read_pixel(row + static_cast<size_t>(x) * bytes, v);
			for (uint32_t c = 0; c < channels; ++c) {
				stats[c].Add(v[c]);
			}
			if (y % step == 0 && x % step == 0) {
				uint8_t* pixel = &out.rgba[(static_cast<size_t>(y / step) * out.width + x / step) * 4u];
				if (channels == 1) { // one channel is shown as grey
					pixel[0] = pixel[1] = pixel[2] = ToByte(v[0]);
				} else {
					pixel[0] = ToByte(v[0]);
					pixel[1] = ToByte(v[1]);
					pixel[2] = channels >= 3 ? ToByte(v[2]) : 0;
				}
			}
		}
	}
	static constexpr const char* names[4] = {"r", "g", "b", "a"};
	for (uint32_t c = 0; c < channels; ++c) {
		const double counted = static_cast<double>((width + stats_step - 1) / stats_step) * ((height + stats_step - 1) / stats_step);
		const double mean = stats[c].sum / std::max<double>(1.0, (stats_step > 1 ? counted : static_cast<double>(width) * height) - static_cast<double>(stats[c].nan));
		out.stats += fmt::format("{}[{:.4g}..{:.4g} mean {:.4g}{}] ", names[c], stats[c].min, stats[c].max, mean, stats[c].nan != 0 ? fmt::format(" nan {}", stats[c].nan) : std::string {});
	}
	return true;
}

bool WritePng(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint8_t* rgba) {
	std::ofstream stream(file, std::ios::binary | std::ios::trunc);
	if (!stream) {
		return false;
	}
	const auto write = [](void* context, void* data, int size) {
		static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size);
	};
	const int ok = stbi_write_png_to_func(write, &stream, static_cast<int>(width), static_cast<int>(height), 4, rgba, static_cast<int>(width) * 4);
	stream.flush();
	return ok != 0 && static_cast<bool>(stream);
}

} // namespace Libs::Graphics::FrameDump
