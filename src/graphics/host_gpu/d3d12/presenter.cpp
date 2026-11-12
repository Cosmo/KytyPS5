#include "graphics/presentation/presenter.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_blit_fsr1_easu_spv.h"
#include "gpu_blit_shaders/gpu_blit_fsr1_rcas_spv.h"
#include "gpu_blit_shaders/gpu_blit_present_spv.h"
#include "graphics/host_gpu/frameResolution.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/d3d12/windowContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace Libs::Graphics {

// A prepared guest frame: a copy of the guest's flip image on the GPU (the frame's own texture), or a clear color.
struct Presenter::Frame {
	D3D12::ComPtr<ID3D12Resource> image;
	D3D12::FormatInfo             format;
	uint32_t                      width        = 0;
	uint32_t                      height       = 0;
	D3D12_RESOURCE_STATES         state        = D3D12_RESOURCE_STATE_COMMON;
	bool                          blank        = false;
	// An HDR10 frame: PQ pixels (or linear nits in a 16-bit float frame) that the blit converts to SDR.
	bool                          hdr          = false;
	bool                          linear       = false;
	std::array<float, 4>          color        = {0.0f, 0.0f, 0.0f, 1.0f};
	uint64_t                      present_tick = 0;
	bool                          busy         = false;
#ifdef KYTY_HAS_FSR
	D3D12::ComPtr<ID3D12Resource> generated;
	uint64_t guest_sequence = 0;
	bool generated_valid = false;
#endif

	void Configure(GraphicContext& graphics, uint32_t frame_width, uint32_t frame_height, vk::Format frame_format);
	void Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES after);
	void CopyFrom(CommandBuffer& command, Image& source, const ImageViewInfo& source_view);
};

void Presenter::Frame::Configure(GraphicContext& graphics, uint32_t frame_width, uint32_t frame_height, vk::Format frame_format) {
	const auto info = D3D12::GetFormatInfo(frame_format);
	EXIT_IF(!info.Supported() || info.IsDepth());
	if (image != nullptr && width == frame_width && height == frame_height && format.family == info.family) {
		format = info;
		return;
	}
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width            = frame_width;
	desc.Height           = frame_height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels        = 1;
	desc.Format           = info.family;
	desc.SampleDesc.Count = 1;
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	image.Reset();
	D3D12::Check(graphics.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&image)),
	             "create a presentation frame");
	format = info;
	width  = frame_width;
	height = frame_height;
	state  = D3D12_RESOURCE_STATE_COMMON;
}

void Presenter::Frame::Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES after) {
	if (state == after) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource   = image.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = state;
	barrier.Transition.StateAfter  = after;
	list->ResourceBarrier(1, &barrier);
	state = after;
}

