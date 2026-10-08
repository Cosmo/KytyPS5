#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_INPUT_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_INPUT_H_

#include <cstdint>

// The audio input of the UWP app (implemented in src/uwp/src/audioInput.cpp), which has no SDL audio: the microphone of a headset plugged into
// the controller, as the DualSense's microphone. The desktop builds use SDL's recording devices instead.
namespace Libs::Audio::Input {

class Stream;

// Interleaved 16-bit or float samples with 1 or 2 channels. The stream exists without a microphone as well: it captures whenever the app has one
// (input.h, ControllerMicrophoneDevice) and follows it when the headset is plugged in or out.
Stream*  Open(uint32_t freq, uint32_t channels, bool is_float);
void     Close(Stream* stream);
bool     Present(Stream* stream);   // capturing from a microphone now
uint32_t Available(Stream* stream); // the bytes captured and not read yet
uint32_t Read(Stream* stream, void* dest, uint32_t size); // up to `size` bytes, whole frames; the bytes read
void     Clear(Stream* stream);

} // namespace Libs::Audio::Input

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_INPUT_H_
