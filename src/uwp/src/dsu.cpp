#include "dsu.h"

#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace Kyty::Uwp::Dsu {

namespace {

using namespace Pad;

constexpr uint16_t DEFAULT_PORT       = 26760;
// The app's own port: a fixed one, since the Xbox drops UDP traffic on ports 57344-65535 without an error, and a port the system picks can be
// there. The next ones when it is taken.
constexpr uint16_t LOCAL_PORT         = 26761;
constexpr int      LOCAL_PORT_TRIES   = 20;
constexpr uint16_t PROTOCOL_VERSION   = 1001;
constexpr uint32_t MESSAGE_INFO       = 0x100001;
constexpr uint32_t MESSAGE_DATA       = 0x100002;
constexpr size_t   HEADER_SIZE        = 16;
constexpr size_t   DATA_PACKET_SIZE   = 100;
constexpr int      SLOTS              = 4;
constexpr uint64_t FRESH_US           = 500000;   // data younger than this is the controller's input
constexpr uint64_t GONE_US            = 2000000;  // a controller without data for this long is gone
constexpr uint64_t ANSWERED_US        = 3000000;  // a server that sent nothing for this long does not answer
constexpr uint64_t REQUEST_EVERY_US   = 1000000;  // servers stop sending to a client that stops asking
constexpr uint64_t RESOLVE_EVERY_US   = 5000000;  // an address that could not be found is looked up again
constexpr uint64_t SUMMARY_EVERY_US   = 10000000;
constexpr float    STANDARD_GRAVITY   = 9.80665f;
constexpr float    DEGREES_TO_RADIANS = 3.14159265f / 180.0f;

// DSU Bridge's extension: messages 0x12xxxx with the same header. The server drops what a client asked for 5 s after the last time, so
// feedback is sent again every 200 ms, trigger effects every second while one is on, the microphone request every second.
constexpr uint32_t MESSAGE_HELLO      = 0x120000;
constexpr uint32_t MESSAGE_FEEDBACK   = 0x120001;
constexpr uint32_t MESSAGE_TRIGGERS   = 0x120003;
constexpr uint32_t MESSAGE_SPEAKER    = 0x120010;
constexpr uint32_t MESSAGE_MICROPHONE = 0x120011;
constexpr uint32_t MESSAGE_HAPTICS    = 0x120012;
constexpr uint16_t EXTENSION_VERSION  = 1;
// The capabilities in the server's hello reply.
constexpr uint32_t CAN_LIGHT_BAR   = 1u << 1u;
constexpr uint32_t CAN_PLAYER_LEDS = 1u << 2u;
constexpr uint32_t CAN_VIBRATION   = 1u << 3u;
constexpr uint32_t CAN_TRIGGERS    = 1u << 4u;
constexpr uint32_t CAN_HAPTICS     = 1u << 5u;
constexpr uint32_t CAN_SPEAKER     = 1u << 6u;
constexpr uint32_t CAN_MICROPHONE  = 1u << 7u;
// The feedback message's fields.
constexpr uint8_t  FIELD_LIGHT_BAR   = 1u << 0u;
constexpr uint8_t  FIELD_PLAYER_LEDS = 1u << 1u;
constexpr uint8_t  FIELD_RUMBLE      = 1u << 2u;
// The player lights of players 1 to 4 as on a PS5 (bit 0 is the leftmost of the five).
constexpr uint8_t  PLAYER_LEDS[] = {0x04, 0x0A, 0x15, 0x1B};
// Identify: how long and how strongly a controller rumbles.
constexpr uint64_t IDENTIFY_US     = 500000;
constexpr uint8_t  IDENTIFY_RUMBLE = 160;
constexpr uint32_t CAN_FULL        = 1u << 0u;
// The audio messages: a header (slot, channels, zero, sequence, sample rate), then at most this many samples, so that one fits a frame.
constexpr size_t   AUDIO_HEADER_SIZE = 12;
constexpr uint32_t MAX_AUDIO_SAMPLES = 720;
constexpr uint64_t FEEDBACK_CHECK_US = 8000;
constexpr uint64_t FEEDBACK_EVERY_US = 200000;
constexpr uint64_t TRIGGERS_EVERY_US = 1000000;

uint64_t NowMicroseconds() {
	LARGE_INTEGER counter {};
	LARGE_INTEGER frequency {};
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return static_cast<uint64_t>(counter.QuadPart) * 1000000u / static_cast<uint64_t>(frequency.QuadPart);
}

uint32_t Crc32(const uint8_t* data, size_t size) {
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

template <typename T>
T Read(const uint8_t* data) {
	T value;
	std::memcpy(&value, data, sizeof(T));
	return value;
}

template <typename T>
void Write(std::vector<uint8_t>& packet, size_t offset, T value) {
	std::memcpy(packet.data() + offset, &value, sizeof(T));
}

// A request to a server: the header ("DSUC", the protocol version, the length after the header, the CRC32, our id) and the message.
std::vector<uint8_t> Request(uint32_t client_id, uint32_t type, const std::vector<uint8_t>& payload) {
	std::vector<uint8_t> packet(HEADER_SIZE + 4 + payload.size());
	std::memcpy(packet.data(), "DSUC", 4);
	Write<uint16_t>(packet, 4, PROTOCOL_VERSION);
	Write<uint16_t>(packet, 6, static_cast<uint16_t>(4 + payload.size()));
	Write<uint32_t>(packet, 12, client_id);
	Write<uint32_t>(packet, 16, type);
	std::copy(payload.begin(), payload.end(), packet.begin() + HEADER_SIZE + 4);
	Write<uint32_t>(packet, 8, Crc32(packet.data(), packet.size()));
	return packet;
}

// An audio message's header: slot, channels, two zero bytes, the stream's packet number, the sample rate.
std::vector<uint8_t> AudioHeader(int slot, uint32_t channels, uint32_t sequence, uint32_t rate, size_t samples) {
	std::vector<uint8_t> payload(AUDIO_HEADER_SIZE + samples * sizeof(int16_t));
	payload[0] = static_cast<uint8_t>(slot);
	payload[1] = static_cast<uint8_t>(channels);
	Write<uint32_t>(payload, 4, sequence);
	Write<uint32_t>(payload, 8, rate);
	return payload;
}

// Whether `packet` is a whole server message whose CRC32 matches; `size` becomes its size by the header.
bool Valid(std::vector<uint8_t>& packet, size_t& size) {
	if (packet.size() < HEADER_SIZE + 4 || std::memcmp(packet.data(), "DSUS", 4) != 0 || Read<uint16_t>(packet.data() + 4) != PROTOCOL_VERSION) {
		return false;
	}
	size = HEADER_SIZE + Read<uint16_t>(packet.data() + 6);
	// The length counts the message type at least.
	if (size < HEADER_SIZE + 4 || size > packet.size()) {
		return false;
	}
	const uint32_t crc = Read<uint32_t>(packet.data() + 8);
	std::memset(packet.data() + 8, 0, 4);
	return Crc32(packet.data(), size) == crc;
}

const char* ModelName(uint8_t model) {
	switch (model) {
		case 1: return "no or partial motion sensors";
		case 2: return "motion sensors";
		default: return "no model";
	}
}

const char* ConnectionName(uint8_t connection) {
	switch (connection) {
		case 1: return "USB";
		case 2: return "Bluetooth";
		default: return "no connection type";
	}
}

std::string CapabilityNames(uint32_t capabilities) {
	static constexpr const char* NAMES[] = {"full controller", "light bar", "player LEDs", "vibration",
	                                        "trigger effects", "haptics", "speaker",     "microphone"};
	std::string names;
	for (uint32_t bit = 0; bit < 8; bit++) {
		if ((capabilities & (1u << bit)) != 0) {
			names += names.empty() ? "" : ", ";
			names += NAMES[bit];
		}
	}
	return names.empty() ? "nothing" : names;
}

// The touchpad's range is not part of the protocol: it starts at a DualShock 4's (as DSU Bridge and Dolphin's client) and grows to what the
// server sends.
struct TouchRange {
	uint16_t x = 1919;
	uint16_t y = 941;
};

// A controller data packet (100 bytes; the fields start after the header and the message type) as a PS5 pad. L2 and R2 come from the analog
// triggers alone: the controller library presses their buttons from the axes.
PadData Parse(const uint8_t* fields, TouchRange& range, int64_t& clock_offset) {
	PadData        pad;
	auto&          state    = pad.state;
	const uint8_t  buttons1 = fields[16];
	const uint8_t  buttons2 = fields[17];
	const auto     set      = [&state](bool pressed, uint32_t button) { state.buttons |= pressed ? button : 0u; };
	set((buttons1 & 0x80) != 0, PAD_BUTTON_LEFT);
	set((buttons1 & 0x40) != 0, PAD_BUTTON_DOWN);
	set((buttons1 & 0x20) != 0, PAD_BUTTON_RIGHT);
	set((buttons1 & 0x10) != 0, PAD_BUTTON_UP);
	set((buttons1 & 0x08) != 0, PAD_BUTTON_OPTIONS);
	set((buttons1 & 0x04) != 0, PAD_BUTTON_R3);
	set((buttons1 & 0x02) != 0, PAD_BUTTON_L3);
	set((buttons2 & 0x80) != 0, PAD_BUTTON_SQUARE);
	set((buttons2 & 0x40) != 0, PAD_BUTTON_CROSS);
	set((buttons2 & 0x20) != 0, PAD_BUTTON_CIRCLE);
	set((buttons2 & 0x10) != 0, PAD_BUTTON_TRIANGLE);
	set((buttons2 & 0x08) != 0, PAD_BUTTON_R1);
	set((buttons2 & 0x04) != 0, PAD_BUTTON_L1);
	pad.home = fields[18] != 0;
	set(fields[19] != 0, PAD_BUTTON_TOUCH_PAD);
	// The protocol's stick Y grows upward, the PS5 pad's downward.
	state.axes[static_cast<size_t>(Axis::LeftX)]        = fields[20];
	state.axes[static_cast<size_t>(Axis::LeftY)]        = static_cast<uint8_t>(255 - fields[21]);
	state.axes[static_cast<size_t>(Axis::RightX)]       = fields[22];
	state.axes[static_cast<size_t>(Axis::RightY)]       = static_cast<uint8_t>(255 - fields[23]);
	state.axes[static_cast<size_t>(Axis::TriggerLeft)]  = fields[35];
	state.axes[static_cast<size_t>(Axis::TriggerRight)] = fields[34];

	// The two fingers; the click is the touch button above.
	const auto finger = [&fields, &range](size_t offset, bool& down, float& x, float& y) {
		const uint8_t* touch = fields + offset;
		if (touch[0] == 0) {
			return;
		}
		const uint16_t raw_x = Read<uint16_t>(touch + 2);
		const uint16_t raw_y = Read<uint16_t>(touch + 4);
		range.x              = std::max(range.x, raw_x);
		range.y              = std::max(range.y, raw_y);
		down                 = true;
		x                    = std::clamp(static_cast<float>(raw_x) / range.x, 0.0f, 1.0f);
		y                    = std::clamp(static_cast<float>(raw_y) / range.y, 0.0f, 1.0f);
	};
	finger(36, state.touch, state.touch_x, state.touch_y);
	finger(42, state.touch2, state.touch2_x, state.touch2_y);

	// Motion: the protocol's axes as Dolphin's, Cemu's and Citra's clients read them (g and degrees per second), turned into the controller
	// library's.
	const uint64_t timestamp = Read<uint64_t>(fields + 48);
	const float    accel[3]  = {Read<float>(fields + 56), Read<float>(fields + 60), Read<float>(fields + 64)};
	const float    gyro[3]   = {Read<float>(fields + 68), Read<float>(fields + 72), Read<float>(fields + 76)};
	const bool     motion    = fields[2] == 2 || accel[0] != 0.0f || accel[1] != 0.0f || accel[2] != 0.0f;
	if (motion) {
		pad.accel = {-accel[0] * STANDARD_GRAVITY, -accel[1] * STANDARD_GRAVITY, -accel[2] * STANDARD_GRAVITY};
		pad.gyro  = {gyro[0] * DEGREES_TO_RADIANS, -gyro[1] * DEGREES_TO_RADIANS, -gyro[2] * DEGREES_TO_RADIANS};
		// The server's clock on ours (the emulated motion's too, so that switching between them needs nothing): the offset is set again when
		// they drift apart by more than a tenth of a second.
		const auto now = static_cast<int64_t>(NowMicroseconds());
		if (timestamp == 0) {
			pad.motion_time = static_cast<uint64_t>(now);
		} else {
			if (clock_offset == 0 || std::abs(static_cast<int64_t>(timestamp) + clock_offset - now) > 100000) {
				clock_offset = now - static_cast<int64_t>(timestamp);
			}
			pad.motion_time = static_cast<uint64_t>(static_cast<int64_t>(timestamp) + clock_offset);
		}
	}
	return pad;
}

bool TriggerOff(const std::array<uint8_t, 11>& block) {
	return block[0] == 0x00 || block[0] == 0x05;
}

std::atomic<SOCKET>         g_socket {INVALID_SOCKET};
std::atomic_bool            g_started {false};
std::atomic_bool            g_paused {false};
std::atomic<MicrophoneSink> g_microphone_sink {nullptr};
std::atomic_uint64_t        g_microphone_received {0};

// A slot of a server: the controller in it (none while `received` is 0), the player it plays for, and its feedback and audio streams.
struct Slot {
	PadData    data;
	uint64_t   received = 0; // when the newest data came (NowMicroseconds)
	uint64_t   order    = 0; // when the controller appeared, for the order of the list
	TouchRange range;
	int64_t    clock_offset = 0;
	int        player       = -1;
	uint32_t   packets      = 0; // since the last summary
	// From the data packets: MAC address, connection, model, battery (Controller).
	std::array<uint8_t, 6> mac {};
	uint8_t                connection     = 0;
	uint8_t                model          = 0;
	uint8_t                battery        = 0;
	uint64_t               identify_until = 0;

	std::vector<uint8_t> feedback;
	uint64_t             feedback_time = 0;
	std::vector<uint8_t> triggers;
	uint64_t             triggers_time = 0;
	uint32_t             speaker_sequence    = 0;
	uint32_t             haptics_sequence    = 0;
	uint32_t             microphone_sequence = 0;
};

struct Server {
	std::string text; // as set
	std::string host;
	uint16_t    port = DEFAULT_PORT;
	sockaddr_in address {};
	bool        resolved     = false;
	bool        logged_error = false;
	uint64_t    next_resolve = 0;
	uint64_t    next_request = 0;
	uint64_t    next_summary = 0;
	uint64_t    answered     = 0; // when it last sent anything
	uint32_t    client_id    = 0;
	uint32_t    capabilities = 0;
	uint32_t    microphone   = 0; // microphone packets since the last summary

	std::array<Slot, SLOTS> slots;
};

std::mutex                           g_mutex; // guards everything below and the servers' state
std::vector<std::unique_ptr<Server>> g_servers;
std::vector<std::string>             g_wanted;
bool                                 g_wanted_changed = false;
uint64_t                             g_order          = 0;

void SendTo(const Server& server, uint32_t type, const std::vector<uint8_t>& payload) {
	const SOCKET socket = g_socket;
	if (socket == INVALID_SOCKET || !server.resolved) {
		return;
	}
	const auto packet = Request(server.client_id, type, payload);
	(void)sendto(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
	             reinterpret_cast<const sockaddr*>(&server.address), sizeof(server.address));
}

// A slot's feedback from its player (`state`): what changed at once, the rest again now and then so that the server keeps it. While it
// identifies itself, it rumbles and its light bar is white instead.
void SendFeedback(Server& server, int index, const HostPadFeedback& state, uint64_t now) {
	auto&                slot        = server.slots[index];
	const bool           identifying = now < slot.identify_until;
	std::vector<uint8_t> message(10, 0);
	message[0] = static_cast<uint8_t>(index);
	if ((server.capabilities & CAN_LIGHT_BAR) != 0 && (state.light_bar_set || identifying)) {
		message[1] |= FIELD_LIGHT_BAR;
		if (identifying) {
			std::fill_n(message.begin() + 2, 3, uint8_t {255});
		} else {
			std::copy(state.light_bar.begin(), state.light_bar.end(), message.begin() + 2);
		}
	}
	if ((server.capabilities & CAN_PLAYER_LEDS) != 0) {
		message[1] |= FIELD_PLAYER_LEDS;
		message[5] = slot.player >= 0 ? PLAYER_LEDS[slot.player] : 0;
	}
	if ((server.capabilities & CAN_VIBRATION) != 0) {
		message[1] |= FIELD_RUMBLE;
		message[6] = identifying ? IDENTIFY_RUMBLE : state.large_motor;
		message[7] = identifying ? IDENTIFY_RUMBLE : state.small_motor;
	}
	if (message[1] != 0 && (message != slot.feedback || now - slot.feedback_time >= FEEDBACK_EVERY_US)) {
		SendTo(server, MESSAGE_FEEDBACK, message);
		slot.feedback      = std::move(message);
		slot.feedback_time = now;
	}

	if ((server.capabilities & CAN_TRIGGERS) != 0) {
		std::vector<uint8_t> effects(2 + 2 * 11, 0);
		effects[0] = static_cast<uint8_t>(index);
		effects[1] = 0x03; // both triggers
		std::copy(state.trigger_blocks[0].begin(), state.trigger_blocks[0].end(), effects.begin() + 2);
		std::copy(state.trigger_blocks[1].begin(), state.trigger_blocks[1].end(), effects.begin() + 2 + 11);
		const bool on = !TriggerOff(state.trigger_blocks[0]) || !TriggerOff(state.trigger_blocks[1]);
		if (effects != slot.triggers || (on && now - slot.triggers_time >= TRIGGERS_EVERY_US)) {
			SendTo(server, MESSAGE_TRIGGERS, effects);
			slot.triggers      = std::move(effects);
			slot.triggers_time = now;
		}
	}
}

// The servers as set: the ones still wanted stay as they are, new ones start unresolved.
void TakeWanted() {
	if (!g_wanted_changed) {
		return;
	}
	g_wanted_changed = false;
	std::vector<std::unique_ptr<Server>> servers;
	for (const auto& text: g_wanted) {
		const auto old = std::find_if(g_servers.begin(), g_servers.end(), [&text](const auto& server) { return server != nullptr && server->text == text; });
		if (old != g_servers.end()) {
			servers.push_back(std::move(*old));
			continue;
		}
		auto server       = std::make_unique<Server>();
		server->text      = text;
		server->host      = text;
		server->client_id = std::random_device {}();
		const auto colon  = text.rfind(':');
		if (colon != std::string::npos) {
			server->host = text.substr(0, colon);
			server->port = static_cast<uint16_t>(std::strtoul(text.c_str() + colon + 1, nullptr, 10));
		}
		Log("dsu: asking %s:%u for its controllers\n", server->host.c_str(), server->port);
		servers.push_back(std::move(server));
	}
	for (const auto& server: g_servers) {
		if (server != nullptr) {
			Log("dsu: no longer asking %s\n", server->text.c_str());
		}
	}
	g_servers = std::move(servers);
}

// Looks up the addresses not found yet, without holding the lock (a host name can take a while).
void Resolve(uint64_t now) {
	struct Lookup {
		std::string text;
		std::string host;
		uint16_t    port;
	};
	std::vector<Lookup> lookups;
	{
		std::lock_guard lock(g_mutex);
		for (auto& server: g_servers) {
			if (!server->resolved && now >= server->next_resolve) {
				server->next_resolve = now + RESOLVE_EVERY_US;
				lookups.push_back({server->text, server->host, server->port});
			}
		}
	}
	for (const auto& lookup: lookups) {
		addrinfo  hints {};
		addrinfo* found   = nullptr;
		hints.ai_family   = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		const bool ok     = getaddrinfo(lookup.host.c_str(), std::to_string(lookup.port).c_str(), &hints, &found) == 0 && found != nullptr &&
		                found->ai_addrlen >= sizeof(sockaddr_in);
		sockaddr_in address {};
		if (ok) {
			std::memcpy(&address, found->ai_addr, sizeof(address));
		}
		const int error = WSAGetLastError();
		if (found != nullptr) {
			freeaddrinfo(found);
		}
		std::lock_guard lock(g_mutex);
		for (auto& server: g_servers) {
			if (server->text != lookup.text) {
				continue;
			}
			if (ok) {
				server->address  = address;
				server->resolved = true;
			} else if (!server->logged_error) {
				server->logged_error = true;
				Log("dsu: cannot find %s (error %d)\n", lookup.host.c_str(), error);
			}
		}
	}
}

// The requests, every second: all slots' data, the controller info (so that a server without controllers answers too), the extension's
// hello, and the microphone of player 1's controller.
void SendRequests(Server& server, uint64_t now, bool& microphone_asked) {
	if (now < server.next_request) {
		return;
	}
	server.next_request = now + REQUEST_EVERY_US;
	SendTo(server, MESSAGE_DATA, {0, 0, 0, 0, 0, 0, 0, 0});
	SendTo(server, MESSAGE_INFO, {4, 0, 0, 0, 0, 1, 2, 3});
	SendTo(server, MESSAGE_HELLO, {static_cast<uint8_t>(EXTENSION_VERSION & 0xffu), static_cast<uint8_t>(EXTENSION_VERSION >> 8u), 0, 0});
	if (microphone_asked || (server.capabilities & CAN_MICROPHONE) == 0 || g_microphone_sink.load() == nullptr) {
		return;
	}
	for (int index = 0; index < SLOTS; index++) {
		auto& slot = server.slots[index];
		if (slot.received != 0 && slot.player == 0) {
			SendTo(server, MESSAGE_MICROPHONE, AudioHeader(index, 1, ++slot.microphone_sequence, 48000, 0));
			microphone_asked = true;
			return;
		}
	}
}

void Summarize(Server& server, uint64_t now) {
	if (now < server.next_summary) {
		return;
	}
	if (server.next_summary != 0) {
		for (int index = 0; index < SLOTS; index++) {
			auto& slot = server.slots[index];
			if (slot.received == 0) {
				continue;
			}
			const auto& state = slot.data.state;
			Log("dsu: %s slot %d: %u packets in 10 s, player %d; sticks %u %u %u %u, touch %s; sent %u speaker and %u haptics packets so far\n",
			    server.text.c_str(), index, slot.packets, slot.player + 1, state.axes[0], state.axes[1], state.axes[2], state.axes[3],
			    state.touch ? "on" : "off", slot.speaker_sequence, slot.haptics_sequence);
			slot.packets = 0;
		}
		if (server.microphone != 0) {
			Log("dsu: %s: %u microphone packets in 10 s\n", server.text.c_str(), server.microphone);
			server.microphone = 0;
		}
	}
	server.next_summary = now + SUMMARY_EVERY_US;
}

void Receive(const sockaddr_in& from, std::vector<uint8_t>& packet, uint64_t now) {
	size_t size = 0;
	if (!Valid(packet, size)) {
		return;
	}
	const uint32_t type    = Read<uint32_t>(packet.data() + 16);
	const uint8_t* fields  = packet.data() + HEADER_SIZE + 4;
	const size_t   payload = size - HEADER_SIZE - 4;

	std::lock_guard lock(g_mutex);
	const auto      found = std::find_if(g_servers.begin(), g_servers.end(), [&from](const auto& server) {
        return server->resolved && server->address.sin_addr.s_addr == from.sin_addr.s_addr && server->address.sin_port == from.sin_port;
    });
	if (found == g_servers.end()) {
		return;
	}
	auto& server    = **found;
	server.answered = now;
	if (type == MESSAGE_HELLO && payload >= 8) {
		const uint32_t capabilities = Read<uint32_t>(fields + 4);
		if (server.capabilities != capabilities) {
			server.capabilities = capabilities;
			Log("dsu: %s offers %s\n", server.text.c_str(), CapabilityNames(capabilities).c_str());
		}
		return;
	}
	if (type == MESSAGE_MICROPHONE && payload >= AUDIO_HEADER_SIZE) {
		const int index = fields[0];
		if (index >= SLOTS || server.slots[index].player != 0) {
			return;
		}
		const uint32_t       channels = std::max<uint32_t>(fields[1], 1);
		const uint32_t       frames   = static_cast<uint32_t>((payload - AUDIO_HEADER_SIZE) / sizeof(int16_t) / channels);
		const uint32_t       rate     = Read<uint32_t>(fields + 8);
		std::vector<int16_t> mono(frames);
		for (uint32_t frame = 0; frame < frames; frame++) {
			mono[frame] = Read<int16_t>(fields + AUDIO_HEADER_SIZE + static_cast<size_t>(frame) * channels * sizeof(int16_t));
		}
		if (auto* sink = g_microphone_sink.load(); sink != nullptr && frames != 0 && rate != 0) {
			if (g_microphone_received.exchange(now) == 0) {
				Log("dsu: microphone audio comes from %s slot %d (%u Hz, %u ch)\n", server.text.c_str(), index, rate, channels);
			}
			sink(mono.data(), frames, rate);
			server.microphone++;
		}
		return;
	}
	if (type != MESSAGE_DATA || size < DATA_PACKET_SIZE) {
		return;
	}
	const int index = fields[0];
	if (index >= SLOTS || fields[1] != 2 || fields[11] == 0) {
		return; // the slot has no controller
	}
	auto& slot = server.slots[index];
	if (slot.received == 0) {
		slot       = {};
		slot.order = ++g_order;
		Log("dsu: %s slot %d: a controller with %s, %s, battery 0x%02x\n", server.text.c_str(), index, ModelName(fields[2]),
		    ConnectionName(fields[3]), fields[10]);
	}
	slot.data       = Parse(fields, slot.range, slot.clock_offset);
	slot.received   = now;
	slot.model      = fields[2];
	slot.connection = fields[3];
	std::copy(fields + 4, fields + 10, slot.mac.begin());
	slot.battery = fields[10];
	slot.packets++;
}

void Run() {
	WSADATA wsa {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		Log("dsu: no network (WSAStartup %d)\n", WSAGetLastError());
		return;
	}
	const SOCKET udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp == INVALID_SOCKET) {
		Log("dsu: no socket (error %d)\n", WSAGetLastError());
		return;
	}
	uint16_t local_port = 0;
	for (int i = 0; i < LOCAL_PORT_TRIES && local_port == 0; i++) {
		sockaddr_in local {};
		local.sin_family      = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port        = htons(static_cast<u_short>(LOCAL_PORT + i));
		if (bind(udp, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) {
			local_port = static_cast<uint16_t>(LOCAL_PORT + i);
		}
	}
	if (local_port == 0) {
		Log("dsu: no local port (error %d)\n", WSAGetLastError());
		return;
	}
	// Short, so that feedback goes out soon also while no data comes.
	DWORD timeout_ms = 20;
	setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
	Log("dsu: the client listens on port %u\n", local_port);
	g_socket = udp;

	uint64_t             next_feedback = 0;
	std::vector<uint8_t> packet(2048);
	for (;;) {
		const uint64_t now = NowMicroseconds();
		{
			std::lock_guard lock(g_mutex);
			TakeWanted();
		}
		Resolve(now);
		// The players' feedback, read before the lock (the controller library has locks of its own); while paused no rumble or trigger effects.
		const bool                                   feedback_due = now >= next_feedback;
		std::array<HostPadFeedback, MAX_PLAYERS> feedback {};
		if (feedback_due) {
			next_feedback = now + FEEDBACK_CHECK_US;
			for (int player = 0; player < MAX_PLAYERS; player++) {
				feedback[player] = GetPlayerFeedback(player);
				if (g_paused) {
					feedback[player].large_motor = 0;
					feedback[player].small_motor = 0;
					for (auto& block: feedback[player].trigger_blocks) {
						block    = {};
						block[0] = 0x05;
					}
				}
			}
		}
		{
			std::lock_guard lock(g_mutex);
			bool microphone_asked = false;
			for (auto& server: g_servers) {
				SendRequests(*server, now, microphone_asked);
				for (int index = 0; index < SLOTS; index++) {
					auto& slot = server->slots[index];
					if (slot.received != 0 && now - slot.received > GONE_US) {
						Log("dsu: %s slot %d: the controller is gone\n", server->text.c_str(), index);
						slot = {};
					}
				}
				Summarize(*server, now);
			}
			if (feedback_due) {
				// A controller that plays for none gets none (and no player lights), except while it identifies itself.
				for (auto& server: g_servers) {
					for (int index = 0; index < SLOTS; index++) {
						const auto& slot    = server->slots[index];
						const bool  playing = slot.player >= 0 && slot.player < MAX_PLAYERS;
						if (slot.received != 0 && (playing || now < slot.identify_until + FEEDBACK_EVERY_US)) {
							SendFeedback(*server, index, playing ? feedback[slot.player] : HostPadFeedback {}, now);
						}
					}
				}
			}
		}
		packet.resize(2048);
		sockaddr_in from {};
		int         from_size = sizeof(from);
		const int   received  = recvfrom(udp, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()), 0,
		                                 reinterpret_cast<sockaddr*>(&from), &from_size);
		if (received <= 0) {
			// The timeout, or a server is not there (yet): a closed port answers at once, so wait before the next try.
			if (received < 0 && WSAGetLastError() != WSAETIMEDOUT) {
				Sleep(20);
			}
			continue;
		}
		packet.resize(static_cast<size_t>(received));
		Receive(from, packet, NowMicroseconds());
	}
}

// Whether a slot's controller plays a player's audio of `capability` now (its server offers it, and its data is fresh).
bool SlotPlays(const Server& server, const Slot& slot, int player, uint32_t capability, uint64_t now) {
	return (server.capabilities & capability) != 0 && slot.received != 0 && slot.player == player && now - slot.received <= FRESH_US;
}

// Audio to a player's controllers whose servers play it, in messages of at most MAX_AUDIO_SAMPLES samples.
bool SendAudio(int player, uint32_t type, uint32_t capability, uint32_t Slot::*sequence, const int16_t* samples, uint32_t frames,
               uint32_t channels, uint32_t rate) {
	if (samples == nullptr || frames == 0 || player < 0) {
		return false;
	}
	const uint64_t  now         = NowMicroseconds();
	const uint32_t  per_message = MAX_AUDIO_SAMPLES / channels;
	bool            sent        = false;
	std::lock_guard lock(g_mutex);
	for (auto& server: g_servers) {
		for (int index = 0; index < SLOTS; index++) {
			auto& slot = server->slots[index];
			if (!SlotPlays(*server, slot, player, capability, now)) {
				continue;
			}
			for (uint32_t first = 0; first < frames; first += per_message) {
				const uint32_t count   = std::min(per_message, frames - first);
				auto           message = AudioHeader(index, channels, ++(slot.*sequence), rate, static_cast<size_t>(count) * channels);
				std::memcpy(message.data() + AUDIO_HEADER_SIZE, samples + static_cast<size_t>(first) * channels,
				            static_cast<size_t>(count) * channels * sizeof(int16_t));
				SendTo(*server, type, message);
			}
			sent = true;
		}
	}
	return sent;
}

} // namespace

