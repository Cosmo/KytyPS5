#include "graphics/host_gpu/d3d12/graphicContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"
#include "graphics/host_gpu/d3d12/gpuTrace.h"

#include <D3D12MemAlloc.h>
#include <algorithm>
#include <cinttypes>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics {

namespace D3D12 {

static ID3D12Device* g_device = nullptr;

void Check(HRESULT result, const char* operation) {
	if (SUCCEEDED(result)) {
		return;
	}
	const bool removed = result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
	                     result == DXGI_ERROR_DEVICE_HUNG;
	EXIT("D3D12: %s failed, HRESULT=0x%08" PRIx32 "\n%s", operation, static_cast<uint32_t>(result),
	     removed ? DeviceRemovedReport().c_str() : "");
}

static const char* BreadcrumbName(D3D12_AUTO_BREADCRUMB_OP op) {
	switch (op) {
		case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
		case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
		case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
		case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
		case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
		case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
		default: return nullptr;
	}
}

static std::string AllocationList(const D3D12_DRED_ALLOCATION_NODE* node) {
	std::string text;
	for (int count = 0; node != nullptr && count < 16; node = node->pNext, count++) {
		text += fmt::format("    {} (type {})\n",
		                    node->ObjectNameA != nullptr ? node->ObjectNameA : "unnamed",
		                    static_cast<int>(node->AllocationType));
	}
	return text.empty() ? "    none\n" : text;
}

std::string DeviceRemovedReport() {
	if (g_device == nullptr) {
		return {};
	}
	std::string report = fmt::format("Device removed reason: HRESULT=0x{:08x}\n",
	                                 static_cast<uint32_t>(g_device->GetDeviceRemovedReason()));
	ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&dred)))) {
		return report + "DRED: unavailable\n";
	}

	// The command lists the GPU started but didn't finish, with the operations around the last
	// one it completed.
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs {};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
		int lists = 0;
		for (const auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr && lists < 8;
		     node = node->pNext) {
			const uint32_t completed = node->pLastBreadcrumbValue != nullptr
			                               ? *node->pLastBreadcrumbValue
			                               : 0;
			if (node->pCommandHistory == nullptr || completed >= node->BreadcrumbCount) {
				continue;
			}
			lists++;
			report += fmt::format("Unfinished command list ({} of {} operations completed):\n",
			                      completed, node->BreadcrumbCount);
			const uint32_t first = completed > 4 ? completed - 4 : 0;
			const uint32_t last  = std::min(node->BreadcrumbCount, completed + 4);
			for (uint32_t index = first; index < last; index++) {
				const auto  op = node->pCommandHistory[index];
				const char* op_name = BreadcrumbName(op);
				report += fmt::format("  {}{:5} {}\n", index == completed ? "> " : "  ", index,
				                      op_name != nullptr ? op_name
				                                     : fmt::format("op {}", static_cast<int>(op)));
			}
		}
		if (lists == 0) {
			report += "DRED: no unfinished command lists\n";
		}
	}

	// The pipeline the GPU was stuck in, from the GPU trace; its shaders are saved.
	const auto unfinished = GetGpuTrace().Unfinished(4);
	for (size_t i = 0; i < unfinished.size(); i++) {
		const auto&       pipeline = *unfinished[i];
		const std::string name(pipeline.name.begin(), pipeline.name.end());
		if (i == 0) {
			std::vector<std::span<const uint8_t>>  dxil;
			std::vector<std::span<const uint32_t>> spirv;
			for (const auto* shader: pipeline.dxil) {
				dxil.emplace_back(shader->bytecode);
			}
			for (const auto& program: pipeline.spirv) {
				spirv.emplace_back(*program);
			}
			const auto file = "pipeline_" + name.substr(9, name.find(':') - 9);
			report += fmt::format("GPU stopped in {}: shaders saved to {}\\{}.*\n", name,
			                      SaveShaderDump(file, dxil, spirv), file);
		} else {
			report += fmt::format("  also started, not finished: {}\n", name);
		}
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT page_fault {};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
		report += fmt::format("Page fault at GPU address 0x{:016x}\n  live resources there:\n{}"
		                      "  recently freed resources there:\n{}",
		                      page_fault.PageFaultVA,
		                      AllocationList(page_fault.pHeadExistingAllocationNode),
		                      AllocationList(page_fault.pHeadRecentFreedAllocationNode));
	}
	return report;
}

} // namespace D3D12