void Presenter::Frame::CopyFrom(CommandBuffer& command, Image& source, const ImageViewInfo& source_view) {
	auto*      list = command.Handle();
	const auto view = source.FindView(source_view);
	source.Use(command, D3D12_RESOURCE_STATE_COPY_SOURCE, ImageSubresourceRange {0, 1, 0, 1}, view);
	Transition(list, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION destination {};
	destination.pResource = image.Get();
	destination.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION from {};
	from.pResource = source.Resource(view);
	from.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const D3D12_BOX box {0, 0, 0, std::min(source.backing.extent.width, width), std::min(source.backing.extent.height, height), 1};
	list->CopyTextureRegion(&destination, 0, 0, 0, &from, &box);
	// The presentation command list samples the frame.
	Transition(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

namespace {

// Frame ownership between VideoOut and the presentation; the rules are those of the Vulkan presenter.
class FramePool final {
public:
	explicit FramePool(CommandScheduler& scheduler): m_scheduler(scheduler) {}
	KYTY_CLASS_NO_COPY(FramePool);

	void Initialize(uint32_t count) {
		Common::LockGuard lock(m_mutex);
		EXIT_IF(count == 0 || !m_frames.empty());
		for (uint32_t i = 0; i < count; i++) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
	}

	Presenter::Frame* Acquire() {
		m_mutex.Lock();
		EXIT_IF(m_frames.empty());
		// A synchronized flip may need more frames than the swap chain has buffers.
		if (m_free.empty()) {
			auto frame = std::make_unique<Presenter::Frame>();
			m_free.push_back(frame.get());
			m_frames.push_back(std::move(frame));
		}
		auto* frame = m_free.back();
		m_free.pop_back();
		EXIT_IF(frame->busy);
		frame->busy = true;
		m_mutex.Unlock();

		m_scheduler.Wait(frame->present_tick);
		return frame;
	}

	void ValidateForPresent(Presenter::Frame* frame) {
		Common::LockGuard lock(m_mutex);
		if (frame == nullptr || !frame->busy) {
			EXIT("prepared frame has invalid presentation ownership\n");
		}
	}

	void Release(Presenter::Frame* frame) {
		if (frame == nullptr) {
			EXIT("cannot release a null prepared frame\n");
		}
		Common::LockGuard lock(m_mutex);
		if (!frame->busy) {
			EXIT("prepared frame was released twice\n");
		}
		frame->busy = false;
		m_free.push_back(frame);
	}

private:
	CommandScheduler&                              m_scheduler;
	Common::Mutex                                  m_mutex;
	std::vector<std::unique_ptr<Presenter::Frame>> m_frames;
	std::vector<Presenter::Frame*>                 m_free;
};

// A DXGI flip-model composition swap chain, which the host shows in its window (a SwapChainPanel in the UWP app).
class Swapchain final {
public:
	static constexpr uint32_t    BufferCount = 3;
	static constexpr DXGI_FORMAT Format      = DXGI_FORMAT_B8G8R8A8_UNORM;

	explicit Swapchain(WindowContext& window): m_window(window) {}
	~Swapchain() { ReleaseBuffers(); }
	KYTY_CLASS_NO_COPY(Swapchain);

	void Create() {
		auto& graphics = m_window.graphic_ctx;

		const auto [width, height] = SurfaceSize();
		DXGI_SWAP_CHAIN_DESC1 desc {};
		desc.Width            = width;
		desc.Height           = height;
		desc.Format           = Format;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount      = BufferCount;
		desc.Scaling          = DXGI_SCALING_STRETCH;
		desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

		D3D12::ComPtr<IDXGIFactory2> factory2;
		D3D12::Check(graphics.factory->QueryInterface(IID_PPV_ARGS(&factory2)), "query IDXGIFactory2");
		D3D12::Check(factory2->CreateSwapChainForComposition(graphics.queue, &desc, nullptr, &m_swapchain), "CreateSwapChainForComposition");
		D3D12::Check(m_swapchain.As(&m_swapchain3), "query IDXGISwapChain3");

		D3D12_DESCRIPTOR_HEAP_DESC heap {};
		heap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heap.NumDescriptors = BufferCount;
		D3D12::Check(graphics.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_rtv_heap)), "create the swap chain RTV heap");
		m_rtv_stride = graphics.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		AcquireBuffers(width, height);

		EXIT_IF(!m_window.attach_swapchain);
		m_window.attach_swapchain(m_swapchain.Get());
	}

	// True when the host's surface changed size. The caller guarantees the GPU no longer uses the old buffers before Resize.
	[[nodiscard]] bool NeedsResize() const {
		const auto [width, height] = SurfaceSize();
		return width != m_width || height != m_height;
	}

	void Resize() {
		const auto [width, height] = SurfaceSize();
		ReleaseBuffers();
		D3D12::Check(m_swapchain->ResizeBuffers(BufferCount, width, height, Format, 0), "resize the swap chain");
		AcquireBuffers(width, height);
		Common::DebugCounters::Add(Common::DebugCounters::Counter::SwapchainRecreations);
	}

	[[nodiscard]] uint32_t Width() const noexcept { return m_width; }
	[[nodiscard]] uint32_t Height() const noexcept { return m_height; }

	// Makes the current back buffer a render target and returns its view.
	D3D12_CPU_DESCRIPTOR_HANDLE BeginRecord(ID3D12GraphicsCommandList* list) {
		Transition(list, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		auto rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += static_cast<SIZE_T>(m_swapchain3->GetCurrentBackBufferIndex()) * m_rtv_stride;
		return rtv;
	}

	void EndRecord(ID3D12GraphicsCommandList* list) { Transition(list, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT); }

	void Present() {
		// The composition swap chain has no frame statistics to pace with (measured), so the interval does it; the other modes do not wait.
		UINT interval = 1;
		switch (Config::GetPresentMode()) {
			case Config::PresentMode::Fifo: break;
			case Config::PresentMode::Mailbox:
			case Config::PresentMode::Immediate: interval = 0; break;
		}
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		D3D12::Check(m_swapchain->Present(interval, 0), "present");
	}

private:
	// Resizing to 8192x8192 and presenting removes the console's device (guard G8): the size stays within 4K.
	[[nodiscard]] std::pair<uint32_t, uint32_t> SurfaceSize() const {
		const auto size = m_window.surface_size ? m_window.surface_size() : std::pair<uint32_t, uint32_t> {1, 1};
		return {std::clamp<uint32_t>(size.first, 1, 3840), std::clamp<uint32_t>(size.second, 1, 2160)};
	}

	void Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
		D3D12_RESOURCE_BARRIER barrier {};
		barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource   = m_buffers[m_swapchain3->GetCurrentBackBufferIndex()].Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter  = after;
		list->ResourceBarrier(1, &barrier);
	}

	void AcquireBuffers(uint32_t width, uint32_t height) {
		auto* device = m_window.graphic_ctx.device;
		auto  rtv    = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
		for (uint32_t i = 0; i < BufferCount; i++) {
			D3D12::Check(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_buffers[i])), "get a swap chain buffer");
			device->CreateRenderTargetView(m_buffers[i].Get(), nullptr, rtv);
			rtv.ptr += m_rtv_stride;
		}
		m_width  = width;
		m_height = height;
		FrameResolution::NoteDisplay(width, height);
	}

	void ReleaseBuffers() {
		for (auto& buffer: m_buffers) {
			buffer.Reset();
		}
	}

	WindowContext&                                         m_window;
	D3D12::ComPtr<IDXGISwapChain1>                         m_swapchain;
	D3D12::ComPtr<IDXGISwapChain3>                         m_swapchain3;
	D3D12::ComPtr<ID3D12DescriptorHeap>                    m_rtv_heap;
	std::array<D3D12::ComPtr<ID3D12Resource>, BufferCount> m_buffers;
	uint32_t                                               m_rtv_stride = 0;
	uint32_t                                               m_width      = 0;
	uint32_t                                               m_height     = 0;
};