void SetServers(const std::vector<std::string>& servers) {
	{
		std::lock_guard lock(g_mutex);
		if (servers != g_wanted) {
			g_wanted         = servers;
			g_wanted_changed = true;
		}
	}
	// No socket and no thread until there is a server to ask (DSU controllers off: never).
	if (!servers.empty() && !g_started.exchange(true)) {
		std::thread(Run).detach();
	}
}

std::vector<Controller> Controllers() {
	std::vector<std::pair<uint64_t, Controller>> found;
	const uint64_t                               now = NowMicroseconds();
	{
		std::lock_guard lock(g_mutex);
		for (const auto& server: g_servers) {
			for (int index = 0; index < SLOTS; index++) {
				const auto& slot = server->slots[index];
				if (slot.received == 0) {
					continue;
				}
				Controller controller;
				controller.server      = server->text;
				controller.slot        = index;
				controller.fresh       = now - slot.received <= FRESH_US;
				controller.data        = slot.data;
				controller.mac         = slot.mac;
				controller.connection  = slot.connection;
				controller.model       = slot.model;
				controller.battery     = slot.battery;
				controller.full        = (server->capabilities & CAN_FULL) != 0;
				controller.can_vibrate = (server->capabilities & CAN_VIBRATION) != 0;
				controller.player      = slot.player;
				found.emplace_back(slot.order, std::move(controller));
			}
		}
	}
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<Controller> controllers;
	for (auto& [order, controller]: found) {
		controllers.push_back(std::move(controller));
	}
	return controllers;
}