// Forwards debug-layer warnings and errors to the emulator log; the layer otherwise only writes
// them to the debugger output.
static void CALLBACK DebugMessage(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity,
                                  D3D12_MESSAGE_ID id, LPCSTR description, void* /*context*/) {
	// Clears of resources created without an optimized clear value are only slower.
	if (severity > D3D12_MESSAGE_SEVERITY_WARNING ||
	    id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE ||
	    id == D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE) {
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

// What the device supports of what the backend uses: devices differ most here (the Xbox's UWP
// games get feature level 11.0, for example).
static void LogCapabilities(ID3D12Device* device) {
	static constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
	                                               D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
	                                               D3D_FEATURE_LEVEL_12_2};
	D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels {std::size(levels), levels};
	(void)device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &feature_levels,
	                                  sizeof(feature_levels));
	D3D12_FEATURE_DATA_SHADER_MODEL model {D3D_SHADER_MODEL_6_8};
	while (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) &&
	       model.HighestShaderModel > D3D_SHADER_MODEL_5_1) {
		model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(model.HighestShaderModel - 1);
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS  options {};
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 {};
	D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 {};
	D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 {};
	(void)device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
	(void)device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1));
	(void)device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3));
	(void)device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
	Log::WriteToConsoleAndLog(fmt::format(
	    "D3D12 features: level {:x}.{:x}, shader model {}.{}, binding tier {}, tiled resources tier "
	    "{}, typed UAV loads {}, wave ops {}, barycentrics {}, mesh shaders {}\n",
	    static_cast<unsigned>(feature_levels.MaxSupportedFeatureLevel) >> 12u,
	    (static_cast<unsigned>(feature_levels.MaxSupportedFeatureLevel) >> 8u) & 0xfu,
	    static_cast<unsigned>(model.HighestShaderModel) >> 4u,
	    static_cast<unsigned>(model.HighestShaderModel) & 0xfu,
	    static_cast<int>(options.ResourceBindingTier), static_cast<int>(options.TiledResourcesTier),
	    options.TypedUAVLoadAdditionalFormats ? "yes" : "no", options1.WaveOps ? "yes" : "no",
	    options3.BarycentricsSupported ? "yes" : "no",
	    options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED ? "yes" : "no"));
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

	Check(CreateDXGIFactory2(debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)),
	      "CreateDXGIFactory2");

	// DRED records the GPU's progress and page faults, so a device removal can be explained
	// (D3D12::DeviceRemovedReport). It has to be set up before the device is created.
	D3D12::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
		dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	} else {
		LOGF("D3D12: DRED unavailable; device removals won't be explained\n");
	}

	D3D12::ComPtr<IDXGIFactory6> factory6;
	Check(factory->QueryInterface(IID_PPV_ARGS(&factory6)), "query IDXGIFactory6");
	D3D12::ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0; factory6->EnumAdapterByGpuPreference(
	                         index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
	                         IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf())) != DXGI_ERROR_NOT_FOUND;
	     index++) {
		DXGI_ADAPTER_DESC1 desc {};
		adapter->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			continue;
		}
		// The lowest level D3D12 has: what the backend needs are features (shader model,
		// binding tier, ...), which the device reports whatever its level. UWP games on the Xbox
		// get feature level 11.0 (with Series X|S hardware behind it).
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
		                                IID_PPV_ARGS(&device)))) {
			device_name = Utf8(desc.Description);
			// Budget queries are optional; without IDXGIAdapter3 the caches use fixed limits.
			(void)adapter->QueryInterface(IID_PPV_ARGS(&this->adapter));
			break;
		}
	}
	if (device == nullptr) {
		EXIT("D3D12: no hardware adapter with D3D12\n");
	}
	D3D12::g_device = device;
	D3D12::GetGpuTrace().Create(device);

	D3D12MA::ALLOCATOR_DESC allocator_desc {};
	// Guest data overwrites new memory before use, as with VMA in the Vulkan build.
	allocator_desc.Flags    = D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;
	allocator_desc.pDevice  = device;
	allocator_desc.pAdapter = adapter.Get();
	Check(D3D12MA::CreateAllocator(&allocator_desc, &allocator), "create memory allocator");
	for (const auto& [type, pool]: {std::pair {D3D12_HEAP_TYPE_UPLOAD, &upload_pool},
	                                std::pair {D3D12_HEAP_TYPE_READBACK, &readback_pool}}) {
		D3D12MA::POOL_DESC pool_desc {};
		pool_desc.HeapProperties = device->GetCustomHeapProperties(0, type);
		pool_desc.HeapFlags      = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
		Check(allocator->CreatePool(&pool_desc, pool), "create memory pool");
	}

	D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 {};
	mesh_shaders = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
	                                                     sizeof(options7))) &&
	               options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;

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
	LogCapabilities(device);
}

void GraphicContext::Destroy() {
	for (auto** pool: {&upload_pool, &readback_pool}) {
		if (*pool != nullptr) {
			(*pool)->Release();
			*pool = nullptr;
		}
	}
	if (allocator != nullptr) {
		allocator->Release();
		allocator = nullptr;
	}
	if (queue != nullptr) {
		queue->Release();
		queue = nullptr;
	}
	if (device != nullptr) {
		D3D12::g_device = nullptr;
		D3D12::GetGpuTrace().Destroy();
		device->Release();
		device = nullptr;
	}
	if (adapter != nullptr) {
		adapter->Release();
		adapter = nullptr;
	}
	if (factory != nullptr) {
		factory->Release();
		factory = nullptr;
	}
}

static DXGI_QUERY_VIDEO_MEMORY_INFO LocalMemory(IDXGIAdapter3* adapter) {
	DXGI_QUERY_VIDEO_MEMORY_INFO info {};
	if (adapter != nullptr) {
		(void)adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
	}
	return info;
}

bool GraphicContext::SupportsDepthTargetFormat(vk::Format format, uint32_t samples) const {
	const auto info = D3D12::GetFormatInfo(format);
	if (!info.IsDepth()) {
		return false;
	}
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support {info.depth_view};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
	                                       sizeof(support))) ||
	    (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) == 0) {
		return false;
	}
	if (samples <= 1) {
		return true;
	}
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels {info.depth_view, samples};
	return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
	                                             sizeof(levels))) &&
	       levels.NumQualityLevels > 0;
}

void GraphicContext::LogMemoryBudget() const {
	const auto info = LocalMemory(adapter);
	LOGF("D3D12 local memory: usage=%" PRIu64 ", budget=%" PRIu64 "\n", info.CurrentUsage,
	     info.Budget);
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	return LocalMemory(adapter).CurrentUsage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	// Leave headroom for the rest of the system, like the Vulkan backend.
	const auto budget = LocalMemory(adapter).Budget;
	return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
}

} // namespace Libs::Graphics
