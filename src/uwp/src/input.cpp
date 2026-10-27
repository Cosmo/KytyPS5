#include "input.h"
#include "dsu.h"
#include "fileAccess.h"

#include "padHost.h"
#include "log.h"

#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Devices.Power.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Kyty::Uwp {

namespace {

namespace Controller = Kyty::Uwp::Pad;
namespace gaming     = winrt::Windows::Gaming::Input;
using Clock          = std::chrono::steady_clock;

constexpr auto POLL_INTERVAL = std::chrono::milliseconds(4);
constexpr auto SCAN_INTERVAL = std::chrono::milliseconds(250);
// How often the scan looks up the chat headset (ChatHeadset).
constexpr auto CHAT_INTERVAL = std::chrono::seconds(2);
// Wireless controllers can briefly drop out of Gamepad::Gamepads(); their player stays connected,
// with neutral input, this long.
constexpr auto MISSING_GRACE = std::chrono::seconds(2);
// With View held: the left stick deflection that puts the finger down, and the right stick's dead
// zone for tilting.
constexpr double TOUCH_START    = 0.25;
constexpr double TILT_DEAD_ZONE = 0.1;
// Identify (IdentifyController): how long and how strongly an Xbox controller rumbles.
constexpr auto    IDENTIFY_TIME   = std::chrono::milliseconds(500);
constexpr uint8_t IDENTIFY_RUMBLE = 160;
// Adaptive triggers resist while squeezed; an Xbox controller rumbles the trigger softly instead, evenly while it moves deeper than the dead
// zone (by more than the reading's jitter) and briefly after. A trigger's vibration rumbles it while it is held there (pulsed at frequencies the
// motor can show), and a weapon effect that gives way kicks once. Pressed all the way, a trigger is still: the effect has nothing left to give.
constexpr uint8_t TRIGGER_DEAD_ZONE       = 12;
constexpr uint8_t TRIGGER_FULL            = 248;
constexpr int     TRIGGER_MOVE_MIN        = 3;
constexpr double  TRIGGER_RUMBLE_SCALE    = 0.4;
constexpr auto    TRIGGER_RUMBLE_HOLD     = std::chrono::milliseconds(60);
constexpr int     TRIGGER_PULSE_BELOW_HZ  = 20;
constexpr auto    TRIGGER_KICK            = std::chrono::milliseconds(60);
constexpr uint8_t TRIGGER_KICK_STRENGTH   = 128;
// The DualSense's haptic feedback as soft bumps on the small motor, which spins up quickly enough for them (the large one lags and thumps): the
// controller library turns the haptics' attacks into short pulses (the stronger of the two actuators counts). Nothing below the gate; above it
// at least enough to spin the motor up for a short pulse, and at most about 25 %.
constexpr uint8_t HAPTIC_GATE        = 10;
constexpr int     HAPTIC_RUMBLE_MIN = 24;
constexpr int     HAPTIC_RUMBLE_MAX = 64;

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

// An Xbox controller, in the list of controllers with the DSU ones (in the order they appeared). Only the polling thread uses these.
struct XboxController {
	gaming::Gamepad pad {nullptr};
	// RawGameController::NonRoamableId: Gamepad objects can be replaced while the controller stays
	// connected.
	std::wstring      id;
	std::string       key; // in the settings' manual assignment
	uint64_t          order  = 0;
	int               player = -1; // the player it plays for (-1: none)
	bool              missing = false;
	Clock::time_point missing_since;
	// For the lists: the model, and the connection and battery (XboxDetail, looked up on every scan).
	std::wstring name;
	std::wstring detail;
	// It rumbles until then to show which one it is (IdentifyController).
	Clock::time_point identify_until {};
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

