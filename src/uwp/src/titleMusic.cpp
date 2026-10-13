#include "titleMusic.h"

#include "gameSource.h"
#include "log.h"

#include "common/archive.h"
#include "common/file.h"

#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

extern "C" {
#include "libatrac9.h"
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Kyty::Uwp::TitleMusic {

namespace {

using Clock = std::chrono::steady_clock;

// The focus rests this long on a game before its music starts, so that moving along the row plays nothing.
constexpr auto  WAIT     = std::chrono::milliseconds(700);
constexpr auto  FADE_IN  = std::chrono::milliseconds(1000);
constexpr auto  FADE_OUT = std::chrono::milliseconds(300);
constexpr auto  STEP     = std::chrono::milliseconds(25);
constexpr float VOLUME   = 0.6f;
// LibAtrac9 reads a few bytes past a frame's end: the data is decoded from a copy with zeros after it.
constexpr size_t PADDING = 16;

// Decoded music: interleaved 16-bit samples, and the loop in sample frames (length 0: all of it).
struct Music {
	uint32_t             channels = 0;
	uint32_t             rate     = 0;
	std::vector<int16_t> pcm;
	uint32_t             loop_begin  = 0;
	uint32_t             loop_length = 0;
};

uint32_t Le32(const uint8_t* data) {
	return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) | (static_cast<uint32_t>(data[2]) << 16u) |
	       (static_cast<uint32_t>(data[3]) << 24u);
}

uint16_t Le16(const uint8_t* data) {
	return static_cast<uint16_t>(data[0] | (data[1] << 8u));
}

// snd0.at9: a RIFF WAVE file with the ATRAC9 format (WAVEFORMATEXTENSIBLE, the codec's configuration at byte 44), the length and the
// encoder's delay (fact), the loop (smpl) and the data.
std::optional<Music> Decode(const std::vector<uint8_t>& file, std::string& problem) {
	if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
		problem = "not a RIFF WAVE file";
		return std::nullopt;
	}
	const uint8_t* format      = nullptr;
	size_t         format_size = 0;
	const uint8_t* data        = nullptr;
	size_t         data_size   = 0;
	uint32_t       total       = 0;
	uint32_t       delay       = 0;
	uint32_t       loop_start  = 0;
	uint32_t       loop_end    = 0;
	bool           looped      = false;
	for (size_t offset = 12; offset + 8 <= file.size();) {
		const uint8_t* chunk   = file.data() + offset;
		const size_t   payload = offset + 8;
		const size_t   size    = std::min<size_t>(Le32(chunk + 4), file.size() - payload);
		if (std::memcmp(chunk, "fmt ", 4) == 0) {
			format      = chunk + 8;
			format_size = size;
		} else if (std::memcmp(chunk, "fact", 4) == 0 && size >= 8) {
			total = Le32(chunk + 8);
			delay = Le32(chunk + 12);
		} else if (std::memcmp(chunk, "smpl", 4) == 0 && size >= 36 + 24 && Le32(chunk + 8 + 28) != 0) {
			loop_start = Le32(chunk + 8 + 36 + 8);
			loop_end   = Le32(chunk + 8 + 36 + 12);
			looped     = true;
		} else if (std::memcmp(chunk, "data", 4) == 0) {
			data      = chunk + 8;
			data_size = size;
		}
		offset = payload + size + (size & 1u);
	}
	if (format == nullptr || format_size < 48 || Le16(format) != 0xFFFE || data == nullptr) {
		problem = "no ATRAC9 format or no data";
		return std::nullopt;
	}
	void*         handle = Atrac9GetHandle();
	unsigned char config[ATRAC9_CONFIG_DATA_SIZE];
	std::memcpy(config, format + 44, sizeof(config));
	Atrac9CodecInfo info {};
	if (handle == nullptr || Atrac9InitDecoder(handle, config) != 0 || Atrac9GetCodecInfo(handle, &info) != 0 || info.channels <= 0 ||
	    info.frameSamples <= 0 || info.superframeSize <= 0) {
		if (handle != nullptr) {
			Atrac9ReleaseHandle(handle);
		}
		problem = "an ATRAC9 configuration LibAtrac9 does not take";
		return std::nullopt;
	}
	Music music;
	music.channels = static_cast<uint32_t>(info.channels);
	music.rate     = static_cast<uint32_t>(info.samplingRate);
	std::vector<uint8_t> input(data, data + data_size);
	input.resize(data_size + PADDING, 0);
	std::vector<int16_t> frame(static_cast<size_t>(info.frameSamples) * music.channels);
	const auto           superframe = static_cast<size_t>(info.superframeSize);
	bool                 broken     = false;
	for (size_t start = 0; start + superframe <= data_size && !broken; start += superframe) {
		size_t position = start;
		for (int index = 0; index < info.framesInSuperframe; index++) {
			int used = 0;
			if (Atrac9Decode(handle, input.data() + position, frame.data(), &used, 0) != 0 || used <= 0) {
				broken = true;
				break;
			}
			music.pcm.insert(music.pcm.end(), frame.begin(), frame.end());
			position += static_cast<size_t>(used);
		}
	}
	Atrac9ReleaseHandle(handle);
	// The encoder's delay goes, and what lies past the length.
	const size_t frames = music.pcm.size() / music.channels;
	const size_t first  = std::min<size_t>(delay, frames);
	const size_t count  = total != 0 ? std::min<size_t>(total, frames - first) : frames - first;
	music.pcm.erase(music.pcm.begin(), music.pcm.begin() + static_cast<std::ptrdiff_t>(first * music.channels));
	music.pcm.resize(count * music.channels);
	if (count == 0) {
		problem = broken ? "no frame decodes" : "no samples";
		return std::nullopt;
	}
	if (looped && loop_end > loop_start && loop_start < count) {
		music.loop_begin  = loop_start;
		music.loop_length = static_cast<uint32_t>(std::min<size_t>(static_cast<size_t>(loop_end) + 1, count) - loop_start);
	}
	return music;
}

// The sound files of the game's sce_sys, for the log when it has no snd0.at9.
std::string SoundFiles(const std::filesystem::path& game) {
	const auto folder = IsArchivePath(game) ? Common::MakeArchivePath(game, "sce_sys") : game / L"sce_sys";
	std::string list;
	for (const auto& entry: Common::File::GetDirEntries(folder)) {
		auto name = entry.name;
		std::transform(name.begin(), name.end(), name.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		const bool sound = name.ends_with(".at9") || name.ends_with(".wav") || name.ends_with(".ogg") || name.ends_with(".mp3") ||
		                   name.ends_with(".opus") || name.find("snd") != std::string::npos || name.find("bgm") != std::string::npos ||
		                   name.find("music") != std::string::npos;
		if (sound) {
			list += (list.empty() ? "" : ", ") + entry.name;
		}
	}
	return list.empty() ? "none" : list;
}

std::shared_ptr<Music> Load(const GameEntry& game) {
	static std::set<std::filesystem::path> logged; // once per game and run, however often it gets the focus
	const bool log   = logged.insert(game.folder).second;
	const auto bytes = ReadGameBinary(game.folder / L"sce_sys" / L"snd0.at9");
	if (!bytes) {
		if (log) {
			Log("title music: %s has no sce_sys/snd0.at9; sound files in sce_sys: %s\n", game.title_id.c_str(), SoundFiles(game.folder).c_str());
		}
		return nullptr;
	}
	const auto  started = Clock::now();
	std::string problem;
	auto        music   = Decode(*bytes, problem);
	if (!music) {
		if (log) {
			Log("title music: %s's snd0.at9 (%zu bytes) can't be played: %s\n", game.title_id.c_str(), bytes->size(), problem.c_str());
		}
		return nullptr;
	}
	if (log) {
		const double seconds = static_cast<double>(music->pcm.size() / music->channels) / music->rate;
		Log("title music: %s: snd0.at9, %u ch, %u Hz, %.1f s, %s, decoded in %lld ms\n", game.title_id.c_str(), music->channels, music->rate,
		    seconds,
		    music->loop_length != 0 ? ("loop from " + std::to_string(music->loop_begin * 1000 / music->rate) + " ms").c_str() : "loops whole",
		    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()));
	}
	return std::make_shared<Music>(std::move(*music));
}

// The music thread's wishes: the game whose music should play (none: silence), numbered so that the thread sees each new one.
std::mutex               g_mutex;
std::condition_variable  g_wake;
uint64_t                 g_generation = 0;
std::optional<GameEntry> g_wanted;
std::once_flag           g_thread;

bool Superseded(uint64_t generation) {
	std::lock_guard lock(g_mutex);
	return g_generation != generation;
}

struct Player {
	IXAudio2*               engine = nullptr;
	IXAudio2MasteringVoice* master = nullptr;
	IXAudio2SourceVoice*    voice  = nullptr;
	std::shared_ptr<Music>  music; // the samples the voice reads
	bool                    failed = false;

	void FadeOut() {
		if (voice == nullptr) {
			return;
		}
		float volume = 0.0f;
		voice->GetVolume(&volume);
		const int steps = static_cast<int>(FADE_OUT / STEP);
		for (int step = steps - 1; step >= 0; step--) {
			voice->SetVolume(volume * static_cast<float>(step) / static_cast<float>(steps));
			std::this_thread::sleep_for(STEP);
		}
		voice->Stop();
		voice->DestroyVoice();
		voice = nullptr;
		music.reset();
	}

	bool Start(std::shared_ptr<Music> next, uint64_t generation) {
		if (engine == nullptr && !failed) {
			if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(engine->CreateMasteringVoice(&master))) {
				Log("title music: no audio output\n");
				failed = true;
			}
		}
		if (failed || next->channels > 2) {
			return false;
		}
		WAVEFORMATEX format {};
		format.wFormatTag      = WAVE_FORMAT_PCM;
		format.nChannels       = static_cast<WORD>(next->channels);
		format.nSamplesPerSec  = next->rate;
		format.wBitsPerSample  = 16;
		format.nBlockAlign     = static_cast<WORD>(next->channels * 2);
		format.nAvgBytesPerSec = next->rate * format.nBlockAlign;
		if (FAILED(engine->CreateSourceVoice(&voice, &format))) {
			voice = nullptr;
			return false;
		}
		XAUDIO2_BUFFER buffer {};
		buffer.Flags      = XAUDIO2_END_OF_STREAM;
		buffer.AudioBytes = static_cast<UINT32>(next->pcm.size() * sizeof(int16_t));
		buffer.pAudioData = reinterpret_cast<const BYTE*>(next->pcm.data());
		buffer.LoopBegin  = next->loop_begin;
		buffer.LoopLength = next->loop_length;
		buffer.LoopCount  = XAUDIO2_LOOP_INFINITE;
		music             = std::move(next);
		voice->SetVolume(0.0f);
		if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start())) {
			voice->DestroyVoice();
			voice = nullptr;
			music.reset();
			return false;
		}
		const int steps = static_cast<int>(FADE_IN / STEP);
		for (int step = 1; step <= steps && !Superseded(generation); step++) {
			voice->SetVolume(VOLUME * static_cast<float>(step) / static_cast<float>(steps));
			std::this_thread::sleep_for(STEP);
		}
		return true;
	}
};

