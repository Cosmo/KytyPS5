#include "libs/dualSenseHaptics.h"

#include "common/logging/log.h"
#include "common/threads.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Libs::Controller::DualSenseHaptics {

struct Stream {
	uint32_t           freq       = 0;
	bool               speaker    = false;
	SDL_AudioStream*   sdl        = nullptr;
	SDL_AudioDeviceID  device     = 0;
	uint64_t           next_check = 0;
	std::vector<float> buffer;
};

namespace {

constexpr int FRAME_BYTES = 4 * sizeof(float);
Common::Mutex g_mutex;
int           g_controller       = -1;
int           g_playback_streams = 0;
uint8_t       g_large_motor = 0, g_small_motor = 0;
uint64_t      g_rumble_until = 0, g_haptics_until = 0;
int           g_speaker = -1, g_speaker_streams = 0;

void ApplyRumble() { // Caller holds g_mutex; no calls into Audio or Controller.
	SDL_LockJoysticks();
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(g_controller));
	    pad != nullptr) {
		const auto now   = SDL_GetTicks();
		const bool muted = g_haptics_until > now || g_rumble_until <= now;
		(void)SDL_RumbleGamepad(
		    pad, muted ? 0 : g_large_motor * 0x101U, muted ? 0 : g_small_motor * 0x101U,
		    g_rumble_until > now ? static_cast<uint32_t>(g_rumble_until - now) : 0);
	}
	SDL_UnlockJoysticks();
}

void SelectController(int controller) {
	if (g_controller != controller) {
		g_haptics_until = g_rumble_until = 0;
		ApplyRumble();
		g_controller  = controller;
		g_large_motor = g_small_motor = 0;
	}
}

void RouteSpeaker(int controller, bool speaker) { // Caller holds g_mutex.
	// Route the right channel to the speaker and mute the headphones with the values Linux uses
	// without headphones (output report 0x02). Plugged-in headphones are not checked.
	std::array<uint8_t, 38> report {};
	report[0] = 0x80; // Audio control valid
	if (speaker) {
		report[0] |= 0x20; // Speaker volume valid
		report[1]  = 0x80; // Audio control 2 valid
		report[5]  = 0x64; // Speaker volume
		report[7]  = 0x30; // Output path: right channel to the speaker, headphones muted
		report[37] = 0x02; // Speaker preamp +6 dB
	}
	SDL_LockJoysticks();
	if (auto* pad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(controller)); pad != nullptr) {
		(void)SDL_SendGamepadEffect(pad, report.data(), static_cast<int>(report.size()));
	}
	SDL_UnlockJoysticks();
}

void SelectSpeaker(int controller) { // Caller holds g_mutex; -1 restores the headphone routing.
	if (g_speaker != -1 && g_speaker != controller) {
		RouteSpeaker(g_speaker, false);
	}
	if (controller != -1) {
		RouteSpeaker(controller, true);
	}
	g_speaker = controller;
}

void SDLCALL UpdateRumble(void*, SDL_AudioStream*, int, int) {
	Common::LockGuard lock(g_mutex);
	if (g_haptics_until != 0 && SDL_GetTicks() >= g_haptics_until) {
		g_haptics_until = 0;
		ApplyRumble();
	}
}

void CloseDevice(Stream* stream) {
	if (stream->sdl != nullptr) {
		// Destroy outside g_mutex: SDL waits for any running get callback.
		SDL_DestroyAudioStream(stream->sdl);
		stream->sdl = nullptr;
		Common::LockGuard lock(g_mutex);
		if (--g_playback_streams == 0 && g_haptics_until != 0) {
			g_haptics_until = 0;
			ApplyRumble();
		}
		// Route back to the headphones once no speaker port plays on the controller.
		if (stream->speaker && --g_speaker_streams == 0) {
			SelectSpeaker(-1);
		}
	}
}

SDL_AudioDeviceID FindDevice() {
	int               count   = 0;
	auto*             devices = SDL_GetAudioPlaybackDevices(&count);
	SDL_AudioDeviceID device  = 0;
	for (int i = 0; i < count && device == 0; i++) {
		const char*   name = SDL_GetAudioDeviceName(devices[i]);
		SDL_AudioSpec spec {};
		if (name != nullptr && SDL_strcasestr(name, "DualSense") != nullptr &&
		    SDL_GetAudioDeviceFormat(devices[i], &spec, nullptr) && spec.channels == 4) {
			device = devices[i];
		}
	}
	SDL_free(devices);
	return device;
}

} // namespace

Stream* Open(uint32_t freq, bool speaker) {
	if (freq == 0 || !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		return nullptr;
	}
	return new Stream {.freq = freq, .speaker = speaker};
}

