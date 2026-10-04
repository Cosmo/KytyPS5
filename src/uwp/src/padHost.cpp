#include "padHost.h"

#include <mutex>

namespace Kyty::Uwp::Pad {

namespace {

std::mutex                                g_mutex;
std::array<bool, MAX_PLAYERS>             g_connected {};
std::array<HostPadState, MAX_PLAYERS>     g_state {};

bool Valid(int player) {
	return player >= 0 && player < MAX_PLAYERS;
}

} // namespace

void SetPlayerConnected(int player, bool connected) {
	if (Valid(player)) {
		std::lock_guard lock(g_mutex);
		g_connected[player] = connected;
	}
}

void SetPlayerState(int player, const HostPadState& state) {
	if (Valid(player)) {
		std::lock_guard lock(g_mutex);
		g_state[player] = state;
	}
}

HostPadFeedback GetPlayerFeedback(int /*player*/) {
	return {}; // nothing asks for rumble without a game
}

} // namespace Kyty::Uwp::Pad
