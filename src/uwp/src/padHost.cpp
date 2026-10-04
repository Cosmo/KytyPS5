#include "padHost.h"

#include "libs/controller.h"

#include <windows.h>

#include <cmath>
#include <mutex>

// The app's pad state goes to the emulator's controller library as host input (the library's keyboard-style path, which is always connected once the
// emulator started): player 1 is the game's pad. The state is forwarded as changes, since every call adds a state to the pad's history.
namespace Kyty::Uwp::Pad {

namespace {

namespace C = Libs::Controller;

constexpr uint32_t BUTTONS[] = {PAD_BUTTON_L3,  PAD_BUTTON_R3,       PAD_BUTTON_OPTIONS, PAD_BUTTON_UP,     PAD_BUTTON_RIGHT, PAD_BUTTON_DOWN,
                                PAD_BUTTON_LEFT, PAD_BUTTON_L1,      PAD_BUTTON_R1,      PAD_BUTTON_TRIANGLE, PAD_BUTTON_CIRCLE, PAD_BUTTON_CROSS,
                                PAD_BUTTON_SQUARE};

constexpr float    STANDARD_GRAVITY = 9.80665f;
constexpr float    MAX_TILT         = 0.7853982f; // 45 degrees

std::mutex   g_mutex;
HostPadState g_last {};
bool         g_ready_before = false;

bool Valid(int player) {
	return player >= 0 && player < MAX_PLAYERS;
}

uint64_t NowMicroseconds() {
	LARGE_INTEGER counter {};
	LARGE_INTEGER frequency {};
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return static_cast<uint64_t>(counter.QuadPart) * 1000000u / static_cast<uint64_t>(frequency.QuadPart);
}

// The pad's tilt as the accelerometer would feel it: gravity rotated by the tilt (the pad flat is gravity along +y).
void SendTilt(float tilt_x, float tilt_y) {
	const float a        = tilt_x * MAX_TILT;
	const float b        = tilt_y * MAX_TILT;
	const float accel[3] = {STANDARD_GRAVITY * std::sin(a), STANDARD_GRAVITY * std::cos(a) * std::cos(b), STANDARD_GRAVITY * std::cos(a) * std::sin(b)};
	const float gyro[3]  = {0.0f, 0.0f, 0.0f};
	const auto  now      = NowMicroseconds();
	C::SetSensor(C::HOST_INPUT_CONTROLLER_ID, C::Sensor::Accel, accel, now);
	C::SetSensor(C::HOST_INPUT_CONTROLLER_ID, C::Sensor::Gyro, gyro, now);
}

} // namespace

void SetPlayerConnected(int /*player*/, bool /*connected*/) {
	// The game's pad is always connected (as the keyboard's is on a desktop).
}

void SetPlayerState(int player, const HostPadState& state) {
	if (player != 0) {
		return; // one local player for now
	}
	std::lock_guard lock(g_mutex);
	// Until the emulator's controller library exists there is nothing to forward to; once it does, the first state goes in full.
	const bool ready = C::GetActiveControllerId() != -1;
	if (!ready) {
		g_ready_before = false;
		return;
	}
	const auto id   = C::HOST_INPUT_CONTROLLER_ID;
	auto&      last = g_last;
	if (!g_ready_before) {
		last           = {};
		g_ready_before = true;
	}

	const uint32_t changed = state.buttons ^ last.buttons;
	for (const auto button: BUTTONS) {
		if ((changed & button) != 0) {
			C::SetButton(id, button, (state.buttons & button) != 0);
		}
	}
	constexpr C::Axis AXES[] = {C::Axis::LeftX, C::Axis::LeftY, C::Axis::RightX, C::Axis::RightY, C::Axis::TriggerLeft, C::Axis::TriggerRight};
	for (size_t i = 0; i < 6; i++) {
		if (state.axes[i] != last.axes[i]) {
			C::SetAxis(id, AXES[i], state.axes[i]);
		}
	}
	// A finger on the touchpad also presses its button in the library; a click without a finger is the button alone.
	if (state.touch != last.touch || (state.touch && (state.touch_x != last.touch_x || state.touch_y != last.touch_y))) {
		C::SetTouchPad(id, 0, state.touch, state.touch_x, state.touch_y);
	}
	if (!state.touch && (changed & PAD_BUTTON_TOUCH_PAD) != 0) {
		C::SetButton(id, PAD_BUTTON_TOUCH_PAD, (state.buttons & PAD_BUTTON_TOUCH_PAD) != 0);
	}
	if (state.tilt_x != last.tilt_x || state.tilt_y != last.tilt_y) {
		SendTilt(state.tilt_x, state.tilt_y);
	}
	last = state;
}

HostPadFeedback GetPlayerFeedback(int player) {
	if (player != 0) {
		return {};
	}
	const auto feedback = C::GetHostFeedback();
	return {feedback.large_motor, feedback.small_motor, 0, 0};
}

} // namespace Kyty::Uwp::Pad
