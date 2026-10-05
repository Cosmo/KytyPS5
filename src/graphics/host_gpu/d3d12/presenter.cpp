#include "graphics/presentation/presenter.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_blit_present_spv.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/d3d12/windowContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
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
	std::array<float, 4>          color        = {0.0f, 0.0f, 0.0f, 1.0f};
	uint64_t                      present_tick = 0;
	bool                          busy         = false;

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
		parameters[1].Constants.Num32BitValues = 1; // premultiplied
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

	void Record(ID3D12GraphicsCommandList* list, const Presenter::Frame& frame, D3D12_CPU_DESCRIPTOR_HANDLE target, uint32_t width, uint32_t height,
	            bool overlay, bool premultiplied) {
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
		view.Format                  = frame.format.view;
		view.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = frame.format.mapping;
		view.Texture2D.MipLevels     = 1;
		m_device->CreateShaderResourceView(frame.image.Get(), &view, cpu);

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
		const uint32_t premultiplied_constant = premultiplied ? 1u : 0u;
		list->SetGraphicsRoot32BitConstants(1, 1, &premultiplied_constant, 0);
		list->SetGraphicsRootDescriptorTable(2, gpu);
		const D3D12_VIEWPORT viewport {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
		const D3D12_RECT     scissor {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
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

	void Present();

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
	CommandScheduler     present_scheduler;
	FramePool            frames;
	Common::Mutex        present_mutex;
	std::array<Layer, 2> layers {};
};

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() {
	m_impl->present_scheduler.Wait(m_impl->present_scheduler.CurrentTick() - 1);
}

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto*             frame = m_impl->frames.Acquire();
	Common::LockGuard render_lock(m_impl->renderer.GetMutex());
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
		return *frame;
	}
	frame->Configure(m_impl->window.graphic_ctx, image.backing.extent.width, image.backing.extent.height, frame_format);
	frame->CopyFrom(buffer, image, SurfaceView(info));
	frame->blank = false;
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t /*width*/, uint32_t /*height*/, bool opaque, CommandBuffer* /*producer*/) {
	KYTY_PROFILER_FUNCTION();
	auto* frame  = m_impl->frames.Acquire();
	frame->blank = true;
	frame->color = {0.0f, 0.0f, 0.0f, opaque ? 1.0f : 0.0f};
	return *frame;
}

bool Presenter::PresentLastFrame() {
	Common::LockGuard lock(m_impl->present_mutex);
	if (m_impl->layers[0].frame == nullptr && m_impl->layers[1].frame == nullptr) {
		return false;
	}
	m_impl->Present();
	return true;
}

bool Presenter::IsGuestPaused() const noexcept {
	return m_impl->window.paused.load(std::memory_order_acquire);
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
	Common::LockGuard lock(m_impl->present_mutex);
	for (const auto& layer: layers) {
		EXIT_IF(layer.bus < 0 || layer.bus >= static_cast<int>(m_impl->layers.size()));
		m_impl->frames.ValidateForPresent(layer.frame);
		auto& previous = m_impl->layers[layer.bus];
		if (previous.frame != nullptr) {
			m_impl->frames.Release(previous.frame);
		}
		previous = layer;
	}
	m_impl->Present();
}

void Presenter::ClearLayer(int bus) {
	if (static_cast<size_t>(bus) >= m_impl->layers.size()) {
		return;
	}
	Common::LockGuard lock(m_impl->present_mutex);
	auto&             layer = m_impl->layers[bus];
	if (layer.frame != nullptr) {
		m_impl->frames.Release(layer.frame);
		layer = {};
	}
}

void Presenter::Impl::Present() {
	KYTY_PROFILER_FUNCTION();

	if (swapchain.NeedsResize()) {
		present_scheduler.Wait(present_scheduler.CurrentTick() - 1);
		swapchain.Resize();
	}
	{
		Common::LockGuard render_lock(renderer.GetMutex());
		auto*             list   = present_scheduler.BeginCommand().Handle();
		const auto        target = swapchain.BeginRecord(list);
		const std::array<float, 4> black {0.0f, 0.0f, 0.0f, 1.0f};
		const auto* base = layers[0].frame;
		if (base == nullptr || base->blank) {
			list->ClearRenderTargetView(target, base != nullptr ? base->color.data() : black.data(), 0, nullptr);
		} else {
			blitter.Record(list, *base, target, swapchain.Width(), swapchain.Height(), false, true);
		}
		if (const auto* overlay = layers[1].frame; overlay != nullptr) {
			if (!overlay->blank) {
				blitter.Record(list, *overlay, target, swapchain.Width(), swapchain.Height(), true, layers[1].premultiplied_alpha);
			} else if (overlay->color[3] > 0.0f) {
				list->ClearRenderTargetView(target, overlay->color.data(), 0, nullptr);
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
