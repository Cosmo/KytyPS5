#include "input.h"
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
#include <winrt/Windows.System.h>

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
	// Per trigger: the deepest point since it last moved back, and when it last moved deeper.
	std::array<uint8_t, 2>           trigger_deepest {};
	std::array<Clock::time_point, 2> trigger_squeezed {};
	std::array<uint32_t, 2>          trigger_shots {};
	std::array<Clock::time_point, 2> trigger_kick_until {};
	// The controller's headset events: they may report a headset that Gamepad::Headset() does not.
	gaming::Gamepad::HeadsetConnected_revoker    headset_connected;
	gaming::Gamepad::HeadsetDisconnected_revoker headset_disconnected;
};

std::array<Player, Controller::MAX_PLAYERS> g_players;

std::mutex                  g_shared_mutex; // guards the chord handlers, g_controllers and the headset
std::function<void()>       g_menu_handler;
std::function<void()>       g_overlay_handler;
std::vector<ControllerInfo> g_controllers;
std::atomic_bool            g_paused  = false;
// The settings page's controller options (SetControllerOptions).
std::atomic_bool            g_impulse_triggers = true;
std::atomic_bool            g_haptic_rumble    = true;
std::atomic_bool            g_headset_speaker  = true;
std::atomic_bool            g_headset_mic      = false;
// The audio devices of a headset plugged into player 1's controller (empty: none); guarded by g_shared_mutex.
std::wstring                g_headset_render;
std::wstring                g_headset_capture;
// The headset player 1's controller reported in its HeadsetConnected event (used when Gamepad::Headset() has none); guarded by g_shared_mutex.
std::wstring                g_event_headset_render;
std::wstring                g_event_headset_capture;
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

void ListControllers() {
	std::vector<ControllerInfo> controllers;
	for (int index = 0; index < Controller::MAX_PLAYERS; index++) {
		if (const auto& pad = g_players[index].pad; pad != nullptr) {
			std::wstring name = L"Controller";
			if (auto raw = gaming::RawGameController::FromGameController(pad)) {
				const auto* model = ControllerModel(raw.HardwareVendorId(), raw.HardwareProductId());
				name              = model != nullptr ? std::wstring(model) : std::wstring(raw.DisplayName());
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

void ApplyFeedback(int index, Player& player, const Controller::HostPadState& state,
                   Clock::time_point now) {
	// A trigger moves deeper once it passed the last deeper point by more than the reading's jitter; moving back resets that point.
	std::array<uint8_t, 2> positions {};
	for (int trigger = 0; trigger < 2; trigger++) {
		const auto position = state.axes[static_cast<int>(Controller::Axis::TriggerLeft) + trigger];
		auto&      deepest  = player.trigger_deepest[trigger];
		if (position >= deepest + TRIGGER_MOVE_MIN) {
			player.trigger_squeezed[trigger] = now;
			deepest                          = position;
		} else if (position < deepest) {
			deepest = position;
		}
		positions[trigger] = position;
	}
	const auto             feedback = Controller::GetPlayerFeedback(index);
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
		if (feedback.trigger_shots[trigger] != player.trigger_shots[trigger]) {
			if (feedback.trigger_shots[trigger] - player.trigger_shots[trigger] == 1) {
				player.trigger_kick_until[trigger] = now + TRIGGER_KICK;
			}
			player.trigger_shots[trigger] = feedback.trigger_shots[trigger];
		}
		if (!g_impulse_triggers) {
			continue;
		}
		const auto resistance = TriggerRumble(feedback.trigger_resistance[trigger], positions[trigger], player.trigger_squeezed[trigger], now);
		const auto shaking =
		    TriggerVibration(feedback.trigger_vibration[trigger], feedback.trigger_frequency[trigger], positions[trigger], now);
		const auto kick = now < player.trigger_kick_until[trigger] ? TRIGGER_KICK_STRENGTH : uint8_t {0};
		vibration[2 + trigger] = std::max({resistance, shaking, kick});
	}
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
	Log("controller connected: player %d (user: %s, headset: %s)\n", index + 1, pad.User() != nullptr ? "assigned" : "none",
	    pad.Headset() != nullptr ? "yes" : "none");
	player.headset_connected = pad.HeadsetConnected(winrt::auto_revoke, [index](auto const&, gaming::Headset const& headset) {
		Log("controller headset event: connected to player %d\n", index + 1);
		if (index == 0) {
			std::lock_guard lock(g_shared_mutex);
			g_event_headset_render  = headset.RenderDeviceId();
			g_event_headset_capture = headset.CaptureDeviceId();
		}
	});
	player.headset_disconnected = pad.HeadsetDisconnected(winrt::auto_revoke, [index](auto const&, auto const&) {
		Log("controller headset event: disconnected from player %d\n", index + 1);
		if (index == 0) {
			std::lock_guard lock(g_shared_mutex);
			g_event_headset_render.clear();
			g_event_headset_capture.clear();
		}
	});
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
	if (index == 0) {
		std::lock_guard lock(g_shared_mutex);
		g_event_headset_render.clear();
		g_event_headset_capture.clear();
	}
	if (g_game_input) {
		Controller::SetPlayerState(index, {});
		Controller::SetPlayerConnected(index, false);
	}
	Log("controller disconnected: player %d\n", index + 1);
	ListControllers();
}

// Matches the connected controllers to the players; new ones take the lowest free player.
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

	// Player 1's headset: Windows lists its speaker and microphone like any other audio device. Gamepad::Headset(), or else what the
	// controller's HeadsetConnected event reported, or else the chat headset.
	std::wstring render;
	std::wstring capture;
	const char*  source = "none";
	if (const auto& player = g_players[0]; player.pad != nullptr && !player.missing) {
		if (const auto headset = player.pad.Headset()) {
			render  = headset.RenderDeviceId();
			capture = headset.CaptureDeviceId();
			source  = "the controller's";
		}
	}
	{
		std::lock_guard lock(g_shared_mutex);
		if (render.empty() && capture.empty() && !g_event_headset_render.empty()) {
			render  = g_event_headset_render;
			capture = g_event_headset_capture;
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
	if (g_game_input) {
		Controller::StepPlayerMotion(index);
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
		for (auto& controller: controllers) {
			controller.headset_mic = controller.player == 0 && !g_headset_capture.empty();
		}
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
