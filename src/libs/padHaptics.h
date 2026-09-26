#ifndef EMULATOR_INCLUDE_EMULATOR_PAD_HAPTICS_H_
#define EMULATOR_INCLUDE_EMULATOR_PAD_HAPTICS_H_

#include <cstdint>

// DualSense audio haptics: games drive the controller's two actuators with a vibration audio port.
// Over USB the DualSense is a 4-channel audio device whose back channels feed the actuators.
namespace Libs::Audio::PadHaptics {

struct Stream;

// nullptr for a zero `freq` or without SDL audio. The controller may be connected later.
[[nodiscard]] Stream* Open(uint32_t freq);
void                  Close(Stream* stream);
// The first two channels drive the left and right actuators; a mono port drives both.
void Queue(Stream* stream, const void* data, uint32_t frames, uint32_t channels, bool is_float,
           const int* volume);

// Exposed for tests.
float ToControllerFrames(const void* data, uint32_t frames, uint32_t channels, bool is_float,
                         const int* volume, float* out);
[[nodiscard]] bool  IsControllerName(const char* name);
[[nodiscard]] float PlaybackRatio(int queued_bytes, int target_bytes);

} // namespace Libs::Audio::PadHaptics

#endif // EMULATOR_INCLUDE_EMULATOR_PAD_HAPTICS_H_