int ServerControllers(const std::string& text) {
	const uint64_t  now = NowMicroseconds();
	std::lock_guard lock(g_mutex);
	for (const auto& server: g_servers) {
		if (server->text != text) {
			continue;
		}
		const auto count = std::count_if(server->slots.begin(), server->slots.end(), [](const Slot& slot) { return slot.received != 0; });
		if (count != 0) {
			return static_cast<int>(count);
		}
		return server->answered != 0 && now - server->answered <= ANSWERED_US ? -1 : 0;
	}
	return 0;
}

void SetPlayer(const std::string& text, int slot, int player) {
	if (slot < 0 || slot >= SLOTS) {
		return;
	}
	std::lock_guard lock(g_mutex);
	for (auto& server: g_servers) {
		if (server->text == text && server->slots[slot].received != 0) {
			server->slots[slot].player = player >= 0 && player < MAX_PLAYERS ? player : -1;
		}
	}
}

void SetPaused(bool paused) {
	g_paused = paused;
}

void Identify(const std::string& text, int slot) {
	if (slot < 0 || slot >= SLOTS) {
		return;
	}
	std::lock_guard lock(g_mutex);
	for (auto& server: g_servers) {
		if (server->text == text && server->slots[slot].received != 0) {
			server->slots[slot].identify_until = NowMicroseconds() + IDENTIFY_US;
			Log("dsu: %s slot %d identifies itself\n", text.c_str(), slot);
		}
	}
}

