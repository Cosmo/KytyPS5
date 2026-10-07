#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_FRAMEDUMP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_FRAMEDUMP_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Debugging: the render targets of one frame as PNG files (the game menu's "Save frame dump"). The menu asks, the renderer takes the request at the next flip, and the result
// (or the failure) is a line of text for the menu.
namespace Libs::Graphics::FrameDump {

// A request records the draw log of one whole frame (the draws of the frame after the next flip, up to the flip after it) and then asks for the render targets of that moment.
void                      Request();
// The renderer calls this at every flip (the end of a frame).
void                      OnFrameEnd();
// The renderer asks, at a safe moment, whether a dump is due: true once per request, after the recorded frame ended. `max_wait_ms` > 0 also takes a request that has waited that long without a frame
// ending (the renderer's fallback when no flip comes): its draw log is then partial.
[[nodiscard]] bool        TakeRequest(uint32_t max_wait_ms = 0);

// The draw log: one text entry per draw or dispatch of the recorded frame (and per draw that was skipped, with the reason), for finding where the rendering of two devices differs.
[[nodiscard]] bool        LogActive() noexcept; // true while a frame is being recorded (cheap)
void                      LogDraw(const std::string& text, bool skipped = false);
void                      LogDispatch(const std::string& text);
void                      LogEvent(const std::string& text);
// Images that compute dispatches wrote in the recorded frame (guest addresses): the dump saves them as well as the render targets.
void                      NoteWrittenImage(uint64_t guest_address);
[[nodiscard]] bool        IsWrittenImage(uint64_t guest_address);
// The text of the draw log with a header; call it right after TakeRequest returned true.
[[nodiscard]] std::string TakeLog();
void                      SetStatus(const std::string& text);
[[nodiscard]] std::string GetStatus();

// The layouts of the pixels that a dump can show.
enum class PixelKind : uint8_t {
	Unsupported,
	Rgba8,
	Bgra8,
	R8,
	Rg8,
	Rgba16Unorm,
	Rgba16Float,
	Rg16Float,
	R16Float,
	R32Float,
	Rg32Float,
	Rgba32Float,
	R11G11B10Float,
	A2B10G10R10,
};

struct Decoded {
	uint32_t             width  = 0; // of the picture (at most 1280: a larger target is sampled)
	uint32_t             height = 0;
	std::vector<uint8_t> rgba;       // 8 bits per channel; floats are clamped to 0..1, alpha is 255
	std::string          stats;      // per channel: minimum, maximum and mean of the target itself (all pixels)
};

// `rows` are tight rows of `row_bytes` bytes.
// `stats_step` > 1 takes the statistics from every stats_step-th texel in both directions only (the picture then is as sparse).
[[nodiscard]] bool Decode(PixelKind kind, const uint8_t* rows, uint32_t width, uint32_t height, uint32_t row_bytes, bool srgb_encoded, Decoded& out, uint32_t stats_step = 1);
[[nodiscard]] bool WritePng(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint8_t* rgba);

} // namespace Libs::Graphics::FrameDump

#endif