// The largest rectangle of the source's aspect ratio centred in the target (upstream's swapchain.cpp).
static D3D12_RECT FitAspect(uint32_t source_width, uint32_t source_height, uint32_t width, uint32_t height) {
	const uint64_t src_w = std::max(source_width, 1u);
	const uint64_t src_h = std::max(source_height, 1u);
	uint32_t       w     = width;
	uint32_t       h     = height;
	if (src_w * h > src_h * w) {
		h = std::max<uint32_t>(1, static_cast<uint32_t>(src_h * w / src_w));
	} else {
		w = std::max<uint32_t>(1, static_cast<uint32_t>(src_w * h / src_h));
	}
	const auto left = static_cast<LONG>((width - w) / 2);
	const auto top  = static_cast<LONG>((height - h) / 2);
	return {left, top, left + static_cast<LONG>(w), top + static_cast<LONG>(h)};
}

// Scales frames onto the swap chain with a fullscreen triangle; D3D12 has no blit. The base layer replaces what is there, the overlay layer is blended
// over it as premultiplied alpha.
class FrameBlitter final {
public:
	FrameBlitter(GraphicContext& graphics, const D3D12::DxilCompiler& compiler): m_device(graphics.device) {
		using Kind = D3D12::LinkedStage::Kind;
		// The vertex shader emits Vulkan clip space; its Y flip maps it to D3D12's.
		const D3D12::LinkedStage stages[] {{GPU_BLIT_FS_TRIANGLE_SPV, Kind::Vertex, true}, {GPU_BLIT_PRESENT_SPV, Kind::Pixel}};
		const auto               shaders = compiler.CompileLinked(stages, 0);
		if (!shaders[0].IsValid() || !shaders[1].IsValid()) {
			// Nothing can be shown without it; the frames stay black.
			Log::WriteToConsoleAndLog("D3D12: the presentation shaders could not be translated; frames are not shown\n");
			return;
		}

		D3D12_DESCRIPTOR_RANGE1 range {};
		range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		D3D12_ROOT_PARAMETER1 parameters[3] {};
		parameters[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[0].Constants.Num32BitValues = D3D12::RuntimeDataDwords;
		parameters[0].Constants.RegisterSpace  = D3D12::RuntimeDataSpace;
		parameters[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[1].Constants.Num32BitValues = 1; // flags (gpu_blit_present.frag)
		parameters[1].Constants.RegisterSpace  = D3D12::PushConstantSpace;
		parameters[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
		parameters[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[2].DescriptorTable.NumDescriptorRanges = 1;
		parameters[2].DescriptorTable.pDescriptorRanges   = &range;
		parameters[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC sampler {};
		sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.MaxLOD           = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister   = 1;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
		desc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
		desc.Desc_1_1.NumParameters     = 3;
		desc.Desc_1_1.pParameters       = parameters;
		desc.Desc_1_1.NumStaticSamplers = 1;
		desc.Desc_1_1.pStaticSamplers   = &sampler;
		D3D12::ComPtr<ID3DBlob> blob;
		D3D12::ComPtr<ID3DBlob> error;
		if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
			EXIT("D3D12: present root signature serialization failed: %s\n", error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
		}
		D3D12::Check(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root_signature)),
		             "create the present root signature");

		for (const bool overlay: {false, true}) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline {};
			pipeline.pRootSignature = m_root_signature.Get();
			pipeline.VS             = {shaders[0].bytecode.data(), shaders[0].bytecode.size()};
			pipeline.PS             = {shaders[1].bytecode.data(), shaders[1].bytecode.size()};
			pipeline.BlendState.RenderTarget[0] = {overlay ? TRUE : FALSE,
			                                       FALSE,
			                                       D3D12_BLEND_ONE,
			                                       overlay ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO,
			                                       D3D12_BLEND_OP_ADD,
			                                       D3D12_BLEND_ONE,
			                                       overlay ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO,
			                                       D3D12_BLEND_OP_ADD,
			                                       D3D12_LOGIC_OP_NOOP,
			                                       D3D12_COLOR_WRITE_ENABLE_ALL};
			pipeline.SampleMask                      = UINT_MAX;
			pipeline.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
			pipeline.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
			pipeline.RasterizerState.DepthClipEnable = TRUE;
			pipeline.DepthStencilState.DepthFunc     = D3D12_COMPARISON_FUNC_ALWAYS;
			pipeline.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
			pipeline.DepthStencilState.BackFace  = pipeline.DepthStencilState.FrontFace;
			pipeline.PrimitiveTopologyType       = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pipeline.NumRenderTargets            = 1;
			pipeline.RTVFormats[0]               = Swapchain::Format;
			pipeline.SampleDesc.Count            = 1;
			D3D12::Check(m_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipelines[overlay ? 1 : 0])), "create a present pipeline");
		}

		D3D12_DESCRIPTOR_HEAP_DESC heap {};
		heap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap.NumDescriptors = Slots;
		heap.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		D3D12::Check(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_views)), "create the present view heap");
		m_increment = m_device->GetDescriptorHandleIncrementSize(heap.Type);
	}
	KYTY_CLASS_NO_COPY(FrameBlitter);

	[[nodiscard]] bool Ready() const noexcept { return m_root_signature != nullptr; }

	void Record(ID3D12GraphicsCommandList* list, const Presenter::Frame& frame, D3D12_CPU_DESCRIPTOR_HANDLE target, const D3D12_RECT& rect,
	            bool overlay, bool premultiplied, ID3D12Resource* generated = nullptr) {
		const auto width  = static_cast<uint32_t>(rect.right - rect.left);
		const auto height = static_cast<uint32_t>(rect.bottom - rect.top);
		if (!Ready()) {
			return;
		}
		EXIT_IF(frame.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		// The slots are used round robin: a slot is rewritten long after the GPU finished with its last use.
		const auto slot = m_next++ % Slots;
		auto       cpu  = m_views->GetCPUDescriptorHandleForHeapStart();
		auto       gpu  = m_views->GetGPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(slot) * m_increment;
		gpu.ptr += static_cast<UINT64>(slot) * m_increment;
		D3D12_SHADER_RESOURCE_VIEW_DESC view {};
		view.Format                  = generated ? DXGI_FORMAT_R8G8B8A8_UNORM : frame.format.view;
		view.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = generated ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING : frame.format.mapping;
		view.Texture2D.MipLevels     = 1;
		m_device->CreateShaderResourceView(generated ? generated : frame.image.Get(), &view, cpu);

		D3D12::VertexRuntimeData runtime {};
		runtime.yz_flip_mask    = 1;
		runtime.viewport_width  = static_cast<float>(width);
		runtime.viewport_height = static_cast<float>(height);
		uint32_t constants[D3D12::RuntimeDataDwords] {};
		std::memcpy(constants, &runtime, sizeof(runtime));

		ID3D12DescriptorHeap* heaps[] {m_views.Get()};
		list->SetDescriptorHeaps(1, heaps);
		list->SetGraphicsRootSignature(m_root_signature.Get());
		list->SetPipelineState(m_pipelines[overlay ? 1 : 0].Get());
		list->SetGraphicsRoot32BitConstants(0, D3D12::RuntimeDataDwords, constants, 0);
		// The base layer (the main bus) ignores alpha, as upstream's.
		const uint32_t flags = (premultiplied ? 1u : 0u) | (frame.hdr ? 4u : 0u) | (frame.linear ? 8u : 0u) | (overlay ? 0u : 16u);
		list->SetGraphicsRoot32BitConstants(1, 1, &flags, 0);
		list->SetGraphicsRootDescriptorTable(2, gpu);
		const D3D12_VIEWPORT viewport {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(width), static_cast<float>(height),
		                               0.0f, 1.0f};
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &rect);
		list->OMSetRenderTargets(1, &target, FALSE, nullptr);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->DrawInstanced(3, 1, 0, 0);
	}

