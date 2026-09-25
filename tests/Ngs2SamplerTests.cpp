// Exercise sampler controls and rendering without a host audio device.
#include "libs/ngs2.cpp"

#include <cstdio>
#include <cstdlib>

using namespace Libs::Audio::Ngs2;

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "Ngs2SamplerTests: %s\n", message);
		std::abort();
	}
}

struct Fixture {
	Ngs2Internal      system;
	Ngs2RackInternal  rack {};
	Ngs2VoiceInternal voice;

	Fixture(uint32_t rate, uint32_t channels = 1, Ngs2RackType type = Ngs2RackType::Sampler) {
		system.option.sample_rate = 48000;
		rack.ngs                  = &system;
		rack.type                 = type;
		voice.rack                = &rack;
		struct Setup {
			Ngs2VoiceParamHeader header {40, 0, 0x10000000};
			Ngs2WaveformFormat   format;
			uint32_t             flags = 0, reserved = 0;
		} setup;
		setup.header.id = type == Ngs2RackType::Sampler ? 0x10000000 : 0x40010000;
		setup.format    = {0x12, channels, rate};
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &setup.header) == OK,
		      "sampler setup failed");
		voice.state = Ngs2VoicePlayState::Playing;
	}

	void Queue(const int16_t* data, uint32_t frames, uint32_t flags, uint32_t repeats = 0,
	           uint32_t skip = 0) {
		Ngs2WaveformBlock block {
		    0,  size_t(frames + skip) * voice.channels * sizeof(int16_t), repeats, skip, frames, 0,
		    123};
		struct Blocks {
			Ngs2VoiceParamHeader     header {32, 0, 0x10000001};
			const void*              data;
			uint32_t                 flags, count;
			const Ngs2WaveformBlock* blocks;
		} param {{32, 0, 0x10000001}, data, flags, 1, &block};
		param.header.id = rack.type == Ngs2RackType::Sampler ? 0x10000001 : 0x40010001;
		Check(Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&voice), &param.header) == OK,
		      "sampler queue failed");
	}

	void Render(uint32_t frames) {
		voice.rendered = false;
		Ngs2RenderVoice(voice, {}, frames);
	}
};

void TestStreamingResample() {
	Fixture              f(44100, 2);
	std::vector<int16_t> pcm(441 * 2);
	for (int i = 0; i < 441; ++i) {
		pcm[i * 2]     = static_cast<int16_t>(i * 32);
		pcm[i * 2 + 1] = static_cast<int16_t>(-i * 32);
	}
	f.Queue(pcm.data(), 200, 1);
	f.Queue(pcm.data() + 400, 241, 0);
	for (uint32_t grain = 0; grain < 3; ++grain) {
		f.Render(160);
		for (uint32_t i = 0; i < 160; ++i) {
			const float expected = std::min((grain * 160 + i) * 44100.0 / 48000.0, 440.0) / 1024.0;
			Check(std::abs(f.voice.samples[i] - expected) < 0.00001f,
			      "resampling discontinuity across grain or block boundary");
			Check(std::abs(f.voice.samples[160 + i] + expected) < 0.00001f,
			      "stereo channels were mixed");
		}
	}
	Check(f.voice.blocks.empty(), "final streaming block did not finish");
	Check(f.voice.state == Ngs2VoicePlayState::Empty, "finished voice remained active");
	Ngs2SamplerVoiceState state {};
	Ngs2VoiceGetState(reinterpret_cast<uintptr_t>(&f.voice), &state.voice_state, sizeof(state));
	Check(state.num_decoded_samples == 441 && state.decoded_data_size == pcm.size() * 2,
	      "streaming progress does not match consumed source data");
}

void TestPitchLoopAndSkip() {
	Fixture       f(24000);
	const int16_t pcm[] = {-30000, 8192, 16384};
	f.Queue(pcm, 2, 0, 1, 1);
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x10000005};
		float                ratio    = 2.0f;
		uint32_t             reserved = 0;
	} pitch;
	Ngs2VoiceControl(reinterpret_cast<uintptr_t>(&f.voice), &pitch.header);
	f.Render(5);
	const float expected[] = {0.25f, 0.5f, 0.25f, 0.5f, 0.0f};
	for (size_t i = 0; i < 5; ++i) {
		Check(std::abs(f.voice.samples[i] - expected[i]) < 0.00001f,
		      "pitch, repeat count, or skipped samples incorrect");
	}
}

