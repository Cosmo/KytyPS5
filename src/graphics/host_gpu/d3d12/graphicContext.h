#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GRAPHICCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GRAPHICCONTEXT_H_

#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGIAdapter3;
struct IDXGIFactory4;

namespace D3D12MA {
class Allocator;
class Pool;
} // namespace D3D12MA

namespace Libs::Graphics {

// D3D12 device objects shared by the renderer and the presenter. Kept free of Windows headers
// so shared code can include the backend without pulling them in.
struct GraphicContext {
	GraphicContext() = default;
	KYTY_CLASS_NO_COPY(GraphicContext);

	void Create(bool debug_layer);
	void Destroy();

	[[nodiscard]] const char* DeviceName() const { return device_name.c_str(); }

	void                   LogMemoryBudget() const;
	[[nodiscard]] bool     CanReportMemoryUsage() const noexcept { return adapter != nullptr; }
	// GPU memory used by this process.
	[[nodiscard]] uint64_t GetDeviceMemoryUsage() const;
	// GPU memory the caches may fill before they start evicting.
	[[nodiscard]] uint64_t GetTotalMemoryBudget() const;
	[[nodiscard]] uint64_t UniformBufferAlignment() const noexcept { return 256; }
	// Raw buffer views start at 16-byte offsets and hold at most 2^27 dwords.
	[[nodiscard]] uint64_t StorageMinAlignment() const noexcept { return 16; }
	[[nodiscard]] uint64_t MaxStorageBufferRange() const noexcept { return uint64_t {4} << 27u; }
	[[nodiscard]] bool     SupportsDepthTargetFormat(vk::Format format, uint32_t samples) const;
	// spirv_to_dxil runs pixel shaders per sample on request.
	[[nodiscard]] bool     SupportsSampleRateShading() const noexcept { return true; }
	// Rasterizing without attachments forces one of these sample counts.
	[[nodiscard]] bool     SupportsSamplesWithoutAttachments(uint32_t samples) const noexcept {
		return samples == 1 || samples == 4 || samples == 8 || samples == 16;
	}

	IDXGIFactory4*      factory = nullptr;
	IDXGIAdapter3*      adapter = nullptr;
	ID3D12Device*       device  = nullptr;
	ID3D12CommandQueue* queue   = nullptr;
	// Suballocates buffers and images from pooled heaps (D3D12 Memory Allocator). The pools hold
	// the CPU-visible buffers: custom heaps that, unlike the typed upload and readback heaps,
	// allow unordered access and free state transitions.
	D3D12MA::Allocator* allocator     = nullptr;
	D3D12MA::Pool*      upload_pool   = nullptr;
	D3D12MA::Pool*      readback_pool = nullptr;
	Common::Mutex       queue_mutex;
	std::string         device_name;
	bool                debug_layer = false;
	// The device runs mesh shaders (tier 1); they also need shader model 6.5 (DxilCompiler).
	bool                mesh_shaders = false;

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GRAPHICCONTEXT_H_
