#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Config {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Config";
	static constexpr auto        initialize = Config::Initialize;
	static constexpr auto        shutdown   = Config::Shutdown;
};

enum class ShaderOptimizationType { None, Size, Performance };

enum class LogDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

using Keymap = std::vector<std::string>;
using ControllerColor = std::array<uint8_t, 3>;

constexpr uint32_t DEFAULT_CONSOLE_LANGUAGE = 1;
constexpr uint32_t MAX_CONSOLE_LANGUAGE     = 29;
constexpr std::size_t MAX_USER_NAME_LENGTH = 16;
constexpr int32_t DEFAULT_USER_ID           = 1000;

constexpr int     MAX_PLAYERS         = 4;
constexpr int32_t PLAYER_USER_ID_STEP = 0x100;
constexpr int32_t MAX_USER_ID =
    std::numeric_limits<int32_t>::max() - (MAX_PLAYERS - 1) * PLAYER_USER_ID_STEP;

constexpr bool IsConfiguredUserIdValid(int32_t user_id) {
	constexpr int32_t USER_ID_EVERYONE = 0xfe;
	constexpr int32_t USER_ID_SYSTEM   = 0xff;
	return user_id >= 0 && user_id <= MAX_USER_ID && user_id != USER_ID_EVERYONE &&
	       user_id != USER_ID_SYSTEM;
}

constexpr uint32_t kGuestRamBudgetAuto = 0xFFFFFFFFu;

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	std::string            user_name                   = "Kyty";
	int32_t                user_id                     = DEFAULT_USER_ID;
	std::string            audio_input_device;
	std::optional<ControllerColor> controller_color;
	uint32_t               controller_speaker_volume      = 50;
	uint32_t               controller_vibration_intensity = 100;
	// 0 shares player 1 with the first gamepad; 1..MAX_PLAYERS dedicates that player to the keyboard.
	uint32_t               keyboard_player                = 0;
	PresentMode            present_mode                = PresentMode::Mailbox;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	bool                   hdr_enabled                 = false;
	bool                   hide_cursor_enabled         = false;
	bool                   splash_enabled              = true;
	bool                   vr_enabled                  = false;
	bool                   amd_cpu_enabled             = false;
	bool                   network_connected           = false;
	uint32_t               vblank_frequency            = 60;
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	bool                   shader_cache_enabled        = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	LogDirection           shader_log_direction        = LogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	bool                   graphics_debug_dump_enabled = false;
	LogDirection           printf_direction            = LogDirection::Silent;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	bool                   profiler_enabled            = false;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = false;
	bool                   readback_tiled_images       = false;
	bool                   sync_raw_image_buffers      = false;
	bool                   tessellation_enabled        = false;
	bool                   occlusion_queries_enabled   = false;
	bool                   trophy_enabled              = true;
	uint32_t               trophy_duration_seconds     = 6;
	bool                   perf_overlay_enabled            = false;
	bool                   playgo_hack_enabled         = false;
	bool                   skip_notice_screen          = false;
	// Where the host keeps the fonts that stand in for the console's system fonts (Roboto); empty: the font library looks in the source tree.
	std::filesystem::path font_folder;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
	// Debugging (D3D12 build): the shader limits of the Xbox's UWP games on any device (shader
	// model 6.4, 16-bit ops as 32-bit, no barycentrics, no mesh shaders), to reproduce Xbox
	// problems on a PC.
	bool xbox_gpu_limits_enabled = false;
	// Debugging: which limits of the DXIL compiler apply with xbox_gpu_limits_enabled: bit 0 shader model 6.4, bit 1 16-bit operations lowered.
	uint32_t xbox_limits_dxil_parts = 3;
	// Debugging: save the shaders of the pipeline with this number when it is created (0: none).
	uint32_t dump_pipeline = 0;
	// Debugging (D3D12 build): a folder whose DXIL files replace shaders of pipelines when they are created: <size>.ps.dxil replaces the pixel stage
	// (and <size>.vs.dxil the vertex stage) of the pipelines whose pixel program's SPIR-V is <size> bytes. Signed or not; the emulator validates and signs
	// them. Empty: off. For trying hand-edited or differently built DXIL of one shader without rebuilding the emulator.
	std::filesystem::path shader_override_folder;
	// Debugging: where the game menu's frame dump puts its files (a folder for each dump is made in it).
	std::filesystem::path frame_dump_folder;
	// Debugging: the frame dump's draw log also gets the average color of every large floating-point render target after each draw that writes it (a readback each, slow).
	bool frame_dump_snapshots = false;
	// Debugging (D3D12 build): a thread watches the GPU and ends the emulator, with the report of a GPU hang, as soon as the device is removed or the
	// oldest GPU work has not finished for this many seconds (0: off). It also logs how many draws of the pipelines with overridden shaders finished.
	uint32_t gpu_hang_exit_seconds = 0;
	// Debugging (D3D12 build): run merged geometry programs as compute shaders (the emulation of
	// mesh shaders) even where the device has mesh shaders, to compare the two.
	bool mesh_compute_forced = false;
	// The pager (xbox/lazyRegions.h): when the lazily committed guest memory is above this many MB, the coldest of it is kept in a file instead of in memory.
	// 0: off. kGuestRamBudgetAuto (not set): on where the app's memory limit is small (the Series X: 5 GiB), with that limit as the cap; off elsewhere.
	uint32_t guest_ram_budget_mb = 0;
	// Debugging the pager: 0 normal; 1 sweep cold windows and give them back when they are touched, but never take one out of memory; 2 take a window out and page it straight back in.
	uint32_t guest_ram_pager_mode = 0;
	// Debugging the pager: a commit of guest memory fails above this many MB of guest memory (a wall like the console's, which the PC does not have). 0: none.
	uint32_t guest_commit_limit_mb = 0;
	// Low quality: a sampled, block-compressed 2D texture whose larger side has at least texture_mip_skip_min_size texels (and enough mip levels) is made on the host
	// without its texture_mip_skip finest mip levels: a quarter of the memory for one level. 0: off.
	uint32_t texture_mip_skip          = 0;
	uint32_t texture_mip_skip_min_size = 2048;
	// GPU memory savings (a scratch buffer made when needed, committed small pool buffers, 16 MB allocator blocks) and the video decoder's threads: on in the UWP app, unless a
	// setting turns them off for testing; off in the desktop build, which keeps upstream's behaviour.