void Run() {
	Player                 player;
	uint64_t               seen = 0;
	std::filesystem::path  playing; // whose music the voice plays
	std::filesystem::path  cached_folder;
	std::shared_ptr<Music> cached;
	for (;;) {
		std::optional<GameEntry> wanted;
		uint64_t                 generation = 0;
		{
			std::unique_lock lock(g_mutex);
			g_wake.wait(lock, [&seen] { return g_generation != seen; });
			seen       = g_generation;
			generation = seen;
			wanted     = g_wanted;
			if (wanted && wanted->folder == playing && player.voice != nullptr) {
				continue; // its music plays already
			}
			// A moment on the game first; another wish within it replaces this one.
			if (wanted && g_wake.wait_for(lock, WAIT, [generation] { return g_generation != generation; })) {
				continue;
			}
		}
		player.FadeOut();
		playing.clear();
		if (!wanted) {
			continue;
		}
		if (wanted->folder != cached_folder) {
			cached        = Load(*wanted);
			cached_folder = wanted->folder;
		}
		if (cached != nullptr && !Superseded(generation) && player.Start(cached, generation)) {
			playing = wanted->folder;
		}
	}
}

void Wish(std::optional<GameEntry> game) {
	std::call_once(g_thread, [] { std::thread(Run).detach(); });
	{
		std::lock_guard lock(g_mutex);
		g_wanted = std::move(game);
		g_generation++;
	}
	g_wake.notify_all();
}

} // namespace

void Play(const GameEntry& game) {
	Wish(game);
}

void Stop() {
	Wish(std::nullopt);
}

} // namespace Kyty::Uwp::TitleMusic
