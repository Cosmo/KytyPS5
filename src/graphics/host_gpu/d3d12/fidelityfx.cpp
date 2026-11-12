#ifdef KYTY_HAS_FSR

#include "graphics/host_gpu/d3d12/fidelityfx.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_pssr_input_spv.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/d3d12/renderContext.h"

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_fsr3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fmt/format.h>
#include <vector>

namespace Libs::Graphics {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto ComputeRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto UnorderedAccess = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto GeneratedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
	list->ResourceBarrier(1, &barrier);
}

bool CreateTexture(ID3D12Device* device, DXGI_FORMAT format, uint32_t width, uint32_t height,
                   D3D12::ComPtr<ID3D12Resource>& result) {
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support {format};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
	    !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
		return false;
	}
	D3D12_HEAP_PROPERTIES heap {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Format = format;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, UnorderedAccess,
	                                                 nullptr, IID_PPV_ARGS(&result)));
}

FfxResource Wrap(ID3D12Resource* image, FfxResourceStates state, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) {
	auto desc = ffxGetResourceDescriptionDX12(image);
	if (format != DXGI_FORMAT_UNKNOWN) {
		desc.format = ffxGetSurfaceFormatDX12(format);
	}
	return ffxGetResourceDX12(image, desc, nullptr, state);
}

struct Texture {
	D3D12::ComPtr<ID3D12Resource> image;
	D3D12::ComPtr<ID3D12DescriptorHeap> views;
	D3D12_CPU_DESCRIPTOR_HANDLE srv {};
	D3D12_CPU_DESCRIPTOR_HANDLE uav {};

	bool Ensure(RenderContext& renderer, DXGI_FORMAT format, uint32_t width, uint32_t height, bool exact = false) {
		if (image && (exact ? image->GetDesc().Width == width && image->GetDesc().Height == height
		                    : image->GetDesc().Width >= width && image->GetDesc().Height >= height)) {
			return true;
		}
		auto* device = renderer.GetGraphics().device;
		D3D12::ComPtr<ID3D12Resource> next;
		if (!CreateTexture(device, format, width, height, next)) {
			return false;
		}
		if (!views) {
			D3D12_DESCRIPTOR_HEAP_DESC desc {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.NumDescriptors = 2;
			if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&views)))) {
				return false;
			}
			srv = views->GetCPUDescriptorHandleForHeapStart();
			uav.ptr = srv.ptr + device->GetDescriptorHandleIncrementSize(desc.Type);
		}
		// Keep old storage alive until the last recorded use (possibly in the current list).
		if (image) {
			renderer.GetCommandScheduler().DeferOperation([old = std::move(image)] {});
		}
		image = std::move(next);
		device->CreateShaderResourceView(image.Get(), nullptr, srv);
		device->CreateUnorderedAccessView(image.Get(), nullptr, nullptr, uav);
		return true;
	}
};

struct Effect {
	struct Description {
		uint32_t width, height, output_width, output_height, flags;
		DXGI_FORMAT format;
		bool operator==(const Description&) const = default;
	};

	std::unique_ptr<FfxFsr3Context> context;
	std::vector<std::byte> scratch;
	Description desc {};
	std::array<uint64_t, 3> ticks {};
	uint64_t frame_id = 0;
	bool ready = false;
	bool reset = true;

	~Effect() {
		if (ready) {
			ffxFsr3ContextDestroy(context.get());
		}
	}