#if defined(KYTY_PLATFORM_UWP)
	bool gpu_memory_savings   = true;
	bool video_decode_threads = true;
#else
	bool gpu_memory_savings   = false;
	bool video_decode_threads = false;
#endif
	// Debugging: where the GPU memory is (images and buffers by kind and size) in the log every 10 seconds. It walks every image and buffer under the
	// texture cache's lock, so it is off unless a setting turns it on.
	bool gpu_memory_log = false;
	// Development: the pipeline cache of a game (its translated shaders) is kept across builds, also dirty ones; only the shader compiler's own
	// signature still ties it. A build that translates shaders differently then uses the old translations, which can draw wrongly or crash, so it
	// is off unless a setting turns it on.
	bool pipeline_cache_any_build = false;
	// The app's memory use as the OS counts it (the Series X ends the app at its limit), and the limit: set by the UWP host. Null or 0 elsewhere.
	uint64_t (*app_memory_usage)() = nullptr;
	uint64_t app_memory_limit_bytes = 0;
#endif
	Keymap keymap;
	// Ordinary boots need a non-null argv[0]; --guest-exec supplies the caller's arguments.
	std::vector<std::string> guest_arguments {""};
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
const std::string& GetAudioInputDevice();
const std::optional<ControllerColor>& GetControllerColor();
uint32_t GetControllerSpeakerVolume();
uint32_t GetControllerVibrationIntensity();
uint32_t GetKeyboardPlayer();
// Player 0 is the configured user; later players derive their IDs from it.
int32_t  GetPlayerUserId(int player);
// Returns -1 when the user ID does not belong to any player.
int      GetPlayerFromUserId(int32_t user_id);
// Players after the first are guests: they have no profile, trophies or NP account.
bool     IsGuestUserId(int32_t user_id);
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
bool     HdrEnabled();
bool     HideCursorEnabled();
bool     VrEnabled();
bool     AmdCpuEnabled();
bool     IsNetworkConnected();
uint32_t GetVblankFrequency();
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
bool                   ShaderCacheEnabled();
ShaderOptimizationType GetShaderOptimizationType();
LogDirection           GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool GraphicsDebugDumpEnabled();

LogDirection          GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

bool ProfilerEnabled();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool ReadbackTiledImagesEnabled();
bool SyncRawImageBuffersEnabled();
bool TessellationEnabled();
bool OcclusionQueriesEnabled();
bool TrophyEnabled();
uint32_t GetTrophyDurationSeconds();
bool PlayGoHackEnabled();
bool SkipNoticeScreen();
std::filesystem::path FontFolder();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
bool XboxGpuLimitsEnabled();
uint32_t XboxLimitsDxilParts();
uint32_t DumpPipeline();
std::filesystem::path ShaderOverrideFolder();
std::filesystem::path FrameDumpFolder();
bool                  FrameDumpSnapshots();
uint32_t              GpuHangExitSeconds();
bool MeshComputeForced();
uint32_t GuestRamBudgetMb();
uint32_t GuestRamPagerMode();
uint32_t GuestCommitLimitMb();
uint32_t TextureMipSkip();
uint32_t TextureMipSkipMinSize();
bool GpuMemorySavings();
bool GpuMemoryLog();
bool PipelineCacheAnyBuild();
bool VideoDecodeThreads();
uint64_t AppMemoryUsage();
uint64_t AppMemoryLimit();
#endif

const Keymap& GetKeymap();
const std::vector<std::string>& GetGuestArguments();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
