// Exercise the haptics sample conversion and device matching without a host audio device.
#include "libs/controller.h"
#include "libs/padHaptics.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Libs::Controller {

void SetAudioHapticsPlaying(bool /*playing*/) {
	std::fprintf(stderr, "PadHapticsTests: failed: unexpected SetAudioHapticsPlaying\n");
	std::abort();
}

} // namespace Libs::Controller

namespace {

namespace PadHaptics = Libs::Audio::PadHaptics;

constexpr int VOLUME_UNITY = 32768;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "PadHapticsTests: failed: %s\n", text);
		std::abort();
	}
}

// A frame of the controller's channels: silent speaker, then the actuators.
bool FrameIs(const float* frame, float left, float right) {
	return std::abs(frame[0]) < 1e-7f && std::abs(frame[1]) < 1e-7f &&
	       std::abs(frame[2] - left) < 1e-7f && std::abs(frame[3] - right) < 1e-7f;
}

void TestStereoFloatGoesToBackChannels() {
	const std::array<float, 4> input {0.5f, -0.25f, 1.0f, 0.0f};
	const std::array<int, 2>   volume {VOLUME_UNITY, VOLUME_UNITY / 2};
	std::array<float, 8>       out {};
	out.fill(9.0f);

	const float peak =
	    PadHaptics::ToControllerFrames(input.data(), 2, 2, true, volume.data(), out.data());

	Check(FrameIs(out.data(), 0.5f, -0.125f), "stereo float: first frame");
	Check(FrameIs(out.data() + 4, 1.0f, 0.0f), "stereo float: second frame");
	Check(std::abs(peak - 1.0f) < 1e-7f, "stereo float: peak");
}

void TestMono16DrivesBothActuators() {
	const std::array<int16_t, 2> input {16384, -32768};
	const std::array<int, 1>     volume {VOLUME_UNITY};
	std::array<float, 8>         out {};

	const float peak =
	    PadHaptics::ToControllerFrames(input.data(), 2, 1, false, volume.data(), out.data());

	Check(FrameIs(out.data(), 0.5f, 0.5f), "mono s16: first frame");
	Check(FrameIs(out.data() + 4, -1.0f, -1.0f), "mono s16: second frame");
	Check(std::abs(peak - 1.0f) < 1e-7f, "mono s16: peak");
}

void TestExtraChannelsAreIgnored() {
	const std::array<float, 8> input {0.1f, 0.2f, 0.9f, 0.9f, 0.3f, 0.4f, 0.9f, 0.9f};
	const std::array<int, 4>   volume {VOLUME_UNITY, VOLUME_UNITY, VOLUME_UNITY, VOLUME_UNITY};
	std::array<float, 8>       out {};

	const float peak =
	    PadHaptics::ToControllerFrames(input.data(), 2, 4, true, volume.data(), out.data());

	Check(FrameIs(out.data(), 0.1f, 0.2f), "four channels: first frame");
	Check(FrameIs(out.data() + 4, 0.3f, 0.4f), "four channels: second frame");
	Check(std::abs(peak - 0.4f) < 1e-7f, "four channels: peak");
}

void TestSilenceHasNoPeak() {
	const std::array<float, 4> input {};
	const std::array<int, 2>   volume {VOLUME_UNITY, VOLUME_UNITY};
	std::array<float, 8>       out {};

	const float peak =
	    PadHaptics::ToControllerFrames(input.data(), 2, 2, true, volume.data(), out.data());

	Check(peak == 0.0f, "silence: peak");
}

void TestControllerNames() {
	Check(PadHaptics::IsControllerName("Speakers (DualSense Wireless Controller)"),
	      "names: Windows");
	Check(PadHaptics::IsControllerName("Speakers (2- DualSense Wireless Controller)"),
	      "names: Windows, second instance");
	Check(PadHaptics::IsControllerName("DualSense Edge Wireless Controller Analog Surround 4.0"),
	      "names: DualSense Edge");
	Check(PadHaptics::IsControllerName("dualsense wireless controller"), "names: case");
	Check(!PadHaptics::IsControllerName("Speakers (Wireless Controller)"), "names: DualShock 4");
	Check(!PadHaptics::IsControllerName("Speakers (High Definition Audio Device)"),
	      "names: other device");
	Check(!PadHaptics::IsControllerName(nullptr), "names: null");
}

void TestPlaybackRatioHoldsTheQueue() {
	constexpr int target = 48000 * 40 / 1000 * 4 * static_cast<int>(sizeof(float)); // 40 ms

	Check(PadHaptics::PlaybackRatio(0, target) < 1.0f, "ratio: underrun slows down");
	Check(PadHaptics::PlaybackRatio(target / 2 - 1, target) < 1.0f, "ratio: low queue slows down");
	Check(PadHaptics::PlaybackRatio(target / 2, target) == 1.0f, "ratio: low edge");
	Check(PadHaptics::PlaybackRatio(target, target) == 1.0f, "ratio: on target");
	Check(PadHaptics::PlaybackRatio(target + target / 2, target) == 1.0f, "ratio: high edge");
	Check(PadHaptics::PlaybackRatio(2 * target, target) > 1.0f, "ratio: backlog speeds up");
}

void TestInvalidArgumentsAreIgnored() {
	const std::array<float, 2> input {1.0f, 1.0f};
	const std::array<int, 2>   volume {VOLUME_UNITY, VOLUME_UNITY};

	PadHaptics::Queue(nullptr, input.data(), 1, 2, true, volume.data());
	PadHaptics::Close(nullptr);
	Check(PadHaptics::Open(0) == nullptr, "open: no sample rate");
}

} // namespace

int main() {
	TestStereoFloatGoesToBackChannels();
	TestMono16DrivesBothActuators();
	TestExtraChannelsAreIgnored();
	TestSilenceHasNoPeak();
	TestControllerNames();
	TestPlaybackRatioHoldsTheQueue();
	TestInvalidArgumentsAreIgnored();
	std::printf("PadHapticsTests: all cases passed\n");
	return 0;
}
