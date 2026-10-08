#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_OUTPUT_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_OUTPUT_H_

#include <cstdint>

// The audio output of the UWP app (implemented in src/uwp/src/audioOutput.cpp), which has no SDL audio: one stream per guest port, played in the order
// queued. The desktop builds use SDL's audio streams instead.
namespace Libs::Audio::Output {

class Stream;

// A stream of interleaved 16-bit or float samples with 1, 2 or 8 channels (SDL's channel order); nullptr when there is no audio device. A
// controller speaker stream (a pad speaker port) plays on a headset plugged into the controller when the app has one for it, and follows it
// when it is plugged in or out; otherwise it plays on the main output.
Stream*  Open(uint32_t freq, uint32_t channels, bool is_float, bool controller_speaker = false);
void     Close(Stream* stream);
bool     Queue(Stream* stream, const void* data, uint32_t size);
uint32_t QueuedSize(Stream* stream); // The bytes not played yet.
void     Clear(Stream* stream);

} // namespace Libs::Audio::Output

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_OUTPUT_H_
