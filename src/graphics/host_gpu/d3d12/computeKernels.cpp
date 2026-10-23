#include "graphics/host_gpu/d3d12/computeKernels.h"

#include "common/assert.h"
#include "graphics/host_gpu/d3d12/buffer.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/descriptorHeap.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/d3d12/render.h"
#include "graphics/host_gpu/d3d12/renderContext.h"

namespace Libs::Graphics {

namespace {

enum RootParameter : UINT { Input, Output, Params, Constants, RuntimeData, Count };
// An image kernel's: the sampled and the storage image's views in one table.
enum ImageRootParameter : UINT { ImageViews, ImageConstants, ImageRuntimeData, ImageCount };
// Image kernels' pipelines in m_pipelines (buffer kernels' have writable_input there).
constexpr uint32_t ImageKernelKey = 2;

} // namespace

ComputeKernels::ComputeKernels(GraphicContext& graphics, const D3D12::DxilCompiler& compiler)
    : m_graphics(graphics), m_compiler(compiler) {
	CreateRootSignature(false);
	CreateRootSignature(true);
}

void ComputeKernels::CreateRootSignature(bool writable_input) {
	D3D12_ROOT_PARAMETER1 parameters[RootParameter::Count] {};
	parameters[Input].ParameterType              = writable_input ? D3D12_ROOT_PARAMETER_TYPE_UAV
	                                                              : D3D12_ROOT_PARAMETER_TYPE_SRV;
	parameters[Input].Descriptor.ShaderRegister  = 0;
	if (writable_input) {
		parameters[Input].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
	}
	parameters[Output].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
	parameters[Output].Descriptor.ShaderRegister = 1;
	parameters[Output].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
	parameters[Params].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[Params].Descriptor.ShaderRegister = 2;
	parameters[Constants].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[Constants].Constants.Num32BitValues = MaxConstants;
	parameters[Constants].Constants.RegisterSpace  = D3D12::PushConstantSpace;
	parameters[RuntimeData].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[RuntimeData].Constants.Num32BitValues = D3D12::RuntimeDataDwords;
	parameters[RuntimeData].Constants.RegisterSpace  = D3D12::RuntimeDataSpace;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
	desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = RootParameter::Count;
	desc.Desc_1_1.pParameters   = parameters;
	D3D12::ComPtr<ID3DBlob> blob;
	D3D12::ComPtr<ID3DBlob> error;
	if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
		EXIT("D3D12: kernel root signature serialization failed: %s\n",
		     error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
	}
	D3D12::Check(m_graphics.device->CreateRootSignature(0, blob->GetBufferPointer(),
	                                                    blob->GetBufferSize(),
	                                                    IID_PPV_ARGS(&m_root_signatures[writable_input ? 1 : 0])),
	             "create kernel root signature");
}

ComputeKernels::~ComputeKernels() {
	for (auto& [key, pipeline]: m_pipelines) {
		pipeline->Release();
	}
	for (auto* root_signature: m_root_signatures) {
		if (root_signature != nullptr) {
			root_signature->Release();
		}
	}
	if (m_image_root_signature != nullptr) {
		m_image_root_signature->Release();
	}
}

void ComputeKernels::CreateImageRootSignature() {
	D3D12_DESCRIPTOR_RANGE1 ranges[2] {};
	ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors                    = 1;
	ranges[0].BaseShaderRegister                = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors                    = 1;
	ranges[1].BaseShaderRegister                = 1;
	ranges[1].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
	ranges[1].OffsetInDescriptorsFromTableStart = 1;
	D3D12_ROOT_PARAMETER1 parameters[ImageRootParameter::ImageCount] {};
	parameters[ImageViews].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[ImageViews].DescriptorTable.NumDescriptorRanges = 2;
	parameters[ImageViews].DescriptorTable.pDescriptorRanges   = ranges;
	parameters[ImageConstants].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[ImageConstants].Constants.Num32BitValues = MaxConstants;
	parameters[ImageConstants].Constants.RegisterSpace  = D3D12::PushConstantSpace;
	parameters[ImageRuntimeData].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[ImageRuntimeData].Constants.Num32BitValues = D3D12::RuntimeDataDwords;
	parameters[ImageRuntimeData].Constants.RegisterSpace  = D3D12::RuntimeDataSpace;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
	desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = ImageRootParameter::ImageCount;
	desc.Desc_1_1.pParameters   = parameters;
	D3D12::ComPtr<ID3DBlob> blob;
	D3D12::ComPtr<ID3DBlob> error;
	if (FAILED(D3D12SerializeVersionedRootSignature(&desc, &blob, &error))) {
		EXIT("D3D12: image kernel root signature serialization failed: %s\n",
		     error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "");
	}
	D3D12::Check(m_graphics.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
	                                                    IID_PPV_ARGS(&m_image_root_signature)),
	             "create image kernel root signature");
}

ID3D12PipelineState* ComputeKernels::GetImageKernel(std::span<const uint32_t> spirv) {
	auto [found, inserted] = m_pipelines.try_emplace(Key {spirv.data(), {ImageKernelKey}}, nullptr);
	if (!inserted) {
		return found->second;
	}
	if (m_image_root_signature == nullptr) {
		CreateImageRootSignature();
	}
	const auto shader = m_compiler.Compile(spirv, ShaderType::Compute, 0, false, {});
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
	desc.pRootSignature     = m_image_root_signature;
	desc.CS.pShaderBytecode = shader.bytecode.data();
	desc.CS.BytecodeLength  = shader.bytecode.size();
	D3D12::Check(m_graphics.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&found->second)),
	             "create image kernel pipeline");
	return found->second;
}

