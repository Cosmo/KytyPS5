// The emulator's audio output in the UWP app (libs/audioOutput.h): XAudio2, which UWP apps have on Windows and on the Xbox (SDL's desktop audio backends are
// not available there). One source voice per stream; XAudio2 resamples and mixes to the device's format. An engine per device: the default one, and
// the headset of a controller for the controller speaker streams (input.h, ControllerSpeakerDevice).

#include "libs/audioOutput.h"

#include "common/logging/log.h"
#include "input.h"
#include "log.h"

#include <windows.h>

#include <mmreg.h>
#include <xaudio2.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Audio::Output {

namespace {

// An XAudio2 engine and its mastering voice on one device: the default one (empty id) or a controller's headset.
struct Device final: public IXAudio2EngineCallback {
	std::wstring            id;
	IXAudio2*               engine = nullptr;
	IXAudio2MasteringVoice* master = nullptr;
	// The device went away (a headset was unplugged): its voices play nothing any more.
	std::atomic_bool failed = false;

	void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
	void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
	void STDMETHODCALLTYPE OnCriticalError(HRESULT error) noexcept override {
		failed = true;
		LOGF("AudioOut: XAudio2 device lost (0x%08x)\n", static_cast<unsigned>(error));
	}
};

} // namespace

// The buffers stay alive until XAudio2 has played (or flushed) them: it ends them in order.
class Stream final: public IXAudio2VoiceCallback {
public:
	IXAudio2SourceVoice*             voice              = nullptr;
	Device*                          device             = nullptr;
	WAVEFORMATEXTENSIBLE             format             {};
	bool                             controller_speaker = false;
	uint64_t                         last_route         = 0; // when the stream last checked where it should play
	std::mutex                       mutex;
	std::deque<std::vector<uint8_t>> buffers;
	std::atomic_uint32_t             queued_size {0};
	uint64_t                         last_report = 0;

	void STDMETHODCALLTYPE OnBufferEnd(void* /*context*/) noexcept override {
		std::lock_guard lock(mutex);
		queued_size -= static_cast<uint32_t>(buffers.front().size());
		buffers.pop_front();
	}

	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 /*bytes_required*/) noexcept override {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
	void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
	void STDMETHODCALLTYPE OnBufferStart(void* /*context*/) noexcept override {}
	void STDMETHODCALLTYPE OnLoopEnd(void* /*context*/) noexcept override {}
	void STDMETHODCALLTYPE OnVoiceError(void* /*context*/, HRESULT error) noexcept override {
		LOGF("AudioOut: XAudio2 voice error 0x%08x\n", static_cast<unsigned>(error));
	}
};

