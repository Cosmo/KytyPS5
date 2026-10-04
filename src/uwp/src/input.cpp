#include "input.h"

#include "padHost.h"
#include "log.h"

#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Gaming.Input.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace Controller = Kyty::Uwp::Pad;
namespace gaming     = winrt::Windows::Gaming::Input;
using Clock          = std::chrono::steady_clock;

constexpr auto POLL_INTERVAL = std::chrono::milliseconds(4);
constexpr auto SCAN_INTERVAL = std::chrono::milliseconds(250);
// Wireless controllers can briefly drop out of Gamepad::Gamepads(); their player stays connected,
// with neutral input, this long.
constexpr auto MISSING_GRACE = std::chrono::seconds(2);
// With View held: the left stick deflection that puts the finger down, and the right stick's dead
// zone for tilting.
constexpr double TOUCH_START    = 0.25;
constexpr double TILT_DEAD_ZONE = 0.1;
// Adaptive triggers resist while squeezed; an Xbox controller rumbles the trigger instead, while it
// moves deeper than the dead zone and briefly after.
constexpr uint8_t TRIGGER_DEAD_ZONE   = 12;
constexpr auto    TRIGGER_RUMBLE_HOLD = std::chrono::milliseconds(60);

constexpr std::array BUTTONS = {
    std::pair {gaming::GamepadButtons::A, Controller::PAD_BUTTON_CROSS},
    std::pair {gaming::GamepadButtons::B, Controller::PAD_BUTTON_CIRCLE},
    std::pair {gaming::GamepadButtons::X, Controller::PAD_BUTTON_SQUARE},
    std::pair {gaming::GamepadButtons::Y, Controller::PAD_BUTTON_TRIANGLE},
    std::pair {gaming::GamepadButtons::Menu, Controller::PAD_BUTTON_OPTIONS},
    std::pair {gaming::GamepadButtons::LeftShoulder, Controller::PAD_BUTTON_L1},
    std::pair {gaming::GamepadButtons::RightShoulder, Controller::PAD_BUTTON_R1},
    std::pair {gaming::GamepadButtons::LeftThumbstick, Controller::PAD_BUTTON_L3},
    std::pair {gaming::GamepadButtons::RightThumbstick, Controller::PAD_BUTTON_R3},
    std::pair {gaming::GamepadButtons::DPadUp, Controller::PAD_BUTTON_UP},
    std::pair {gaming::GamepadButtons::DPadRight, Controller::PAD_BUTTON_RIGHT},
    std::pair {gaming::GamepadButtons::DPadDown, Controller::PAD_BUTTON_DOWN},
    std::pair {gaming::GamepadButtons::DPadLeft, Controller::PAD_BUTTON_LEFT},
};

// A local player's controller. Only the polling thread uses these.
struct Player {
	gaming::Gamepad pad {nullptr};
	// RawGameController::NonRoamableId: Gamepad objects can be replaced while the controller stays
	// connected.
	std::wstring      id;
	bool              missing = false;
	Clock::time_point missing_since;
	// Buttons held when the controller was assigned (the A that started the game): ignored until
	// released.
	uint32_t blocked  = 0;
	bool     touching = false;
	// For the launcher's activity mark: the last reading.
	gaming::GamepadReading previous {};
	// View + Menu are held for the game menu (or, with both triggers, the overlay): the game gets
	// nothing until they are released.
	bool chord          = false;
	bool chord_triggers = false;

	Controller::HostPadState sent;
	// Large and small motor, left and right trigger.
	std::array<uint8_t, 4>           vibration {};
	std::array<uint8_t, 2>           trigger_position {};
	std::array<Clock::time_point, 2> trigger_squeezed {};
};

std::array<Player, Controller::MAX_PLAYERS> g_players;

std::mutex                  g_shared_mutex; // guards the chord handlers and g_controllers
std::function<void()>       g_menu_handler;
std::function<void()>       g_overlay_handler;
std::vector<ControllerInfo> g_controllers;
std::atomic_bool            g_paused  = false;
// The game gets controller input (ActivateGameInput); g_activate asks the input thread for it.
std::atomic_bool            g_game_input = false;
std::atomic_bool            g_activate   = false;
// Per player, when its controller last had input (steady clock, milliseconds; 0: none yet).
std::array<std::atomic<int64_t>, Controller::MAX_PLAYERS> g_last_input {};
// Set when the game resumes: the input thread then blocks the buttons still held.
std::atomic_bool            g_resumed = false;

