#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMPUTEKERNELS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMPUTEKERNELS_H_

#include "common/common.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace Libs::Graphics {

class Buffer;
class CommandBuffer;
struct GraphicContext;

// Compute shaders the renderer runs for itself (tiling, format conversion, row copies). They are
// the GLSL shaders under host_gpu/shaders, compiled to SPIR-V at build time and to DXIL once at
// first use. All share one root signature matching their SPIR-V bindings:
//   set 0 binding 0: read-only storage buffer (t0), binding 1: storage buffer (u1),
//   binding 2: uniform buffer (b2), push constants: root constants (b0, PushConstantSpace),
//   plus spirv_to_dxil's runtime data (b0, RuntimeDataSpace).
class ComputeKernels {
public:
	ComputeKernels(GraphicContext& graphics, const D3D12::DxilCompiler& compiler);
	~ComputeKernels();
	KYTY_CLASS_NO_COPY(ComputeKernels);

	static constexpr uint32_t MaxConstants = 16;

	struct Bindings {
		const Buffer*             input         = nullptr;
		uint64_t                  input_offset  = 0;
		const Buffer*             output        = nullptr;
		uint64_t                  output_offset = 0;
		const Buffer*             params        = nullptr; // constant buffer, 256-byte aligned
		uint64_t                  params_offset = 0;
		std::span<const uint32_t> constants;
	};

	[[nodiscard]] ID3D12PipelineState*
	Get(std::span<const uint32_t> spirv, std::span<const D3D12::SpecializationConstant> constants = {});
	// Records one dispatch; the output range is visible to later reads afterwards.
	void Dispatch(CommandBuffer& command, ID3D12PipelineState* pipeline, const Bindings& bindings,
	              uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);

private:
	using Key = std::pair<const uint32_t*, std::vector<uint32_t>>;

	GraphicContext&                          m_graphics;
	const D3D12::DxilCompiler&               m_compiler;
	ID3D12RootSignature*                     m_root_signature = nullptr;
	std::map<Key, ID3D12PipelineState*>      m_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMPUTEKERNELS_H_