void TestStarvationAndPause() {
	Fixture       f(48000);
	const int16_t pcm[] = {16384, 8192};
	f.Queue(pcm, 2, 1);
	f.Render(4);
	Check(f.voice.state == Ngs2VoicePlayState::Playing, "stream starvation stopped voice");
	Check(f.voice.samples[2] == 0.0f, "starved output was not silent");
	f.Queue(pcm, 2, 0);
	f.voice.SetEvent(16);
	f.Render(2);
	Check(!f.voice.has_samples && f.voice.blocks.front().cursor == 0, "paused voice consumed data");
	f.voice.SetEvent(32);
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == 0.25f,
	      "stream failed to resume after starvation/pause");
}

void TestMonoRateAndRouting() {
	Fixture              f(22050);
	std::vector<int16_t> pcm(441, 16384);
	f.Queue(pcm.data(), 441, 0);
	Ngs2RackInternal mastering_rack {};
	mastering_rack.type = Ngs2RackType::Mastering;
	Ngs2VoiceInternal mastering;
	mastering.rack     = &mastering_rack;
	mastering.channels = 2;
	mastering.state    = Ngs2VoicePlayState::Playing;
	f.voice.matrices   = {{0.5f, 0.25f}};
	f.voice.ports.push_back({&mastering, 0, 1.0f, 0});
	for (int i = 0; i < 3; ++i) {
		mastering.rendered = false;
		f.voice.rendered   = false;
		Ngs2RenderVoice(mastering, {&f.voice, &mastering}, 320);
		Check(mastering.has_samples, "standard sampler did not reach mastering voice");
		for (uint32_t j = 0; j < 320; ++j) {
			Check(mastering.samples[j] == 0.25f && mastering.samples[320 + j] == 0.125f,
			      "mono PCM routing matrix was not applied");
		}
	}
	Check(f.voice.decoded_samples == 441 && f.voice.blocks.empty(),
	      "22.05 kHz effect duration was incorrect");
}

void TestCustomPcmStillPlays() {
	Fixture       f(48000, 1, Ngs2RackType::CustomSampler);
	const int16_t pcm[] = {8192, -16384};
	f.Queue(pcm, 2, 0);
	f.Render(4);
	Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == -0.5f &&
	          f.voice.samples[2] == 0.0f && f.voice.blocks.empty(),
	      "custom PCM sampler regressed");
}

void TestPlayStateBeforeRender() {
	Fixture       f(48000);
	const int16_t pcm[] = {8192, -16384};
	f.voice.state       = Ngs2VoicePlayState::Empty;
	f.Queue(pcm, 2, 0);
	Ngs2VoiceEventParam event {{sizeof(Ngs2VoiceEventParam), 0, 6}, 1};
	const auto handle = reinterpret_cast<uintptr_t>(&f.voice);
	Ngs2VoiceControl(handle, &event.header);
	uint32_t flags = 0;
	Ngs2VoiceGetStateFlags(handle, &flags);
	Check(flags == 3, "play command was reported idle before render");
	Ngs2SamplerVoiceState state {};
	Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state));
	Check(state.voice_state.state_flags == flags && state.num_decoded_samples == 0,
	      "state queries disagreed or play consumed samples before render");
	f.Render(4);
	Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == -0.5f,
	      "one-shot effect was dropped before its first render");
}

void TestStopThenPlayBeforeRender() {
	Fixture       f(48000);
	const int16_t pcm[] = {16384, -8192};
	f.Queue(pcm, 2, 0);
	f.voice.SetEvent(2);
	f.voice.SetEvent(1);
	Check(Ngs2GetStateFlags(&f.voice) == 3, "stopped voice could not restart");
	f.voice.SetEvent(8);
	f.voice.SetEvent(1);
	Check(Ngs2GetStateFlags(&f.voice) == 3, "kill/play commands lost their ordering");
	f.Render(2);
	Check(f.voice.samples[0] == 0.5f && f.voice.samples[1] == -0.25f,
	      "restarted voice produced no audio");
}

struct CallbackEvent {
	Ngs2VoiceInternal* voice;
	uint32_t           event;
	uint32_t           calls = 0;
};

void KYTY_SYSV_ABI ApplyCallbackEvent(const void* info) {
	const auto data    = *static_cast<const uintptr_t*>(info);
	auto&      context = *reinterpret_cast<CallbackEvent*>(data);
	if (++context.calls == 1) {
		context.voice->SetEvent(context.event);
	}
}