	bool Ensure(RenderContext& renderer, Description next, bool interpolate) {
		if (desc.flags == next.flags && desc.format == next.format && desc.output_width == next.output_width &&
		    desc.output_height == next.output_height) {
			next.width = std::max(desc.width, next.width);
			next.height = std::max(desc.height, next.height);
		}
		if (desc == next) {
			return ready;
		}
		auto& scheduler = renderer.GetCommandScheduler();
		scheduler.Wait(*std::max_element(ticks.begin(), ticks.end()));
		if (ready) {
			ffxFsr3ContextDestroy(context.get());
			ready = false;
		}
		desc = next;
		frame_id = 0;
		reset = true;
		ticks.fill(0);

		const size_t count = interpolate ? FFX_FSR3_CONTEXT_COUNT : FFX_FSR3UPSCALER_CONTEXT_COUNT;
		scratch.assign(ffxGetScratchMemorySizeDX12(count), std::byte {});
		context = std::make_unique<FfxFsr3Context>();
		FfxInterface backend {};
		FfxFsr3ContextDescription parameters {};
		parameters.flags = desc.flags;
		parameters.maxRenderSize = {desc.width, desc.height};
		parameters.maxUpscaleSize = {desc.output_width, desc.output_height};
		parameters.displaySize = parameters.maxUpscaleSize;
		parameters.backBufferFormat = ffxGetSurfaceFormatDX12(desc.format);
		auto result = ffxGetInterfaceDX12(&backend, ffxGetDeviceDX12(renderer.GetGraphics().device),
		                                 scratch.data(), scratch.size(), count);
		if (result == FFX_OK) {
			parameters.backendInterfaceUpscaling = backend;
			parameters.backendInterfaceSharedResources = backend;
			parameters.backendInterfaceFrameInterpolation = backend;
			result = ffxFsr3ContextCreate(context.get(), &parameters);
			ready = result == FFX_OK;
		}
		if (ready && interpolate) {
			// The UWP backend leaves swapchain configuration to Kyty.
			FfxFrameGenerationConfig config {};
			config.frameGenerationEnabled = true;
			result = ffxFsr3ConfigureFrameGeneration(context.get(), &config);
		}
		// (in the emulator log: LOGF reaches only the console, which nobody sees on the Xbox)
		if (result != FFX_OK) {
			Log::WriteToConsoleAndLog(fmt::format("FSR: {} could not start (FidelityFX error 0x{:08x})\n", interpolate ? "frame generation" : "upscaling",
			                                      static_cast<unsigned>(result)));
		} else {
			Log::WriteToConsoleAndLog(fmt::format("FSR: {} {}x{} to {}x{}\n", interpolate ? "frame generation for" : "upscaling", desc.width, desc.height,
			                                      desc.output_width, desc.output_height));
		}
		return result == FFX_OK;
	}

	void Begin(RenderContext& renderer) {
		auto& scheduler = renderer.GetCommandScheduler();
		auto& tick = ticks[frame_id++ % ticks.size()];
		scheduler.Wait(tick);
		tick = scheduler.CurrentTick();
	}
};

float Elapsed(Clock::time_point& previous) {
	const auto now = Clock::now();
	const auto elapsed = std::chrono::duration<float, std::milli>(now - previous).count();
	previous = now;
	return std::clamp(elapsed, 1.0f, 100.0f);
}

// The guest PSSR packet carries normalized depth, with no projection matrix or clip planes.
constexpr float CameraNear = 0.1f;
constexpr float CameraFar = 1000.0f;
constexpr float CameraFov = 1.0f;

} // namespace

struct FidelityFx::Impl {
	explicit Impl(RenderContext& owner): renderer(owner) {}

	RenderContext& renderer;
	Effect upscale;
	Effect interpolate;
	std::array<Texture, 4> inputs;
	Texture output;
	Pssr::Request request {};
	uint32_t output_width = 0;
	uint32_t output_height = 0;
	uint32_t requests_since_flip = 0;
	uint64_t previous_upscale_owner = 0;
	uint64_t previous_interpolate_owner = 0;
	bool pending = false;
	bool has_color = false;
	bool has_exposure = false;
	Clock::time_point upscale_time = Clock::now();
	Clock::time_point interpolate_time = Clock::now();

	uint32_t DepthFlags() const {
		return (request.flags & Pssr::kReverseDepth) ? FFX_FSR3_ENABLE_DEPTH_INVERTED : 0;
	}

	FfxResource ReadInput(size_t slot) const {
		return Wrap(inputs[slot].image.Get(), FFX_RESOURCE_STATE_COMPUTE_READ);
	}
};

FidelityFx::FidelityFx(RenderContext& renderer): m_impl(std::make_unique<Impl>(renderer)) {}
FidelityFx::~FidelityFx() = default;

bool FidelityFx::BeginPssr(const Pssr::Request& request, uint32_t width, uint32_t height) {
	auto& fx = *m_impl;
	fx.requests_since_flip = std::min(2u, fx.requests_since_flip + 1);
	fx.pending = false;
	fx.has_color = false;
	fx.has_exposure = false;
	fx.request = request;
	fx.output_width = width;
	fx.output_height = height;
	if ((request.flags & Pssr::kSpatialOnly) ||
	    request.width > width || request.height > height || width > 3840 || height > 2160 ||
	    !std::isfinite(request.jitter[0]) || !std::isfinite(request.jitter[1]) ||
	    !std::isfinite(request.motion_scale[0]) || !std::isfinite(request.motion_scale[1])) {
		fx.upscale.reset = true;
		return false;
	}
	return true;
}