private:
	static constexpr uint32_t Slots = 256;

	ID3D12Device*                       m_device = nullptr;
	D3D12::ComPtr<ID3D12RootSignature>  m_root_signature;
	D3D12::ComPtr<ID3D12PipelineState>  m_pipelines[2];
	D3D12::ComPtr<ID3D12DescriptorHeap> m_views;
	uint32_t                            m_increment = 0;
	uint32_t                            m_next      = 0;
};

uint32_t FloatBits(float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

// FsrEasuCon in ffx_fsr1.h for the whole input image.
std::array<uint32_t, 16> EasuConstants(float input_width, float input_height, float output_width, float output_height) {
	return {FloatBits(input_width / output_width),
	        FloatBits(input_height / output_height),
	        FloatBits(0.5f * input_width / output_width - 0.5f),
	        FloatBits(0.5f * input_height / output_height - 0.5f),
	        FloatBits(1.0f / input_width),
	        FloatBits(1.0f / input_height),
	        FloatBits(1.0f / input_width),
	        FloatBits(-1.0f / input_height),
	        FloatBits(-1.0f / input_width),
	        FloatBits(2.0f / input_height),
	        FloatBits(1.0f / input_width),
	        FloatBits(2.0f / input_height),
	        FloatBits(0.0f),
	        FloatBits(4.0f / input_height),
	        0,
	        0};
}

// FsrRcasCon in ffx_fsr1.h (its packed half values are for the 16-bit shader, which is not used); then the top left of the picture on the swap chain
// (gpu_blit_fsr1_rcas.frag).
std::array<uint32_t, 16> RcasConstants(float sharpness, uint32_t left, uint32_t top) {
	return {FloatBits(std::exp2(-sharpness)), 0, 0, 0, left, top, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}

// RCAS's sharpness in stops below its maximum for Config::fsr1_upscaling 1..3 (low, medium, high); AMD's samples use 0.25.
float Fsr1Sharpness(uint32_t mode) {
	return mode >= 3 ? 0.0f : mode == 2 ? 0.25f : 1.0f;
}

// Experiment: AMD FidelityFX FSR 1 (3rdparty/fidelityfx-fsr, MIT) instead of the blitter for game pictures smaller than their place on the screen: its
// edge adaptive upsampling (EASU) scales the picture into a texture of that place's size, and its robust contrast adaptive sharpening (RCAS) draws that
// onto the swap chain. Both are fullscreen triangles like the blitter's (32-bit: the Series X has no 16-bit shader ops).
class Fsr1Scaler final {
public:
	Fsr1Scaler(GraphicContext& graphics, const D3D12::DxilCompiler& compiler): m_device(graphics.device) {
		using Kind = D3D12::LinkedStage::Kind;
		const D3D12::LinkedStage easu[] {{GPU_BLIT_FS_TRIANGLE_SPV, Kind::Vertex, true}, {GPU_BLIT_FSR1_EASU_SPV, Kind::Pixel}};
		const D3D12::LinkedStage rcas[] {{GPU_BLIT_FS_TRIANGLE_SPV, Kind::Vertex, true}, {GPU_BLIT_FSR1_RCAS_SPV, Kind::Pixel}};
		const auto               easu_shaders = compiler.CompileLinked(easu, 0);
		const auto               rcas_shaders = compiler.CompileLinked(rcas, 0);
		if (!easu_shaders[0].IsValid() || !easu_shaders[1].IsValid() || !rcas_shaders[0].IsValid() || !rcas_shaders[1].IsValid()) {
			Log::WriteToConsoleAndLog("FSR 1: its shaders could not be translated; pictures are scaled without it\n");
			return;
		}

		// As the blitter's, with FSR's 16 constants.
		D3D12_DESCRIPTOR_RANGE1 range {};
		range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		D3D12_ROOT_PARAMETER1 parameters[3] {};
		parameters[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[0].Constants.Num32BitValues = D3D12::RuntimeDataDwords;
		parameters[0].Constants.RegisterSpace  = D3D12::RuntimeDataSpace;
		parameters[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[1].Constants.Num32BitValues = 16;
		parameters[1].Constants.RegisterSpace  = D3D12::PushConstantSpace;
		parameters[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
		parameters[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[2].DescriptorTable.NumDescriptorRanges = 1;
		parameters[2].DescriptorTable.pDescriptorRanges   = &range;
		parameters[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC sampler {};
		sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.MaxLOD           = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister   = 1;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
		desc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
		desc.Desc_1_1.NumParameters     = 3;
		desc.Desc_1_1.pParameters       = parameters;
		desc.Desc_1_1.NumStaticSamplers = 1;
		desc.Desc_1_1.pStaticSamplers   = &sampler;
		D3D12::ComPtr<ID3DBlob> blob;
		D3D12::ComPtr<ID3DBlob> error;
		if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error)) ||
		    FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root_signature)))) {
			Log::WriteToConsoleAndLog("FSR 1: no root signature; pictures are scaled without it\n");
			m_root_signature.Reset();
			return;
		}

		for (int pass = 0; pass < 2; pass++) {
			const auto&                        shaders = pass == 0 ? easu_shaders : rcas_shaders;
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline {};
			pipeline.pRootSignature                  = m_root_signature.Get();
			pipeline.VS                              = {shaders[0].bytecode.data(), shaders[0].bytecode.size()};
			pipeline.PS                              = {shaders[1].bytecode.data(), shaders[1].bytecode.size()};
			pipeline.BlendState.RenderTarget[0]      = {FALSE,
			                                            FALSE,
			                                            D3D12_BLEND_ONE,
			                                            D3D12_BLEND_ZERO,
			                                            D3D12_BLEND_OP_ADD,
			                                            D3D12_BLEND_ONE,
			                                            D3D12_BLEND_ZERO,
			                                            D3D12_BLEND_OP_ADD,
			                                            D3D12_LOGIC_OP_NOOP,
			                                            D3D12_COLOR_WRITE_ENABLE_ALL};
			pipeline.SampleMask                      = UINT_MAX;
			pipeline.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
			pipeline.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
			pipeline.RasterizerState.DepthClipEnable = TRUE;
			pipeline.DepthStencilState.DepthFunc     = D3D12_COMPARISON_FUNC_ALWAYS;
			pipeline.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
			pipeline.DepthStencilState.BackFace  = pipeline.DepthStencilState.FrontFace;
			pipeline.PrimitiveTopologyType       = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pipeline.NumRenderTargets            = 1;
			pipeline.RTVFormats[0]               = Swapchain::Format;
			pipeline.SampleDesc.Count            = 1;
			if (FAILED(m_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipelines[pass])))) {
				Log::WriteToConsoleAndLog("FSR 1: no pipeline; pictures are scaled without it\n");
				m_root_signature.Reset();
				return;
			}
		}

		D3D12_DESCRIPTOR_HEAP_DESC heap {};
		heap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap.NumDescriptors = Slots;
		heap.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		D3D12::Check(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_views)), "create the FSR 1 view heap");
		m_increment = m_device->GetDescriptorHandleIncrementSize(heap.Type);
		D3D12_DESCRIPTOR_HEAP_DESC rtv {};
		rtv.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtv.NumDescriptors = 1;
		D3D12::Check(m_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_rtv_heap)), "create the FSR 1 RTV heap");
		Log::WriteToConsoleAndLog("FSR 1: on\n");
	}
	KYTY_CLASS_NO_COPY(Fsr1Scaler);

	[[nodiscard]] bool Ready() const noexcept { return m_root_signature != nullptr; }

	// Records both passes: the source (input_width x input_height, in PIXEL_SHADER_RESOURCE) onto rect of the back buffer. The scaled texture follows
	// rect's size; it is only replaced after the GPU finished with it (the presenter waits for its last submission first).
	bool Record(ID3D12GraphicsCommandList* list, ID3D12Resource* source, DXGI_FORMAT view_format, UINT mapping, uint32_t input_width,
	            uint32_t input_height, D3D12_CPU_DESCRIPTOR_HANDLE target, const D3D12_RECT& rect, float sharpness, const std::function<void()>& wait_idle) {
		const auto width  = static_cast<uint32_t>(rect.right - rect.left);
		const auto height = static_cast<uint32_t>(rect.bottom - rect.top);
		if (m_scaled == nullptr || m_width != width || m_height != height) {
			if (m_scaled != nullptr) {
				wait_idle();
			}
			if (!CreateScaled(width, height)) {
				return false;
			}
		}
		Transition(list, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const D3D12_RECT scaled_rect {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
		Draw(list, m_pipelines[0].Get(), source, view_format, mapping, m_rtv_heap->GetCPUDescriptorHandleForHeapStart(), scaled_rect,
		     EasuConstants(static_cast<float>(input_width), static_cast<float>(input_height), static_cast<float>(width), static_cast<float>(height)));
		Transition(list, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Draw(list, m_pipelines[1].Get(), m_scaled.Get(), Swapchain::Format, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, target, rect,
		     RcasConstants(sharpness, static_cast<uint32_t>(rect.left), static_cast<uint32_t>(rect.top)));
		return true;
	}

private:
	static constexpr uint32_t Slots = 64;

	bool CreateScaled(uint32_t width, uint32_t height) {
		D3D12_RESOURCE_DESC desc {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width            = width;
		desc.Height           = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = 1;
		desc.Format           = Swapchain::Format;
		desc.SampleDesc.Count = 1;
		desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_HEAP_PROPERTIES heap {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		m_scaled.Reset();
		m_width  = 0;
		m_height = 0;
		if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
		                                             IID_PPV_ARGS(&m_scaled)))) {
			// No memory for it: this picture is scaled without FSR 1.
			m_scaled.Reset();
			return false;
		}
		m_device->CreateRenderTargetView(m_scaled.Get(), nullptr, m_rtv_heap->GetCPUDescriptorHandleForHeapStart());
		m_state  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		m_width  = width;
		m_height = height;
		return true;
	}

	void Transition(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES after) {
		if (m_state == after) {
			return;
		}
		D3D12_RESOURCE_BARRIER barrier {};
		barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource   = m_scaled.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = m_state;
		barrier.Transition.StateAfter  = after;
		list->ResourceBarrier(1, &barrier);
		m_state = after;
	}

	void Draw(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline, ID3D12Resource* source, DXGI_FORMAT view_format, UINT mapping,
	          D3D12_CPU_DESCRIPTOR_HANDLE target, const D3D12_RECT& rect, const std::array<uint32_t, 16>& constants) {
		const auto width  = static_cast<uint32_t>(rect.right - rect.left);
		const auto height = static_cast<uint32_t>(rect.bottom - rect.top);
		// The slots are used round robin, as the blitter's.
		const auto slot = m_next++ % Slots;
		auto       cpu  = m_views->GetCPUDescriptorHandleForHeapStart();
		auto       gpu  = m_views->GetGPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(slot) * m_increment;
		gpu.ptr += static_cast<UINT64>(slot) * m_increment;
		D3D12_SHADER_RESOURCE_VIEW_DESC view {};
		view.Format                  = view_format;
		view.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = mapping;
		view.Texture2D.MipLevels     = 1;
		m_device->CreateShaderResourceView(source, &view, cpu);

		D3D12::VertexRuntimeData runtime {};
		runtime.yz_flip_mask    = 1;
		runtime.viewport_width  = static_cast<float>(width);
		runtime.viewport_height = static_cast<float>(height);
		uint32_t runtime_constants[D3D12::RuntimeDataDwords] {};
		std::memcpy(runtime_constants, &runtime, sizeof(runtime));

		ID3D12DescriptorHeap* heaps[] {m_views.Get()};
		list->SetDescriptorHeaps(1, heaps);
		list->SetGraphicsRootSignature(m_root_signature.Get());
		list->SetPipelineState(pipeline);
		list->SetGraphicsRoot32BitConstants(0, D3D12::RuntimeDataDwords, runtime_constants, 0);
		list->SetGraphicsRoot32BitConstants(1, 16, constants.data(), 0);
		list->SetGraphicsRootDescriptorTable(2, gpu);
		const D3D12_VIEWPORT viewport {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(width), static_cast<float>(height),
		                               0.0f, 1.0f};
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &rect);
		list->OMSetRenderTargets(1, &target, FALSE, nullptr);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->DrawInstanced(3, 1, 0, 0);
	}

	ID3D12Device*                       m_device = nullptr;
	D3D12::ComPtr<ID3D12RootSignature>  m_root_signature;
	D3D12::ComPtr<ID3D12PipelineState>  m_pipelines[2];
	D3D12::ComPtr<ID3D12DescriptorHeap> m_views;
	D3D12::ComPtr<ID3D12DescriptorHeap> m_rtv_heap;
	D3D12::ComPtr<ID3D12Resource>       m_scaled;
	D3D12_RESOURCE_STATES               m_state     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	uint32_t                            m_width     = 0;
	uint32_t                            m_height    = 0;
	uint32_t                            m_increment = 0;
	uint32_t                            m_next      = 0;
};

