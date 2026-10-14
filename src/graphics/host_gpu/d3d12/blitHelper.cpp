#include "graphics/host_gpu/d3d12/blitHelper.h"

#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_blit_color_to_ms_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "graphics/host_gpu/d3d12/commandScheduler.h"
#include "graphics/host_gpu/d3d12/descriptorHeap.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"
#include "graphics/host_gpu/d3d12/formats.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/d3d12/pipelineCache.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/renderer/image/image.h"

#include <cstring>

namespace Libs::Graphics {

BlitHelper::BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler): m_graphics(graphics), m_scheduler(scheduler) {}

BlitHelper::~BlitHelper() {
	for (auto& pipeline: m_pipelines) {
		pipeline.state->Release();
	}
	if (m_root_signature != nullptr) {
		m_root_signature->Release();
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
	// Each guest draw sets its root signature, pipeline, targets and dynamic state again (RenderExecutor), so nothing is left to restore.
}

} // namespace Libs::Graphics
