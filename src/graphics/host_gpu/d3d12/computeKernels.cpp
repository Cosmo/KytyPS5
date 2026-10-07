#include "graphics/host_gpu/d3d12/computeKernels.h"

#include "common/assert.h"
#include "graphics/host_gpu/d3d12/buffer.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/d3d12/render.h"

namespace Libs::Graphics {

namespace {

enum RootParameter : UINT { Input, Output, Params, Constants, RuntimeData, Count };

} // namespace

ComputeKernels::ComputeKernels(GraphicContext& graphics, const D3D12::DxilCompiler& compiler)
    : m_graphics(graphics), m_compiler(compiler) {
	D3D12_ROOT_PARAMETER1 parameters[RootParameter::Count] {};
	parameters[Input].ParameterType              = D3D12_ROOT_PARAMETER_TYPE_SRV;
	parameters[Input].Descriptor.ShaderRegister  = 0;
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
	D3D12::Check(graphics.device->CreateRootSignature(0, blob->GetBufferPointer(),
	                                                  blob->GetBufferSize(),
	                                                  IID_PPV_ARGS(&m_root_signature)),
	             "create kernel root signature");
}

ComputeKernels::~ComputeKernels() {
	for (auto& [key, pipeline]: m_pipelines) {
		pipeline->Release();
	}
	if (m_root_signature != nullptr) {
		m_root_signature->Release();
	}
}

ID3D12PipelineState* ComputeKernels::Get(std::span<const uint32_t>                       spirv,
                                          std::span<const D3D12::SpecializationConstant> constants) {
	Key key {spirv.data(), {}};
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
	desc.pRootSignature     = m_root_signature;
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
		bindings.input->Use(command, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}
	bindings.output->Use(command, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (bindings.params != nullptr) {
		bindings.params->Use(command, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	}
	list->SetComputeRootSignature(m_root_signature);
	list->SetPipelineState(pipeline);
	if (bindings.input != nullptr) {
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

	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = bindings.output->Resource();
	list->ResourceBarrier(1, &barrier);
}

} // namespace Libs::Graphics
