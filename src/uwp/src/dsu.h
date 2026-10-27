#ifndef EMULATOR_SRC_UWP_DSU_H_
#define EMULATOR_SRC_UWP_DSU_H_

#include "padHost.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Controllers over the network with the DSU protocol (cemuhook's: https://github.com/v1993/cemuhook-protocol), for example DualSenses that a
// PC serves with DSU Bridge, or a phone. The app is the client of any number of servers: it asks each one for the data of all its slots once
// a second and gets a packet per change: buttons, sticks, analog triggers, the touchpad and the motion sensors. input.cpp lists them with
// the Xbox controllers and gives each the player it plays for.
//
// With a server that speaks DSU Bridge's extension (its hello reply says what it offers), each controller gets the feedback of its player:
// the light bar, the player lights (the player's number), rumble, the adaptive triggers' effects, the speaker's and the haptics' audio; and
// the microphone of player 1's controller comes to the app. Plain DSU servers ignore all of it.
namespace Kyty::Uwp::Dsu {

// The newest state of a controller, already as a PS5 pad.
struct PadData {
	Pad::HostPadState state;
	// The PS button (DSU's Home): the game never sees it; it opens the game menu.
	bool home = false;
	// The motion sensors in the pad's frame as the controller library takes them (SDL's: x right, y up, z toward the player): m/s² and
	// rad/s, at `motion_time` (microseconds on the same clock as padHost's). 0: the server sends no motion.
	std::array<float, 3> accel {};
	std::array<float, 3> gyro {};
	uint64_t             motion_time = 0;
};

// A controller a server serves now: the server (its address as set), its slot there, and its newest data.
struct Controller {
	std::string server;
	int         slot = 0;
	// The data is less than half a second old (older data is kept for up to 2 s, then the controller is gone).
	bool    fresh = false;
	PadData data;
	// What the protocol tells about it: its MAC address (all zero: none sent), the connection (1 USB, 2 Bluetooth, 0 unknown), the model
	// (2: full motion sensors, 1: partial, 0: none) and the battery (DSU's codes: 1 to 5 empty to full, 0xEE charging, 0xEF charged, 0 not
	// known).
	std::array<uint8_t, 6> mac {};
	uint8_t                connection = 0;
	uint8_t                model      = 0;
	uint8_t                battery    = 0;
	// What its server's hello reply offers: whole controllers (DSU Bridge), and rumble.
	bool full        = false;
	bool can_vibrate = false;
	// The player the client has for it (SetPlayer; -1: none). It starts at none whenever the controller comes back after a gap.
	int player = -1;
};

// The servers to ask (IPv4 addresses or host names, with or without :port; the default port is 26760), at app start and whenever they
// change: the client runs from the first call on. The log says when data comes and what each controller is.
void SetServers(const std::vector<std::string>& servers);

// The controllers the servers serve now, in the order they appeared.
[[nodiscard]] std::vector<Controller> Controllers();

// What a server answers: 0 when it never did (or is not reachable), else how many controllers it serves now (-1: it answers, but serves none).
[[nodiscard]] int ServerControllers(const std::string& server);

// The player a controller plays for (0 to 3; -1: none), for its feedback, player lights, audio and microphone (input.cpp).
void SetPlayer(const std::string& server, int slot, int player);

// While the game menu is open: no rumble or trigger effects on the controllers (input.cpp, as on the Xbox controller).
void SetPaused(bool paused);

// Shows which controller it is: it rumbles for half a second and its light bar shows white, where its server offers them (the server keeps
// the white until the game's colour or, after 5 s, its own).
void Identify(const std::string& server, int slot);

// Audio for a player's controllers, from the audio threads: 16-bit samples at `rate` (the guest's ports play 48000 Hz), the speaker's mono,
// the haptics' stereo (left and right actuator). False when none of the player's controllers plays it: the caller plays it elsewhere or not
// at all.
bool SendSpeaker(int player, const int16_t* samples, uint32_t frames, uint32_t rate);
bool SendHaptics(int player, const int16_t* samples, uint32_t frames, uint32_t rate);
// Whether any of the player's controllers plays the speaker's (`speaker`) or the haptics' audio now: without one, the caller need not
// prepare the samples.
[[nodiscard]] bool PlaysAudio(int player, bool speaker);

// The microphone of player 1's controller: while a sink is set, the server is asked for it, and the sink gets its 16-bit mono samples (with
// their rate) on the client's thread. MicrophoneLive: samples came in the last half second.
using MicrophoneSink = void (*)(const int16_t* samples, uint32_t frames, uint32_t rate);
void               SetMicrophoneSink(MicrophoneSink sink);
[[nodiscard]] bool MicrophoneLive();

} // namespace Kyty::Uwp::Dsu

#endif // EMULATOR_SRC_UWP_DSU_H_