void ComputeKernels::DispatchImages(CommandBuffer& command, ID3D12PipelineState* pipeline, D3D12_CPU_DESCRIPTOR_HANDLE input,
                                    D3D12_CPU_DESCRIPTOR_HANDLE output, std::span<const uint32_t> constants, uint32_t groups_x,
                                    uint32_t groups_y, uint32_t groups_z) {
	EXIT_IF(m_image_root_signature == nullptr || constants.size() > MaxConstants);
	// (Allocating binds the shader-visible heaps to the list.)
	const auto views = command.GetContext().GetDescriptorHeap().AllocateViews(command, 2);
	m_graphics.device->CopyDescriptorsSimple(1, views.Cpu(0), input, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_graphics.device->CopyDescriptorsSimple(1, views.Cpu(1), output, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	auto* list = command.Handle();
	list->SetComputeRootSignature(m_image_root_signature);
	list->SetPipelineState(pipeline);
	list->SetComputeRootDescriptorTable(ImageViews, views.Gpu(0));
	if (!constants.empty()) {
		list->SetComputeRoot32BitConstants(ImageConstants, static_cast<UINT>(constants.size()), constants.data(), 0);
	}
	const uint32_t runtime[D3D12::RuntimeDataDwords] = {groups_x, groups_y, groups_z};
	list->SetComputeRoot32BitConstants(ImageRuntimeData, D3D12::RuntimeDataDwords, runtime, 0);
	list->Dispatch(groups_x, groups_y, groups_z);

	// (The view names no resource here: a barrier for all unordered access.)
	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	list->ResourceBarrier(1, &barrier);
}

ID3D12PipelineState* ComputeKernels::Get(std::span<const uint32_t>                       spirv,
                                          std::span<const D3D12::SpecializationConstant> constants,
                                          bool writable_input) {
	Key key {spirv.data(), {writable_input ? 1u : 0u}};
	for (const auto& constant: constants) {
		key.second.push_back(constant.id);
		key.second.push_back(constant.value);
	}
	auto [found, inserted] = m_pipelines.try_emplace(std::move(key), nullptr);
	if (!inserted) {
		return found->second;
	}
	const auto shader = m_compiler.Compile(spirv, ShaderType::Compute, 0, false, constants);
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
	desc.pRootSignature     = m_root_signatures[writable_input ? 1 : 0];
	desc.CS.pShaderBytecode = shader.bytecode.data();
	desc.CS.BytecodeLength  = shader.bytecode.size();
	D3D12::Check(m_graphics.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&found->second)),
	             "create kernel pipeline");
	return found->second;
}

void ComputeKernels::Dispatch(CommandBuffer& command, ID3D12PipelineState* pipeline,
                              const Bindings& bindings, uint32_t groups_x, uint32_t groups_y,
                              uint32_t groups_z) {
	EXIT_IF(bindings.output == nullptr || bindings.input == bindings.output ||
	        bindings.constants.size() > MaxConstants);
	auto* list = command.Handle();
	if (bindings.input != nullptr) {
		bindings.input->Use(command, bindings.input_writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
		                                                     : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	bindings.output->Use(command, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (bindings.params != nullptr) {
		bindings.params->Use(command, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	}
	list->SetComputeRootSignature(m_root_signatures[bindings.input_writable ? 1 : 0]);
	list->SetPipelineState(pipeline);
	if (bindings.input != nullptr && bindings.input_writable) {
		list->SetComputeRootUnorderedAccessView(
		    Input, bindings.input->GpuAddress() + bindings.input_offset);
	} else if (bindings.input != nullptr) {
		list->SetComputeRootShaderResourceView(
		    Input, bindings.input->GpuAddress() + bindings.input_offset);
	}
	list->SetComputeRootUnorderedAccessView(
	    Output, bindings.output->GpuAddress() + bindings.output_offset);
	if (bindings.params != nullptr) {
		list->SetComputeRootConstantBufferView(
		    Params, bindings.params->GpuAddress() + bindings.params_offset);
	}
	if (!bindings.constants.empty()) {
		list->SetComputeRoot32BitConstants(Constants, static_cast<UINT>(bindings.constants.size()),
		                                   bindings.constants.data(), 0);
	}
	// spirv_to_dxil's compute runtime data: group counts, padding, base group.
	const uint32_t runtime[D3D12::RuntimeDataDwords] = {groups_x, groups_y, groups_z};
	list->SetComputeRoot32BitConstants(RuntimeData, D3D12::RuntimeDataDwords, runtime, 0);
	list->Dispatch(groups_x, groups_y, groups_z);

	D3D12_RESOURCE_BARRIER barriers[2] {};
	barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barriers[0].UAV.pResource = bindings.output->Resource();
	barriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barriers[1].UAV.pResource = bindings.input_writable ? bindings.input->Resource() : nullptr;
	list->ResourceBarrier(bindings.input_writable ? 2 : 1, barriers);
}

} // namespace Libs::Graphics