namespace {

// The devices, created when first needed and kept for the app's lifetime: the default one (with the first stream) and the headsets. A headset
// that went away keeps its engine (voices may still be on it); plugged in again, it gets a new one.
std::mutex                           g_engine_mutex;
std::unique_ptr<Device>              g_main;
bool                                 g_engine_failed = false;
std::vector<std::unique_ptr<Device>> g_headsets;
// How often a controller speaker stream looks for the headset.
constexpr uint64_t ROUTE_INTERVAL_MS = 1000;

// Caller holds g_engine_mutex.
std::unique_ptr<Device> OpenDevice(const std::wstring& id) {
	auto    device = std::make_unique<Device>();
	HRESULT result = XAudio2Create(&device->engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (SUCCEEDED(result)) {
		result = device->engine->RegisterForCallbacks(device.get());
	}
	if (SUCCEEDED(result)) {
		result = device->engine->CreateMasteringVoice(&device->master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0,
		                                              id.empty() ? nullptr : id.c_str());
	}
	if (FAILED(result)) {
		LOGF("AudioOut: XAudio2 unavailable on %s (0x%08x)\n", id.empty() ? "the default device" : "the headset", static_cast<unsigned>(result));
		if (device->engine != nullptr) {
			device->engine->Release();
		}
		return nullptr;
	}
	device->id = id;
	XAUDIO2_VOICE_DETAILS details {};
	device->master->GetVoiceDetails(&details);
	LOGF("AudioOut: XAudio2 %s: %u Hz, %u ch\n", id.empty() ? "device" : "headset", details.InputSampleRate, details.InputChannels);
	return device;
}

bool StartEngine() {
	std::lock_guard lock(g_engine_mutex);
	if (g_main != nullptr || g_engine_failed) {
		return g_main != nullptr;
	}
	g_main = OpenDevice({});
	if (g_main == nullptr) {
		LOGF("AudioOut: no sound\n");
		g_engine_failed = true;
		return false;
	}
	return true;
}

// The device a stream should play on now: a controller speaker stream on the controller's headset, when there is one that works.
Device* Target(const Stream& stream) {
	const auto      id = stream.controller_speaker ? Kyty::Uwp::ControllerSpeakerDevice() : std::wstring();
	std::lock_guard lock(g_engine_mutex);
	if (id.empty()) {
		return g_main.get();
	}
	for (const auto& headset: g_headsets) {
		if (headset->id == id && !headset->failed) {
			return headset.get();
		}
	}
	if (auto headset = OpenDevice(id); headset != nullptr) {
		return g_headsets.emplace_back(std::move(headset)).get();
	}
	return g_main.get();
}

// Plays the stream on `device` from now on, with a new voice; what the old one still had queued is dropped.
bool Move(Stream* stream, Device* device) {
	IXAudio2SourceVoice* voice  = nullptr;
	const HRESULT        result = device->engine->CreateSourceVoice(&voice, &stream->format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, stream);
	if (FAILED(result)) {
		LOGF("AudioOut: XAudio2 voice for %u Hz, %u ch failed: 0x%08x\n", static_cast<unsigned>(stream->format.Format.nSamplesPerSec),
		     static_cast<unsigned>(stream->format.Format.nChannels), static_cast<unsigned>(result));
		return false;
	}
	if (stream->voice != nullptr) {
		// No callbacks of the old voice after this returns.
		stream->voice->DestroyVoice();
		std::lock_guard lock(stream->mutex);
		stream->buffers.clear();
		stream->queued_size = 0;
	}
	stream->voice  = voice;
	stream->device = device;
	voice->Start(0);
	if (stream->controller_speaker) {
		Kyty::Uwp::Log("audio: the controller speaker plays on %s\n", device == g_main.get() ? "the main output" : "the controller's headset");
	}
	return true;
}

// A controller speaker stream, now and then and as soon as its headset went away: onto the device it should play on.
void Route(Stream* stream) {
	const auto now = GetTickCount64();
	if (!stream->controller_speaker || (now - stream->last_route < ROUTE_INTERVAL_MS && !stream->device->failed)) {
		return;
	}
	stream->last_route = now;
	if (auto* device = Target(*stream); device != stream->device) {
		(void)Move(stream, device);
	}
}

DWORD ChannelMask(uint32_t channels) {
	switch (channels) {
		case 1: return SPEAKER_FRONT_CENTER;
		case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
		// Front, center, LFE, back, side: SDL's order is also WAVEFORMATEXTENSIBLE's.
		case 8:
			return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
			       SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
		default: return 0;
	}
}

// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT (ksmedia.h): the format tag in the base GUID.
GUID SubFormat(WORD format_tag) {
	return {format_tag, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}

} // namespace

Stream* Open(uint32_t freq, uint32_t channels, bool is_float, bool controller_speaker) {
	if (ChannelMask(channels) == 0 || !StartEngine()) {
		return nullptr;
	}

	auto* stream               = new Stream;
	stream->controller_speaker = controller_speaker;
	stream->last_route         = GetTickCount64();

	const WORD            bytes_per_sample = is_float ? sizeof(float) : sizeof(int16_t);
	WAVEFORMATEXTENSIBLE& format           = stream->format;
	format.Format.wFormatTag          = WAVE_FORMAT_EXTENSIBLE;
	format.Format.nChannels            = static_cast<WORD>(channels);
	format.Format.nSamplesPerSec       = freq;
	format.Format.wBitsPerSample       = static_cast<WORD>(bytes_per_sample * 8);
	format.Format.nBlockAlign          = static_cast<WORD>(bytes_per_sample * channels);
	format.Format.nAvgBytesPerSec      = freq * format.Format.nBlockAlign;
	format.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	format.Samples.wValidBitsPerSample = format.Format.wBitsPerSample;
	format.dwChannelMask               = ChannelMask(channels);
	format.SubFormat                   = SubFormat(is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);

	if (!Move(stream, Target(*stream))) {
		delete stream;
		return nullptr;
	}
	LOGF("AudioOut: opened XAudio2 stream (%u Hz, %u ch, %s)\n", freq, channels, is_float ? "float" : "16-bit");
	Kyty::Uwp::Log("audio: opened an XAudio2 stream (%u Hz, %u ch, %s)\n", freq, channels, is_float ? "float" : "16-bit");
	return stream;
}

void Close(Stream* stream) {
	if (stream == nullptr) {
		return;
	}
	// No callbacks after this returns; the buffers go with the stream.
	stream->voice->DestroyVoice();
	delete stream;
}

bool Queue(Stream* stream, const void* data, uint32_t size) {
	if (stream == nullptr || data == nullptr || size == 0) {
		return false;
	}
	Route(stream);
	const uint8_t* bytes = nullptr;
	{
		std::lock_guard lock(stream->mutex);
		if (stream->buffers.size() >= XAUDIO2_MAX_QUEUED_BUFFERS) {
			return false;
		}
		const auto* begin = static_cast<const uint8_t*>(data);
		bytes             = stream->buffers.emplace_back(begin, begin + size).data();
		stream->queued_size += size;
	}
	XAUDIO2_BUFFER buffer {};
	buffer.AudioBytes    = size;
	buffer.pAudioData    = bytes;
	const HRESULT result = stream->voice->SubmitSourceBuffer(&buffer);
	if (FAILED(result)) {
		// Not submitted, so no callback will end it; it is still the newest buffer.
		std::lock_guard lock(stream->mutex);
		stream->queued_size -= size;
		stream->buffers.pop_back();
		LOGF("AudioOut: XAudio2 submit failed: 0x%08x\n", static_cast<unsigned>(result));
		return false;
	}
	// Every 5 seconds, per stream, the samples the device has played (in the app's own small log): proof that the sound arrives.
	const auto now = GetTickCount64();
	if (now - stream->last_report >= 5000) {
		stream->last_report = now;
		XAUDIO2_VOICE_STATE state {};
		stream->voice->GetState(&state);
		Kyty::Uwp::Log("audio: %llu samples played, %u buffers queued\n", static_cast<unsigned long long>(state.SamplesPlayed), state.BuffersQueued);
	}
	return true;
}

uint32_t QueuedSize(Stream* stream) {
	return stream != nullptr ? stream->queued_size.load() : 0;
}

void Clear(Stream* stream) {
	if (stream != nullptr) {
		// The removed buffers still end through OnBufferEnd.
		stream->voice->FlushSourceBuffers();
	}
}

} // namespace Libs::Audio::Output
