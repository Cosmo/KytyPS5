#include "libs/padHaptics.h"

#include "common/logging/log.h"
#include "common/threads.h"
#include "libs/controller.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace Libs::Audio::PadHaptics {

namespace {

// A DualSense's USB audio channels in SDL's quad order: its speaker, then its two actuators.
constexpr uint32_t DEVICE_CHANNELS = 4;
constexpr uint32_t LEFT_ACTUATOR   = 2;
constexpr uint32_t RIGHT_ACTUATOR  = 3;
constexpr int      FRAME_BYTES     = static_cast<int>(DEVICE_CHANNELS * sizeof(float));

constexpr float VOLUME_UNITY = 32768.0f;
constexpr float S16_SCALE    = 1.0f / 32768.0f;

constexpr Uint64 DEVICE_CHECK_MS = 2000;           // Hot-plug polling interval.
constexpr float  SILENCE_LEVEL   = 1.0f / 1024.0f; // About -60 dBFS.
// Rumble comes back this long after the haptics fall silent, not in every short pause.
constexpr Uint64 STOP_MS = 250;
// About what Audio::QueueSdlAudio keeps queued, so haptics and sound have similar latency.
constexpr uint32_t LATENCY_MS = 40;
// Playback runs up to 3% slow or fast to hold the queue near LATENCY_MS when the port is fed off
// real time (a port without a device paces on a sleep timer; two devices have two clocks).
constexpr float RATE_CORRECTION = 0.03f;

Common::Mutex g_playing_mutex;
int           g_playing_streams = 0;
// Why haptics are unavailable is logged once, until they play again.
std::atomic_bool g_logged_unavailable {false};

} // namespace

struct Stream {
	uint32_t           freq          = 0;
	SDL_AudioDeviceID  device        = 0;
	SDL_AudioDeviceID  failed_device = 0; // Skipped until reconnected, which gives it a new ID.
	SDL_AudioStream*   sdl           = nullptr;
	float              ratio         = 1.0f;
	Uint64             next_check    = 0;
	Uint64             last_sound    = 0;
	bool               playing       = false;
	std::vector<float> converted;
	std::vector<float> silence;
};

float ToControllerFrames(const void* data, uint32_t frames, uint32_t channels, bool is_float,
                         const int* volume, float* out) {
	const uint32_t right_channel = (channels > 1 ? 1 : 0);
	const float    left_gain     = static_cast<float>(volume[0]) / VOLUME_UNITY;
	const float    right_gain    = static_cast<float>(volume[right_channel]) / VOLUME_UNITY;

	float peak = 0.0f;
	for (uint32_t frame = 0; frame < frames; frame++) {
		const size_t src_index = static_cast<size_t>(frame) * channels;
		float        left      = 0.0f;
		float        right     = 0.0f;
		if (is_float) {
			const auto* src = static_cast<const float*>(data) + src_index;
			left            = src[0];
			right           = src[right_channel];
		} else {
			const auto* src = static_cast<const int16_t*>(data) + src_index;
			left            = static_cast<float>(src[0]) * S16_SCALE;
			right           = static_cast<float>(src[right_channel]) * S16_SCALE;
		}
		left *= left_gain;
		right *= right_gain;

		float* dst = out + static_cast<size_t>(frame) * DEVICE_CHANNELS;
		std::fill_n(dst, DEVICE_CHANNELS, 0.0f);
		dst[LEFT_ACTUATOR]  = left;
		dst[RIGHT_ACTUATOR] = right;
		peak                = std::max({peak, std::fabs(left), std::fabs(right)});
	}
	return peak;
}

bool IsControllerName(const char* name) {
	// Hosts decorate the name, e.g. "Speakers (DualSense Wireless Controller)" on Windows.
	return name != nullptr && SDL_strcasestr(name, "DualSense") != nullptr;
}

float PlaybackRatio(int queued_bytes, int target_bytes) {
	if (queued_bytes < target_bytes / 2) {
		return 1.0f - RATE_CORRECTION;
	}
	if (queued_bytes > target_bytes + target_bytes / 2) {
		return 1.0f + RATE_CORRECTION;
	}
	return 1.0f;
}

// The controller holds a DualSense's rumble off while any port plays.
static void SetPlaying(Stream* stream, bool playing) {
	if (stream->playing == playing) {
		return;
	}
	stream->playing = playing;

	Common::LockGuard lock(g_playing_mutex);
	const bool        was_playing = g_playing_streams > 0;
	g_playing_streams += (playing ? 1 : -1);
	if (was_playing != (g_playing_streams > 0)) {
		Controller::SetAudioHapticsPlaying(g_playing_streams > 0);
	}
}

static bool DevicePresent(SDL_AudioDeviceID device) {
	int                count   = 0;
	SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
	const bool         present =
	    devices != nullptr && std::find(devices, devices + count, device) != devices + count;
	SDL_free(devices);
	return present;
}

// The first DualSense with the actuators' channels, or 0.
static SDL_AudioDeviceID FindDevice(const Stream* stream) {
	int                count   = 0;
	SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
	SDL_AudioDeviceID  found   = 0;
	std::string        too_few_channels;
	int                too_few_channels_num = 0;
	for (int i = 0; i < count && found == 0; i++) {
		const char* name = SDL_GetAudioDeviceName(devices[i]);
		if (devices[i] == stream->failed_device || !IsControllerName(name)) {
			continue;
		}
		SDL_AudioSpec spec {};
		if (!SDL_GetAudioDeviceFormat(devices[i], &spec, nullptr)) {
			continue;
		}
		if (spec.channels >= static_cast<int>(DEVICE_CHANNELS)) {
			found = devices[i];
		} else if (too_few_channels.empty()) {
			too_few_channels     = name;
			too_few_channels_num = spec.channels;
		}
	}
	SDL_free(devices);

	if (found == 0 && !g_logged_unavailable.exchange(true)) {
		if (!too_few_channels.empty()) {
			// A stereo device would play the actuators' channels on the controller's speaker. SDL
			// reads the channel count when the device appears.
			LOGF("PadHaptics: '%s' has %d channels, the actuators need %u; switch the device to a "
			     "4-channel (quadraphonic) speaker setup, then reconnect the controller\n",
			     too_few_channels.c_str(), too_few_channels_num, DEVICE_CHANNELS);
		} else {
			LOGF("PadHaptics: no DualSense audio device; haptics need the controller connected by "
			     "USB\n");
		}
	}
	return found;
}

