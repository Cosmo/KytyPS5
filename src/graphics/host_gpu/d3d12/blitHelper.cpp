#include "graphics/host_gpu/d3d12/blitHelper.h"

#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_blit_color_to_ms_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_clear_selected_float_spv.h"
#include "gpu_blit_shaders/gpu_clear_selected_sint_spv.h"
#include "gpu_blit_shaders/gpu_clear_selected_uint_spv.h"
#include "graphics/host_gpu/d3d12/buffer.h"
#include "graphics/host_gpu/d3d12/commandScheduler.h"
#include "graphics/host_gpu/d3d12/descriptorHeap.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/d3d12/pipelineCache.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

BlitHelper::BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler): m_graphics(graphics), m_scheduler(scheduler) {}

BlitHelper::~BlitHelper() {
	for (auto& pipeline: m_pipelines) {
		pipeline.state->Release();
	}
	if (m_root_signature != nullptr) {
		m_root_signature->Release();
	}
	for (auto& pipeline: m_clear_pipelines) {
		pipeline.state->Release();
	}
	if (m_clear_root_signature != nullptr) {
		m_clear_root_signature->Release();
	}
}

ID3D12PipelineState* BlitHelper::GetPipeline(uint32_t samples, uint32_t format) {
	for (const auto& pipeline: m_pipelines) {
		if (pipeline.samples == samples && pipeline.format == format) {
			return pipeline.state;
		}
	}
	if (m_root_signature == nullptr) {
		using Kind = D3D12::LinkedStage::Kind;
		// The vertex shader emits Vulkan clip space; its Y flip maps it to D3D12's (as the presenter's).
		const D3D12::LinkedStage stages[] {{GPU_BLIT_FS_TRIANGLE_SPV, Kind::Vertex, true}, {GPU_BLIT_COLOR_TO_MS_DEPTH_SPV, Kind::Pixel}};
		const auto               shaders = m_scheduler.Context().GetPipelineCache().GetCompiler().CompileLinked(stages, 0);
		EXIT_IF(shaders.size() != 2);
		if (!shaders[0].IsValid() || !shaders[1].IsValid()) {
			EXIT("D3D12: the shaders that reinterpret color as multisampled depth could not be translated\n");
		}
		m_vertex_shader = shaders[0].bytecode;
		m_pixel_shader  = shaders[1].bytecode;

		D3D12_DESCRIPTOR_RANGE1 range {};
		range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		D3D12_ROOT_PARAMETER1 parameters[2] {};
		parameters[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[0].Constants.Num32BitValues            = D3D12::RuntimeDataDwords;
		parameters[0].Constants.RegisterSpace             = D3D12::RuntimeDataSpace;
		parameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[1].DescriptorTable.NumDescriptorRanges = 1;
		parameters[1].DescriptorTable.pDescriptorRanges   = &range;
		parameters[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
		desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
		desc.Desc_1_1.NumParameters = 2;
		desc.Desc_1_1.pParameters   = parameters;
		D3D12::ComPtr<ID3DBlob> blob;
		D3D12::ComPtr<ID3DBlob> error;
		if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
			EXIT("D3D12: color-to-MS-depth root signature serialization failed: %s\n",
			     error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
		}
		D3D12::Check(m_graphics.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root_signature)),
		             "create the color-to-MS-depth root signature");
	}

	// As upstream's pipeline: no color targets, depth written with the test always passing, one invocation per sample.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
	desc.pRootSignature                    = m_root_signature;
	desc.VS                                = {m_vertex_shader.data(), m_vertex_shader.size()};
	desc.PS                                = {m_pixel_shader.data(), m_pixel_shader.size()};
	desc.SampleMask                        = UINT_MAX;
	desc.RasterizerState.FillMode          = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode          = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable   = TRUE;
	desc.DepthStencilState.DepthEnable     = TRUE;
	desc.DepthStencilState.DepthWriteMask  = D3D12_DEPTH_WRITE_MASK_ALL;
	desc.DepthStencilState.DepthFunc       = D3D12_COMPARISON_FUNC_ALWAYS;
	desc.DepthStencilState.FrontFace       = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
	desc.DepthStencilState.BackFace        = desc.DepthStencilState.FrontFace;
	desc.PrimitiveTopologyType             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.DSVFormat                         = static_cast<DXGI_FORMAT>(format);
	desc.SampleDesc.Count                  = samples;
	Pipeline pipeline {samples, format, nullptr};
	D3D12::Check(m_graphics.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline.state)), "create a color-to-MS-depth pipeline");
	m_pipelines.push_back(pipeline);
	return pipeline.state;
}

