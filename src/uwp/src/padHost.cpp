#include "padHost.h"

#include "libs/controller.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

// The app's pad state goes to the emulator's controller library as host input (the library's keyboard-style path, which is always connected once the
// emulator started): player 1 is the game's pad. The state is forwarded as changes, since every call adds a state to the pad's history.
namespace Kyty::Uwp::Pad {

namespace {

namespace C = Libs::Controller;

constexpr uint32_t BUTTONS[] = {PAD_BUTTON_L3,  PAD_BUTTON_R3,       PAD_BUTTON_OPTIONS, PAD_BUTTON_UP,     PAD_BUTTON_RIGHT, PAD_BUTTON_DOWN,
                                PAD_BUTTON_LEFT, PAD_BUTTON_L1,      PAD_BUTTON_R1,      PAD_BUTTON_TRIANGLE, PAD_BUTTON_CIRCLE, PAD_BUTTON_CROSS,
                                PAD_BUTTON_SQUARE};

constexpr float STANDARD_GRAVITY = 9.80665f;
constexpr float PI               = 3.14159265f;
// The emulated motion: full stick deflection turns the pad 180 degrees, and the pad follows the stick at up to 1800 degrees a second, inside the
// range of a DualSense's gyroscope, so that a game sees a motion a real pad could make.
constexpr float MAX_ANGLE = PI;
constexpr float MAX_RATE  = 10.0f * PI;

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

struct Quat {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 1.0f;
};

Quat Multiply(const Quat& a, const Quat& b) {
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat Conjugate(const Quat& q) {
	return {-q.x, -q.y, -q.z, q.w};
}

Quat AxisAngle(float x, float y, float z, float angle) {
	const float s = std::sin(angle * 0.5f);
	return {x * s, y * s, z * s, std::cos(angle * 0.5f)};
}

// The pad's orientation for a state (pad to world; y is up when the pad lies flat): turned about the world's up axis, leaned about its forward
// axis, tipped about its left-right axis.
Quat Target(const HostPadState& state) {
	return Multiply(Multiply(AxisAngle(0.0f, 1.0f, 0.0f, -state.turn * MAX_ANGLE), AxisAngle(0.0f, 0.0f, 1.0f, state.tilt_x * MAX_ANGLE)),
	                AxisAngle(1.0f, 0.0f, 0.0f, -state.tilt_y * MAX_ANGLE));
}

// The emulated pad: its orientation, when its sensors were last sent, and whether it rests at its target (and was sent so).
Quat     g_motion {};
uint64_t g_motion_time    = 0;
bool     g_motion_settled = true;

// The motion sensors as the pad feels them: gravity in the pad's frame, and the turn per second (in the pad's frame too, as a gyroscope measures).
void SendMotion(const std::array<float, 3>& rate, uint64_t now) {
	const Quat  up       = Multiply(Multiply(Conjugate(g_motion), {0.0f, STANDARD_GRAVITY, 0.0f, 0.0f}), g_motion);
	const float accel[3] = {up.x, up.y, up.z};
	C::SetSensor(C::HOST_INPUT_CONTROLLER_ID, C::Sensor::Accel, accel, now);
	C::SetSensor(C::HOST_INPUT_CONTROLLER_ID, C::Sensor::Gyro, rate.data(), now);
	g_motion_time = now;
}

// One step of the emulated pad toward `target`, at most MAX_RATE for the time since the last one. The controller library turns its own
// orientation by the gyroscope's rate times the same time, so it follows the pad exactly.
void StepMotion(const Quat& target) {
	const auto now   = NowMicroseconds();
	Quat       delta = Multiply(Conjugate(g_motion), target);
	if (delta.w < 0.0f) {
		delta = {-delta.x, -delta.y, -delta.z, -delta.w}; // the shorter way round
	}
	const float half_sin = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
	const float angle    = 2.0f * std::atan2(half_sin, delta.w);
	if (angle < 1e-4f) {
		if (!g_motion_settled) {
			g_motion = target;
			SendMotion({0.0f, 0.0f, 0.0f}, now); // at rest: the turning stops
			g_motion_settled = true;
		}
		return;
	}
	// The library ignores a gyroscope sample that comes long after the one before: a motion starts with a sample at rest, the first step
	// follows on the next poll.
	const uint64_t elapsed = now - g_motion_time;
	if (g_motion_settled || now <= g_motion_time || elapsed > 90000) {
		SendMotion({0.0f, 0.0f, 0.0f}, now);
		g_motion_settled = false;
		return;
	}
	const float dt   = static_cast<float>(elapsed) * 0.000001f;
	const float step = std::min(angle, MAX_RATE * dt);
	const float x    = delta.x / half_sin;
	const float y    = delta.y / half_sin;
	const float z    = delta.z / half_sin;
	g_motion         = Multiply(g_motion, AxisAngle(x, y, z, step));
	const float norm = std::sqrt(g_motion.x * g_motion.x + g_motion.y * g_motion.y + g_motion.z * g_motion.z + g_motion.w * g_motion.w);
	g_motion         = {g_motion.x / norm, g_motion.y / norm, g_motion.z / norm, g_motion.w / norm};
	SendMotion({x * step / dt, y * step / dt, z * step / dt}, now);
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
		last             = {};
		g_ready_before   = true;
		g_motion         = {};
		g_motion_settled = true;
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
	last = state;
}

void StepPlayerMotion(int player) {
	if (player != 0) {
		return;
	}
	std::lock_guard lock(g_mutex);
	if (!g_ready_before || C::GetActiveControllerId() == -1) {
		return;
	}
	StepMotion(Target(g_last));
}

void StartAutoPress(uint32_t interval_ms) {
	static std::atomic_bool started = false;
	if (interval_ms == 0 || started.exchange(true)) {
		return;
	}
	Log("auto press: Cross (A on an Xbox controller) is pressed every %u ms\n", interval_ms);
	constexpr DWORD HOLD_MS = 120;
	std::thread([interval_ms] {
		uint32_t presses = 0;
		for (;;) {
			Sleep(interval_ms);
			if (C::GetActiveControllerId() == -1) {
				continue; // the game's controller library does not exist yet
			}
			const auto id = C::HOST_INPUT_CONTROLLER_ID;
			{
				std::lock_guard lock(g_mutex);
				C::SetButton(id, PAD_BUTTON_CROSS, true);
			}
			Sleep(HOLD_MS);
			{
				std::lock_guard lock(g_mutex);
				C::SetButton(id, PAD_BUTTON_CROSS, false);
			}
			if (++presses == 1 || presses % 30 == 0) {
				Log("auto press: %u presses so far\n", presses);
			}
		}
	}).detach();
}

HostPadFeedback GetPlayerFeedback(int player) {
	if (player != 0) {
		return {};
	}
	const auto feedback = C::GetHostFeedback();
	return {feedback.large_motor,       feedback.small_motor,       feedback.trigger_resistance, feedback.trigger_vibration,
	        feedback.trigger_frequency, feedback.trigger_shots,     feedback.haptic_left,        feedback.haptic_right};
}

} // namespace Kyty::Uwp::Pad
