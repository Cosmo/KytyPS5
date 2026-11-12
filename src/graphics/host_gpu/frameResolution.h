#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_FRAMERESOLUTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_FRAMERESOLUTION_H_

#include <atomic>
#include <cstdint>

namespace Libs::Graphics::FrameResolution {

// What a game renders at and outputs, per frame, for the app's overlay: the size of the picture it flips (output) and the largest viewport it
// draws its scene into (render: draws with color targets and a depth target, so neither shadow maps nor 2D passes), taken at each flip. A game
// that upscales by itself into a full-size target shows the full size as render. Display: the size the presenter scales that picture to on the screen.
struct Sizes {
	uint32_t output_width   = 0;
	uint32_t output_height  = 0;
	uint32_t render_width   = 0; // 0: the frame drew no scene
	uint32_t render_height  = 0;
	uint32_t display_width  = 0;
	uint32_t display_height = 0;
	bool     fsr            = false; // the picture was upscaled by FSR as the game's PSSR upscaler
};

namespace Detail {
inline std::atomic<uint64_t> g_render {0};      // the frame being drawn: width << 32 | height
inline std::atomic<uint64_t> g_last_output {0}; // the last flipped frame
inline std::atomic<uint64_t> g_last_render {0};
inline std::atomic<uint64_t> g_display {0};     // the presenter's swap chain
inline std::atomic<bool>     g_fsr1 {false};    // the last picture was scaled with FSR 1 (D3D12 presenter)
inline std::atomic<bool>     g_fsr {false};     // the frame being drawn was upscaled by FSR (PSSR)
inline std::atomic<bool>     g_last_fsr {false};

[[nodiscard]] inline uint64_t Pack(uint32_t width, uint32_t height) {
	return (static_cast<uint64_t>(width) << 32u) | height;
}
[[nodiscard]] inline uint64_t Area(uint64_t packed) {
	return (packed >> 32u) * (packed & 0xffffffffu);
}
} // namespace Detail

// A draw into color targets with a depth target, with its viewport (the renderer).
inline void NoteRender(uint32_t width, uint32_t height) {
	const uint64_t size    = Detail::Pack(width, height);
	uint64_t       current = Detail::g_render.load(std::memory_order_relaxed);
	while (Detail::Area(size) > Detail::Area(current) &&
	       !Detail::g_render.compare_exchange_weak(current, size, std::memory_order_relaxed)) {
	}
}

// A flip of the game's picture (the presenter): the frame drawn since the last flip is done.
inline void NoteOutput(uint32_t width, uint32_t height) {
	Detail::g_last_output.store(Detail::Pack(width, height), std::memory_order_relaxed);
	Detail::g_last_render.store(Detail::g_render.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
	Detail::g_last_fsr.store(Detail::g_fsr.exchange(false, std::memory_order_relaxed), std::memory_order_relaxed);
}

// The game's PSSR request was upscaled by FSR (the D3D12 renderer).
inline void NoteFsrUpscale() {
	Detail::g_fsr.store(true, std::memory_order_relaxed);
}

// The presenter's picture on the screen got this size (its swap chain).
inline void NoteDisplay(uint32_t width, uint32_t height) {
	Detail::g_display.store(Detail::Pack(width, height), std::memory_order_relaxed);
}

// Whether the presenter scaled the game's last picture with FSR 1 (only when it is on and the picture is smaller than its place on the screen).
inline void NoteFsr1(bool used) {
	Detail::g_fsr1.store(used, std::memory_order_relaxed);
}
[[nodiscard]] inline bool Fsr1Used() {
	return Detail::g_fsr1.load(std::memory_order_relaxed);
}

[[nodiscard]] inline Sizes Last() {
	const uint64_t output  = Detail::g_last_output.load(std::memory_order_relaxed);
	const uint64_t render  = Detail::g_last_render.load(std::memory_order_relaxed);
	const uint64_t display = Detail::g_display.load(std::memory_order_relaxed);
	return {static_cast<uint32_t>(output >> 32u),  static_cast<uint32_t>(output),
	        static_cast<uint32_t>(render >> 32u),  static_cast<uint32_t>(render),
	        static_cast<uint32_t>(display >> 32u), static_cast<uint32_t>(display),
	        Detail::g_last_fsr.load(std::memory_order_relaxed)};
}

} // namespace Libs::Graphics::FrameResolution

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_FRAMERESOLUTION_H_
