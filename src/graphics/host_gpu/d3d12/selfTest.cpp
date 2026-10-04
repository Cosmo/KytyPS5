#include "graphics/host_gpu/d3d12/selfTest.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include "gpu_tiler_shaders/gpu_tiler_depth_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_render_target_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard256_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_swap_bgra16_spv.h"

#include <chrono>
#include <fmt/format.h>
#include <span>

namespace Libs::Graphics::D3D12 {

namespace {

void Say(const std::string& text) {
	Log::WriteToConsoleAndLog("D3D12 self test: " + text + "\n");
}

// The tiling shaders' resources as spirv_to_dxil lays them out: a read-only storage buffer (t0), a storage buffer (u1), a uniform buffer (b2), their push
// constants (b0 in PushConstantSpace) and the translator's runtime data (b0 in RuntimeDataSpace).
ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device) {
	D3D12_ROOT_PARAMETER1 parameters[5] {};
	parameters[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants.Num32BitValues = 16;
	parameters[0].Constants.RegisterSpace  = PushConstantSpace;
	parameters[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[1].Descriptor.RegisterSpace = RuntimeDataSpace;
	parameters[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_SRV;
	parameters[2].Descriptor.ShaderRegister = 0;
	parameters[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_UAV;
	parameters[3].Descriptor.ShaderRegister = 1;
	parameters[4].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[4].Descriptor.ShaderRegister = 2;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
	desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = 5;
	desc.Desc_1_1.pParameters   = parameters;

	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> error;
	Check(D3D12SerializeVersionedRootSignature(&desc, &blob, &error), "serialize root signature");
	ComPtr<ID3D12RootSignature> root_signature;
	Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature)), "CreateRootSignature");
	return root_signature;
}

struct Kernel {
	const char*               name;
	std::span<const uint32_t> spirv;
};

} // namespace

bool RunShaderSelfTest(bool debug_layer) {
	using Clock = std::chrono::steady_clock;
	const auto milliseconds = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };

	GraphicContext graphics;
	graphics.Create(debug_layer);
	Say(fmt::format("device {}", graphics.DeviceName()));

	bool ok = true;
	{
		const auto start = Clock::now();
		DxilCompiler compiler(graphics.device);
		Say(fmt::format("DXIL validator loaded in {:.1f} ms", milliseconds(start)));

		const auto root_signature = CreateRootSignature(graphics.device);
		const Kernel kernels[] = {
		    {"swap_bgra16", GPU_TILER_SWAP_BGRA16_SPV}, {"standard4", GPU_TILER_STANDARD4_SPV}, {"standard64", GPU_TILER_STANDARD64_SPV},
		    {"standard256", GPU_TILER_STANDARD256_SPV}, {"depth", GPU_TILER_DEPTH_SPV},         {"render_target", GPU_TILER_RENDER_TARGET_SPV},
		    {"prt", GPU_TILER_PRT_SPV},
		};
		for (const auto& kernel: kernels) {
			const auto begin  = Clock::now();
			const auto dxil   = compiler.Compile(kernel.spirv, ShaderType::Compute, 0, false);
			const auto signed_ms = milliseconds(begin);

			D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
			desc.pRootSignature     = root_signature.Get();
			desc.CS.pShaderBytecode = dxil.bytecode.data();
			desc.CS.BytecodeLength  = dxil.bytecode.size();
			ComPtr<ID3D12PipelineState> pipeline;
			const auto                  made   = Clock::now();
			const HRESULT               result = graphics.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
			if (FAILED(result)) {
				ok = false;
				Say(fmt::format("{}: translated and signed ({} bytes of SPIR-V, {} bytes of DXIL, {:.1f} ms) but CreateComputePipelineState failed: 0x{:08x}",
				                kernel.name, kernel.spirv.size_bytes(), dxil.bytecode.size(), signed_ms, static_cast<uint32_t>(result)));
				continue;
			}
			Say(fmt::format("{}: {} bytes of SPIR-V, {} bytes of DXIL, translated and signed in {:.1f} ms, pipeline in {:.1f} ms", kernel.name,
			                kernel.spirv.size_bytes(), dxil.bytecode.size(), signed_ms, milliseconds(made)));
		}
	}
	graphics.Destroy();
	Say(ok ? "passed" : "FAILED");
	return ok;
}

} // namespace Libs::Graphics::D3D12