	// Large and small motor, left and right trigger.
	std::array<uint8_t, 4>           vibration {};
	// Per trigger: the deepest point since it last moved back, and when it last moved deeper.
	std::array<uint8_t, 2>           trigger_deepest {};
	std::array<Clock::time_point, 2> trigger_squeezed {};
	std::array<uint32_t, 2>          trigger_shots {};
	std::array<Clock::time_point, 2> trigger_kick_until {};
	// The controller's headset events: they may report a headset that Gamepad::Headset() does not. The devices are guarded by g_shared_mutex.
	gaming::Gamepad::HeadsetConnected_revoker    headset_connected;
	gaming::Gamepad::HeadsetDisconnected_revoker headset_disconnected;
	std::wstring                                 event_headset_render;
	std::wstring                                 event_headset_capture;
};

// A DSU controller (dsu.h) in the list. Only the polling thread uses these.
struct DsuController {
	std::string  server;
	int          slot = 0;
	std::string  key; // its MAC address, or its server and slot
	uint64_t     order  = 0;
	int          player = -1;
	bool         home   = false; // its PS button was down
	Dsu::PadData data;
	bool         fresh = false;
	// For the lists: what it is, inferred from what the protocol tells (DsuName, DsuDetail), and whether it can rumble to show which one
	// it is.
	std::wstring name;
	std::wstring detail;
	bool         can_identify = false;
	bool         light_bar    = false; // a DualSense through DSU Bridge: identifying it also lights up its light bar
	// For the launcher's activity mark: the state at the last poll.
	Controller::HostPadState previous;
};

// In the order they appeared (both share g_order). g_xbox_controllers changes under g_shared_mutex (the headset events read it).
std::vector<std::unique_ptr<XboxController>> g_xbox_controllers;
std::vector<DsuController>                   g_dsu_controllers;
uint64_t                                     g_order = 0;
// Per player: the state it was sent, whether it is connected (player 1 always is), and its network controller's last motion sample.
std::array<Controller::HostPadState, Controller::MAX_PLAYERS> g_sent {};
std::array<bool, Controller::MAX_PLAYERS>                     g_connected {};
std::array<uint64_t, Controller::MAX_PLAYERS>                 g_motion_time {};

std::mutex                  g_shared_mutex; // guards the chord handlers, g_controllers, the assignment and the headset
std::function<void()>       g_menu_handler;
std::function<void()>       g_overlay_handler;
std::vector<ControllerInfo> g_controllers;
// When each of g_controllers last had input (steady clock, milliseconds; 0: none yet).
std::vector<int64_t>        g_controller_inputs;
// How controllers get players (SetControllerAssignment); g_assignment_changed asks the input thread to apply it.
bool                        g_automatic = true;
std::map<std::string, int>  g_manual;
std::atomic_bool            g_assignment_changed = false;
// The controllers to identify (IdentifyController), for the input thread.
std::vector<std::string>    g_identify;
std::atomic_bool            g_paused  = false;
// The settings page's controller options (SetControllerOptions).
std::atomic_bool            g_impulse_triggers = true;
std::atomic_bool            g_haptic_rumble    = true;
std::atomic_bool            g_headset_speaker  = true;
std::atomic_bool            g_headset_mic      = false;
// The audio devices of a headset plugged into player 1's controller (empty: none); guarded by g_shared_mutex.
std::wstring                g_headset_render;
std::wstring                g_headset_capture;
// The game gets controller input (ActivateGameInput); g_activate asks the input thread for it.
std::atomic_bool            g_game_input = false;
std::atomic_bool            g_activate   = false;
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

// The model of a Microsoft controller by its USB product id (as SDL's controller_list.h and Linux's
// xpad.c list them); Windows names every one of them "Xbox One Game Controller".
const wchar_t* ControllerModel(uint16_t vendor, uint16_t product) {
	if (vendor != 0x045e) {
		return nullptr;
	}
	switch (product) {
		case 0x02d1:
		case 0x02dd: return L"Xbox One Controller";
		case 0x02e0:
		case 0x02ea:
		case 0x02fd:
		case 0x0b20: return L"Xbox One S Controller";
		case 0x02e3: return L"Xbox Elite Controller";
		case 0x0b00:
		case 0x0b05:
		case 0x0b22: return L"Xbox Elite Series 2";
		case 0x0b0a:
		case 0x0b0c:
		case 0x0b21: return L"Xbox Adaptive Controller";
		case 0x0b12:
		case 0x0b13: return L"Xbox Series X|S Controller";
		default: return nullptr;
	}
}

// Player 1's Xbox controller, for its headset: the first one that plays for player 1 (nullptr: none).
XboxController* Player1XboxController() {
	for (auto& controller: g_xbox_controllers) {
		if (controller->player == 0 && !controller->missing) {
			return controller.get();
		}
	}
	return nullptr;
}

// An Xbox controller's model, by its product id, else the name Windows gives it.
std::wstring XboxName(gaming::Gamepad const& pad) {
	if (auto raw = gaming::RawGameController::FromGameController(pad)) {
		const auto* model = ControllerModel(raw.HardwareVendorId(), raw.HardwareProductId());
		return model != nullptr ? std::wstring(model) : std::wstring(raw.DisplayName());
	}
	return L"Controller";
}

// An Xbox controller's connection and battery, for the settings: "Wireless · Battery 80%", "Wireless · Charging", "Wired".
std::wstring XboxDetail(gaming::Gamepad const& pad) {
	std::wstring detail = pad.IsWireless() ? L"Wireless" : L"Wired";
	if (const auto report = pad.TryGetBatteryReport()) {
		const auto status = report.Status();
		if (status == winrt::Windows::System::Power::BatteryStatus::Charging) {
			detail += L" · Charging";
		} else if (status != winrt::Windows::System::Power::BatteryStatus::NotPresent && report.RemainingCapacityInMilliwattHours() != nullptr &&
		           report.FullChargeCapacityInMilliwattHours() != nullptr && report.FullChargeCapacityInMilliwattHours().Value() > 0) {
			const auto percent = report.RemainingCapacityInMilliwattHours().Value() * 100 / report.FullChargeCapacityInMilliwattHours().Value();
			detail += L" · Battery " + std::to_wstring(std::clamp(percent, 0, 100)) + L"%";
		}
	}
	return detail;
}

// What a DSU controller is, inferred from what the protocol tells: a DualSense where the server serves whole controllers (DSU Bridge),
// else a motion controller (such as a phone) or a controller.
std::wstring DsuName(const Dsu::Controller& controller) {
	if (controller.full) {
		return L"DualSense";
	}
	return controller.model == 2 ? L"Motion controller" : L"Controller";
}

// A DSU controller's connection and battery, and on a second line its server and the end of its MAC address (which tells two alike apart):
// "USB · Battery full", then "From 10.0.0.2 · MAC address ending in 3A:7F".
std::wstring DsuDetail(const Dsu::Controller& controller) {
	std::vector<std::wstring> parts;
	if (controller.connection == 1) {
		parts.emplace_back(L"USB");
	} else if (controller.connection == 2) {
		parts.emplace_back(L"Bluetooth");
	}
	switch (controller.battery) {
		case 0x01: parts.emplace_back(L"Battery almost empty"); break;
		case 0x02: parts.emplace_back(L"Battery low"); break;
		case 0x03: parts.emplace_back(L"Battery medium"); break;
		case 0x04: parts.emplace_back(L"Battery high"); break;
		case 0x05: parts.emplace_back(L"Battery full"); break;
		case 0xEE: parts.emplace_back(L"Charging"); break;
		case 0xEF: parts.emplace_back(L"Charged"); break;
		default: break;
	}
	std::wstring detail;
	for (const auto& part: parts) {
		detail += (detail.empty() ? L"" : L" · ") + part;
	}
	std::wstring source = L"From " + std::wstring(winrt::to_hstring(controller.server));
	if (std::any_of(controller.mac.begin(), controller.mac.end(), [](uint8_t byte) { return byte != 0; })) {
		wchar_t end[8] = {};
		swprintf_s(end, L"%02X:%02X", controller.mac[4], controller.mac[5]);
		source += L" · MAC address ending in " + std::wstring(end);
	}
	return detail.empty() ? source : detail + L"\n" + source;
}

// The key a DSU controller is remembered by: its MAC address, the same on every server and slot, or else its server and slot.
std::string DsuKey(const Dsu::Controller& controller) {
	if (std::none_of(controller.mac.begin(), controller.mac.end(), [](uint8_t byte) { return byte != 0; })) {
		return "dsu:" + controller.server + "#" + std::to_string(controller.slot + 1);
	}
	char mac[24] = {};
	snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", controller.mac[0], controller.mac[1], controller.mac[2], controller.mac[3],
	         controller.mac[4], controller.mac[5]);
	return std::string("dsu:") + mac;
}

