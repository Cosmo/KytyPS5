#include "libs/videoHardware.h"

#include <atomic>
#include <cstdio>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
}

namespace Libs::VideoHardware {

namespace {
std::atomic<AVBufferRef*> g_device {nullptr};

void LogError(const char* what, int error) {
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(error, text, sizeof(text));
	std::printf("AvPlayer: %s: %s\n", what, text);
}
} // namespace

void UseDevice(ID3D12Device* device) {
	if (device == nullptr || g_device.load(std::memory_order_acquire) != nullptr) {
		return;
	}
	AVBufferRef* context = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
	if (context == nullptr) {
		return;
	}
	auto* hw   = static_cast<AVD3D12VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(context->data)->hwctx);
	hw->device = device; // the renderer's: FFmpeg releases only devices it made itself
	device->AddRef();    // kept for the app's lifetime
	if (const int error = av_hwdevice_ctx_init(context); error < 0) {
		LogError("no hardware video decoding", error);
		av_buffer_unref(&context);
		device->Release();
		return;
	}
	g_device.store(context, std::memory_order_release);
	std::printf("AvPlayer: hardware video decoding with D3D12 on the renderer's device\n");
}

AVBufferRef* Device() {
	return g_device.load(std::memory_order_acquire);
}

bool MakeFrames(AVCodecContext* c) {
	if (c->hw_device_ctx == nullptr) {
		return false;
	}
	AVBufferRef* frames = nullptr;
	if (const int error = avcodec_get_hw_frames_parameters(c, c->hw_device_ctx, AV_PIX_FMT_D3D12, &frames); error < 0) {
		LogError("no D3D12 frame pool", error);
		return false;
	}
	auto* pool   = reinterpret_cast<AVHWFramesContext*>(frames->data);
	pool->width  = (c->coded_width + 15) & ~15; // 16-pixel blocks, as FFmpeg's D3D11 path rounds its surfaces
	pool->height = (c->coded_height + 15) & ~15;
	if (const int error = av_hwframe_ctx_init(frames); error < 0) {
		LogError("no D3D12 frame pool", error);
		av_buffer_unref(&frames);
		return false;
	}
	av_buffer_unref(&c->hw_frames_ctx);
	c->hw_frames_ctx = frames;
	return true;
}

} // namespace Libs::VideoHardware

#else

namespace Libs::VideoHardware {

void UseDevice(ID3D12Device* /*device*/) {}

AVBufferRef* Device() {
	return nullptr;
}

bool MakeFrames(AVCodecContext* /*c*/) {
	return false;
}

} // namespace Libs::VideoHardware

#endif