// The view VideoOut reads a guest flip image through.
ImageViewInfo SurfaceView(const ImageInfo& info) {
	ImageViewInfo view {};
	view.format = info.pixel_format;
	view.type   = vk::ImageViewType::e2D;
	view.aspect = vk::ImageAspectFlagBits::eColor;
	view.usage  = vk::ImageUsageFlagBits::eTransferSrc;
	return view;
}

} // namespace

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner), blitter(owner.graphic_ctx, renderer.GetPipelineCache().GetCompiler()),
	      present_scheduler(renderer, owner.graphic_ctx), frames(present_scheduler) {
		swapchain.Create();
		frames.Initialize(Swapchain::BufferCount);
	}

	void Present(bool interpolate = false);

	// The game's picture (or the frame generated before it) onto rect of the back buffer: with FSR 1 when it is on and the picture is smaller than
	// rect, else with the blitter. Not for HDR10 pictures: FSR 1 has no conversion to SDR.
	void DrawBase(ID3D12GraphicsCommandList* list, const Frame& frame, D3D12_CPU_DESCRIPTOR_HANDLE target, const D3D12_RECT& rect,
	              ID3D12Resource* generated = nullptr) {
		const auto mode   = Config::GetFsr1Upscaling();
		const auto width  = static_cast<uint32_t>(rect.right - rect.left);
		const auto height = static_cast<uint32_t>(rect.bottom - rect.top);
		bool       scaled = false;
		if (mode != 0 && !frame.hdr && !frame.linear && (frame.width < width || frame.height < height)) {
			if (fsr1 == nullptr) {
				fsr1 = std::make_unique<Fsr1Scaler>(window.graphic_ctx, renderer.GetPipelineCache().GetCompiler());
			}
			if (fsr1->Ready()) {
				scaled = fsr1->Record(list, generated != nullptr ? generated : frame.image.Get(),
				                      generated != nullptr ? DXGI_FORMAT_R8G8B8A8_UNORM : frame.format.view,
				                      generated != nullptr ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING : frame.format.mapping, frame.width, frame.height,
				                      target, rect, Fsr1Sharpness(mode),
				                      [this] { present_scheduler.Wait(present_scheduler.CurrentTick() - 1); });
			}
		}
		FrameResolution::NoteFsr1(scaled);
		if (!scaled) {
			blitter.Record(list, frame, target, rect, false, true, generated);
		}
	}

	Image& ResolveSurface(const ImageInfo& info) {
		TextureCache::ImageDesc desc {};
		desc.info      = info;
		desc.view_info = SurfaceView(info);
		desc.type      = TextureCache::BindingType::VideoOut;

		auto&      cache    = renderer.GetTextureCache();
		const auto image_id = cache.FindImage(desc);
		auto&      image    = cache.GetImage(image_id);
		image.usage.video_out = true;
		cache.UpdateImage(image_id);
		return image;
	}

	RenderContext&       renderer;
	WindowContext&       window;
	Swapchain            swapchain;
	FrameBlitter         blitter;
	// Made the first time a picture needs it (Config::fsr1_upscaling).
	std::unique_ptr<Fsr1Scaler> fsr1;
	CommandScheduler     present_scheduler;
	FramePool            frames;
	Common::Mutex        present_mutex;
	std::array<Layer, 2> layers {};