void BlitHelper::ReinterpretColorAsMsDepth(Image& source, Image& destination) {
	const auto& source_info      = source.info;
	const auto& destination_info = destination.info;
	EXIT_IF(DepthAspectTransferFormat(source_info.pixel_format) != vk::Format::eUndefined ||
	        DepthAspectTransferFormat(destination_info.pixel_format) == vk::Format::eUndefined ||
	        source_info.samples != 1 || destination_info.samples <= 1 ||
	        destination_info.samples > 4 || source.backing.image_type != vk::ImageType::e2D ||
	        destination.backing.image_type != vk::ImageType::e2D ||
	        source_info.extent.width != destination_info.extent.width ||
	        source_info.extent.height != destination_info.extent.height ||
	        source_info.extent.depth != 1 || destination_info.extent.depth != 1 ||
	        source.backing.image == nullptr || destination.backing.image == nullptr);

	ImageViewInfo source_view_info {};
	source_view_info.format = source_info.pixel_format;
	source_view_info.type   = vk::ImageViewType::e2D;
	source_view_info.aspect = vk::ImageAspectFlagBits::eColor;
	source_view_info.usage  = vk::ImageUsageFlagBits::eSampled;
	const auto source_view  = source.FindView(source_view_info);

	ImageViewInfo destination_view_info {};
	destination_view_info.format = destination_info.pixel_format;
	destination_view_info.type   = vk::ImageViewType::e2D;
	destination_view_info.aspect = vk::ImageAspectFlagBits::eDepth;
	destination_view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	const auto destination_view  = destination.FindView(destination_view_info);

	auto* pipeline = GetPipeline(destination.backing.samples, static_cast<uint32_t>(D3D12::GetFormatInfo(destination.backing.format).depth_view));
	auto& heap     = m_scheduler.Context().GetDescriptorHeap();
	if (!heap.Fits(1, 0)) {
		m_scheduler.Flush();
	}
	auto& command = m_scheduler.Current();
	auto* list    = command.Handle();
	source.Use(command, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, ImageSubresourceRange {0, 1, 0, 1}, source_view);
	destination.Use(command, D3D12_RESOURCE_STATE_DEPTH_WRITE, ImageSubresourceRange {0, 1, 0, 1}, destination_view);

	const auto views = heap.AllocateViews(command, 1);
	m_graphics.device->CopyDescriptorsSimple(1, views.Cpu(0), source.ShaderResourceView(source_view), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const auto depth = destination.DepthStencilView(destination_view);

	D3D12::VertexRuntimeData runtime {};
	runtime.yz_flip_mask    = 1;
	runtime.viewport_width  = static_cast<float>(destination_info.extent.width);
	runtime.viewport_height = static_cast<float>(destination_info.extent.height);
	uint32_t constants[D3D12::RuntimeDataDwords] {};
	std::memcpy(constants, &runtime, sizeof(runtime));

	// Upstream clears the depth to 0 before the draw (its attachment's load operation).
	list->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
	list->SetGraphicsRootSignature(m_root_signature);
	list->SetPipelineState(pipeline);
	list->SetGraphicsRoot32BitConstants(0, D3D12::RuntimeDataDwords, constants, 0);
	list->SetGraphicsRootDescriptorTable(1, views.Gpu(0));
	const D3D12_VIEWPORT viewport {0.0f, 0.0f, static_cast<float>(destination_info.extent.width),
	                               static_cast<float>(destination_info.extent.height), 0.0f, 1.0f};
	const D3D12_RECT     scissor {0, 0, static_cast<LONG>(destination_info.extent.width), static_cast<LONG>(destination_info.extent.height)};
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	list->OMSetRenderTargets(0, nullptr, FALSE, &depth);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->DrawInstanced(3, 1, 0, 0);
	// Each guest draw sets its root signature, pipeline and targets again (RenderExecutor), and its viewports and other dynamic
	// state when they differ from the list's: those are forgotten here.
	command.InvalidateDrawState();
}

// The clear shader's push constants (gpu_clear_selected.frag): the color as floats and as bits, and the predicate.
struct ClearConstants {
	float    color_float[4] {};
	uint32_t color_bits[4] {};
	uint32_t predicate = 0;
};
constexpr uint32_t ClearConstantDwords = sizeof(ClearConstants) / sizeof(uint32_t);

ID3D12PipelineState* BlitHelper::GetClearPipeline(uint32_t format, uint32_t kind) {
	for (const auto& pipeline: m_clear_pipelines) {
		if (pipeline.format == format && pipeline.kind == kind) {
			return pipeline.state;
		}
	}
	if (m_clear_vertex_shaders[kind].empty()) {
		using Kind = D3D12::LinkedStage::Kind;
		const std::span<const uint32_t> pixel[] {GPU_CLEAR_SELECTED_FLOAT_SPV, GPU_CLEAR_SELECTED_UINT_SPV, GPU_CLEAR_SELECTED_SINT_SPV};
		const D3D12::LinkedStage        stages[] {{GPU_BLIT_FS_TRIANGLE_SPV, Kind::Vertex, true}, {pixel[kind], Kind::Pixel}};
		const auto shaders = m_scheduler.Context().GetPipelineCache().GetCompiler().CompileLinked(stages, 0);
		EXIT_IF(shaders.size() != 2);
		if (!shaders[0].IsValid() || !shaders[1].IsValid()) {
			EXIT("D3D12: the selected clear's shaders could not be translated\n");
		}
		m_clear_vertex_shaders[kind] = shaders[0].bytecode;
		m_clear_pixel_shaders[kind]  = shaders[1].bytecode;
	}
	if (m_clear_root_signature == nullptr) {
		// The push constants, the vertex stage's runtime data, and the predicates as a uint texel buffer.
		D3D12_DESCRIPTOR_RANGE1 range {};
		range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		D3D12_ROOT_PARAMETER1 parameters[3] {};
		parameters[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[0].Constants.Num32BitValues            = ClearConstantDwords;
		parameters[0].Constants.RegisterSpace             = D3D12::PushConstantSpace;
		parameters[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
		parameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[1].Constants.Num32BitValues            = D3D12::RuntimeDataDwords;
		parameters[1].Constants.RegisterSpace             = D3D12::RuntimeDataSpace;
		parameters[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[2].DescriptorTable.NumDescriptorRanges = 1;
		parameters[2].DescriptorTable.pDescriptorRanges   = &range;
		parameters[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
		desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
		desc.Desc_1_1.NumParameters = 3;
		desc.Desc_1_1.pParameters   = parameters;
		D3D12::ComPtr<ID3DBlob> blob;
		D3D12::ComPtr<ID3DBlob> error;
		if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
			EXIT("D3D12: selected clear root signature serialization failed: %s\n",
			     error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
		}
		D3D12::Check(m_graphics.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_clear_root_signature)),
		             "create the selected clear root signature");
	}
	// As ClearRenderTargetView: every channel written, no blending, no depth.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
	desc.pRootSignature                                   = m_clear_root_signature;
	desc.VS                                               = {m_clear_vertex_shaders[kind].data(), m_clear_vertex_shaders[kind].size()};
	desc.PS                                               = {m_clear_pixel_shaders[kind].data(), m_clear_pixel_shaders[kind].size()};
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask                                       = UINT_MAX;
	desc.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable                  = TRUE;
	desc.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets                                 = 1;
	desc.RTVFormats[0]                                    = static_cast<DXGI_FORMAT>(format);
	desc.SampleDesc.Count                                 = 1;
	ClearPipeline pipeline {format, kind, nullptr};
	D3D12::Check(m_graphics.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline.state)), "create a selected clear pipeline");
	m_clear_pipelines.push_back(pipeline);
	return pipeline.state;
}

void BlitHelper::PrepareClearSelected() {
	// (Also room for the check's kernel, which runs in between.)
	if (!m_scheduler.Context().GetDescriptorHeap().Fits(16, 0)) {
		m_scheduler.Flush();
	}
}

void BlitHelper::ClearSelected(Image& image, vk::Format format, uint32_t level, uint32_t layer, uint32_t layers, uint32_t keys,
                               std::span<const vk::ClearAttachment> clears, const Buffer& predicates) {
	EXIT_IF(image.backing.samples != 1);
	const auto     numeric  = std::string_view(vk::componentNumericFormat(format, 0));
	const uint32_t kind     = numeric == "UINT" ? 1u : numeric == "SINT" ? 2u : 0u;
	auto*          pipeline = GetClearPipeline(static_cast<uint32_t>(D3D12::GetFormatInfo(format).view), kind);
	auto&          heap     = m_scheduler.Context().GetDescriptorHeap();
	EXIT_IF(!heap.Fits(1, 0)); // (PrepareClearSelected made room)
	auto& command = m_scheduler.Current();
	auto* list    = command.Handle();
	// (The check leaves its predicates for predication; here the pixel shader reads them.)
	predicates.Use(command, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	const auto                      views = heap.AllocateViews(command, 1);
	D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
	srv.Format                  = DXGI_FORMAT_R32_UINT;
	srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Buffer.NumElements      = static_cast<UINT>(predicates.Size() / sizeof(uint32_t));
	m_graphics.device->CreateShaderResourceView(predicates.Resource(), &srv, views.Cpu(0));

	const uint32_t           width  = std::max(image.info.extent.width >> level, 1u);
	const uint32_t           height = std::max(image.info.extent.height >> level, 1u);
	D3D12::VertexRuntimeData runtime {};
	runtime.yz_flip_mask    = 1;
	runtime.viewport_width  = static_cast<float>(width);
	runtime.viewport_height = static_cast<float>(height);
	uint32_t runtime_dwords[D3D12::RuntimeDataDwords] {};
	std::memcpy(runtime_dwords, &runtime, sizeof(runtime));
	const D3D12_VIEWPORT viewport {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
	const D3D12_RECT     scissor {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

	for (uint32_t slice = 0; slice < layers; slice++) {
		ImageViewInfo view {};
		view.format       = format;
		view.type         = vk::ImageViewType::e2DArray;
		view.aspect       = vk::ImageAspectFlagBits::eColor;
		view.base_level   = level;
		view.base_layer   = layer + slice;
		view.layer_count  = 1;
		const auto handle = image.FindView(view);
		image.Use(command, D3D12_RESOURCE_STATE_RENDER_TARGET, ImageSubresourceRange {level, 1, layer + slice, 1}, handle);
		const auto target = image.RenderTargetView(handle);
		EXIT_IF(target.ptr == 0);
		list->SetGraphicsRootSignature(m_clear_root_signature);
		list->SetPipelineState(pipeline);
		list->SetGraphicsRoot32BitConstants(1, D3D12::RuntimeDataDwords, runtime_dwords, 0);
		list->SetGraphicsRootDescriptorTable(2, views.Gpu(0));
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
		list->OMSetRenderTargets(1, &target, FALSE, nullptr);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (uint32_t key = 0; key < clears.size(); key++) {
			if ((keys & (1u << key)) == 0) {
				continue;
			}
			const auto&    clear = clears[key].clearValue.color;
			ClearConstants constants;
			for (int i = 0; i < 4; i++) {
				constants.color_float[i] = clear.float32[i];
				constants.color_bits[i]  = clear.uint32[i];
			}
			constants.predicate = slice * static_cast<uint32_t>(clears.size()) + key;
			list->SetGraphicsRoot32BitConstants(0, ClearConstantDwords, &constants, 0);
			list->DrawInstanced(3, 1, 0, 0);
		}
	}
	// As after ReinterpretColorAsMsDepth: the next guest draw sets its state again.
	command.InvalidateDrawState();
}

} // namespace Libs::Graphics