static void CloseDevice(Stream* stream) {
	if (stream->sdl != nullptr) {
		SDL_DestroyAudioStream(stream->sdl);
	}
	stream->sdl    = nullptr;
	stream->device = 0;
}

static void FailDevice(Stream* stream, SDL_AudioDeviceID device, const char* call) {
	if (!g_logged_unavailable.exchange(true)) {
		LOGF("PadHaptics: %s failed: %s\n", call, SDL_GetError());
	}
	CloseDevice(stream);
	stream->failed_device = device;
}

static void OpenDevice(Stream* stream, SDL_AudioDeviceID device) {
	SDL_AudioSpec spec {};
	spec.freq     = static_cast<int>(stream->freq);
	spec.format   = SDL_AUDIO_F32;
	spec.channels = static_cast<int>(DEVICE_CHANNELS);

	stream->sdl = SDL_OpenAudioDeviceStream(device, &spec, nullptr, nullptr);
	if (stream->sdl == nullptr) {
		FailDevice(stream, device, "SDL_OpenAudioDeviceStream");
		return;
	}
	if (!SDL_ResumeAudioStreamDevice(stream->sdl)) {
		FailDevice(stream, device, "SDL_ResumeAudioStreamDevice");
		return;
	}

	stream->device = device;
	stream->ratio  = 1.0f;
	g_logged_unavailable.store(false);
	const char* name = SDL_GetAudioDeviceName(device);
	LOGF("PadHaptics: playing on '%s' (%u Hz)\n", name != nullptr ? name : "?", stream->freq);
}

static void CheckDevice(Stream* stream) {
	if (stream->sdl != nullptr) {
		if (DevicePresent(stream->device)) {
			return;
		}
		LOGF("PadHaptics: the DualSense audio device is gone\n");
		CloseDevice(stream);
	}
	if (stream->failed_device != 0 && !DevicePresent(stream->failed_device)) {
		stream->failed_device = 0;
	}
	if (const auto device = FindDevice(stream); device != 0) {
		OpenDevice(stream, device);
	}
}

static bool PutConverted(Stream* stream, uint32_t frames) {
	const int latency_bytes = static_cast<int>(stream->freq * LATENCY_MS / 1000) * FRAME_BYTES;
	const int block_bytes   = static_cast<int>(frames) * FRAME_BYTES;

	int queued = SDL_GetAudioStreamQueued(stream->sdl);
	// A backlog after a stall is dropped rather than played late.
	if (queued > 2 * latency_bytes) {
		SDL_ClearAudioStream(stream->sdl);
		queued = 0;
	}
	// Starting, or after an underrun: silence first, up to the target latency.
	if (queued <= 0 && latency_bytes > block_bytes) {
		queued = latency_bytes - block_bytes;
		stream->silence.assign(static_cast<size_t>(queued) / sizeof(float), 0.0f);
		if (!SDL_PutAudioStreamData(stream->sdl, stream->silence.data(), queued)) {
			return false;
		}
	}

	const float ratio = PlaybackRatio(queued + block_bytes, latency_bytes);
	if (ratio != stream->ratio && SDL_SetAudioStreamFrequencyRatio(stream->sdl, ratio)) {
		stream->ratio = ratio;
	}
	return SDL_PutAudioStreamData(stream->sdl, stream->converted.data(), block_bytes);
}

Stream* Open(uint32_t freq) {
	if (freq == 0) {
		return nullptr;
	}
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		LOGF("PadHaptics: SDL audio init failed: %s\n", SDL_GetError());
		return nullptr;
	}

	auto* stream = new Stream;
	stream->freq = freq;
	return stream;
}

void Close(Stream* stream) {
	if (stream == nullptr) {
		return;
	}

	SetPlaying(stream, false);
	CloseDevice(stream);
	delete stream;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Queue(Stream* stream, const void* data, uint32_t frames, uint32_t channels, bool is_float,
           const int* volume) {
	if (stream == nullptr || data == nullptr || volume == nullptr || frames == 0 || channels == 0) {
		return;
	}

	const Uint64 now = SDL_GetTicks();
	if (now >= stream->next_check) {
		stream->next_check = now + DEVICE_CHECK_MS;
		CheckDevice(stream);
	}
	if (stream->sdl == nullptr) {
		SetPlaying(stream, false);
		return;
	}

	stream->converted.resize(static_cast<size_t>(frames) * DEVICE_CHANNELS);
	const float peak =
	    ToControllerFrames(data, frames, channels, is_float, volume, stream->converted.data());
	if (peak > SILENCE_LEVEL) {
		stream->last_sound = now;
		SetPlaying(stream, true);
	} else if (now - stream->last_sound >= STOP_MS) {
		SetPlaying(stream, false);
	}

	if (!PutConverted(stream, frames)) {
		SetPlaying(stream, false);
		FailDevice(stream, stream->device, "SDL_PutAudioStreamData");
	}
}

} // namespace Libs::Audio::PadHaptics