#ifdef KYTY_HAS_FSR
	uint64_t guest_sequence = 0;
	bool real_frame_pending = false;
#endif
};

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() {
	m_impl->present_scheduler.Wait(m_impl->present_scheduler.CurrentTick() - 1);
}

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info, bool hdr,
                                          bool /*headset*/, [[maybe_unused]] bool main_bus) {
	KYTY_PROFILER_FUNCTION();
	// No HDR output here (IsHdrSupported): an HDR10 frame is converted to SDR when it is shown (FrameBlitter::Record).
	// (No headset either: the OpenXR output is Vulkan only, and the headset port shows like the main one.)
	EXIT_IF(buffer.IsInvalid());
	auto*             frame = m_impl->frames.Acquire();
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
#ifdef KYTY_HAS_FSR
	frame->generated_valid = false;
	frame->guest_sequence = main_bus ? ++m_impl->guest_sequence : 0;
	auto* fx = main_bus ? m_impl->renderer.GetFidelityFx() : nullptr;
#endif
	auto&             image        = m_impl->ResolveSurface(info);
	auto              frame_format = info.pixel_format;
	switch (frame_format) {
		case vk::Format::eR8G8B8A8Srgb: frame_format = vk::Format::eR8G8B8A8Unorm; break;
		case vk::Format::eB8G8R8A8Srgb: frame_format = vk::Format::eB8G8R8A8Unorm; break;
		default: break;
	}
	if (image.backing.resources.empty() || !D3D12::GetFormatInfo(frame_format).Supported()) {
		// An image the backend cannot hold (an unsupported format): the frame stays black.
		frame->blank = true;
		frame->color = {0.0f, 0.0f, 0.0f, 1.0f};
#ifdef KYTY_HAS_FSR
		if (fx && Config::FsrFrameGenerationEnabled()) {
			fx->SkipFlip();
		}
#endif
		return *frame;
	}
	frame->Configure(m_impl->window.graphic_ctx, image.backing.extent.width, image.backing.extent.height, frame_format);
	frame->CopyFrom(buffer, image, SurfaceView(info));
	frame->blank  = false;
	frame->hdr    = hdr;
	frame->linear = frame_format == vk::Format::eR16G16B16A16Sfloat;
#ifdef KYTY_HAS_FSR
	if (fx && Config::FsrFrameGenerationEnabled()) {
		frame->generated_valid = fx->Interpolate(buffer, frame->image.Get(), frame->format.view,
		                                         hdr || frame->linear, frame->generated);
	}
#endif
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t /*width*/, uint32_t /*height*/, bool opaque, [[maybe_unused]] CommandBuffer* producer, [[maybe_unused]] bool main_bus) {
	KYTY_PROFILER_FUNCTION();
	auto* frame  = m_impl->frames.Acquire();
#ifdef KYTY_HAS_FSR
	frame->generated_valid = false;
	frame->guest_sequence = 0;
	if (main_bus && producer) {
		Common::LockGuard lock(m_impl->renderer.GetMutex());
		frame->guest_sequence = ++m_impl->guest_sequence;
		if (auto* fx = m_impl->renderer.GetFidelityFx(); fx && Config::FsrFrameGenerationEnabled()) {
			fx->SkipFlip();
		}
	}
#endif
	frame->blank = true;
	frame->hdr   = false;
	frame->color = {0.0f, 0.0f, 0.0f, opaque ? 1.0f : 0.0f};
	return *frame;
}

