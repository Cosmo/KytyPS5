#ifndef EMULATOR_SRC_UWP_PADHOST_H_
#define EMULATOR_SRC_UWP_PADHOST_H_

#include <array>
#include <cstdint>

// The controller state the app's input code (input.cpp) hands to whoever consumes it: up to four local players, each with a PS5 pad state
// already mapped from the Xbox controller, and what the game asks of the controller in return (rumble, adaptive trigger resistance).
// This build has no emulator, so padHost.cpp only keeps the state; when the emulator is part of the build, these functions forward to its
// controller library and the types here are the ones it declares.
namespace Kyty::Uwp::Pad {

constexpr int MAX_PLAYERS = 4;

// PS5 pad button bits (the same values as the emulator's controller library).
constexpr uint32_t PAD_BUTTON_L3        = 0x00000002;
constexpr uint32_t PAD_BUTTON_R3        = 0x00000004;
constexpr uint32_t PAD_BUTTON_OPTIONS   = 0x00000008;
constexpr uint32_t PAD_BUTTON_UP        = 0x00000010;
constexpr uint32_t PAD_BUTTON_RIGHT     = 0x00000020;
constexpr uint32_t PAD_BUTTON_DOWN      = 0x00000040;
constexpr uint32_t PAD_BUTTON_LEFT      = 0x00000080;
constexpr uint32_t PAD_BUTTON_L2        = 0x00000100;
constexpr uint32_t PAD_BUTTON_R2        = 0x00000200;
constexpr uint32_t PAD_BUTTON_L1        = 0x00000400;
constexpr uint32_t PAD_BUTTON_R1        = 0x00000800;
constexpr uint32_t PAD_BUTTON_TRIANGLE  = 0x00001000;
constexpr uint32_t PAD_BUTTON_CIRCLE    = 0x00002000;
constexpr uint32_t PAD_BUTTON_CROSS     = 0x00004000;
constexpr uint32_t PAD_BUTTON_SQUARE    = 0x00008000;
constexpr uint32_t PAD_BUTTON_TOUCH_PAD = 0x00100000;

enum class Axis { LeftX = 0, LeftY = 1, RightX = 2, RightY = 3, TriggerLeft = 4, TriggerRight = 5, AxisMax };

// A host controller's state, already mapped to the PS5 pad.
struct HostPadState {
	uint32_t               buttons = 0;
	std::array<uint8_t, 6> axes {128, 128, 128, 128, 0, 0}; // in Axis order; triggers set L2/R2
	// One finger on the touchpad; x and y in 0..1 from the top left.
	bool  touch   = false;
	float touch_x = 0.5f;
	float touch_y = 0.5f;
	// Emulated motion: the pad tilted the way a stick points, -1..1 like the PS5 stick axes (x to the right, y down); 0 is at rest.
	float tilt_x = 0.0f;
	float tilt_y = 0.0f;

	bool operator==(const HostPadState&) const = default;
};

// What the game asks of a player's controller.
struct HostPadFeedback {
	uint8_t large_motor = 0;
	uint8_t small_motor = 0;
	// The strongest adaptive trigger resistance, 0..255; 0 without an effect.
	uint8_t left_trigger  = 0;
	uint8_t right_trigger = 0;
};

void                          SetPlayerConnected(int player, bool connected);
void                          SetPlayerState(int player, const HostPadState& state);
[[nodiscard]] HostPadFeedback GetPlayerFeedback(int player);

} // namespace Kyty::Uwp::Pad

#endif // EMULATOR_SRC_UWP_PADHOST_H_