void RunChord(bool overlay) {
	Log(overlay ? "chord: View + Menu + triggers (overlay)\n" : "chord: View + Menu (game menu)\n");
	std::function<void()> handler;
	{
		std::lock_guard lock(g_shared_mutex);
		handler = overlay ? g_overlay_handler : g_menu_handler;
	}
	if (handler) {
		handler();
	}
}

void ListControllers() {
	std::vector<ControllerInfo> controllers;
	for (int index = 0; index < Controller::MAX_PLAYERS; index++) {
		if (const auto& pad = g_players[index].pad; pad != nullptr) {
			std::wstring name = L"Controller";
			if (auto raw = gaming::RawGameController::FromGameController(pad)) {
				name = std::wstring(raw.DisplayName());
			}
			controllers.push_back({index, std::move(name)});
		}
	}
	std::lock_guard lock(g_shared_mutex);
	g_controllers = std::move(controllers);
}

std::wstring ControllerId(gaming::Gamepad const& pad) {
	if (auto raw = gaming::RawGameController::FromGameController(pad)) {
		return std::wstring(raw.NonRoamableId());
	}
	return {};
}

bool IsController(const Player& player, gaming::Gamepad const& pad, const std::wstring& id) {
	return player.pad != nullptr && (id.empty() ? player.pad == pad : player.id == id);
}

void SetVibration(gaming::Gamepad const& pad, const std::array<uint8_t, 4>& strengths) {
	gaming::GamepadVibration vibration {};
	vibration.LeftMotor    = strengths[0] / 255.0;
	vibration.RightMotor   = strengths[1] / 255.0;
	vibration.LeftTrigger  = strengths[2] / 255.0;
	vibration.RightTrigger = strengths[3] / 255.0;
	pad.Vibration(vibration);
}

uint8_t StickAxis(double value) {
	return static_cast<uint8_t>(std::lround((std::clamp(value, -1.0, 1.0) + 1.0) * 127.5));
}

