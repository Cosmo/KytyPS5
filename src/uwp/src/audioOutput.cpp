// The emulator's audio output in the UWP app (libs/audioOutput.h): XAudio2, which UWP apps have on Windows and on the Xbox (SDL's desktop audio backends are
// not available there). One source voice per stream; XAudio2 resamples and mixes to the device's format.

#include "libs/audioOutput.h"

#include "common/logging/log.h"

#include <windows.h>

#include <mmreg.h>
#include <xaudio2.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace Libs::Audio::Output {

// The buffers stay alive until XAudio2 has played (or flushed) them: it ends them in order.
class Stream final: public IXAudio2VoiceCallback {
public:
	IXAudio2SourceVoice*             voice = nullptr;
	std::mutex                       mutex;
	std::deque<std::vector<uint8_t>> buffers;
	std::atomic_uint32_t             queued_size {0};

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

// The engine and its mastering voice (the default device), created with the first stream and kept for the app's lifetime.
std::mutex              g_engine_mutex;
IXAudio2*               g_engine        = nullptr;
IXAudio2MasteringVoice* g_master        = nullptr;
bool                    g_engine_failed = false;

bool StartEngine() {
	std::lock_guard lock(g_engine_mutex);
	if (g_master != nullptr || g_engine_failed) {
		return g_master != nullptr;
	}
	HRESULT result = XAudio2Create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
	if (SUCCEEDED(result)) {
		result = g_engine->CreateMasteringVoice(&g_master);
	}
	if (FAILED(result)) {
		LOGF("AudioOut: XAudio2 unavailable (0x%08x); no sound\n", static_cast<unsigned>(result));
		if (g_engine != nullptr) {
			g_engine->Release();
			g_engine = nullptr;
		}
		g_engine_failed = true;
		return false;
	}
	XAUDIO2_VOICE_DETAILS details {};
	g_master->GetVoiceDetails(&details);
	LOGF("AudioOut: XAudio2 device: %u Hz, %u ch\n", details.InputSampleRate, details.InputChannels);
	return true;
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

Stream* Open(uint32_t freq, uint32_t channels, bool is_float) {
	if (ChannelMask(channels) == 0 || !StartEngine()) {
		return nullptr;
	}

	const WORD           bytes_per_sample = is_float ? sizeof(float) : sizeof(int16_t);
	WAVEFORMATEXTENSIBLE format {};
	format.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
	format.Format.nChannels            = static_cast<WORD>(channels);
	format.Format.nSamplesPerSec       = freq;
	format.Format.wBitsPerSample       = static_cast<WORD>(bytes_per_sample * 8);
	format.Format.nBlockAlign          = static_cast<WORD>(bytes_per_sample * channels);
	format.Format.nAvgBytesPerSec      = freq * format.Format.nBlockAlign;
	format.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	format.Samples.wValidBitsPerSample = format.Format.wBitsPerSample;
	format.dwChannelMask               = ChannelMask(channels);
	format.SubFormat                   = SubFormat(is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);

	auto*         stream = new Stream;
	const HRESULT result = g_engine->CreateSourceVoice(&stream->voice, &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, stream);
	if (FAILED(result)) {
		LOGF("AudioOut: XAudio2 voice for %u Hz, %u ch failed: 0x%08x\n", freq, channels, static_cast<unsigned>(result));
		delete stream;
		return nullptr;
	}
	stream->voice->Start(0);
	LOGF("AudioOut: opened XAudio2 stream (%u Hz, %u ch, %s)\n", freq, channels, is_float ? "float" : "16-bit");
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
