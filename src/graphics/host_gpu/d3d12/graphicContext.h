#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GRAPHICCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GRAPHICCONTEXT_H_

#include "common/common.h"
#include "common/threads.h"

#include <cstdint>
#include <string>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGIFactory4;

namespace Libs::Graphics {

// The D3D12 device objects the renderer and the presenter share. Free of Windows headers so that shared code can include the backend.
struct GraphicContext {
	GraphicContext() = default;
	KYTY_CLASS_NO_COPY(GraphicContext);

	void Create(bool debug_layer);
	void Destroy();

	[[nodiscard]] const char* DeviceName() const { return device_name.c_str(); }

	IDXGIFactory4*      factory = nullptr;
	ID3D12Device*       device  = nullptr;
	ID3D12CommandQueue* queue   = nullptr;
	// Orders queue submissions and presents from different threads.
	Common::Mutex queue_mutex;
	std::string   device_name;
	bool          debug_layer = false;

	uint32_t screen_width  = 0;
	uint32_t screen_height = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GRAPHICCONTEXT_H_