uint8_t TriggerAxis(double value) {
	return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

float Tilt(double value) {
	const double magnitude = std::min(std::abs(value), 1.0);
	if (magnitude < TILT_DEAD_ZONE) {
		return 0.0f;
	}
	return static_cast<float>(
	    std::copysign((magnitude - TILT_DEAD_ZONE) / (1.0 - TILT_DEAD_ZONE), value));
}

Controller::HostPadState MapReading(Player& player, const gaming::GamepadReading& reading) {
	auto held = static_cast<uint32_t>(reading.Buttons);
	player.blocked &= held;
	held &= ~player.blocked;
	const auto is_held = [held](gaming::GamepadButtons button) {
		return (held & static_cast<uint32_t>(button)) != 0;
	};

	// View + Menu: decided on release, so the order of the presses doesn't matter. The game menu,
	// or the overlay when both triggers were pulled as well. Until then the game gets nothing.
	const bool view     = is_held(gaming::GamepadButtons::View);
	const bool menu     = is_held(gaming::GamepadButtons::Menu);
	const bool triggers = reading.LeftTrigger > 0.5 && reading.RightTrigger > 0.5;
	if (view && menu) {
		player.chord = true;
	}
	if (player.chord) {
		player.chord_triggers = player.chord_triggers || triggers;
		const bool released   = !view && !menu && (!player.chord_triggers ||
		                                          (reading.LeftTrigger < 0.1 && reading.RightTrigger < 0.1));
		if (released) {
			const bool overlay    = player.chord_triggers;
			player.chord          = false;
			player.chord_triggers = false;
			RunChord(overlay);
		}
		player.touching = false;
		return {};
	}

	Controller::HostPadState state;
	for (const auto& [button, pad_button]: BUTTONS) {
		if (is_held(button)) {
			state.buttons |= pad_button;
		}
	}
	// PS5 sticks point down for positive Y.
	state.axes = {StickAxis(reading.LeftThumbstickX),  StickAxis(-reading.LeftThumbstickY),
	              StickAxis(reading.RightThumbstickX), StickAxis(-reading.RightThumbstickY),
	              TriggerAxis(reading.LeftTrigger),    TriggerAxis(reading.RightTrigger)};

	if (!is_held(gaming::GamepadButtons::View)) {
		player.touching = false;
		return state;
	}

	// View held: the left stick is a finger on the touchpad, from when it moves (or clicks) until
	// View is released, and its click is the touchpad click; the right stick tilts the controller.
	const double x          = reading.LeftThumbstickX;
	const double y          = reading.LeftThumbstickY;
	const bool   left_click = is_held(gaming::GamepadButtons::LeftThumbstick);
	if (left_click || std::hypot(x, y) > TOUCH_START) {
		player.touching = true;
	}
	state.buttons &= ~Controller::PAD_BUTTON_L3;
	if (left_click) {
		state.buttons |= Controller::PAD_BUTTON_TOUCH_PAD;
	}
	if (player.touching) {
		state.touch   = true;
		state.touch_x = static_cast<float>((std::clamp(x, -1.0, 1.0) + 1.0) / 2.0);
		state.touch_y = static_cast<float>((1.0 - std::clamp(y, -1.0, 1.0)) / 2.0);
	}
	state.tilt_x = Tilt(reading.RightThumbstickX);
	state.tilt_y = Tilt(-reading.RightThumbstickY);
	std::fill_n(state.axes.begin(), 4, uint8_t {128});
	return state;
}

uint8_t TriggerRumble(uint8_t strength, uint8_t position, Clock::time_point squeezed,
                      Clock::time_point now) {
	if (strength == 0 || position <= TRIGGER_DEAD_ZONE || now - squeezed > TRIGGER_RUMBLE_HOLD) {
		return 0;
	}
	return static_cast<uint8_t>(strength * (position - TRIGGER_DEAD_ZONE) /
	                            (255 - TRIGGER_DEAD_ZONE));
}

void ApplyFeedback(int index, Player& player, const Controller::HostPadState& state,
                   Clock::time_point now) {
	for (int trigger = 0; trigger < 2; trigger++) {
		const auto position = state.axes[static_cast<int>(Controller::Axis::TriggerLeft) + trigger];
		if (position > player.trigger_position[trigger]) {
			player.trigger_squeezed[trigger] = now;
		}
		player.trigger_position[trigger] = position;
	}
	const auto                   feedback = Controller::GetPlayerFeedback(index);
	const std::array<uint8_t, 4> vibration {
	    feedback.large_motor, feedback.small_motor,
	    TriggerRumble(feedback.left_trigger, player.trigger_position[0], player.trigger_squeezed[0],
	                  now),
	    TriggerRumble(feedback.right_trigger, player.trigger_position[1],
	                  player.trigger_squeezed[1], now)};
	if (vibration != player.vibration) {
		SetVibration(player.pad, vibration);
		player.vibration = vibration;
	}
}

void Assign(int index, gaming::Gamepad const& pad, std::wstring id) {
	auto& player   = g_players[index];
	player         = {};
	player.pad     = pad;
	player.id      = std::move(id);
	player.blocked  = static_cast<uint32_t>(pad.GetCurrentReading().Buttons);
	player.previous = pad.GetCurrentReading();
	SetVibration(pad, {});
	if (g_game_input) {
		Controller::SetPlayerConnected(index, true);
	}
	Log("controller connected: player %d\n", index + 1);
	ListControllers();
}

void Release(int index) {
	auto& player = g_players[index];
	try {
		SetVibration(player.pad, {});
	} catch (const winrt::hresult_error&) {
	}
	player              = {};
	g_last_input[index] = 0;
	if (g_game_input) {
		Controller::SetPlayerState(index, {});
		Controller::SetPlayerConnected(index, false);
	}
	Log("controller disconnected: player %d\n", index + 1);
	ListControllers();
}

// Matches the connected controllers to the players; new ones take the lowest free player.
void Scan(Clock::time_point now) {
	std::vector<std::pair<gaming::Gamepad, std::wstring>> connected;
	for (auto const& pad: gaming::Gamepad::Gamepads()) {
		connected.emplace_back(pad, ControllerId(pad));
	}

	for (int index = 0; index < Controller::MAX_PLAYERS; index++) {
		auto& player = g_players[index];
		if (player.pad == nullptr) {
			continue;
		}
		const auto found = std::find_if(connected.begin(), connected.end(), [&](const auto& pad) {
			return IsController(player, pad.first, pad.second);
		});
		if (found != connected.end()) {
			player.pad     = found->first;
			player.missing = false;
			connected.erase(found);
		} else if (!player.missing) {
			player.missing       = true;
			player.missing_since = now;
		} else if (now - player.missing_since >= MISSING_GRACE) {
			Release(index);
		}
	}

	for (auto& [pad, id]: connected) {
		const auto free = std::find_if(g_players.begin(), g_players.end(),
		                               [](const Player& player) { return player.pad == nullptr; });
		if (free == g_players.end()) {
			break;
		}
		Assign(static_cast<int>(free - g_players.begin()), pad, std::move(id));
	}
}

// Whether the controller was used between two readings: a button, or a stick or trigger moved.
bool HadInput(const gaming::GamepadReading& before, const gaming::GamepadReading& after) {
	constexpr double moved = 0.2;
	return before.Buttons != after.Buttons ||
	       std::abs(before.LeftThumbstickX - after.LeftThumbstickX) > moved ||
	       std::abs(before.LeftThumbstickY - after.LeftThumbstickY) > moved ||
	       std::abs(before.RightThumbstickX - after.RightThumbstickX) > moved ||
	       std::abs(before.RightThumbstickY - after.RightThumbstickY) > moved ||
	       std::abs(before.LeftTrigger - after.LeftTrigger) > moved ||
	       std::abs(before.RightTrigger - after.RightTrigger) > moved;
}

void Update(int index, Clock::time_point now) {
	auto& player = g_players[index];
	if (player.pad == nullptr) {
		return;
	}
	Controller::HostPadState state;
	if (!player.missing) {
		try {
			const auto reading = player.pad.GetCurrentReading();
			if (HadInput(player.previous, reading)) {
				g_last_input[index] =
				    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
			}
			player.previous = reading;
			if (!g_game_input) {
				return;
			}
			if (g_paused) {
				player.touching = false;
				if (player.vibration != std::array<uint8_t, 4> {}) {
					SetVibration(player.pad, {});
					player.vibration = {};
				}
			} else {
				state = MapReading(player, reading);
				ApplyFeedback(index, player, state, now);
			}
		} catch (const winrt::hresult_error&) {
			player.missing       = true;
			player.missing_since = now;
			state                = {};
		}
	}
	if (g_game_input && state != player.sent) {
		Controller::SetPlayerState(index, state);
		player.sent = state;
	}
}

void PollControllers() {
	winrt::init_apartment();
	// Windows fills Gamepad::Gamepads() in the background once the app listens for controllers.
	gaming::Gamepad::GamepadAdded([](auto const&, auto const&) {});

	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                                      TIMER_ALL_ACCESS);
	auto   next_scan = Clock::now();
	for (;;) {
		const auto now = Clock::now();
		if (now >= next_scan) {
			try {
				Scan(now);
			} catch (const winrt::hresult_error& error) {
				Log("controller scan failed: 0x%08x\n", static_cast<uint32_t>(error.code()));
			}
			next_scan = now + SCAN_INTERVAL;
		}
		if (g_activate.exchange(false)) {
			// The game starts: the connected controllers become its players. Buttons still held
			// (the A that started it) count once released.
			for (int index = 0; index < Controller::MAX_PLAYERS; index++) {
				auto& player = g_players[index];
				if (player.pad != nullptr) {
					Controller::SetPlayerConnected(index, true);
					try {
						player.blocked = static_cast<uint32_t>(player.pad.GetCurrentReading().Buttons);
					} catch (const winrt::hresult_error&) {
					}
				}
			}
			g_game_input = true;
		}
		if (g_resumed.exchange(false)) {
			for (auto& player: g_players) {
				if (player.pad != nullptr && !player.missing) {
					try {
						player.blocked = static_cast<uint32_t>(player.pad.GetCurrentReading().Buttons);
					} catch (const winrt::hresult_error&) {
					}
				}
			}
		}
		for (int index = 0; index < Controller::MAX_PLAYERS; index++) {
			Update(index, now);
		}

		LARGE_INTEGER due {};
		due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(POLL_INTERVAL).count() / 100;
		if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
			WaitForSingleObject(timer, INFINITE);
		} else {
			Sleep(static_cast<DWORD>(POLL_INTERVAL.count()));
		}
	}
}

} // namespace

void OnChords(std::function<void()> menu, std::function<void()> overlay) {
	std::lock_guard lock(g_shared_mutex);
	g_menu_handler    = std::move(menu);
	g_overlay_handler = std::move(overlay);
}

std::vector<ControllerInfo> ConnectedControllers() {
	std::vector<ControllerInfo> controllers;
	{
		std::lock_guard lock(g_shared_mutex);
		controllers = g_controllers;
	}
	int64_t newest = 0;
	for (auto& controller: controllers) {
		newest = std::max(newest, g_last_input[controller.player].load());
	}
	for (auto& controller: controllers) {
		controller.latest = newest != 0 && g_last_input[controller.player] == newest;
	}
	return controllers;
}

void ActivateGameInput() {
	g_activate = true;
}

void PauseGameInput(bool paused) {
	if (g_paused.exchange(paused) && !paused) {
		g_resumed = true;
	}
}

void StartInput() {
	static std::atomic_bool started = false;
	if (!started.exchange(true)) {
		// Runs until the app exits.
		std::thread(PollControllers).detach();
	}
}

} // namespace Kyty::Uwp