void TestCallbackStopsConsumption() {
	for (auto rack: {Ngs2RackType::Sampler, Ngs2RackType::CustomSampler}) {
		for (uint32_t event: {2u, 4u, 8u, 16u}) {
			Fixture       f(48000, 1, rack);
			const int16_t pcm[] = {8192, 16384};
			f.Queue(pcm, 1, 1);
			f.Queue(pcm + 1, 1, 1);
			CallbackEvent context {&f.voice, event};
			f.voice.callback       = reinterpret_cast<uintptr_t>(&ApplyCallbackEvent);
			f.voice.callback_data  = reinterpret_cast<uintptr_t>(&context);
			f.voice.callback_flags = 1;
			f.Render(2);
			Check(f.voice.state != Ngs2VoicePlayState::Playing && context.calls == 1,
			      "completion callback failed to interrupt playback");
			Check(f.voice.samples[0] == 0.25f && f.voice.samples[1] == 0.0f &&
			          f.voice.decoded_samples == 1 && f.voice.blocks.size() == 1 &&
			          f.voice.blocks.front().cursor == 0,
			      "callback pause/stop consumed the next queued block");
			if (event == 16) {
				f.voice.SetEvent(32);
				f.Render(1);
				Check(f.voice.samples[0] == 0.5f && f.voice.decoded_samples == 2,
				      "callback pause failed to preserve queued audio for resume");
			}
		}
	}
}

void TestCallbackPauseDuringLargeStep() {
	Fixture       f(48000);
	const int16_t pcm[] = {8192, 16384, 24576, 4096};
	f.Queue(pcm, 1, 1);
	f.Queue(pcm + 1, 1, 1);
	f.Queue(pcm + 2, 2, 1);
	f.voice.sample_step = uint64_t(120000) << 32u;
	CallbackEvent context {&f.voice, 16};
	f.voice.callback       = reinterpret_cast<uintptr_t>(&ApplyCallbackEvent);
	f.voice.callback_data  = reinterpret_cast<uintptr_t>(&context);
	f.voice.callback_flags = 1;
	f.Render(2);
	Check(context.calls == 1 && f.voice.decoded_samples == 1 &&
	          f.voice.sample_phase == (uint64_t(72000) << 32u),
	      "pause advanced across additional blocks in a large resampling step");
	f.voice.SetEvent(32);
	f.Render(1);
	Check(std::abs(f.voice.samples[0] - 0.4375f) < 1e-7f && context.calls == 3,
	      "resume lost fractional position or read past the next short block");
}

void TestUpstreamCustomSamplerControls() {
	Fixture f(24000, 1, Ngs2RackType::CustomSampler);
	const int16_t pcm[] = {8192, 16384, 24576, 4096};
	f.Queue(pcm, 4, 1);
	f.Render(1);
	Check(f.voice.samples[0] == 0.25f, "custom sampler source rate was rejected");
	f.Queue(pcm + 2, 2, 4);
	f.Render(1);
	Check(f.voice.samples[0] == 0.75f && f.voice.blocks.size() == 1,
	      "replacement blocks retained old data or fractional position");
	struct Pitch {
		Ngs2VoiceParamHeader header {16, 0, 0x40010005};
		float                ratio = 0.0f;
		uint32_t             reserved = 0;
	} pitch;
	const auto handle = reinterpret_cast<uintptr_t>(&f.voice);
	Ngs2VoiceControl(handle, &pitch.header);
	f.Render(2);
	Check(f.voice.samples[0] == 0.4375f && f.voice.samples[1] == 0.4375f,
	      "zero custom sampler pitch advanced playback");
	pitch.ratio = 4.0f;
	Ngs2VoiceControl(handle, &pitch.header);
	f.Render(1);
	Check(f.voice.blocks.empty(), "custom sampler pitch did not advance playback");
	f.rack.option.custom_sampler.custom_rack_option.state_size =
	    sizeof(Ngs2CustomSamplerVoiceState);
	Ngs2CustomSamplerVoiceState state {};
	Check(Ngs2VoiceGetState(handle, &state.voice_state, sizeof(state)) == OK &&
	          state.voice_state.state_flags == Ngs2GetStateFlags(&f.voice),
	      "custom sampler state layout regressed");
}

} // namespace

int main() {
	TestUpstreamCustomSamplerControls();
	TestCallbackStopsConsumption();
	TestCallbackPauseDuringLargeStep();
	TestStreamingResample();
	TestPitchLoopAndSkip();
	TestStarvationAndPause();
	TestMonoRateAndRouting();
	TestCustomPcmStillPlays();
	TestPlayStateBeforeRender();
	TestStopThenPlayBeforeRender();
	std::puts("Ngs2SamplerTests: all cases passed");
}
