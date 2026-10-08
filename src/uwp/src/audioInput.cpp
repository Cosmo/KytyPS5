// The emulator's audio input in the UWP app (libs/audioInput.h): WASAPI on the microphone of the headset plugged into player 1's controller
// (input.h, ControllerMicrophoneDevice), as the DualSense's microphone. A thread per stream captures into a buffer the guest's reads take from;
// WASAPI converts to the stream's format. Without a microphone the stream stays silent and looks for one every second.

#include "libs/audioInput.h"

#include "common/logging/log.h"
#include "input.h"
#include "log.h"

#include <windows.h>

#include <unknwn.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace Libs::Audio::Input {

namespace {

// At most this much captured audio waits for the guest; older audio goes.
constexpr uint32_t BUFFER_MS = 1000;
// How often the thread looks for the microphone while it has none, and checks that it is still the one to use while it captures.
constexpr DWORD CHECK_MS = 1000;
// WASAPI's buffer, in 100-nanosecond units.
constexpr REFERENCE_TIME DEVICE_BUFFER = 200 * 10000;
// How long the activation may take.
constexpr DWORD ACTIVATION_MS = 10000;

// ActivateAudioInterfaceAsync's completion handler (winrt::implements makes it agile, as the function needs).
struct Activation: winrt::implements<Activation, IActivateAudioInterfaceCompletionHandler> {
	winrt::handle done {CreateEventW(nullptr, TRUE, FALSE, nullptr)};

	HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* /*operation*/) noexcept override {
		SetEvent(done.get());
		return S_OK;
	}
};

// An audio client on the device; null with the reason when it cannot be had (no access to the microphone, the device gone).
winrt::com_ptr<IAudioClient> ActivateClient(const std::wstring& id, HRESULT* reason) {
	auto                                                  activation = winrt::make_self<Activation>();
	winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
	HRESULT result = ActivateAudioInterfaceAsync(id.c_str(), __uuidof(IAudioClient), nullptr, activation.get(), operation.put());
	if (SUCCEEDED(result) && WaitForSingleObject(activation->done.get(), ACTIVATION_MS) != WAIT_OBJECT_0) {
		result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	}
	winrt::com_ptr<IUnknown> unknown;
	if (SUCCEEDED(result)) {
		HRESULT activated = E_FAIL;
		result            = operation->GetActivateResult(&activated, unknown.put());
		if (SUCCEEDED(result)) {
			result = activated;
		}
	}
	winrt::com_ptr<IAudioClient> client;
	if (SUCCEEDED(result)) {
		result = unknown->QueryInterface(__uuidof(IAudioClient), client.put_void());
	}
	*reason = result;
	return SUCCEEDED(result) ? client : nullptr;
}

} // namespace

class Stream {
public:
	WAVEFORMATEX     format {};
	uint32_t         max_bytes = 0; // BUFFER_MS of the format
	std::thread      thread;
	winrt::handle    stop_event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
	std::atomic_bool stop      = false;
	std::atomic_bool capturing = false;

	std::mutex          mutex;
	std::deque<uint8_t> buffer; // guarded by mutex

	void Run();
	// Captures from the microphone until it goes away, another one is to be used, or the stream closes.
	void Capture(const std::wstring& id);
	void Push(const BYTE* data, uint32_t bytes, bool silent);

private:
	std::wstring failed_id; // the microphone that failed last: logged once
};

void Stream::Run() {
	winrt::init_apartment();
	while (!stop) {
		if (const auto id = Kyty::Uwp::ControllerMicrophoneDevice(); !id.empty()) {
			Capture(id);
		}
		WaitForSingleObject(stop_event.get(), CHECK_MS);
	}
	winrt::uninit_apartment();
}

