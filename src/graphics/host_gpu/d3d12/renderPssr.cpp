#include "common/assert.h"
#include "common/threads.h"
#include "gpu_blit_shaders/gpu_pssr_linear_spv.h"
#include "graphics/guest_gpu/pssr.h"
#include "graphics/host_gpu/d3d12/computeKernels.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/render.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <cstring>

// Upstream's linear PSSR (renderer/renderPssr.cpp) on D3D12: the same checks and the same kernel (gpu_pssr_linear.comp), run as one of
// the renderer's image kernels.

namespace Libs::Graphics {
namespace {

struct PssrParameters {
	uint32_t input_size[2];
	uint32_t output_size[2];
	uint32_t output_channels[4];
};

} // namespace

void RenderExecutor::ProcessPssr(CommandBuffer& buffer, const Pssr::Request& request) {
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopPendingOperations();
	Common::LockGuard     lock(m_context.GetMutex());
	ShaderTextureResource input_descriptor {}, output_descriptor {};
	std::copy_n(request.input, 8, input_descriptor.fields);
	std::copy_n(request.output, 8, output_descriptor.fields);
	const auto is_2d = [](const ShaderTextureResource& descriptor) {
		return !descriptor.IsNull() &&
		       (descriptor.Type() == Prospero::ImageType::kColor2D || descriptor.Type() == Prospero::ImageType::kColor2DArray) &&
		       descriptor.TileMode() != Prospero::TileMode::kDepth;
	};
	EXIT_IF(!is_2d(input_descriptor) || !is_2d(output_descriptor) || !Pssr::IsColorFormat(input_descriptor.Format()) ||
	        output_descriptor.Format() != Prospero::BufferFormat::k11_11_10Float ||
	        output_descriptor.TileMode() != Prospero::TileMode::kStandard256B || output_descriptor.BaseLevel() != 0 ||
	        output_descriptor.LastLevel() != 0 || output_descriptor.MaxMip() != 0 || output_descriptor.BaseArray5() != 0 ||
	        output_descriptor.Depth() != 0 || request.width == 0 || request.height == 0);
	const auto input_width  = std::max(1u, (input_descriptor.Width5() + 1u) >> input_descriptor.BaseLevel());
	const auto input_height = std::max(1u, (input_descriptor.Height5() + 1u) >> input_descriptor.BaseLevel());
	EXIT_IF(request.width > input_width || request.height > input_height);
	PssrParameters parameters {{request.width, request.height}, {output_descriptor.Width5() + 1u, output_descriptor.Height5() + 1u}, {4, 4, 4, 4}};
	for (uint32_t component = 0; component < 3; ++component) {
		for (uint32_t source = 0; source < 4; ++source) {
			if (GetDstSel(output_descriptor.DstSelXYZW(), source) == 4u + component) {
				parameters.output_channels[component] = source;
				break;
			}
		}
	}
	const auto resolve = [&](const uint32_t* words, bool output) {
		ShaderRecompiler::IR::ImageResource resource {};
		resource.resource_class = output ? ShaderRecompiler::IR::ImageResourceClass::Storage : ShaderRecompiler::IR::ImageResourceClass::Sampled;
		resource.numeric_class  = Prospero::TextureNumericClass::Float;
		resource.dimension      = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
		resource.read           = !output;
		resource.written        = output;
		ShaderRecompiler::IR::DescriptorValue value {};
		std::copy_n(words, 8, value.dwords.begin());
		value.dword_count                  = 8;
		auto binding                       = ResolveTexture(resource, value);
		binding.desc.view_info.level_count = 1;
		binding.desc.view_info.min_lod     = 0;
		return binding;
	};
	auto& cache = m_context.GetTextureCache();
	auto  input = resolve(request.input, false);
	BindImage(input.image_id, false);
	auto output = resolve(request.output, true);
	BindImage(output.image_id, true);
	EXIT_IF(input.desc.info.data.address < output.desc.info.data.End() && output.desc.info.data.address < input.desc.info.data.End());
	const auto acquire = [&](TextureBinding& binding, const uint32_t* words, bool storage) {
		auto& previous = cache.GetImage(binding.image_id);
		if (!previous.registered || previous.binding.needs_rebind) {
			previous.binding = {};
			binding          = resolve(words, storage);
			BindImage(binding.image_id, storage);
		}
		binding.image_view = cache.FindTexture(binding.image_id, binding.desc);
	};
	acquire(input, request.input, false);
	acquire(output, request.output, true);
	auto&       source           = cache.GetImage(input.image_id);
	auto&       destination      = cache.GetImage(output.image_id);
	const auto& source_view      = input.desc.view_info;
	const auto& destination_view = output.desc.view_info;
	EXIT_IF(source.info.IsDepth() || destination.info.IsDepth() || source.backing.samples != 1 || destination.backing.samples != 1 ||
	        (input.image_id == output.image_id && source_view.base_level == destination_view.base_level &&
	         source_view.base_layer == destination_view.base_layer));

	auto&      kernels  = m_context.GetComputeKernels();
	auto*      pipeline = kernels.GetImageKernel(GPU_PSSR_LINEAR_SPV);
	source.Use(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ImageSubresourceRange {source_view.base_level, 1, source_view.base_layer, 1},
	           input.image_view);
	destination.Use(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	                ImageSubresourceRange {destination_view.base_level, 1, destination_view.base_layer, 1}, output.image_view);
	uint32_t constants[sizeof(PssrParameters) / sizeof(uint32_t)] {};
	std::memcpy(constants, &parameters, sizeof(parameters));
	kernels.DispatchImages(buffer, pipeline, source.ShaderResourceView(input.image_view), destination.UnorderedAccessView(output.image_view),
	                       constants, (parameters.output_size[0] + 7) / 8, (parameters.output_size[1] + 7) / 8, 1);
	ResetBindings();
}

} // namespace Libs::Graphics