bool FidelityFx::CopyInput(CommandBuffer& command, Input input, D3D12_CPU_DESCRIPTOR_HANDLE source,
                           uint32_t width, uint32_t height, uint32_t x, uint32_t y) {
	auto& fx = *m_impl;
	constexpr std::array formats {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
	                              DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT};
	const auto slot = static_cast<size_t>(input);
	auto& texture = fx.inputs[slot];
	if (!texture.Ensure(fx.renderer, formats[slot], width, height)) {
		fx.upscale.reset = true;
		return false;
	}
	auto& kernels = fx.renderer.GetComputeKernels();
	const uint32_t constants[] {width, height, x, y};
	kernels.DispatchImages(command, kernels.GetImageKernel(GPU_PSSR_INPUT_SPV), source, texture.uav, constants,
	                       (width + 7) / 8, (height + 7) / 8, 1);
	if (input == Input::Color) {
		fx.has_color = true;
	}
	if (input == Input::Motion) {
		fx.pending = true;
	}
	if (input == Input::Exposure) {
		fx.has_exposure = true;
	}
	return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE FidelityFx::Upscale(CommandBuffer& command) {
	auto& fx = *m_impl;
	auto& effect = fx.upscale;
	if (!fx.pending || !fx.has_color || !fx.output.Ensure(fx.renderer, DXGI_FORMAT_R16G16B16A16_FLOAT, fx.output_width, fx.output_height, true)) {
		effect.reset = true;
		return {};
	}
	const uint32_t flags = FFX_FSR3_ENABLE_UPSCALING_ONLY | FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE |
	                       FFX_FSR3_ENABLE_DYNAMIC_RESOLUTION | fx.DepthFlags() |
	                       (!fx.has_exposure || (fx.request.flags & Pssr::kAutoExposure) ? FFX_FSR3_ENABLE_AUTO_EXPOSURE : 0);
	if (!effect.Ensure(fx.renderer, {fx.request.width, fx.request.height, fx.output_width, fx.output_height, flags,
	                                DXGI_FORMAT_R16G16B16A16_FLOAT}, false)) {
		return {};
	}
	effect.Begin(fx.renderer);
	auto* list = command.Handle();
	for (size_t i = 0; i < (fx.has_exposure ? 4u : 3u); ++i) {
		Transition(list, fx.inputs[i].image.Get(), UnorderedAccess, ComputeRead);
	}
	FfxFsr3DispatchUpscaleDescription params {};
	params.commandList = ffxGetCommandListDX12(list);
	params.color = fx.ReadInput(0);
	params.depth = fx.ReadInput(1);
	params.motionVectors = fx.ReadInput(2);
	if (fx.has_exposure) {
		params.exposure = fx.ReadInput(3);
	}
	params.upscaleOutput = Wrap(fx.output.image.Get(), FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	params.jitterOffset = {fx.request.jitter[0], fx.request.jitter[1]};
	params.motionVectorScale = {fx.request.motion_scale[0], fx.request.motion_scale[1]};
	params.renderSize = {fx.request.width, fx.request.height};
	params.upscaleSize = {fx.output_width, fx.output_height};
	params.frameTimeDelta = Elapsed(fx.upscale_time);
	params.preExposure = std::isfinite(fx.request.pre_exposure) && fx.request.pre_exposure > 0 ? fx.request.pre_exposure : 1.0f;
	params.reset = effect.reset || fx.previous_upscale_owner != fx.request.context || (fx.request.flags & Pssr::kReset);
	params.cameraNear = CameraNear;
	params.cameraFar = CameraFar;
	params.cameraFovAngleVertical = CameraFov;
	params.viewSpaceToMetersFactor = 1.0f;
	params.frameID = effect.frame_id;
	const auto result = ffxFsr3ContextDispatchUpscale(effect.context.get(), &params);
	fx.renderer.GetDescriptorHeap().InvalidateBinding();
	for (size_t i = 0; i < (fx.has_exposure ? 4u : 3u); ++i) {
		Transition(list, fx.inputs[i].image.Get(), ComputeRead, UnorderedAccess);
	}
	effect.reset = result != FFX_OK;
	fx.previous_upscale_owner = fx.request.context;
	if (effect.reset) {
		Log::WriteToConsoleAndLog(fmt::format("FSR: an upscaling dispatch failed (FidelityFX error 0x{:08x})\n", static_cast<unsigned>(result)));
		return {};
	}
	Transition(list, fx.output.image.Get(), UnorderedAccess, ComputeRead);
	return fx.output.srv;
}

void FidelityFx::FinishUpscale(CommandBuffer& command) {
	Transition(command.Handle(), m_impl->output.image.Get(), ComputeRead, UnorderedAccess);
}

void FidelityFx::SkipFlip() {
	auto& fx = *m_impl;
	fx.requests_since_flip = 0;
	fx.pending = false;
	fx.interpolate.reset = true;
}

bool FidelityFx::Interpolate(CommandBuffer& command, ID3D12Resource* color, DXGI_FORMAT format, bool hdr,
                             D3D12::ComPtr<ID3D12Resource>& generated) {
	auto& fx = *m_impl;
	auto& effect = fx.interpolate;
	const auto desc = color->GetDesc();
	if (!fx.pending || fx.requests_since_flip != 1 || hdr ||
	    (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
	    desc.Width != fx.output_width || desc.Height != fx.output_height) {
		SkipFlip();
		return false;
	}
	fx.requests_since_flip = 0;
	fx.pending = false;
	const auto width = fx.output_width;
	const auto height = fx.output_height;
	if (!generated || generated->GetDesc().Width != width || generated->GetDesc().Height != height) {
		generated.Reset();
		if (!CreateTexture(fx.renderer.GetGraphics().device, GeneratedFormat, width, height, generated)) {
			effect.reset = true;
			return false;
		}
	}
	const uint32_t flags = FFX_FSR3_ENABLE_INTERPOLATION_ONLY | fx.DepthFlags();
	if (!effect.Ensure(fx.renderer, {fx.request.width, fx.request.height, width, height, flags, format}, true)) {
		return false;
	}
	effect.Begin(fx.renderer);
	auto* list = command.Handle();
	Transition(list, color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, ComputeRead);
	for (size_t i: {1, 2}) {
		Transition(list, fx.inputs[i].image.Get(), UnorderedAccess, ComputeRead);
	}
	FfxFsr3DispatchFrameGenerationPrepareDescription prepare {};
	prepare.commandList = ffxGetCommandListDX12(list);
	prepare.depth = fx.ReadInput(1);
	prepare.motionVectors = fx.ReadInput(2);
	prepare.jitterOffset = {fx.request.jitter[0], fx.request.jitter[1]};
	prepare.motionVectorScale = {fx.request.motion_scale[0], fx.request.motion_scale[1]};
	prepare.renderSize = {fx.request.width, fx.request.height};
	prepare.frameTimeDelta = Elapsed(fx.interpolate_time);
	prepare.cameraNear = CameraNear;
	prepare.cameraFar = CameraFar;
	prepare.cameraFovAngleVertical = CameraFov;
	prepare.viewSpaceToMetersFactor = 1.0f;
	prepare.frameID = effect.frame_id;
	FfxFrameGenerationDispatchDescription interpolate {};
	interpolate.commandList = prepare.commandList;
	interpolate.presentColor = Wrap(color, FFX_RESOURCE_STATE_COMPUTE_READ, format);
	interpolate.outputs[0] = Wrap(generated.Get(), FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	interpolate.numInterpolatedFrames = 1;
	interpolate.reset = effect.reset || fx.previous_interpolate_owner != fx.request.context || (fx.request.flags & Pssr::kReset);
	interpolate.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
	interpolate.minMaxLuminance[1] = 100.0f;
	interpolate.interpolationRect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
	interpolate.frameID = prepare.frameID;
	auto result = ffxFsr3ContextDispatchFrameGenerationPrepare(effect.context.get(), &prepare);
	if (result == FFX_OK) {
		result = ffxFsr3DispatchFrameGeneration(&interpolate);
	}
	fx.renderer.GetDescriptorHeap().InvalidateBinding();
	Transition(list, color, ComputeRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	for (size_t i: {1, 2}) {
		Transition(list, fx.inputs[i].image.Get(), ComputeRead, UnorderedAccess);
	}
	effect.reset = result != FFX_OK;
	fx.previous_interpolate_owner = fx.request.context;
	if (effect.reset) {
		Log::WriteToConsoleAndLog(fmt::format("FSR: a frame generation dispatch failed (FidelityFX error 0x{:08x})\n", static_cast<unsigned>(result)));
	}
	return !interpolate.reset && !effect.reset;
}

} // namespace Libs::Graphics

#endif