// The list for the launcher, the overlay and the settings (ConnectedControllers): the Xbox and the DSU controllers by player, players 1 to
// 4 and then those that play for none; the controllers of one player in the order they appeared.
void ListControllers() {
	struct Listed {
		int            player;
		uint64_t       order;
		ControllerInfo info;
	};
	std::vector<Listed> listed;
	const auto*         headset_controller = Player1XboxController();
	for (const auto& controller: g_xbox_controllers) {
		ControllerInfo info;
		info.player       = controller->player;
		info.name         = controller->name;
		info.detail       = controller->detail;
		info.id           = controller->key;
		info.headset_mic  = controller.get() == headset_controller;
		info.can_identify = true;
		listed.push_back({controller->player, controller->order, std::move(info)});
	}
	for (const auto& controller: g_dsu_controllers) {
		ControllerInfo info;
		info.player       = controller.player;
		info.name         = controller.name;
		info.detail       = controller.detail;
		info.id           = controller.key;
		info.network      = true;
		info.can_identify = controller.can_identify;
		info.light_bar    = controller.light_bar;
		listed.push_back({controller.player, controller.order, std::move(info)});
	}
	std::sort(listed.begin(), listed.end(), [](const Listed& a, const Listed& b) {
		const int a_player = a.player < 0 ? Controller::MAX_PLAYERS : a.player;
		const int b_player = b.player < 0 ? Controller::MAX_PLAYERS : b.player;
		return a_player != b_player ? a_player < b_player : a.order < b.order;
	});
	std::vector<ControllerInfo> controllers;
	for (auto& entry: listed) {
		controllers.push_back(std::move(entry.info));
	}
	std::lock_guard lock(g_shared_mutex);
	// The input times stay with their controllers.
	std::vector<int64_t> inputs(controllers.size(), 0);
	for (size_t i = 0; i < controllers.size(); i++) {
		for (size_t old = 0; old < g_controllers.size(); old++) {
			if (g_controllers[old].id == controllers[i].id && old < g_controller_inputs.size()) {
				inputs[i] = g_controller_inputs[old];
			}
		}
	}
	g_controllers       = std::move(controllers);
	g_controller_inputs = std::move(inputs);
}

// A controller of the list had input now (for the launcher's activity mark).
void NoteInput(const std::string& key, Clock::time_point now) {
	const auto      ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	std::lock_guard lock(g_shared_mutex);
	for (size_t i = 0; i < g_controllers.size() && i < g_controller_inputs.size(); i++) {
		if (g_controllers[i].id == key) {
			g_controller_inputs[i] = ms;
		}
	}
}

std::wstring ControllerId(gaming::Gamepad const& pad) {
	if (auto raw = gaming::RawGameController::FromGameController(pad)) {
		return std::wstring(raw.NonRoamableId());
	}
	return {};
}

