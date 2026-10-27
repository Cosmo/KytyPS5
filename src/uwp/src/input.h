#ifndef EMULATOR_SRC_UWP_INPUT_H_
#define EMULATOR_SRC_UWP_INPUT_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

// The controllers for the emulator: the Xbox controllers, through Windows.Gaming.Input (the same on a PC and on Xbox), and the DSU servers'
// controllers (dsu.h), in one list in the order they appeared. Each plays for one of the four local players (Libs::Controller::MAX_PLAYERS)
// or for none: automatically the lowest free player when it appears, or manually the chosen one (SetControllerAssignment); several can play
// for one player, and their input adds up. In a game each player is a user of its own with a pad of its own (local co-op;
// Libs::Controller::LOCAL_PLAYERS_MAX).
//
// Mapping: A/B/X/Y Cross/Circle/Square/Triangle, Menu Options, LB/RB L1/R1, LT/RT L2/R2 (analog),
// sticks and their clicks L3/R3, D-pad. Holding View turns the sticks into the DualSense's extras:
// the left stick puts a finger on the touchpad (its click is the touchpad click) and the right stick
// sets the controller's angle for its motion sensors: up and down tip it, left and right lean it, or
// turn it with the stick pressed in; full deflection is 180 degrees. Rumble and adaptive triggers (as rumble of the trigger's motor: while it
// is squeezed against resistance, while it vibrates, and a kick when a weapon effect gives way) come back to the controller, and the haptic
// feedback as gentle rumble. View + Menu together open the game menu, or
// with both triggers pulled as well toggle the overlay: on every Xbox controller, whatever its player.
namespace Kyty::Uwp {

// Starts reading the controllers (at app start, for the launcher's list); the game gets their
// input from ActivateGameInput on.
void StartInput();
// Emulator thread, once the emulator's pad library is up: the controllers become its players.
void ActivateGameInput();

// The handlers run on the input thread when a player releases View + Menu: `menu` normally,
// `overlay` when both triggers were pulled during the chord as well.
void OnChords(std::function<void()> menu, std::function<void()> overlay);

struct ControllerInfo {
	int          player = 0; // 0 is player 1; -1: it plays for none
	std::wstring name;   // the model, or for a DSU controller what it is as far as the protocol tells ("DualSense", "Motion controller")
	std::wstring detail; // its connection and battery; for a DSU controller, a second line with its server and the end of its MAC address
	std::string  id;     // the key of SetControllerAssignment's `players`: an Xbox controller's id, a DSU controller's MAC address
	bool         network      = false; // a DSU controller
	bool         can_identify = false; // IdentifyController can make it rumble
	bool         light_bar    = false; // IdentifyController also turns its light bar white (a DualSense through DSU Bridge)
	bool         latest       = false; // the controller with the most recent input
	// A headset microphone is there for this player (player 1: the controller's headset or the chat headset); the DualSense's microphone when
	// the settings page's option is on.
	bool headset_mic = false;
};
// The symbol shown with a controller: the Xbox's own (Segoe Xbox Symbol U+E42D) on the Xbox, the gamepad of Segoe MDL2 Assets (U+E7FC)
// elsewhere.
struct ControllerSymbol {
	const wchar_t* font;
	wchar_t        glyph[2];
};
[[nodiscard]] ControllerSymbol GetControllerSymbol();
// The Xbox controller's buttons as symbols: Segoe Xbox MDL2 Symbol on the Xbox (the GlyphProbe app's table), Segoe Fluent Icons on a PC (View
// from Segoe MDL2 Assets).
enum class XboxButton { A, B, X, Y, LeftBumper, RightBumper, LeftTrigger, RightTrigger, LeftStick, RightStick, DPad, Menu, View };
[[nodiscard]] ControllerSymbol GetButtonSymbol(XboxButton button);
// The headset shown after a controller that has a headset microphone (Segoe MDL2 Assets, on a PC and on the Xbox): bright when the settings page
// uses it as the controller's microphone, dim otherwise.
constexpr const wchar_t* HEADSET_SYMBOL_FONT = L"Segoe MDL2 Assets";
constexpr const wchar_t* HEADSET_SYMBOL      = L"";

// The connected controllers by player: players 1 to 4, then those that play for none; one player's in the order they appeared.
[[nodiscard]] std::vector<ControllerInfo> ConnectedControllers();
// Shows which controller it is: it rumbles for half a second (a DualSense through DSU Bridge also shows a white light bar).
void IdentifyController(const std::string& id);
// How controllers get players (AppSettings' controller_auto, controller_players) and the DSU servers to ask (dsu_servers): at app start and
// whenever the settings change, also mid-game. `automatic`: each controller takes the lowest free player when it appears; otherwise each
// plays for the player `players` gives for its id (none without one).
void SetControllerAssignment(bool automatic, const std::map<std::string, int>& players, const std::vector<std::string>& dsu_servers);
// While paused (the game menu is open), the game gets neutral input and no rumble; buttons held
// when it resumes count once they are released and pressed again.
void PauseGameInput(bool paused);

// The settings page's controller options (AppSettings), set when a game starts.
struct ControllerOptions {
	// The games' adaptive trigger effects rumble the motors in the triggers (Impulse Triggers).
	bool impulse_triggers = true;
	// The DualSense's haptic feedback rumbles the controller, gently.
	bool haptic_rumble = true;
	// What games play on the DualSense's speaker plays on a headset plugged into player 1's controller.
	bool headset_speaker = true;
	// The microphone of that headset is the DualSense's microphone.
	bool headset_mic = false;
};
void SetControllerOptions(const ControllerOptions& options);

// The headset plugged into player 1's controller as the DualSense's speaker and microphone: its audio devices (Windows device ids, as XAudio2
// and WASAPI take them), or empty when there is none or the option is off. Where the controller reports no headset (the Xbox does not), the
// chat headset: a chat speaker of its own besides the default one, and the chat microphone, which counts without such a speaker too (a PC's
// microphone).
[[nodiscard]] std::wstring ControllerSpeakerDevice();
[[nodiscard]] std::wstring ControllerMicrophoneDevice();
// UI thread: asks the user to let the app use the microphone, unless that was decided already (Windows asks once); the answer goes to the log.
void RequestMicrophoneAccess();

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_INPUT_H_