#ifdef KYTY_HAS_FSR
bool Presenter::PresentPendingFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (!m_impl->real_frame_pending) {
		return false;
	}
	m_impl->real_frame_pending = false;
	m_impl->Present();
	return true;
}
#endif

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::IsHdrSupported() const noexcept {
	return false;
}

bool Presenter::NeedsSystemOverlayRefresh() const noexcept {
	return false;
}

RenderContext& Presenter::Renderer() const noexcept {
	return m_impl->renderer;
}

void Presenter::Present(Frame& frame) {
	const Layer layer {&frame, 0, false};
	Present(std::span(&layer, 1));
}

void Presenter::Present(std::span<const Layer> layers) {
#ifdef KYTY_HAS_FSR
	PresentWithInterpolation(layers, false);
}

void Presenter::PresentWithInterpolation(std::span<const Layer> layers, bool allow_interpolation) {
#endif
	Common::LockGuard lock(m_impl->present_mutex);
	bool interpolate = false;
	for (const auto& layer: layers) {
		EXIT_IF(layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size()));
		m_impl->frames.ValidateForPresent(layer.frame);
		auto& previous = m_impl->layers[layer.bus];
#ifdef KYTY_HAS_FSR
		if (layer.bus == 0) {
			interpolate = allow_interpolation && !m_impl->real_frame_pending && layer.frame->generated_valid &&
			              previous.frame && previous.frame->guest_sequence + 1 == layer.frame->guest_sequence;
		}
#endif
		if (previous.frame != nullptr) {
			m_impl->frames.Release(previous.frame);
		}
		previous = layer;
		if (layer.bus == 0 && !layer.frame->blank) {
			FrameResolution::NoteOutput(layer.frame->width, layer.frame->height);
		}
	}
