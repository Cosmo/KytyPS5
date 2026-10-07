#ifndef EMULATOR_SRC_UWP_INPUT_H_
#define EMULATOR_SRC_UWP_INPUT_H_

#include <functional>
#include <string>
#include <vector>

// Xbox controllers for the emulator, through Windows.Gaming.Input (the same on a PC and on Xbox).
// Up to four controllers, each a local player (Libs::Controller::MAX_PLAYERS): a controller takes
// the lowest free player when it connects.
//
// Mapping: A/B/X/Y Cross/Circle/Square/Triangle, Menu Options, LB/RB L1/R1, LT/RT L2/R2 (analog),
// sticks and their clicks L3/R3, D-pad. Holding View turns the sticks into the DualSense's extras:
// the left stick puts a finger on the touchpad (its click is the touchpad click) and the right stick
// sets the controller's angle for its motion sensors: up and down tip it, left and right lean it, or
// turn it with the stick pressed in; full deflection is 180 degrees. Rumble and adaptive triggers (as rumble of the trigger's motor: while it
// is squeezed against resistance, while it vibrates, and a kick when a weapon effect gives way) come back to the controller, and the haptic
// feedback as gentle rumble. View + Menu together open the game menu, or
// with both triggers pulled as well toggle the overlay.
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
	int          player = 0; // 0 is player 1
	std::wstring name;
	bool         latest = false; // the controller with the most recent input
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

// The connected controllers by player.
[[nodiscard]] std::vector<ControllerInfo> ConnectedControllers();
// While paused (the game menu is open), the game gets neutral input and no rumble; buttons held
// when it resumes count once they are released and pressed again.
void PauseGameInput(bool paused);

// The settings page's controller options (AppSettings), set when a game starts.
struct ControllerOptions {
	// The games' adaptive trigger effects rumble the motors in the triggers (Impulse Triggers).
	bool impulse_triggers = true;
	// The DualSense's haptic feedback rumbles the controller, gently.
	bool haptic_rumble = true;
};
void SetControllerOptions(const ControllerOptions& options);

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_INPUT_H_