void Close(Stream* stream) {
	if (stream != nullptr) {
		CloseDevice(stream);
		delete stream;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
}

uint64_t Queue(Stream* stream, int controller, const void* data, uint32_t frames, uint32_t channels,
               bool is_float, const int* volume) {
	if (stream == nullptr || data == nullptr || frames == 0 || channels == 0 || volume == nullptr) {
		return 0;
	}
	if (SDL_GetGamepadTypeForID(static_cast<SDL_JoystickID>(controller)) != SDL_GAMEPAD_TYPE_PS5) {
		CloseDevice(stream);
		stream->next_check = 0; // Look for the device as soon as a DualSense is active again.
		return 0;
	}
	const auto now = SDL_GetTicks();
	if (now >= stream->next_check) {
		stream->next_check = now + 2000;
		const auto device  = FindDevice();
		if (stream->device != device || stream->sdl == nullptr) {
			CloseDevice(stream);
			stream->device = device;
			if (device != 0) {
				const SDL_AudioSpec desired {SDL_AUDIO_F32, 4, static_cast<int>(stream->freq)};
				auto* sdl = SDL_OpenAudioDeviceStream(device, &desired, UpdateRumble, nullptr);
				SDL_AudioSpec actual {};
				if (sdl != nullptr &&
				    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sdl), &actual, nullptr) &&
				    actual.channels == 4 && SDL_ResumeAudioStreamDevice(sdl)) {
					stream->sdl = sdl;
					{
						Common::LockGuard lock(g_mutex);
						g_playback_streams++;
						if (stream->speaker) {
							g_speaker_streams++;
							SelectSpeaker(controller);
						}
					}
					LOGF("DualSenseHaptics: playing on '%s'\n", SDL_GetAudioDeviceName(device));
				} else {
					SDL_DestroyAudioStream(sdl);
					LOGF("DualSenseHaptics: cannot open quad output: %s\n", SDL_GetError());
				}
			}
		}
	}
	if (stream->sdl == nullptr) {
		return 0;
	}
	stream->buffer.assign(static_cast<size_t>(frames) * 4, 0.0f);
	bool audible = false;
	for (uint32_t frame = 0; frame < frames; frame++) {
		for (uint32_t ch = 0; ch < 2; ch++) {
			const auto src_ch = channels == 1 ? 0 : ch;
			const auto index  = static_cast<size_t>(frame) * channels + src_ch;
			float      value  = is_float ? static_cast<const float*>(data)[index]
			                             : static_cast<const int16_t*>(data)[index] / 32768.0f;
			value *= volume[src_ch] / 32768.0f;
			// The speaker plays the right front channel; the actuators play the back ones.
			stream->buffer[static_cast<size_t>(frame) * 4 + ch + (stream->speaker ? 0 : 2)] = value;
			audible |= std::fabs(value) > 1.0f / 1024;
		}
	}
	auto queued = std::max(0, SDL_GetAudioStreamQueued(stream->sdl));
	if (queued > static_cast<int>(stream->freq * FRAME_BYTES * 80 / 1000)) {
		SDL_ClearAudioStream(stream->sdl);
		queued = 0;
	}
	const int bytes = static_cast<int>(frames * FRAME_BYTES);
	if (audible && !stream->speaker) {
		Common::LockGuard lock(g_mutex);
		if (SDL_GetGamepadTypeForID(static_cast<SDL_JoystickID>(controller)) !=
		    SDL_GAMEPAD_TYPE_PS5) {
			return 0;
		}
		SelectController(controller);
		const auto playback_now = SDL_GetTicks();
		const bool starting     = g_haptics_until <= playback_now;
		const auto duration     = (queued + bytes) * 1000ULL / (stream->freq * FRAME_BYTES);
		g_haptics_until =
		    std::max(g_haptics_until, playback_now + std::max<uint64_t>(duration, 250));
		if (starting) {
			ApplyRumble();
		}
	}
	if (!SDL_PutAudioStreamData(stream->sdl, stream->buffer.data(), bytes)) {
		CloseDevice(stream);
		return 0;
	}
	return static_cast<uint64_t>(queued + bytes) * 1000000 / (stream->freq * FRAME_BYTES);
}

bool SetVibration(int controller, uint8_t large_motor, uint8_t small_motor) {
	if (SDL_GetGamepadTypeForID(static_cast<SDL_JoystickID>(controller)) != SDL_GAMEPAD_TYPE_PS5) {
		return false;
	}
	Common::LockGuard lock(g_mutex);
	SelectController(controller);
	g_large_motor  = large_motor;
	g_small_motor  = small_motor;
	g_rumble_until = SDL_GetTicks() + 0xffff;
	ApplyRumble();
	return true;
}

void Shutdown() {
	Common::LockGuard lock(g_mutex);
	SelectController(-1);
	SelectSpeaker(-1);
}

} // namespace Libs::Controller::DualSenseHaptics
