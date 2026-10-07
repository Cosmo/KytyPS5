#include "graphics/presentation/presenter.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/d3d12/windowContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace Libs::Graphics {

// A prepared guest frame. The guest's images are not on the GPU yet, so for now a frame is its size and a color.
struct Presenter::Frame {
	uint32_t             width        = 0;
	uint32_t             height       = 0;
	std::array<float, 4> color        = {0.0f, 0.0f, 0.0f, 1.0f};
	uint64_t             present_tick = 0;
	bool                 busy         = false;
};

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

	void RecordClear(ID3D12GraphicsCommandList* list, const std::array<float, 4>& color) {
		const auto index  = m_swapchain3->GetCurrentBackBufferIndex();
		auto*      target = m_buffers[index].Get();

		D3D12_RESOURCE_BARRIER barrier {};
		barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource   = target;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
		list->ResourceBarrier(1, &barrier);

		auto rtv = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += static_cast<SIZE_T>(index) * m_rtv_stride;
		list->ClearRenderTargetView(rtv, color.data(), 0, nullptr);

		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		list->ResourceBarrier(1, &barrier);
	}

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
	[[nodiscard]] std::pair<uint32_t, uint32_t> SurfaceSize() const {
		const auto size = m_window.surface_size ? m_window.surface_size() : std::pair<uint32_t, uint32_t> {1, 1};
		return {std::max<uint32_t>(1, size.first), std::max<uint32_t>(1, size.second)};
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

} // namespace

struct Presenter::Impl {
	explicit Impl(WindowContext& owner)
	    : renderer(*owner.render_context), window(owner), swapchain(owner), present_scheduler(renderer, owner.graphic_ctx), frames(present_scheduler) {
		swapchain.Create();
		frames.Initialize(Swapchain::BufferCount);
	}

	void Present();

	RenderContext&   renderer;
	WindowContext&   window;
	Swapchain        swapchain;
	CommandScheduler present_scheduler;
	FramePool        frames;
	Common::Mutex    present_mutex;
	std::array<Layer, 2> layers {};
};

Presenter::Presenter(WindowContext& window): m_impl(std::make_unique<Impl>(window)) {}

Presenter::~Presenter() {
	m_impl->present_scheduler.Wait(m_impl->present_scheduler.CurrentTick() - 1);
}

Presenter::Frame& Presenter::PrepareFrame(CommandBuffer& buffer, const ImageInfo& info) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(buffer.IsInvalid());
	auto* frame   = m_impl->frames.Acquire();
	frame->width  = info.extent.width;
	frame->height = info.extent.height;
	frame->color  = {0.0f, 0.0f, 0.0f, 1.0f};
	return *frame;
}

Presenter::Frame& Presenter::PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque, CommandBuffer* /*producer*/) {
	KYTY_PROFILER_FUNCTION();
	auto* frame   = m_impl->frames.Acquire();
	frame->width  = width;
	frame->height = height;
	frame->color  = {0.0f, 0.0f, 0.0f, opaque ? 1.0f : 0.0f};
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
		auto&             command = present_scheduler.BeginCommand();
		// Until guest images are on the GPU, the frame's color is all there is to show (the first layer's, else black).
		swapchain.RecordClear(command.Handle(), layers[0].frame != nullptr ? layers[0].frame->color : std::array<float, 4> {0.0f, 0.0f, 0.0f, 1.0f});
		const auto tick = present_scheduler.Submit();
		for (const auto& layer: layers) {
			if (layer.frame != nullptr) {
				layer.frame->present_tick = tick;
			}
		}
	}
	swapchain.Present();
	if (window.frame_presented) {
		window.frame_presented();
	}
}

void Presenter::Discard(Frame& frame) {
	m_impl->frames.Release(&frame);
}

} // namespace Libs::Graphics