bool IsController(const XboxController& controller, gaming::Gamepad const& pad, const std::wstring& id) {
	return controller.pad != nullptr && (id.empty() ? controller.pad == pad : controller.id == id);
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

Controller::HostPadState MapReading(XboxController& controller, const gaming::GamepadReading& reading) {
	auto held = static_cast<uint32_t>(reading.Buttons);
	controller.blocked &= held;
	held &= ~controller.blocked;
	const auto is_held = [held](gaming::GamepadButtons button) {
		return (held & static_cast<uint32_t>(button)) != 0;
	};

	// View + Menu: decided on release, so the order of the presses doesn't matter. The game menu,
	// or the overlay when both triggers were pulled as well. Until then the game gets nothing.
	const bool view     = is_held(gaming::GamepadButtons::View);
	const bool menu     = is_held(gaming::GamepadButtons::Menu);
	const bool triggers = reading.LeftTrigger > 0.5 && reading.RightTrigger > 0.5;
	if (view && menu) {
		controller.chord = true;
	}
	if (controller.chord) {
		controller.chord_triggers = controller.chord_triggers || triggers;
		const bool released   = !view && !menu && (!controller.chord_triggers ||
		                                          (reading.LeftTrigger < 0.1 && reading.RightTrigger < 0.1));
		if (released) {
			const bool overlay    = controller.chord_triggers;
			controller.chord          = false;
			controller.chord_triggers = false;
			RunChord(overlay);
		}
		controller.touching = false;
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
		controller.touching = false;
		return state;
	}

	// View held: the left stick is a finger on the touchpad, from when it moves (or clicks) until
	// View is released; the finger alone doesn't click, the stick's click is the touchpad click. The
	// right stick tilts the controller.
	const double x          = reading.LeftThumbstickX;
	const double y          = reading.LeftThumbstickY;
	const bool   left_click = is_held(gaming::GamepadButtons::LeftThumbstick);
	if (left_click || std::hypot(x, y) > TOUCH_START) {
		controller.touching = true;
	}
	state.buttons &= ~Controller::PAD_BUTTON_L3;
	if (left_click) {
		state.buttons |= Controller::PAD_BUTTON_TOUCH_PAD;
	}
	if (controller.touching) {
		state.touch   = true;
		state.touch_x = static_cast<float>((std::clamp(x, -1.0, 1.0) + 1.0) / 2.0);
		state.touch_y = static_cast<float>((1.0 - std::clamp(y, -1.0, 1.0)) / 2.0);
	}
	// The right stick sets the pad's angle: up and down tip it, left and right lean it, or turn it while the stick is pressed in (the click is
	// not the game's R3 then).
	const bool turning = is_held(gaming::GamepadButtons::RightThumbstick);
	state.buttons &= ~Controller::PAD_BUTTON_R3;
	(turning ? state.turn : state.tilt_x) = Tilt(reading.RightThumbstickX);
	state.tilt_y = Tilt(-reading.RightThumbstickY);
	std::fill_n(state.axes.begin(), 4, uint8_t {128});
	return state;
}

uint8_t TriggerStrength(uint8_t strength) {
	return static_cast<uint8_t>(std::lround(strength * TRIGGER_RUMBLE_SCALE));
}

uint8_t TriggerRumble(uint8_t strength, uint8_t position, Clock::time_point squeezed,
                      Clock::time_point now) {
	if (strength == 0 || position <= TRIGGER_DEAD_ZONE || position >= TRIGGER_FULL || now - squeezed > TRIGGER_RUMBLE_HOLD) {
		return 0;
	}
	return TriggerStrength(strength);
}

uint8_t TriggerVibration(uint8_t strength, uint8_t frequency, uint8_t position, Clock::time_point now) {
	if (strength == 0 || position >= TRIGGER_FULL) {
		return 0;
	}
	if (frequency == 0 || frequency >= TRIGGER_PULSE_BELOW_HZ) {
		return TriggerStrength(strength);
	}
	// On for the first half of each period.
	const auto period = 1000 / frequency;
	const auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	return ms % period < period / 2 ? TriggerStrength(strength) : 0;
}

uint8_t HapticRumble(uint8_t level) {
	if (level < HAPTIC_GATE) {
		return 0;
	}
	return static_cast<uint8_t>(HAPTIC_RUMBLE_MIN + (HAPTIC_RUMBLE_MAX - HAPTIC_RUMBLE_MIN) * (level - HAPTIC_GATE) / (255 - HAPTIC_GATE));
}

void ApplyFeedback(int player, XboxController& controller, const Controller::HostPadState& state,
                   Clock::time_point now) {
	// A trigger moves deeper once it passed the last deeper point by more than the reading's jitter; moving back resets that point.
	std::array<uint8_t, 2> positions {};
	for (int trigger = 0; trigger < 2; trigger++) {
		const auto position = state.axes[static_cast<int>(Controller::Axis::TriggerLeft) + trigger];
		auto&      deepest  = controller.trigger_deepest[trigger];
		if (position >= deepest + TRIGGER_MOVE_MIN) {
			controller.trigger_squeezed[trigger] = now;
			deepest                          = position;
		} else if (position < deepest) {
			deepest = position;
		}
		positions[trigger] = position;
	}
	const auto             feedback = Controller::GetPlayerFeedback(player);
	std::array<uint8_t, 4> vibration {feedback.large_motor, feedback.small_motor, 0, 0};
	// Once per session in the log: which of the DualSense's feedback the game uses.
	static bool logged_triggers = false;
	static bool logged_haptics  = false;
	if (!logged_triggers && (feedback.trigger_resistance != decltype(feedback.trigger_resistance) {} ||
	                         feedback.trigger_vibration != decltype(feedback.trigger_vibration) {})) {
		logged_triggers = true;
		Log("controller: the game uses adaptive trigger effects%s\n", g_impulse_triggers ? "" : " (Impulse Triggers are off)");
	}
	if (!logged_haptics && std::max(feedback.haptic_left, feedback.haptic_right) >= HAPTIC_GATE) {
		logged_haptics = true;
		Log("controller: the game plays haptic feedback%s\n", g_haptic_rumble ? "" : " (haptic rumble is off)");
	}
	if (g_haptic_rumble) {
		vibration[1] = std::max(vibration[1], HapticRumble(std::max(feedback.haptic_left, feedback.haptic_right)));
	}
	for (int trigger = 0; trigger < 2; trigger++) {
		// One more shot is a kick; a jump (the first look at the count) is not.
		if (feedback.trigger_shots[trigger] != controller.trigger_shots[trigger]) {
			if (feedback.trigger_shots[trigger] - controller.trigger_shots[trigger] == 1) {
				controller.trigger_kick_until[trigger] = now + TRIGGER_KICK;
			}
			controller.trigger_shots[trigger] = feedback.trigger_shots[trigger];
		}
		if (!g_impulse_triggers) {
			continue;
		}
		const auto resistance = TriggerRumble(feedback.trigger_resistance[trigger], positions[trigger], controller.trigger_squeezed[trigger], now);
		const auto shaking =
		    TriggerVibration(feedback.trigger_vibration[trigger], feedback.trigger_frequency[trigger], positions[trigger], now);
		const auto kick = now < controller.trigger_kick_until[trigger] ? TRIGGER_KICK_STRENGTH : uint8_t {0};
		vibration[2 + trigger] = std::max({resistance, shaking, kick});
	}
	if (vibration != controller.vibration) {
		SetVibration(controller.pad, vibration);
		controller.vibration = vibration;
	}
}

// The players the listed controllers play for.
std::array<bool, Controller::MAX_PLAYERS> UsedPlayers() {
	std::array<bool, Controller::MAX_PLAYERS> used {};
	for (const auto& controller: g_xbox_controllers) {
		if (controller->player >= 0) {
			used[controller->player] = true;
		}
	}
	for (const auto& controller: g_dsu_controllers) {
		if (controller.player >= 0) {
			used[controller.player] = true;
		}
	}
	return used;
}

// The player a controller that appears plays for: automatically the lowest free one (none when all four are taken), manually the chosen
// one (none when there is none).
int PlayerFor(const std::string& key) {
	bool automatic = true;
	int  manual    = -1;
	{
		std::lock_guard lock(g_shared_mutex);
		automatic = g_automatic;
		if (const auto found = g_manual.find(key); found != g_manual.end() && found->second >= 0 && found->second < Controller::MAX_PLAYERS) {
			manual = found->second;
		}
	}
	if (!automatic) {
		return manual;
	}
	const auto used = UsedPlayers();
	for (int player = 0; player < Controller::MAX_PLAYERS; player++) {
		if (!used[player]) {
			return player;
		}
	}
	return -1;
}

// A changed assignment, for all listed controllers: automatically in the order they appeared, manually as chosen.
void Reassign() {
	struct Entry {
		uint64_t           order;
		const std::string* key;
		int*               player;
	};
	std::vector<Entry> entries;
	for (auto& controller: g_xbox_controllers) {
		entries.push_back({controller->order, &controller->key, &controller->player});
	}
	for (auto& controller: g_dsu_controllers) {
		entries.push_back({controller.order, &controller.key, &controller.player});
	}
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
	for (auto& entry: entries) {
		*entry.player = -1;
	}
	for (auto& entry: entries) {
		*entry.player = PlayerFor(*entry.key);
	}
	for (const auto& controller: g_dsu_controllers) {
		Dsu::SetPlayer(controller.server, controller.slot, controller.player);
	}
	bool automatic = true;
	{
		std::lock_guard lock(g_shared_mutex);
		automatic = g_automatic;
	}
	Log("controllers: %s assignment\n", automatic ? "automatic" : "manual");
	ListControllers();
}

// Players 2 to 4 are connected while a controller plays for them (player 1 always is); a player that loses its last one lets go of what it
// held.
void UpdateConnections() {
	if (!g_game_input) {
		return;
	}
	const auto used = UsedPlayers();
	for (int player = 0; player < Controller::MAX_PLAYERS; player++) {
		const bool connected = player == 0 || used[player];
		if (connected == g_connected[player]) {
			continue;
		}
		if (!connected) {
			Controller::SetPlayerState(player, {});
			g_sent[player] = {};
		}
		Controller::SetPlayerConnected(player, connected);
		g_connected[player] = connected;
	}
}

void AddXboxController(gaming::Gamepad const& pad, std::wstring id) {
	auto  added      = std::make_unique<XboxController>();
	auto& controller = *added;
	controller.pad      = pad;
	controller.key      = "xbox:" + (id.empty() ? std::to_string(g_order + 1) : winrt::to_string(id));
	controller.id       = std::move(id);
	controller.order    = ++g_order;
	controller.name     = XboxName(pad);
	controller.detail   = XboxDetail(pad);
	controller.blocked  = static_cast<uint32_t>(pad.GetCurrentReading().Buttons);
	controller.previous = pad.GetCurrentReading();
	SetVibration(pad, {});
	controller.player = PlayerFor(controller.key);
	Log("controller connected: %ls, player %d (user: %s, headset: %s)\n", controller.name.c_str(), controller.player + 1,
	    pad.User() != nullptr ? "assigned" : "none", pad.Headset() != nullptr ? "yes" : "none");
	// The events come on other threads: g_xbox_controllers changes under g_shared_mutex, so a late one finds its controller gone.
	auto*      raw   = added.get();
	const auto known = [raw] {
		return std::any_of(g_xbox_controllers.begin(), g_xbox_controllers.end(), [raw](const auto& listed) { return listed.get() == raw; });
	};
	controller.headset_connected = pad.HeadsetConnected(winrt::auto_revoke, [raw, known](auto const&, gaming::Headset const& headset) {
		Log("controller headset event: connected\n");
		std::lock_guard lock(g_shared_mutex);
		if (known()) {
			raw->event_headset_render  = headset.RenderDeviceId();
			raw->event_headset_capture = headset.CaptureDeviceId();
		}
	});
	controller.headset_disconnected = pad.HeadsetDisconnected(winrt::auto_revoke, [raw, known](auto const&, auto const&) {
		Log("controller headset event: disconnected\n");
		std::lock_guard lock(g_shared_mutex);
		if (known()) {
			raw->event_headset_render.clear();
			raw->event_headset_capture.clear();
		}
	});
	{
		std::lock_guard lock(g_shared_mutex);
		g_xbox_controllers.push_back(std::move(added));
	}
	ListControllers();
}

void RemoveXboxController(size_t index) {
	auto& controller = *g_xbox_controllers[index];
	try {
		SetVibration(controller.pad, {});
	} catch (const winrt::hresult_error&) {
	}
	Log("controller disconnected: %ls, player %d\n", controller.name.c_str(), controller.player + 1);
	// The revokers go with it; the headset events are guarded by g_shared_mutex.
	std::unique_ptr<XboxController> removed;
	{
		std::lock_guard lock(g_shared_mutex);
		removed = std::move(g_xbox_controllers[index]);
		g_xbox_controllers.erase(g_xbox_controllers.begin() + static_cast<std::ptrdiff_t>(index));
	}
	removed.reset();
	ListControllers();
}

// The DSU controllers as the client has them now: new ones join the list (with a player), gone ones leave it, and what the lists show
// follows them (the battery).
void UpdateDsuControllers() {
	auto       reported = Dsu::Controllers();
	bool       changed  = false;
	const auto same     = [](const DsuController& controller, const Dsu::Controller& report) {
        return controller.server == report.server && controller.slot == report.slot;
	};
	for (size_t index = 0; index < g_dsu_controllers.size();) {
		const auto& controller = g_dsu_controllers[index];
		if (std::none_of(reported.begin(), reported.end(), [&](const auto& report) { return same(controller, report); })) {
			Log("controller disconnected: DSU %s #%d, player %d\n", controller.server.c_str(), controller.slot + 1, controller.player + 1);
			g_dsu_controllers.erase(g_dsu_controllers.begin() + static_cast<std::ptrdiff_t>(index));
			changed = true;
		} else {
			index++;
		}
	}
	for (auto& report: reported) {
		auto found = std::find_if(g_dsu_controllers.begin(), g_dsu_controllers.end(), [&](const DsuController& controller) { return same(controller, report); });
		if (found == g_dsu_controllers.end()) {
			DsuController controller;
			controller.server = report.server;
			controller.slot   = report.slot;
			controller.key    = DsuKey(report);
			controller.order  = ++g_order;
			controller.player = PlayerFor(controller.key);
			Log("controller connected: DSU %s #%d (%s), player %d\n", controller.server.c_str(), controller.slot + 1, controller.key.c_str(),
			    controller.player + 1);
			g_dsu_controllers.push_back(std::move(controller));
			found   = g_dsu_controllers.end() - 1;
			changed = true;
		}
		// The client has its player too (for the feedback and audio); it forgets it when the controller comes back after a gap, maybe
		// between two polls, so that this list never saw it go.
		if (report.player != found->player) {
			Dsu::SetPlayer(found->server, found->slot, found->player);
		}
		found->fresh = report.fresh;
		found->data  = report.data;
		auto name    = DsuName(report);
		auto detail  = DsuDetail(report);
		if (name != found->name || detail != found->detail || report.can_vibrate != found->can_identify || report.full != found->light_bar) {
			found->name         = std::move(name);
			found->detail       = std::move(detail);
			found->can_identify = report.can_vibrate;
			found->light_bar    = report.full;
			changed             = true;
		}
	}
	if (changed) {
		ListControllers();
	}
}

// The controllers asked to show which one they are (IdentifyController): an Xbox controller rumbles for half a second, a DSU controller
// asks its server.
void TakeIdentifyRequests(Clock::time_point now) {
	std::vector<std::string> keys;
	{
		std::lock_guard lock(g_shared_mutex);
		keys.swap(g_identify);
	}
	for (const auto& key: keys) {
		for (auto& controller: g_xbox_controllers) {
			if (controller->key == key) {
				controller->identify_until = now + IDENTIFY_TIME;
			}
		}
		for (const auto& controller: g_dsu_controllers) {
			if (controller.key == key) {
				Dsu::Identify(controller.server, controller.slot);
			}
		}
	}
}

// The log's list of the audio devices: each output and input by name, marked when it is the default one, the default one for chat, or player 1's
// controller headset.
void LogAudioDevices() {
	namespace media       = winrt::Windows::Media::Devices;
	namespace enumeration = winrt::Windows::Devices::Enumeration;
	try {
		std::wstring headset[2];
		{
			std::lock_guard lock(g_shared_mutex);
			headset[0] = g_headset_render;
			headset[1] = g_headset_capture;
		}
		for (int input = 0; input < 2; input++) {
			const std::wstring standard(input != 0 ? media::MediaDevice::GetDefaultAudioCaptureId(media::AudioDeviceRole::Default)
			                                       : media::MediaDevice::GetDefaultAudioRenderId(media::AudioDeviceRole::Default));
			const std::wstring chat(input != 0 ? media::MediaDevice::GetDefaultAudioCaptureId(media::AudioDeviceRole::Communications)
			                                   : media::MediaDevice::GetDefaultAudioRenderId(media::AudioDeviceRole::Communications));
			const auto selector = input != 0 ? media::MediaDevice::GetAudioCaptureSelector() : media::MediaDevice::GetAudioRenderSelector();
			const auto devices  = enumeration::DeviceInformation::FindAllAsync(selector).get();
			Log("audio %s devices: %u\n", input != 0 ? "input" : "output", devices.Size());
			for (const auto& device: devices) {
				const std::wstring id(device.Id());
				Log("  \"%ls\"%s%s%s\n", device.Name().c_str(), id == standard ? " (default)" : "", id == chat ? " (chat)" : "",
				    !id.empty() && id == headset[input] ? " (player 1's controller headset)" : "");
			}
		}
	} catch (const winrt::hresult_error& error) {
		Log("audio devices: cannot list them (0x%08x)\n", static_cast<uint32_t>(error.code()));
	}
}

// The headset the device uses for chat, as its speaker and microphone: on the Xbox, a headset on a controller is the chat device (the TV stays
// the default one) also where Gamepad::Headset() does not report it. Only a chat speaker of its own counts: where it is the default speaker too
// (a PC's speakers), there is no headset speaker. The chat microphone counts without one (a PC's microphone). Looked up every CHAT_INTERVAL.
std::pair<std::wstring, std::wstring> ChatHeadset(Clock::time_point now) {
	namespace media = winrt::Windows::Media::Devices;
	static std::pair<std::wstring, std::wstring> headset;
	static Clock::time_point                     next {};
	if (now < next) {
		return headset;
	}
	next = now + CHAT_INTERVAL;
	try {
		const std::wstring speaker(media::MediaDevice::GetDefaultAudioRenderId(media::AudioDeviceRole::Communications));
		const std::wstring standard(media::MediaDevice::GetDefaultAudioRenderId(media::AudioDeviceRole::Default));
		headset = {speaker != standard ? speaker : std::wstring(),
		           std::wstring(media::MediaDevice::GetDefaultAudioCaptureId(media::AudioDeviceRole::Communications))};
	} catch (const winrt::hresult_error&) {
		headset = {};
	}
	return headset;
}

void Scan(Clock::time_point now) {
	std::vector<std::pair<gaming::Gamepad, std::wstring>> connected;
	for (auto const& pad: gaming::Gamepad::Gamepads()) {
		connected.emplace_back(pad, ControllerId(pad));
	}

	bool details_changed = false;
	for (size_t index = 0; index < g_xbox_controllers.size();) {
		auto&      controller = *g_xbox_controllers[index];
		const auto found      = std::find_if(connected.begin(), connected.end(), [&](const auto& pad) {
            return IsController(controller, pad.first, pad.second);
        });
		if (found != connected.end()) {
			controller.pad     = found->first;
			controller.missing = false;
			connected.erase(found);
			// The connection and the battery change now and then.
			if (auto detail = XboxDetail(controller.pad); detail != controller.detail) {
				controller.detail = std::move(detail);
				details_changed   = true;
			}
		} else if (!controller.missing) {
			controller.missing       = true;
			controller.missing_since = now;
		} else if (now - controller.missing_since >= MISSING_GRACE) {
			RemoveXboxController(index);
			continue;
		}
		index++;
	}

	for (auto& [pad, id]: connected) {
		AddXboxController(pad, std::move(id));
	}
	if (details_changed) {
		ListControllers();
	}

	// Player 1's headset: Windows lists its speaker and microphone like any other audio device. Gamepad::Headset(), or else what the
	// controller's HeadsetConnected event reported, or else the chat headset.
	std::wstring render;
	std::wstring capture;
	const char*  source = "none";
	auto*        first  = Player1XboxController();
	if (first != nullptr) {
		if (const auto headset = first->pad.Headset()) {
			render  = headset.RenderDeviceId();
			capture = headset.CaptureDeviceId();
			source  = "the controller's";
		}
	}
	{
		std::lock_guard lock(g_shared_mutex);
		if (render.empty() && capture.empty() && first != nullptr && !first->event_headset_render.empty()) {
			render  = first->event_headset_render;
			capture = first->event_headset_capture;
			source  = "the controller's (from its event)";
		}
	}
	if (render.empty() && capture.empty()) {
		std::tie(render, capture) = ChatHeadset(now);
		if (!render.empty()) {
			source = "the chat headset";
		} else if (!capture.empty()) {
			source = "none; the microphone is the chat microphone";
		}
	}
	bool changed = false;
	{
		std::lock_guard lock(g_shared_mutex);
		changed = render != g_headset_render || capture != g_headset_capture;
		if (changed) {
			Log("controller headset: %s%s\n", source, render.empty() && capture.empty() ? "" : capture.empty() ? ", no microphone" : "");
			g_headset_render  = std::move(render);
			g_headset_capture = std::move(capture);
		}
	}
	// The audio devices once at the start and whenever the headset changes, to see what the device offers.
	static bool listed = false;
	if (changed || !listed) {
		listed = true;
		LogAudioDevices();
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

// Whether a DSU controller was used between two packets: a button or a finger, or a stick or trigger moved (as for the Xbox controller).
bool HadInput(const Controller::HostPadState& before, const Controller::HostPadState& after) {
	constexpr int moved = 51;
	if (before.buttons != after.buttons || before.touch != after.touch) {
		return true;
	}
	for (size_t i = 0; i < before.axes.size(); i++) {
		if (std::abs(static_cast<int>(before.axes[i]) - static_cast<int>(after.axes[i])) > moved) {
			return true;
		}
	}
	return false;
}

// Several controllers playing for one player add up: their buttons, each stick and trigger from the controller that moves it further, the
// first fingers on a touchpad (View + left stick's or a DSU controller's) and the first stick tilt.
void Merge(Controller::HostPadState& state, const Controller::HostPadState& other) {
	state.buttons |= other.buttons;
	const auto deflection = [](const std::array<uint8_t, 6>& axes, size_t x) {
		return std::abs(static_cast<int>(axes[x]) - 128) + std::abs(static_cast<int>(axes[x + 1]) - 128);
	};
	for (const size_t stick: {size_t {0}, size_t {2}}) {
		if (deflection(other.axes, stick) > deflection(state.axes, stick)) {
			state.axes[stick]     = other.axes[stick];
			state.axes[stick + 1] = other.axes[stick + 1];
		}
	}
	for (const size_t trigger: {size_t {4}, size_t {5}}) {
		state.axes[trigger] = std::max(state.axes[trigger], other.axes[trigger]);
	}
	if (!state.touch && !state.touch2 && (other.touch || other.touch2)) {
		state.touch    = other.touch;
		state.touch_x  = other.touch_x;
		state.touch_y  = other.touch_y;
		state.touch2   = other.touch2;
		state.touch2_x = other.touch2_x;
		state.touch2_y = other.touch2_y;
	}
	if (state.tilt_x == 0.0f && state.tilt_y == 0.0f && state.turn == 0.0f) {
		state.tilt_x = other.tilt_x;
		state.tilt_y = other.tilt_y;
		state.turn   = other.turn;
	}
}

// One poll: each controller's input to the player it plays for, merged, and each player's state to the game. Every Xbox controller's View +
// Menu works, whatever its player; a DSU controller with a player opens the game menu with its PS button. The motion sensors of a player's
// first DSU controller that has them replace the emulated ones.
void Update(Clock::time_point now) {
	std::array<Controller::HostPadState, Controller::MAX_PLAYERS> states {};
	std::array<bool, Controller::MAX_PLAYERS>                     has {};
	const auto add = [&states, &has](int player, const Controller::HostPadState& state) {
		if (player < 0 || player >= Controller::MAX_PLAYERS) {
			return;
		}
		if (has[player]) {
			Merge(states[player], state);
		} else {
			states[player] = state;
			has[player]    = true;
		}
	};

	for (auto& listed: g_xbox_controllers) {
		auto& controller = *listed;
		if (controller.missing) {
			continue;
		}
		try {
			const auto reading = controller.pad.GetCurrentReading();
			if (HadInput(controller.previous, reading)) {
				NoteInput(controller.key, now);
			}
			controller.previous = reading;
			// While it identifies itself, it rumbles, game or not; then its rumble is the game's again (or none).
			const bool identifying = now < controller.identify_until;
			if (identifying) {
				constexpr std::array<uint8_t, 4> IDENTIFY = {IDENTIFY_RUMBLE, IDENTIFY_RUMBLE, 0, 0};
				if (controller.vibration != IDENTIFY) {
					SetVibration(controller.pad, IDENTIFY);
					controller.vibration = IDENTIFY;
				}
			}
			const bool playing = g_game_input && !g_paused && controller.player >= 0;
			if (g_game_input && !g_paused) {
				const auto state = MapReading(controller, reading); // View + Menu, also for a controller that plays for none
				if (playing) {
					if (!identifying) {
						ApplyFeedback(controller.player, controller, state, now);
					}
					add(controller.player, state);
				}
			} else if (g_paused) {
				controller.touching = false;
			}
			if (!playing && !identifying && controller.vibration != std::array<uint8_t, 4> {}) {
				SetVibration(controller.pad, {});
				controller.vibration = {};
			}
		} catch (const winrt::hresult_error&) {
			controller.missing       = true;
			controller.missing_since = now;
		}
	}

	std::array<bool, Controller::MAX_PLAYERS> motion_taken {};
	for (auto& controller: g_dsu_controllers) {
		const auto& data = controller.data;
		// The PS button also closes the game menu again.
		const bool home = controller.fresh && data.home && controller.player >= 0;
		if (home && !controller.home && g_game_input) {
			RunChord(false);
		}
		controller.home = home;
		if (!controller.fresh) {
			continue;
		}
		if (HadInput(controller.previous, data.state)) {
			NoteInput(controller.key, now);
		}
		controller.previous = data.state;
		if (!g_game_input || g_paused || controller.player < 0) {
			continue;
		}
		add(controller.player, data.state);
		if (data.motion_time != 0 && !motion_taken[controller.player]) {
			motion_taken[controller.player] = true;
			if (data.motion_time != g_motion_time[controller.player]) {
				Controller::SetPlayerMotion(controller.player, data.accel, data.gyro, data.motion_time);
				g_motion_time[controller.player] = data.motion_time;
			}
		}
	}

	if (!g_game_input) {
		return;
	}
	for (int player = 0; player < Controller::MAX_PLAYERS; player++) {
		if (!g_connected[player]) {
			continue;
		}
		// A player whose controllers let go (or went away) lets go too.
		if (states[player] != g_sent[player]) {
			Controller::SetPlayerState(player, states[player]);
			g_sent[player] = states[player];
		}
		Controller::StepPlayerMotion(player);
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
		try {
			UpdateDsuControllers();
		} catch (const winrt::hresult_error& error) {
			Log("dsu controllers failed: 0x%08x\n", static_cast<uint32_t>(error.code()));
		}
		if (g_assignment_changed.exchange(false)) {
			Reassign();
		}
		TakeIdentifyRequests(now);
		const bool activate = g_activate.exchange(false);
		if (activate || g_resumed.exchange(false)) {
			// The game starts or resumes: buttons still held (the A that started it) count once released.
			for (auto& controller: g_xbox_controllers) {
				if (!controller->missing) {
					try {
						controller->blocked = static_cast<uint32_t>(controller->pad.GetCurrentReading().Buttons);
					} catch (const winrt::hresult_error&) {
					}
				}
			}
		}
		if (activate) {
			// The game starts: the players with controllers connect.
			g_game_input = true;
		}
		UpdateConnections();
		Update(now);

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
	std::vector<int64_t>        inputs;
	{
		std::lock_guard lock(g_shared_mutex);
		controllers = g_controllers;
		inputs      = g_controller_inputs;
		for (auto& controller: controllers) {
			controller.headset_mic = controller.headset_mic && !g_headset_capture.empty();
		}
	}
	const int64_t newest = inputs.empty() ? 0 : *std::max_element(inputs.begin(), inputs.end());
	for (size_t i = 0; i < controllers.size() && i < inputs.size(); i++) {
		controllers[i].latest = newest != 0 && inputs[i] == newest;
	}
	return controllers;
}

void IdentifyController(const std::string& id) {
	std::lock_guard lock(g_shared_mutex);
	g_identify.push_back(id);
}

void SetControllerAssignment(bool automatic, const std::map<std::string, int>& players, const std::vector<std::string>& dsu_servers) {
	{
		std::lock_guard lock(g_shared_mutex);
		if (automatic == g_automatic && players == g_manual) {
			// Only the servers may have changed.
		} else {
			g_automatic          = automatic;
			g_manual             = players;
			g_assignment_changed = true;
		}
	}
	Dsu::SetServers(dsu_servers);
}

void ActivateGameInput() {
	g_activate = true;
}

void SetControllerOptions(const ControllerOptions& options) {
	g_impulse_triggers = options.impulse_triggers;
	g_haptic_rumble    = options.haptic_rumble;
	g_headset_speaker  = options.headset_speaker;
	g_headset_mic      = options.headset_mic;
}

std::wstring ControllerSpeakerDevice() {
	if (!g_headset_speaker) {
		return {};
	}
	std::lock_guard lock(g_shared_mutex);
	return g_headset_render;
}

std::wstring ControllerMicrophoneDevice() {
	if (!g_headset_mic) {
		return {};
	}
	std::lock_guard lock(g_shared_mutex);
	return g_headset_capture;
}

void RequestMicrophoneAccess() {
	// Initializing an audio-only capture is what makes Windows ask; nothing is recorded.
	[]() -> winrt::fire_and_forget {
		namespace capture = winrt::Windows::Media::Capture;
		try {
			capture::MediaCaptureInitializationSettings settings;
			settings.StreamingCaptureMode(capture::StreamingCaptureMode::Audio);
			capture::MediaCapture media;
			co_await media.InitializeAsync(settings);
			media.Close();
			Log("microphone: the app may use it\n");
		} catch (const winrt::hresult_error& error) {
			Log("microphone: the app may not use it, or there is none (0x%08x)\n", static_cast<uint32_t>(error.code()));
		}
	}();
}

void PauseGameInput(bool paused) {
	Dsu::SetPaused(paused);
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

ControllerSymbol GetControllerSymbol() {
	static const bool xbox = IsXbox();
	return xbox ? ControllerSymbol {L"Segoe Xbox Symbol", {L'\uE42D', L'\0'}}
	            : ControllerSymbol {L"Segoe MDL2 Assets", {L'\uE7FC', L'\0'}};
}

ControllerSymbol GetButtonSymbol(XboxButton button) {
	// In XboxButton's order: the glyph in Segoe Xbox MDL2 Symbol (as the GlyphProbe app found them on the Xbox) and in Segoe Fluent Icons.
	struct Glyphs {
		wchar_t xbox;
		wchar_t pc;
	};
	static constexpr std::array<Glyphs, 13> GLYPHS = {{
	    {L'\uE3CE', L'\uF093'}, // A
	    {L'\uE3CD', L'\uF094'}, // B
	    {L'\uE3CB', L'\uF096'}, // X
	    {L'\uE3CC', L'\uF095'}, // Y
	    {L'\uE3ED', L'\uF10C'}, // LB
	    {L'\uE3EB', L'\uF10D'}, // RB
	    {L'\uE3E6', L'\uF10A'}, // LT
	    {L'\uE3E4', L'\uF10B'}, // RT
	    {L'\uE3E7', L'\uF108'}, // left stick
	    {L'\uE425', L'\uF109'}, // right stick
	    {L'\uE3E2', L'\uF10E'}, // D-pad
	    {L'\uE3EC', L'\uEDE3'}, // Menu
	    {L'\uE54A', L'\uEECA'}, // View: Segoe MDL2 Assets on a PC
	}};
	static const bool xbox   = IsXbox();
	const auto&       glyphs = GLYPHS[static_cast<size_t>(button)];
	if (xbox) {
		return {L"Segoe Xbox MDL2 Symbol", {glyphs.xbox, L'\0'}};
	}
	return {button == XboxButton::View ? L"Segoe MDL2 Assets" : L"Segoe Fluent Icons", {glyphs.pc, L'\0'}};
}

} // namespace Kyty::Uwp