bool SendSpeaker(int player, const int16_t* samples, uint32_t frames, uint32_t rate) {
	return SendAudio(player, MESSAGE_SPEAKER, CAN_SPEAKER, &Slot::speaker_sequence, samples, frames, 1, rate);
}

bool SendHaptics(int player, const int16_t* samples, uint32_t frames, uint32_t rate) {
	return SendAudio(player, MESSAGE_HAPTICS, CAN_HAPTICS, &Slot::haptics_sequence, samples, frames, 2, rate);
}

bool PlaysAudio(int player, bool speaker) {
	if (player < 0) {
		return false;
	}
	const uint64_t  now = NowMicroseconds();
	std::lock_guard lock(g_mutex);
	for (const auto& server: g_servers) {
		for (const auto& slot: server->slots) {
			if (SlotPlays(*server, slot, player, speaker ? CAN_SPEAKER : CAN_HAPTICS, now)) {
				return true;
			}
		}
	}
	return false;
}

void SetMicrophoneSink(MicrophoneSink sink) {
	g_microphone_sink = sink;
	if (sink == nullptr) {
		g_microphone_received = 0;
	}
}

bool MicrophoneLive() {
	const uint64_t received = g_microphone_received;
	return received != 0 && g_microphone_sink.load() != nullptr && NowMicroseconds() - received <= FRESH_US;
}

} // namespace Kyty::Uwp::Dsu