#ifdef KYTY_HAS_FSR
	m_impl->real_frame_pending = interpolate;
#endif
	m_impl->Present(interpolate);
}

void Presenter::ClearLayer(int bus) {
	if (static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(m_impl->present_mutex);
	auto&             layer = m_impl->layers[bus];
#ifdef KYTY_HAS_FSR
	if (bus == 0) {
		m_impl->real_frame_pending = false;
	}
#endif
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present([[maybe_unused]] bool interpolate) {
	KYTY_PROFILER_FUNCTION();

	if (swapchain.NeedsResize()) {
		present_scheduler.Wait(present_scheduler.CurrentTick() - 1);
		swapchain.Resize();
	}
	{
		// present_mutex owns the prepared frames and this scheduler. Only queue submission is shared with rendering, and it has its own
		// queue_mutex (the blitter has descriptors of its own).
		auto*      list   = present_scheduler.BeginCommand().Handle();
		const auto target = swapchain.BeginRecord(list);
		const std::array<float, 4> black {0.0f, 0.0f, 0.0f, 1.0f};
		const auto* base    = layers[0].frame;
		const auto* overlay = layers[1].frame;
		// The game's frame keeps its aspect ratio, black bars around it, unless stretched (upstream's swapchain.cpp); the overlay layer
		// takes the same place.
		const auto* aspect_source = base != nullptr && !base->blank ? base : overlay != nullptr && !overlay->blank ? overlay : nullptr;
		const auto  game_rect     = !Config::StretchEnabled() && aspect_source != nullptr
		                                ? FitAspect(aspect_source->width, aspect_source->height, swapchain.Width(), swapchain.Height())
		                                : D3D12_RECT {0, 0, static_cast<LONG>(swapchain.Width()), static_cast<LONG>(swapchain.Height())};
		const bool  full_screen = game_rect.left == 0 && game_rect.top == 0 && game_rect.right == static_cast<LONG>(swapchain.Width()) &&
		                         game_rect.bottom == static_cast<LONG>(swapchain.Height());
		if (base == nullptr || base->blank) {
			list->ClearRenderTargetView(target, base != nullptr ? base->color.data() : black.data(), 0, nullptr);
		} else {
			if (!full_screen) {
				list->ClearRenderTargetView(target, black.data(), 0, nullptr);
			}
			ID3D12Resource* generated = nullptr;
#ifdef KYTY_HAS_FSR
			if (interpolate) {
				generated = base->generated.Get();
				D3D12_RESOURCE_BARRIER barrier {};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition = {generated, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
				list->ResourceBarrier(1, &barrier);
				DrawBase(list, *base, target, game_rect, generated);
				std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
				list->ResourceBarrier(1, &barrier);
			}
#endif
			if (!generated) {
				DrawBase(list, *base, target, game_rect);
			}
		}
		if (overlay != nullptr) {
			if (!overlay->blank) {
				blitter.Record(list, *overlay, target, game_rect, true, layers[1].premultiplied_alpha);
			} else if (overlay->color[3] > 0.0f) {
				list->ClearRenderTargetView(target, overlay->color.data(), 1, &game_rect);
			}
		}
		swapchain.EndRecord(list);
		const auto tick = present_scheduler.Submit();
		for (const auto& layer: layers) {
			if (layer.frame != nullptr) {
				layer.frame->present_tick = tick;
			}
		}
	}
	swapchain.Present();
	renderer.GetPipelineCache().SaveWhenIdle();
	if (window.frame_presented) {
		window.frame_presented();
	}
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

} // namespace Libs::Graphics
