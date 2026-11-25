#include "common/abi.h"
#include "common/alignment.h"
#include "common/singleton.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "kernel/memory.h"
#include "libs/videoDec2Decoder.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace Libs {

LIB_NAME("Videodec2", "Videodec2");

namespace VideoDec2 {

constexpr int32_t VIDEODEC2_ERROR_API_FAIL             = -2128805632; // 0x811d0100
constexpr int32_t VIDEODEC2_ERROR_STRUCT_SIZE          = -2128805631; // 0x811d0101
constexpr int32_t VIDEODEC2_ERROR_ARGUMENT_POINTER     = -2128805630; // 0x811d0102
constexpr int32_t VIDEODEC2_ERROR_DECODER_INSTANCE     = -2128805629; // 0x811d0103
constexpr int32_t VIDEODEC2_ERROR_MEMORY_SIZE          = -2128805628; // 0x811d0104
constexpr int32_t VIDEODEC2_ERROR_MEMORY_POINTER       = -2128805627; // 0x811d0105
constexpr int32_t VIDEODEC2_ERROR_FRAME_BUFFER_SIZE    = -2128805626; // 0x811d0106
constexpr int32_t VIDEODEC2_ERROR_FRAME_BUFFER_POINTER = -2128805625; // 0x811d0107
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT_SIZE     = -2128805619; // 0x811d010d
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT_POINTER  = -2128805618; // 0x811d010e
constexpr int32_t VIDEODEC2_ERROR_OUTPUT_INFO          = -2128805617; // 0x811d010f
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_QUEUE        = -2128805616; // 0x811d0110
constexpr int32_t VIDEODEC2_ERROR_CONFIG_INFO          = -2128805376; // 0x811d0200
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_PIPE_ID      = -2128805375; // 0x811d0201
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_QUEUE_ID     = -2128805374; // 0x811d0202
constexpr int32_t VIDEODEC2_ERROR_RESOURCE_TYPE        = -2128805373; // 0x811d0203
constexpr int32_t VIDEODEC2_ERROR_CODEC_TYPE           = -2128805372; // 0x811d0204
constexpr int32_t VIDEODEC2_ERROR_INPUT_QUEUE_DEPTH    = -2128805370; // 0x811d0206
constexpr int32_t VIDEODEC2_ERROR_DPB_FRAME_COUNT      = -2128805367; // 0x811d0209
constexpr int32_t VIDEODEC2_ERROR_FRAME_WIDTH_HEIGHT   = -2128805366; // 0x811d020a
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT          = -2128805119; // 0x811d0301
constexpr int32_t VIDEODEC2_ERROR_OVERSIZE_DECODE      = -2128805118; // 0x811d0302

constexpr uint32_t VIDEODEC2_RESOURCE_TYPE_COMPUTE = 1;
constexpr uint32_t VIDEODEC2_CODEC_TYPE_AVC        = 1;
constexpr uint32_t VIDEODEC2_CODEC_TYPE_VP9        = 2382845;
constexpr uint32_t VIDEODEC2_CODEC_TYPE_HEVC       = 974921;
constexpr uint32_t VIDEODEC2_HEVC_PROFILE_MAIN10   = 2;
constexpr uint32_t VIDEODEC2_VP9_PROFILE_2         = 2;
constexpr size_t   VIDEODEC2_MIN_MEMORY_SIZE       = 16ull * 1024ull * 1024ull;
constexpr uint32_t VIDEODEC2_FRAME_FORMAT_DEFAULT  = 0;

using Videodec2Decoder      = void*;
using Videodec2ComputeQueue = void*;

struct Videodec2DecoderConfigInfo {
	size_t                this_size;
	uint32_t              resource_type;
	uint32_t              codec_type;
	uint32_t              profile;
	uint32_t              max_level;
	int32_t               max_frame_width;
	int32_t               max_frame_height;
	int32_t               max_dpb_frame_count;
	uint32_t              decode_input_queue_depth;
	Videodec2ComputeQueue compute_queue;
	uint64_t              cpu_affinity_mask;
	int32_t               cpu_thread_priority;
	bool                  optimize_progressive_video;
	bool                  check_memory_type;
	uint8_t               reserved0;
	uint8_t               reserved1;
	void*                 extra_config_info;
};

struct Videodec2HevcExtraConfigInfo {
	size_t   this_size;
	int32_t  tier_type;
	int8_t   max_bit_depth_luma;
	int8_t   max_bit_depth_chroma;
	uint16_t reserved0;
	int32_t  max_temporal_id_to_decode;
	uint32_t hdr_frame_format;
	uint32_t gpu_load_level;
};

struct Videodec2Vp9ExtraConfigInfo {
	size_t   this_size;
	int8_t   max_bit_depth;
	uint32_t hdr_frame_format;
};

struct Videodec2DecoderMemoryInfo {
	size_t   this_size;
	size_t   cpu_memory_size;
	void*    cpu_memory;
	size_t   gpu_memory_size;
	void*    gpu_memory;
	size_t   cpu_gpu_memory_size;
	void*    cpu_gpu_memory;
	size_t   max_frame_buffer_size;
	uint32_t frame_buffer_alignment;
	uint32_t reserved0;
};

struct Videodec2InputData {
	size_t   this_size;
	void*    au_data;
	size_t   au_size;
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
};

struct Videodec2OutputInfo {
	size_t   this_size;
	bool     is_valid;
	bool     is_error_frame;
	uint8_t  picture_count;
	bool     is_discarded_frame;
	uint32_t codec_type;
	uint32_t frame_width;
	uint32_t frame_pitch;
	uint32_t frame_height;
	void*    frame_buffer;
	size_t   frame_buffer_size;
	uint32_t frame_format;
	uint32_t frame_pitch_in_bytes;
};

struct Videodec2FrameBuffer {
	size_t this_size;
	void*  frame_buffer;
	size_t frame_buffer_size;
	bool   is_accepted;
};

using Videodec2AvcPictureInfo = Decoder::AvcPictureInfo;
using Videodec2Vp9PictureInfo = Decoder::Vp9PictureInfo;

// The HEVC picture information up to its bit depths.
struct Videodec2HevcPictureInfoHead {
	size_t   this_size;
	bool     is_valid;
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
	uint32_t pic_width_in_luma_samples;
	uint32_t pic_height_in_luma_samples;
	uint8_t  bit_depth_luma_minus8;
	uint8_t  bit_depth_chroma_minus8;
};

struct Videodec2ComputeMemoryInfo {
	size_t this_size;
	size_t cpu_gpu_memory_size;
	void*  cpu_gpu_memory;
};

struct Videodec2ComputeConfigInfo {
	size_t   this_size;
	uint16_t compute_pipe_id;
	uint16_t compute_queue_id;
	bool     check_memory_type;
	uint8_t  reserved0;
	uint16_t reserved1;
};

using DecoderState = Decoder::Instance;

static_assert(sizeof(Videodec2ComputeMemoryInfo) == 24);
static_assert(sizeof(Videodec2ComputeConfigInfo) == 16);
static_assert(sizeof(Videodec2DecoderConfigInfo) == 72);
static_assert(sizeof(Videodec2HevcExtraConfigInfo) == 32);
static_assert(sizeof(Videodec2Vp9ExtraConfigInfo) == 16);
static_assert(sizeof(Videodec2DecoderMemoryInfo) == 72);
static_assert(sizeof(Videodec2InputData) == 48);
static_assert(sizeof(Videodec2OutputInfo) == 56);
static_assert(sizeof(Videodec2FrameBuffer) == 32);
static_assert(sizeof(Videodec2AvcPictureInfo) == 120);
static_assert(offsetof(Videodec2HevcPictureInfoHead, bit_depth_luma_minus8) == 48);

static std::mutex                g_decoder_mutex;
static std::unordered_set<void*> g_decoders;

static bool IsOutputInfoSizeValid(size_t this_size) {
	return this_size == sizeof(Videodec2OutputInfo) ||
	       (this_size | 8u) == sizeof(Videodec2OutputInfo);
}

static DecoderState* GetDecoder(Videodec2Decoder decoder) {
	std::scoped_lock lock(g_decoder_mutex);
	return g_decoders.contains(decoder) ? static_cast<DecoderState*>(decoder) : nullptr;
}

static void FillNoPictureOutput(const Videodec2FrameBuffer* frame_buffer,
                                Videodec2OutputInfo* output_info, uint32_t codec_type) {
	output_info->is_valid           = false;
	output_info->is_error_frame     = false;
	output_info->picture_count      = 0;
	output_info->is_discarded_frame = false;
	output_info->codec_type         = codec_type;
	output_info->frame_width        = 0;
	output_info->frame_pitch        = 0;
	output_info->frame_height       = 0;
	output_info->frame_buffer      = frame_buffer != nullptr ? frame_buffer->frame_buffer : nullptr;
	output_info->frame_buffer_size = frame_buffer != nullptr ? frame_buffer->frame_buffer_size : 0;
	if (output_info->this_size == sizeof(Videodec2OutputInfo)) {
		output_info->frame_format         = VIDEODEC2_FRAME_FORMAT_DEFAULT;
		output_info->frame_pitch_in_bytes = 0;
	}
}

static int32_t MapDecoderResult(Decoder::Result result) {
	switch (result) {
		case Decoder::Result::Ok: return OK;
		case Decoder::Result::InputFull:
		case Decoder::Result::NoFrame:
		case Decoder::Result::ApiFail: return VIDEODEC2_ERROR_API_FAIL;
		case Decoder::Result::AccessUnit: return VIDEODEC2_ERROR_ACCESS_UNIT;
		case Decoder::Result::FrameBufferSize: return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
		case Decoder::Result::OversizeDecode: return VIDEODEC2_ERROR_OVERSIZE_DECODE;
	}
	return VIDEODEC2_ERROR_API_FAIL;
}

static void ApplyDecodedOutput(const Decoder::Output& decoded, Videodec2FrameBuffer* frame_buffer,
                               Videodec2OutputInfo* output_info) {
	frame_buffer->is_accepted = decoded.buffer_accepted;
	if (!decoded.valid) {
		return;
	}
	output_info->is_valid          = true;
	output_info->is_error_frame    = decoded.error_frame;
	output_info->picture_count     = 1;
	output_info->codec_type        = decoded.codec_type;
	output_info->frame_width       = decoded.width;
	output_info->frame_pitch       = decoded.pitch;
	output_info->frame_height      = decoded.height;
	output_info->frame_buffer      = decoded.buffer;
	output_info->frame_buffer_size = decoded.buffer_size;
	if (output_info->this_size == sizeof(Videodec2OutputInfo)) {
		output_info->frame_format         = VIDEODEC2_FRAME_FORMAT_DEFAULT;
		output_info->frame_pitch_in_bytes = decoded.pitch_in_bytes;
	}
}

static int32_t ValidateDecoderConfig(const Videodec2DecoderConfigInfo* config,
                                     bool                              require_compute_queue) {
	if (config->resource_type != VIDEODEC2_RESOURCE_TYPE_COMPUTE) {
		return VIDEODEC2_ERROR_RESOURCE_TYPE;
	}

	if (!Decoder::IsCodecSupported(config->codec_type)) {
		return VIDEODEC2_ERROR_CODEC_TYPE;
	}

	if (config->reserved0 != 0 || config->reserved1 != 0) {
		return VIDEODEC2_ERROR_CONFIG_INFO;
	}

	if (config->decode_input_queue_depth == 0) {
		return VIDEODEC2_ERROR_INPUT_QUEUE_DEPTH;
	}

	if (config->max_dpb_frame_count < -1 || config->max_dpb_frame_count == 0) {
		return VIDEODEC2_ERROR_DPB_FRAME_COUNT;
	}

	if (config->max_frame_width < -1 || config->max_frame_height < -1 ||
	    config->max_frame_width == 0 || config->max_frame_height == 0) {
		return VIDEODEC2_ERROR_FRAME_WIDTH_HEIGHT;
	}

	if (require_compute_queue && config->compute_queue == nullptr) {
		return VIDEODEC2_ERROR_COMPUTE_QUEUE;
	}

	return OK;
}

static int32_t KYTY_SYSV_ABI
QueryComputeMemoryInfo(Videodec2ComputeMemoryInfo* compute_memory_info) {
	PRINT_NAME();

	if (compute_memory_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (compute_memory_info->this_size != sizeof(Videodec2ComputeMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	compute_memory_info->cpu_gpu_memory_size = VIDEODEC2_MIN_MEMORY_SIZE;
	compute_memory_info->cpu_gpu_memory      = nullptr;

	return OK;
}

static int32_t KYTY_SYSV_ABI AllocateComputeQueue(
    const Videodec2ComputeConfigInfo* compute_config_info,
    const Videodec2ComputeMemoryInfo* compute_memory_info, Videodec2ComputeQueue* compute_queue) {
	PRINT_NAME();

	if (compute_config_info == nullptr || compute_memory_info == nullptr ||
	    compute_queue == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (compute_config_info->this_size != sizeof(Videodec2ComputeConfigInfo) ||
	    compute_memory_info->this_size != sizeof(Videodec2ComputeMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (compute_config_info->reserved0 != 0 || compute_config_info->reserved1 != 0) {
		return VIDEODEC2_ERROR_CONFIG_INFO;
	}

	if (compute_config_info->compute_pipe_id > 4) {
		return VIDEODEC2_ERROR_COMPUTE_PIPE_ID;
	}

	if (compute_config_info->compute_queue_id > 7) {
		return VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
	}

	if (compute_memory_info->cpu_gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE) {
		return VIDEODEC2_ERROR_MEMORY_SIZE;
	}

	if (compute_memory_info->cpu_gpu_memory == nullptr) {
		return VIDEODEC2_ERROR_MEMORY_POINTER;
	}

	*compute_queue = compute_memory_info->cpu_gpu_memory;
	return OK;
}

static int32_t KYTY_SYSV_ABI ReleaseComputeQueue(Videodec2ComputeQueue compute_queue) {
	PRINT_NAME();

	return compute_queue != nullptr ? OK : VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
}

// Without extra configuration, the 10-bit profiles decode at 10 bits.
static bool HasTenBitOutput(const Videodec2DecoderConfigInfo& config) {
	if (config.codec_type == VIDEODEC2_CODEC_TYPE_VP9) {
		const auto* vp9 = static_cast<const Videodec2Vp9ExtraConfigInfo*>(config.extra_config_info);
		return vp9 != nullptr ? vp9->max_bit_depth > 8 : config.profile == VIDEODEC2_VP9_PROFILE_2;
	}
	const auto* hevc = static_cast<const Videodec2HevcExtraConfigInfo*>(config.extra_config_info);
	return config.codec_type == VIDEODEC2_CODEC_TYPE_HEVC &&
	       config.profile == VIDEODEC2_HEVC_PROFILE_MAIN10 &&
	       (hevc == nullptr || hevc->max_bit_depth_luma != 8 || hevc->max_bit_depth_chroma != 8);
}

static size_t FrameBufferSize(const Videodec2DecoderConfigInfo& config) {
	if (config.max_frame_width <= 0 || config.max_frame_height <= 0) {
		return VIDEODEC2_MIN_MEMORY_SIZE;
	}
	// Semi-planar output with a 256-byte pitch, 2 bytes per 10-bit sample and the height aligned
	// to 16 lines (64 for VP9), then 4096 bytes of picture information (8192 for VP9).
	const auto width  = static_cast<size_t>(config.max_frame_width);
	const auto height = static_cast<size_t>(config.max_frame_height);
	const bool vp9    = config.codec_type == VIDEODEC2_CODEC_TYPE_VP9;
	const auto pitch  = Common::AlignUp(HasTenBitOutput(config) ? 2 * width : width, 256);
	return pitch * Common::AlignUp(height, vp9 ? 64 : 16) * 3 / 2 + (vp9 ? 8192 : 4096);
}

static int32_t KYTY_SYSV_ABI QueryDecoderMemoryInfo(const Videodec2DecoderConfigInfo* config,
                                                    Videodec2DecoderMemoryInfo*       memory_info) {
	PRINT_NAME();

	if (config == nullptr || memory_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (config->this_size != sizeof(Videodec2DecoderConfigInfo) ||
	    memory_info->this_size != sizeof(Videodec2DecoderMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	const auto validation_result = ValidateDecoderConfig(config, false);
	if (validation_result != OK) {
		return validation_result;
	}

	memory_info->cpu_memory_size        = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->cpu_memory             = nullptr;
	memory_info->gpu_memory_size        = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->gpu_memory             = nullptr;
	memory_info->cpu_gpu_memory_size    = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->cpu_gpu_memory         = nullptr;
	memory_info->max_frame_buffer_size  = FrameBufferSize(*config);
	memory_info->frame_buffer_alignment = 0x100;
	memory_info->reserved0              = 0;

	return OK;
}

static int32_t CreateDecoderChecked(const Videodec2DecoderConfigInfo* config,
                                    const Videodec2DecoderMemoryInfo* memory_info,
                                    Videodec2Decoder*                 decoder) {
	if (config == nullptr || memory_info == nullptr || decoder == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (config->this_size != sizeof(Videodec2DecoderConfigInfo) ||
	    memory_info->this_size != sizeof(Videodec2DecoderMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	const auto validation_result = ValidateDecoderConfig(config, true);
	if (validation_result != OK) {
		return validation_result;
	}

	if (memory_info->cpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->cpu_gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->max_frame_buffer_size < FrameBufferSize(*config)) {
		return VIDEODEC2_ERROR_MEMORY_SIZE;
	}

	if (memory_info->cpu_memory == nullptr || memory_info->gpu_memory == nullptr ||
	    memory_info->cpu_gpu_memory == nullptr) {
		return VIDEODEC2_ERROR_MEMORY_POINTER;
	}

	Decoder::Config decoder_config {config->codec_type, config->max_frame_width, config->max_frame_height};
	// PS4 (backward compatibility): the PS4's Videodec2 lays frames out as its Videodec does, rows
	// aligned to 64 bytes and the luma plane to 16 rows (shadPS4's videodec2_impl.cpp).
	if (Common::Singleton<Loader::RuntimeLinker>::Instance()->MainProgramIsPs4()) {
		decoder_config.pitch_alignment  = 64;
		decoder_config.height_alignment = 16;
	}
	auto* state = Decoder::Create(decoder_config);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_API_FAIL;
	}

	{
		std::scoped_lock lock(g_decoder_mutex);
		g_decoders.insert(state);
	}

	*decoder = state;
	return OK;
}

// A decoder's creation and deletion in the log, whatever the log is set to (games make few), with the result.
static int32_t KYTY_SYSV_ABI CreateDecoder(const Videodec2DecoderConfigInfo* config,
                                           const Videodec2DecoderMemoryInfo* memory_info,
                                           Videodec2Decoder*                 decoder) {
	PRINT_NAME();

	const auto result = CreateDecoderChecked(config, memory_info, decoder);
	std::printf("Videodec2: the game creates a decoder (codec %d, up to %dx%d): %s0x%08x\n",
	            config != nullptr ? static_cast<int>(config->codec_type) : -1, config != nullptr ? config->max_frame_width : 0,
	            config != nullptr ? config->max_frame_height : 0, result == OK ? "ok " : "error ", static_cast<uint32_t>(result));
	std::fflush(stdout);
	return result;
}

static int32_t DeleteDecoderChecked(Videodec2Decoder decoder) {
	DecoderState* state = nullptr;
	{
		std::scoped_lock lock(g_decoder_mutex);
		auto             it = g_decoders.find(decoder);
		if (it == g_decoders.end()) {
			return VIDEODEC2_ERROR_DECODER_INSTANCE;
		}
		state = static_cast<DecoderState*>(*it);
		g_decoders.erase(it);
	}

	Decoder::Destroy(state);

	return OK;
}

static int32_t KYTY_SYSV_ABI DeleteDecoder(Videodec2Decoder decoder) {
	PRINT_NAME();

	const auto result = DeleteDecoderChecked(decoder);
	std::printf("Videodec2: the game deletes a decoder: %s0x%08x\n", result == OK ? "ok " : "error ", static_cast<uint32_t>(result));
	std::fflush(stdout);
	return result;
}

static int32_t KYTY_SYSV_ABI Decode(Videodec2Decoder decoder, const Videodec2InputData* input_data,
                                    Videodec2FrameBuffer* frame_buffer,
                                    Videodec2OutputInfo*  output_info) {
	PRINT_NAME();

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}

	if (input_data == nullptr || frame_buffer == nullptr || output_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (input_data->this_size != sizeof(Videodec2InputData) ||
	    frame_buffer->this_size != sizeof(Videodec2FrameBuffer) ||
	    !IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (input_data->au_size == 0) {
		return VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
	}

	if (input_data->au_data == nullptr) {
		return VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
	}

	if (frame_buffer->frame_buffer_size == 0) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
	}

	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;
	}

	frame_buffer->is_accepted = false;
	FillNoPictureOutput(frame_buffer, output_info, Decoder::GetCodecType(state));

	Decoder::Output decoded {};
	const auto      result =
	    Decoder::Decode(state,
	                    {input_data->au_data, input_data->au_size, input_data->pts_data,
	                     input_data->dts_data, input_data->attached_data},
	                    {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	ApplyDecodedOutput(decoded, frame_buffer, output_info);
	return MapDecoderResult(result);
}

static int32_t KYTY_SYSV_ABI Flush(Videodec2Decoder decoder, Videodec2FrameBuffer* frame_buffer,
                                   Videodec2OutputInfo* output_info) {
	PRINT_NAME();

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}

	if (frame_buffer == nullptr || output_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (frame_buffer->this_size != sizeof(Videodec2FrameBuffer) ||
	    !IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (frame_buffer->frame_buffer_size == 0) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
	}

	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;
	}

	frame_buffer->is_accepted = false;
	FillNoPictureOutput(frame_buffer, output_info, Decoder::GetCodecType(state));

	Decoder::Output decoded {};
	const auto      result = Decoder::Flush(
	    state, {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	ApplyDecodedOutput(decoded, frame_buffer, output_info);
	return MapDecoderResult(result);
}

static int32_t KYTY_SYSV_ABI Reset(Videodec2Decoder decoder) {
	PRINT_NAME();

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}
	Decoder::Reset(state);
	return OK;
}

static int32_t KYTY_SYSV_ABI GetPictureInfo(const Videodec2OutputInfo* output_info,
                                            void* first_picture_info, void* second_picture_info) {
	PRINT_NAME();

	if (output_info == nullptr || first_picture_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (!IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (!output_info->is_valid || output_info->picture_count == 0 ||
	    output_info->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_OUTPUT_INFO;
	}

	Decoder::PictureInfo decoded {};
	if (!Decoder::GetPictureInfo(output_info->frame_buffer, &decoded) ||
	    decoded.codec_type != output_info->codec_type) {
		return VIDEODEC2_ERROR_OUTPUT_INFO;
	}

	auto fill_common = [&decoded](void* destination, bool valid) -> int32_t {
		auto*      bytes = static_cast<uint8_t*>(destination);
		const auto size  = *static_cast<const size_t*>(destination);
		if (size < 40 || size > 256) {
			return VIDEODEC2_ERROR_STRUCT_SIZE;
		}
		std::memset(bytes + sizeof(size_t), 0, size - sizeof(size_t));
		bytes[8] = valid ? 1 : 0;
		if (valid) {
			std::memcpy(bytes + 16, &decoded.pts, sizeof(decoded.pts));
			std::memcpy(bytes + 24, &decoded.dts, sizeof(decoded.dts));
			std::memcpy(bytes + 32, &decoded.attached_data, sizeof(decoded.attached_data));
		}
		return OK;
	};

	if (output_info->codec_type == VIDEODEC2_CODEC_TYPE_AVC) {
		const auto requested_size = *static_cast<const size_t*>(first_picture_info);
		if (requested_size != sizeof(Videodec2AvcPictureInfo) &&
		    (requested_size | 16u) != sizeof(Videodec2AvcPictureInfo)) {
			return VIDEODEC2_ERROR_STRUCT_SIZE;
		}

		const auto picture = Decoder::MakeAvcPictureInfo(decoded, requested_size);
		std::memcpy(first_picture_info, &picture, requested_size);
	} else if (output_info->codec_type == VIDEODEC2_CODEC_TYPE_VP9) {
		auto* picture = static_cast<Videodec2Vp9PictureInfo*>(first_picture_info);
		if (picture->this_size != sizeof(Videodec2Vp9PictureInfo)) {
			return VIDEODEC2_ERROR_STRUCT_SIZE;
		}
		*picture = Decoder::MakeVp9PictureInfo(decoded);
	} else {
		const auto result = fill_common(first_picture_info, true);
		if (result != OK) {
			return result;
		}
		auto* picture = static_cast<Videodec2HevcPictureInfoHead*>(first_picture_info);
		if (picture->this_size >= sizeof(Videodec2HevcPictureInfoHead)) {
			picture->pic_width_in_luma_samples  = decoded.width;
			picture->pic_height_in_luma_samples = decoded.height;
			picture->bit_depth_luma_minus8      = decoded.bit_depth - 8;
			picture->bit_depth_chroma_minus8    = decoded.bit_depth - 8;
		}
	}

	if (second_picture_info != nullptr) {
		const auto result = fill_common(second_picture_info, false);
		if (result != OK) {
			return result;
		}
	}
	return OK;
}

LIB_DEFINE(InitVideoDec2_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("RnDibcGCPKw", QueryComputeMemoryInfo);
	LIB_FUNC("eD+X2SmxUt4", AllocateComputeQueue);
	LIB_FUNC("UvtA3FAiF4Y", ReleaseComputeQueue);
	LIB_FUNC("qqMCwlULR+E", QueryDecoderMemoryInfo);
	LIB_FUNC("CNNRoRYd8XI", CreateDecoder);
	LIB_FUNC("jwImxXRGSKA", DeleteDecoder);
	LIB_FUNC("852F5+q6+iM", Decode);
	LIB_FUNC("l1hXwscLuCY", Flush);
	LIB_FUNC("wJXikG6QFN8", Reset);
	LIB_FUNC("NtXRa3dRzU0", GetPictureInfo);
	LIB_FUNC("kjrLbcyhEiw", GetPictureInfo);
}

} // namespace VideoDec2

// The PS4 video decoder (backward compatibility): AVC, one access unit in and at most one picture out
// per call, on the same decoder as Videodec2 with the PS4 frame layout. Layouts as in shadPS4.
namespace VideoDecPs4 {

LIB_NAME("Videodec", "Videodec");

namespace Decoder = VideoDec2::Decoder;

constexpr int32_t VIDEODEC_ERROR_API_FAIL             = static_cast<int32_t>(0x80C10000u);
constexpr int32_t VIDEODEC_ERROR_STRUCT_SIZE          = static_cast<int32_t>(0x80C10002u);
constexpr int32_t VIDEODEC_ERROR_HANDLE               = static_cast<int32_t>(0x80C10003u);
constexpr int32_t VIDEODEC_ERROR_AU_SIZE              = static_cast<int32_t>(0x80C10009u);
constexpr int32_t VIDEODEC_ERROR_AU_POINTER           = static_cast<int32_t>(0x80C1000Au);
constexpr int32_t VIDEODEC_ERROR_FRAME_BUFFER_SIZE    = static_cast<int32_t>(0x80C1000Bu);
constexpr int32_t VIDEODEC_ERROR_FRAME_BUFFER_POINTER = static_cast<int32_t>(0x80C1000Cu);
constexpr int32_t VIDEODEC_ERROR_ARGUMENT_POINTER     = static_cast<int32_t>(0x80C1000Fu);
constexpr int32_t VIDEODEC_ERROR_DECODE_AU            = static_cast<int32_t>(0x80C10011u);
constexpr int32_t VIDEODEC_ERROR_MISMATCH_SPEC        = static_cast<int32_t>(0x80C10012u);

struct VideodecConfigInfo {
	uint64_t this_size;
	uint32_t codec_type;
	uint32_t profile;
	uint32_t max_level;
	int32_t  max_frame_width;
	int32_t  max_frame_height;
	int32_t  max_dpb_frame_count;
	uint64_t videodec_flags;
};
static_assert(sizeof(VideodecConfigInfo) == 40);

struct VideodecResourceInfo {
	uint64_t this_size;
	uint64_t cpu_memory_size;
	void*    cpu_memory;
	uint64_t cpu_gpu_memory_size;
	void*    cpu_gpu_memory;
	uint64_t max_frame_buffer_size;
	uint32_t frame_buffer_alignment;
};
static_assert(sizeof(VideodecResourceInfo) == 56);

struct VideodecCtrl {
	uint64_t this_size;
	void*    handle;
	uint64_t version;
};

struct VideodecFrameBuffer {
	uint64_t this_size;
	void*    frame_buffer;
	uint64_t frame_buffer_size;
};

struct VideodecInputData {
	uint64_t this_size;
	void*    au_data;
	uint64_t au_size;
	uint64_t pts_data;
	uint64_t dts_data;
	uint64_t attached_data;
};

struct VideodecAvcInfo {
	uint32_t num_units_in_tick;
	uint32_t time_scale;
	uint8_t  fixed_frame_rate_flag;
	uint8_t  aspect_ratio_idc;
	uint16_t sar_width;
	uint16_t sar_height;
	uint8_t  colour_primaries;
	uint8_t  transfer_characteristics;
	uint8_t  matrix_coefficients;
	uint8_t  video_full_range_flag;
	uint32_t frame_crop_left_offset;
	uint32_t frame_crop_right_offset;
	uint32_t frame_crop_top_offset;
	uint32_t frame_crop_bottom_offset;
};

struct VideodecPictureInfo {
	uint64_t this_size;
	uint32_t is_valid;
	uint32_t codec_type;
	uint32_t frame_width;
	uint32_t frame_pitch;
	uint32_t frame_height;
	uint32_t is_error_pic;
	uint64_t pts_data;
	uint64_t attached_data;
	union {
		uint8_t         reserved[64];
		VideodecAvcInfo avc;
	} codec;
};
static_assert(sizeof(VideodecPictureInfo) == 112);

static std::mutex                            g_decoders_mutex;
static std::unordered_set<Decoder::Instance*> g_decoders;

static Decoder::Instance* GetDecoder(const VideodecCtrl* ctrl) {
	if (ctrl == nullptr) {
		return nullptr;
	}
	std::scoped_lock lock(g_decoders_mutex);
	auto*            instance = static_cast<Decoder::Instance*>(ctrl->handle);
	return g_decoders.contains(instance) ? instance : nullptr;
}

static int32_t MapResult(Decoder::Result result) {
	switch (result) {
		case Decoder::Result::Ok:
		case Decoder::Result::NoFrame: return OK;
		case Decoder::Result::AccessUnit: return VIDEODEC_ERROR_DECODE_AU;
		case Decoder::Result::FrameBufferSize: return VIDEODEC_ERROR_FRAME_BUFFER_SIZE;
		case Decoder::Result::OversizeDecode: return VIDEODEC_ERROR_MISMATCH_SPEC;
		default: return VIDEODEC_ERROR_API_FAIL;
	}
}

// The picture of a decode or flush, written up to the size the game gave.
static void FillPicture(const Decoder::Output& decoded, VideodecPictureInfo* picture) {
	VideodecPictureInfo out {};
	out.this_size = picture->this_size;
	if (decoded.valid) {
		// The host wrote the picture into guest memory: the GPU's copies of it are stale.
		const uint64_t rows = (decoded.height + 15u) & ~15u;
		LibKernel::Memory::InvalidateMemory(reinterpret_cast<uint64_t>(decoded.buffer),
		                                    uint64_t {decoded.pitch_in_bytes} * (rows + (rows + 1u) / 2u));
		Decoder::PictureInfo info {};
		(void)Decoder::GetPictureInfo(decoded.buffer, &info);
		const auto width  = (decoded.width + 15u) & ~15u;
		const auto height = (decoded.height + 15u) & ~15u;
		out.is_valid      = 1;
		out.frame_width   = width;
		out.frame_pitch   = decoded.pitch;
		out.frame_height  = height;
		out.is_error_pic  = decoded.error_frame ? 1u : 0u;
		out.pts_data      = info.pts;
		out.attached_data = info.attached_data;
		auto& avc         = out.codec.avc;
		avc.aspect_ratio_idc         = info.sar_width != 0 && info.sar_height != 0 ? 255 : 0;
		avc.sar_width                = info.sar_width;
		avc.sar_height               = info.sar_height;
		avc.colour_primaries         = info.color_primaries;
		avc.transfer_characteristics = info.color_trc;
		avc.matrix_coefficients      = info.color_space;
		avc.video_full_range_flag    = info.color_range == 2 ? 1 : 0;
		avc.frame_crop_left_offset   = info.crop_left;
		avc.frame_crop_right_offset  = width - decoded.width + info.crop_right;
		avc.frame_crop_top_offset    = info.crop_top;
		avc.frame_crop_bottom_offset = height - decoded.height + info.crop_bottom;
	}
	std::memcpy(picture, &out, std::min<uint64_t>(picture->this_size, sizeof(out)));
}

static int32_t KYTY_SYSV_ABI QueryResourceInfo(const VideodecConfigInfo* config,
                                               VideodecResourceInfo*     resource) {
	PRINT_NAME();
	if (config == nullptr || resource == nullptr) {
		return VIDEODEC_ERROR_ARGUMENT_POINTER;
	}
	if (config->this_size != sizeof(VideodecConfigInfo) ||
	    resource->this_size != sizeof(VideodecResourceInfo)) {
		return VIDEODEC_ERROR_STRUCT_SIZE;
	}
	// The worst case of the configuration: NV12 frames padded to 256 x 16, the DPB and two more.
	const uint64_t width  = config->max_frame_width > 0 ? config->max_frame_width : 1920;
	const uint64_t height = config->max_frame_height > 0 ? config->max_frame_height : 1080;
	const uint64_t frame  = ((width + 255u) & ~uint64_t {255u}) * ((height + 15u) & ~uint64_t {15u}) * 3u / 2u;
	const uint64_t padded = ((frame + 255u) & ~uint64_t {255u}) + 0x4000u;
	const uint64_t dpb    = config->max_dpb_frame_count > 0 ? config->max_dpb_frame_count : 8;
	resource->cpu_memory_size        = 16ull << 20u;
	resource->cpu_memory             = nullptr;
	resource->cpu_gpu_memory_size    = padded * (dpb + 2u) + (8ull << 20u);
	resource->cpu_gpu_memory         = nullptr;
	resource->max_frame_buffer_size  = padded;
	resource->frame_buffer_alignment = 0x100;
	return OK;
}

static int32_t KYTY_SYSV_ABI CreateDecoder(const VideodecConfigInfo*   config,
                                           const VideodecResourceInfo* resource, VideodecCtrl* ctrl) {
	PRINT_NAME();
	if (config == nullptr || resource == nullptr || ctrl == nullptr) {
		return VIDEODEC_ERROR_ARGUMENT_POINTER;
	}
	if (config->this_size != sizeof(VideodecConfigInfo) ||
	    resource->this_size != sizeof(VideodecResourceInfo)) {
		return VIDEODEC_ERROR_STRUCT_SIZE;
	}
	Decoder::Config decoder_config {};
	decoder_config.codec_type       = VideoDec2::VIDEODEC2_CODEC_TYPE_AVC;
	decoder_config.max_width        = config->max_frame_width;
	decoder_config.max_height       = config->max_frame_height;
	decoder_config.pitch_alignment  = 64;
	decoder_config.height_alignment = 16;
	auto* instance                  = Decoder::Create(decoder_config);
	if (instance == nullptr) {
		return VIDEODEC_ERROR_API_FAIL;
	}
	{
		std::scoped_lock lock(g_decoders_mutex);
		g_decoders.insert(instance);
	}
	ctrl->this_size = sizeof(VideodecCtrl);
	ctrl->handle    = instance;
	ctrl->version   = 1;
	return OK;
}

static int32_t KYTY_SYSV_ABI DeleteDecoder(VideodecCtrl* ctrl) {
	PRINT_NAME();
	auto* instance = GetDecoder(ctrl);
	if (instance == nullptr) {
		return VIDEODEC_ERROR_HANDLE;
	}
	{
		std::scoped_lock lock(g_decoders_mutex);
		g_decoders.erase(instance);
	}
	Decoder::Destroy(instance);
	return OK;
}

static int32_t KYTY_SYSV_ABI Decode(VideodecCtrl* ctrl, const VideodecInputData* input,
                                    VideodecFrameBuffer* frame_buffer, VideodecPictureInfo* picture) {
	PRINT_NAME();
	auto* instance = GetDecoder(ctrl);
	if (instance == nullptr) {
		return VIDEODEC_ERROR_HANDLE;
	}
	if (input == nullptr || frame_buffer == nullptr || picture == nullptr) {
		return VIDEODEC_ERROR_ARGUMENT_POINTER;
	}
	if (input->this_size != sizeof(VideodecInputData) ||
	    frame_buffer->this_size != sizeof(VideodecFrameBuffer)) {
		return VIDEODEC_ERROR_STRUCT_SIZE;
	}
	if (input->au_data == nullptr) {
		return VIDEODEC_ERROR_AU_POINTER;
	}
	if (input->au_size == 0) {
		return VIDEODEC_ERROR_AU_SIZE;
	}
	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC_ERROR_FRAME_BUFFER_POINTER;
	}
	Decoder::Output decoded {};
	const auto      result = Decoder::Decode(
        instance, {input->au_data, input->au_size, input->pts_data, input->dts_data, input->attached_data},
        {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	FillPicture(decoded, picture);
	return MapResult(result);
}

static int32_t KYTY_SYSV_ABI Flush(VideodecCtrl* ctrl, VideodecFrameBuffer* frame_buffer,
                                   VideodecPictureInfo* picture) {
	PRINT_NAME();
	auto* instance = GetDecoder(ctrl);
	if (instance == nullptr) {
		return VIDEODEC_ERROR_HANDLE;
	}
	if (frame_buffer == nullptr || picture == nullptr) {
		return VIDEODEC_ERROR_ARGUMENT_POINTER;
	}
	if (frame_buffer->this_size != sizeof(VideodecFrameBuffer)) {
		return VIDEODEC_ERROR_STRUCT_SIZE;
	}
	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC_ERROR_FRAME_BUFFER_POINTER;
	}
	Decoder::Output decoded {};
	const auto      result = Decoder::Flush(
        instance, {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	FillPicture(decoded, picture);
	return MapResult(result);
}

static int32_t KYTY_SYSV_ABI Reset(VideodecCtrl* ctrl) {
	PRINT_NAME();
	auto* instance = GetDecoder(ctrl);
	if (instance == nullptr) {
		return VIDEODEC_ERROR_HANDLE;
	}
	Decoder::Reset(instance);
	return OK;
}

// Memory for the decoder's own use: the host decoder needs none.
static int32_t KYTY_SYSV_ABI MapMemory() {
	PRINT_NAME();
	return OK;
}

LIB_DEFINE(InitVideoDec_1) {
	LIB_FUNC("leCAscipfFY", QueryResourceInfo);
	LIB_FUNC("qkgRiwHyheU", CreateDecoder);
	LIB_FUNC("U0kpGF1cl90", DeleteDecoder);
	LIB_FUNC("q0W5GJMovMs", Decode);
	LIB_FUNC("jeigLlKdp5I", Flush);
	LIB_FUNC("f8AgDv-1X8A", Reset);
	LIB_FUNC("kg+lH0V61hM", MapMemory);
}

} // namespace VideoDecPs4

} // namespace Libs