void Stream::Capture(const std::wstring& id) {
	HRESULT result = S_OK;
	auto    client = ActivateClient(id, &result);
	winrt::com_ptr<IAudioCaptureClient> capture;
	winrt::handle                       ready {CreateEventW(nullptr, FALSE, FALSE, nullptr)};
	if (client != nullptr) {
		result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
		                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
		                            DEVICE_BUFFER, 0, &format, nullptr);
		if (SUCCEEDED(result)) {
			result = client->SetEventHandle(ready.get());
		}
		if (SUCCEEDED(result)) {
			result = client->GetService(__uuidof(IAudioCaptureClient), capture.put_void());
		}
		if (SUCCEEDED(result)) {
			result = client->Start();
		}
	}
	if (FAILED(result)) {
		if (failed_id != id) {
			failed_id = id;
			Kyty::Uwp::Log("microphone: the controller's headset cannot be used (0x%08x%s)\n", static_cast<unsigned>(result),
			               result == E_ACCESSDENIED ? ", no access: turn the option on in the settings or allow it in Windows' privacy settings" : "");
		}
		return;
	}
	failed_id.clear();
	capturing = true;
	Kyty::Uwp::Log("microphone: the controller's headset (%u Hz, %u ch)\n", static_cast<unsigned>(format.nSamplesPerSec),
	               static_cast<unsigned>(format.nChannels));

	const HANDLE waits[] = {ready.get(), stop_event.get()};
	auto         checked = GetTickCount64();
	while (!stop) {
		WaitForMultipleObjects(2, waits, FALSE, CHECK_MS);
		UINT32 packet = 0;
		while (SUCCEEDED(result = capture->GetNextPacketSize(&packet)) && packet > 0) {
			BYTE*  data   = nullptr;
			UINT32 frames = 0;
			DWORD  flags  = 0;
			result        = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			if (FAILED(result)) {
				break;
			}
			Push(data, frames * format.nBlockAlign, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
			capture->ReleaseBuffer(frames);
		}
		if (FAILED(result)) {
			break; // the headset went away
		}
		if (const auto now = GetTickCount64(); now - checked >= CHECK_MS) {
			checked = now;
			if (Kyty::Uwp::ControllerMicrophoneDevice() != id) {
				break;
			}
		}
	}
	client->Stop();
	capturing = false;
	{
		std::lock_guard lock(mutex);
		buffer.clear();
	}
	Kyty::Uwp::Log("microphone: stopped (%s)\n", stop ? "closed" : FAILED(result) ? "the headset went away" : "the option or the headset changed");
}

void Stream::Push(const BYTE* data, uint32_t bytes, bool silent) {
	std::lock_guard lock(mutex);
	if (silent) {
		buffer.insert(buffer.end(), bytes, uint8_t {0});
	} else {
		buffer.insert(buffer.end(), data, data + bytes);
	}
	// Both are whole frames, so what goes is too.
	if (buffer.size() > max_bytes) {
		buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(buffer.size() - max_bytes));
	}
}

Stream* Open(uint32_t freq, uint32_t channels, bool is_float) {
	if (freq == 0 || channels < 1 || channels > 2) {
		return nullptr;
	}
	auto* stream           = new Stream;
	auto& format           = stream->format;
	format.wFormatTag      = is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
	format.nChannels       = static_cast<WORD>(channels);
	format.nSamplesPerSec  = freq;
	format.wBitsPerSample  = is_float ? 32 : 16;
	format.nBlockAlign     = static_cast<WORD>(channels * format.wBitsPerSample / 8);
	format.nAvgBytesPerSec = freq * format.nBlockAlign;
	stream->max_bytes      = freq * BUFFER_MS / 1000 * format.nBlockAlign;
	stream->thread         = std::thread([stream] { stream->Run(); });
	return stream;
}

void Close(Stream* stream) {
	if (stream == nullptr) {
		return;
	}
	stream->stop = true;
	SetEvent(stream->stop_event.get());
	stream->thread.join();
	delete stream;
}

bool Present(Stream* stream) {
	return stream != nullptr && stream->capturing;
}

uint32_t Available(Stream* stream) {
	if (stream == nullptr) {
		return 0;
	}
	std::lock_guard lock(stream->mutex);
	return static_cast<uint32_t>(stream->buffer.size());
}

uint32_t Read(Stream* stream, void* dest, uint32_t size) {
	if (stream == nullptr || dest == nullptr) {
		return 0;
	}
	std::lock_guard lock(stream->mutex);
	const uint32_t  frame = stream->format.nBlockAlign;
	const uint32_t  bytes = std::min<uint32_t>(size, static_cast<uint32_t>(stream->buffer.size())) / frame * frame;
	std::copy_n(stream->buffer.begin(), bytes, static_cast<uint8_t*>(dest));
	stream->buffer.erase(stream->buffer.begin(), stream->buffer.begin() + bytes);
	return bytes;
}

void Clear(Stream* stream) {
	if (stream != nullptr) {
		std::lock_guard lock(stream->mutex);
		stream->buffer.clear();
	}
}

} // namespace Libs::Audio::Input
