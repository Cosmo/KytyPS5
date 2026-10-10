#ifndef EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_
#define EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_

#include "common/abi.h"
#include "common/common.h"

#include <array>

namespace Libs::Controller {

void Initialize();
void Shutdown();
void EmergencyShutdown();

struct Lifecycle {
	static constexpr const char* name               = "Controller";
	static constexpr auto        initialize         = Libs::Controller::Initialize;
	static constexpr auto        shutdown           = Libs::Controller::Shutdown;
	static constexpr auto        emergency_shutdown = Libs::Controller::EmergencyShutdown;
};

constexpr int HOST_INPUT_CONTROLLER_ID = -1000;

// Local co-op: the host's controllers are players 1 to 4, each with a pad of its own (handles 1 to 4) and signed in as a user of its own, as on a
// PS5, where every controller belongs to a user. Player 1 is the configured user and has the keyboard and the SDL gamepads too; players 2 to 4 are
// users Config::GetUserId() + 1 to + 3, signed in from when their controller first connects until the emulator ends (a PS5 user stays signed in
// when the controller turns off).
constexpr int LOCAL_PLAYERS_MAX = 4;

// The input id of a host player's controller (SetButton and the others): player 1's is the host input's.
[[nodiscard]] constexpr int HostPlayerId(int player) {
	return HOST_INPUT_CONTROLLER_ID - player;
}
void SetHostPlayerConnected(int player, bool connected);
[[nodiscard]] int      LocalPlayerUserId(int player);
// The local player a user is, or -1 when the user is not signed in.
[[nodiscard]] int      LocalPlayerOfUser(int user_id);
// The signed-in local players, a bit each (player 1 always).
[[nodiscard]] uint32_t SignedInLocalPlayers();

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

enum class Axis {
	LeftX        = 0,
	LeftY        = 1,
	RightX       = 2,
	RightY       = 3,
	TriggerLeft  = 4,
	TriggerRight = 5,

	AxisMax
};

enum class Sensor { Accel, Gyro };

struct PadControllerInformation;
struct PadData;
struct PadVibrationParam;
struct PadLightBarParam;
struct PadTriggerEffectParam;
struct PadTriggerEffectStateInformation;

inline int controller_get_axis(int min, int max, int value) {
	int v = (255 * (value - min)) / (max - min);
	if (v < 0) {
		return 0;
	}
	if (v > 255) {
		return 255;
	}
	return v;
}

void Connect(int id);
void Disconnect(int id);
void SetButton(int id, uint32_t button, bool down);
void SetAxis(int id, Axis axis, int value);
void SetRightStick(int id, int x, int y);
void SetTouchPad(int id, int finger, bool down, float x, float y);
void SetSensor(int id, Sensor sensor, const float* data, uint64_t time_us);
void ToggleGyroEmulation(int id);
void ResetInputState();
int  GetActiveControllerId();

#if defined(KYTY_PLATFORM_UWP)
// What the game asks of the host controller now, with the intensity setting applied: zero when it asks for nothing or the request ran out. The UWP app
// polls this to drive the Xbox controller's motors (it cannot use the SDL gamepad code).
struct HostFeedback {
	uint8_t large_motor = 0;
	uint8_t small_motor = 0;
	// The adaptive triggers' effects (left, right) where the triggers are now, with the trigger effect intensity applied, for a controller with motors
	// in its triggers: the resistance a trigger gives while it moves and the vibration while it is held (0..255), the vibration's frequency in Hz, and
	// how often a weapon effect has fired so far (a change is a shot).
	std::array<uint8_t, 2>  trigger_resistance {};
	std::array<uint8_t, 2>  trigger_vibration {};
	std::array<uint8_t, 2>  trigger_frequency {};
	std::array<uint32_t, 2> trigger_shots {};
	// The haptic feedback's bump now (left, right actuator; 0..255): a short pulse where what the game plays on the vibration ports suddenly gets
	// louder, 0 otherwise.
	uint8_t haptic_left  = 0;
	uint8_t haptic_right = 0;
};
[[nodiscard]] HostFeedback GetHostFeedback(int player);
// The audio library, for every block a game plays on a local player's vibration port: the block's loudness per actuator (RMS, 0..1, volume
// applied).
void SetHapticLevels(int player, float left, float right);
#endif

enum class Setting { SpeakerVolume, VibrationIntensity, TriggerEffectIntensity };

void  CycleSetting(Setting setting);
float GetSettingScale(Setting setting);

int PadCheckHandle(int handle);

int KYTY_SYSV_ABI PadInit();
int KYTY_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param);
int KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index);
int KYTY_SYSV_ABI PadClose(int handle);
int KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable);
int KYTY_SYSV_ABI PadSetAngularVelocityDeadbandState(int handle, bool enable);
int KYTY_SYSV_ABI PadResetOrientation(int handle);
int KYTY_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info);
int KYTY_SYSV_ABI PadIsRemoteController(int handle, bool* is_remote);
int KYTY_SYSV_ABI PadReadState(int handle, PadData* data);
int KYTY_SYSV_ABI PadRead(int handle, PadData* data, int num);
int KYTY_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param);
int KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const PadTriggerEffectParam* param);
int KYTY_SYSV_ABI PadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info);
int KYTY_SYSV_ABI PadResetLightBar(int handle);
int KYTY_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param);

} // namespace Libs::Controller

#endif /* EMULATOR_INCLUDE_EMULATOR_CONTROLLER_H_ */
