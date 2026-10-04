#include "graphics/host_gpu/d3d12/graphicContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"

#include <cinttypes>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace D3D12 {

void Check(HRESULT result, const char* operation) {
	if (SUCCEEDED(result)) {
		return;
	}
	EXIT("D3D12: %s failed, HRESULT=0x%08" PRIx32 "\n", operation, static_cast<uint32_t>(result));
}

} // namespace D3D12

// Forwards debug-layer warnings and errors to the emulator log; the layer otherwise only writes
// them to the debugger output.
static void CALLBACK DebugMessage(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity,
                                  D3D12_MESSAGE_ID id, LPCSTR description, void* /*context*/) {
	if (severity > D3D12_MESSAGE_SEVERITY_WARNING) {
		return;
	}
	const char* kind = severity == D3D12_MESSAGE_SEVERITY_WARNING ? "warning" : "error";
	Log::WriteToConsoleAndLog(
	    fmt::format("D3D12 debug layer {} {}: {}\n", kind, static_cast<int>(id), description));
}

static std::string Utf8(const wchar_t* text) {
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1) {
		return {};
	}
	std::string result(static_cast<size_t>(size - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
	return result;
}

void GraphicContext::Create(bool enable_debug_layer) {
	using D3D12::Check;
	EXIT_IF(device != nullptr);

	debug_layer = enable_debug_layer;
	if (debug_layer) {
		D3D12::ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
			debug->EnableDebugLayer();
		} else {
			LOGF("D3D12 debug layer requested but unavailable\n");
			debug_layer = false;
		}
	}

	Check(CreateDXGIFactory2(debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

	// Feature level 11_0 is what the Xbox's UWP apps get; the renderer needs nothing above it.
	D3D12::ComPtr<IDXGIAdapter1> best;
	SIZE_T                       best_memory = 0;
	DXGI_ADAPTER_DESC1           best_desc {};
	D3D12::ComPtr<IDXGIAdapter1> candidate;
	for (UINT index = 0; factory->EnumAdapters1(index, candidate.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; index++) {
		DXGI_ADAPTER_DESC1 desc {};
		candidate->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			continue;
		}
		if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
			continue;
		}
		if (!best || desc.DedicatedVideoMemory > best_memory) {
			best        = candidate;
			best_memory = desc.DedicatedVideoMemory;
			best_desc   = desc;
		}
	}
	if (!best) {
		EXIT("D3D12: no hardware adapter supports feature level 11_0\n");
	}
	Check(D3D12CreateDevice(best.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
	device_name = Utf8(best_desc.Description);

	D3D12_COMMAND_QUEUE_DESC queue_desc {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

	if (debug_layer) {
		D3D12::ComPtr<ID3D12InfoQueue1> info_queue;
		DWORD                           cookie = 0;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info_queue))) ||
		    FAILED(info_queue->RegisterMessageCallback(DebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
		                                               nullptr, &cookie))) {
			LOGF("D3D12 debug layer messages are only visible in a debugger\n");
		}
	}

	Log::WriteToConsoleAndLog(fmt::format("D3D12 device: {} (debug layer {})\n", device_name,
	                                      debug_layer ? "on" : "off"));
}

void GraphicContext::Destroy() {
	if (queue != nullptr) {
		queue->Release();
		queue = nullptr;
	}
	if (device != nullptr) {
		device->Release();
		device = nullptr;
	}
	if (factory != nullptr) {
		factory->Release();
		factory = nullptr;
	}
}

} // namespace Libs::Graphics
