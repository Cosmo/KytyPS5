#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/d3d12/gpuTrace.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace IR = ShaderRecompiler::IR;

namespace {

// A raw view of `binding`: a UAV, or an SRV used in `read_state` for buffers the shader only
// reads.
void WriteRawView(ID3D12Device* device, const BufferBinding& binding,
                  D3D12_CPU_DESCRIPTOR_HANDLE destination, CommandBuffer& command,
                  std::optional<D3D12_RESOURCE_STATES> read_state = {}) {
	EXIT_IF(binding.buffer == nullptr || binding.offset % 16 != 0);
	const auto& buffer    = *binding.buffer;
	const auto  available = buffer.Size() - binding.offset;
	const auto  size = binding.size == UINT64_MAX ? available : std::min(binding.size, available);
	const auto  first = binding.offset / 4;
	const auto  count = static_cast<UINT>((size + 3) / 4);
	if (read_state) {
		buffer.Use(command, *read_state);
		D3D12_SHADER_RESOURCE_VIEW_DESC view {};
		view.Format                  = DXGI_FORMAT_R32_TYPELESS;
		view.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Buffer.FirstElement     = first;
		view.Buffer.NumElements      = count;
		view.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
		device->CreateShaderResourceView(buffer.Resource(), &view, destination);
		return;
	}
	buffer.Use(command, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
	view.Format              = DXGI_FORMAT_R32_TYPELESS;
	view.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
	view.Buffer.FirstElement = first;
	view.Buffer.NumElements  = count;
	view.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;
	device->CreateUnorderedAccessView(buffer.Resource(), nullptr, &view, destination);
}

[[nodiscard]] uint32_t DescriptorCount(const std::vector<RootLayout::Range>& ranges) {
	uint32_t count = 0;
	for (const auto& range: ranges) {
		count += range.count;
	}
	return count;
}

} // namespace

void RenderExecutor::CommitBindings(CommandBuffer& buffer, bool graphics,
                                    const PipelineCache::Pipeline&     pipeline,
                                    std::span<PreparedBindings* const> stages,
                                    std::span<const uint8_t>           runtime_data) {
	auto*       list   = buffer.Handle();
	auto*       device = m_context.GetGraphics().device;
	auto&       heap   = m_context.GetDescriptorHeap();
	auto&       cache  = m_context.GetTextureCache();
	const auto& layout = pipeline.layout;

	const auto      view_count    = DescriptorCount(layout.views);
	const auto      sampler_count = DescriptorCount(layout.samplers);
	DescriptorRange views {};
	DescriptorRange samplers {};
	if (view_count != 0) {
		views = heap.AllocateViews(buffer, view_count);
	}
	if (sampler_count != 0) {
		samplers = heap.AllocateSamplers(buffer, sampler_count);
	}
	uint32_t     view_index    = 0;
	uint32_t     sampler_index = 0;
	IR::PushData push {};
	bool         has_push = false;

	const auto sampled_state =
	    graphics ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
	                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
	             : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	// Descriptors follow the root layout: stages in order, their bindings in declaration order.
	for (auto* prepared: stages) {
		EXIT_IF(prepared == nullptr || prepared->runtime == nullptr || !*prepared->runtime);
		const auto& program = *prepared->runtime->program;
		auto&       images  = prepared->images;
		for (auto& binding: images) {
			auto&       image   = cache.GetImage(binding.image_id);
			const auto& view    = binding.desc.view_info;
			const bool  storage = binding.desc.type == TextureCache::BindingType::Storage;
			const auto  state   = storage                             ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
			                      : binding.image_id == m_read_only_depth ? ReadOnlyDepthState
			                                                            : sampled_state;
			image.Use(buffer, state,
			          ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
			                                 view.layer_count},
			          binding.image_view);
		}

		std::vector<uint32_t> occurrences(images.size(), 0);
		for (const auto& binding: program.bindings.descriptors) {
			if (binding.kind == IR::DescriptorBindingKind::Samplers) {
				// An array that is also used for depth comparison is declared twice (the second at the comparison space): the first range holds each sampler
				// without comparison, the second with it, as D3D12 requires of the shader's declaration.
				const bool     both   = HasComparisonSamplers(program, binding);
				const uint32_t ranges = both ? 2 : 1;
				for (uint32_t range = 0; range < ranges; range++) {
					for (const auto resource: binding.resources) {
						auto handle = prepared->samplers.at(resource);
						if (both) {
							const auto& sampler = program.info.samplers.at(resource);
							auto        guest   = DecodeNativeDescriptor<ShaderSamplerResource>(prepared->runtime->resources->samplers[sampler.snapshot_index]);
							if (sampler.force_point_filtering) {
								guest.SetPointFiltering();
							}
							handle = m_context.GetSamplerCache().GetSampler(guest, sampler.integer_border,
							                                                range == 0 ? SamplerCache::Use::Plain : SamplerCache::Use::Comparison);
						}
						device->CopyDescriptorsSimple(1, samplers.Cpu(sampler_index++), handle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
					}
				}
				continue;
			}
			if (IR::ImageBindingResourceClass(binding.kind) != IR::ImageResourceClass::None) {
				for (const auto resource: binding.resources) {
					auto&      texture = images.at(resource);
					auto&      image   = cache.GetImage(texture.image_id);
					const auto element = occurrences.at(resource)++;
					const auto view    = texture.mip_views.empty() ? texture.image_view
					                                               : texture.mip_views.at(element);
					const auto source = texture.desc.type == TextureCache::BindingType::Storage
					                        ? image.UnorderedAccessView(view)
					                        : image.ShaderResourceView(view);
					device->CopyDescriptorsSimple(1, views.Cpu(view_index++), source,
					                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				}
				continue;
			}
			const auto read_state = IR::IsReadOnlyBufferBinding(program.info, binding)
			                            ? std::optional {sampled_state}
			                            : std::nullopt;
			switch (binding.kind) {
				case IR::DescriptorBindingKind::Buffers:
					// The prepared buffers follow this binding's resource list in order; the resource numbers themselves have gaps (a number can
					// belong to another kind of binding), so they are not positions.
					EXIT_IF(prepared->buffers.size() != binding.resources.size());
					for (const auto& view: prepared->buffers) {
						WriteRawView(device, view, views.Cpu(view_index++), buffer, read_state);
					}
					break;
				case IR::DescriptorBindingKind::Gds:
					WriteRawView(device, prepared->gds, views.Cpu(view_index++), buffer);
					break;
				case IR::DescriptorBindingKind::BdaPagetable:
				case IR::DescriptorBindingKind::FaultBuffer: {
					auto*       cache = &m_context.GetBufferCache();
					const auto* table = binding.kind == IR::DescriptorBindingKind::BdaPagetable
					                        ? cache->GetBdaPageTableBuffer()
					                        : cache->GetFaultBuffer();
					WriteRawView(device, {table->Handle(), 0, table->Size()},
					             views.Cpu(view_index++), buffer);
					break;
				}
				case IR::DescriptorBindingKind::FlattenedSrt:
					WriteRawView(device, prepared->flattened_srt, views.Cpu(view_index++), buffer,
					             read_state);
					break;
				case IR::DescriptorBindingKind::ShaderData:
					WriteRawView(device, prepared->shader_data_buffer, views.Cpu(view_index++),
					             buffer, read_state);
					break;
				default:
					EXIT("D3D12: unsupported descriptor binding kind %u\n",
					     static_cast<uint32_t>(binding.kind));
			}
		}
		if (program.bindings.UsesPushData()) {
			std::ranges::copy(prepared->shader_data,
			                  push.dwords.begin() + program.bindings.push_data_start_dword);
		}
		has_push = has_push || UsesPushConstants(program);
	}
	EXIT_IF(view_index != view_count || sampler_index != sampler_count ||
	        has_push != layout.push_constants);

	UINT parameter = 0;
	if (graphics) {
		list->SetGraphicsRootSignature(pipeline.root_signature);
	} else {
		list->SetComputeRootSignature(pipeline.root_signature);
	}
	if (layout.push_constants) {
		if (graphics) {
			list->SetGraphicsRoot32BitConstants(parameter++, IR::PushData::DwordCount,
			                                    push.dwords.data(), 0);
		} else {
			list->SetComputeRoot32BitConstants(parameter++, IR::PushData::DwordCount,
			                                   push.dwords.data(), 0);
		}
	}
	if (layout.runtime_data) {
		EXIT_IF(runtime_data.empty() || runtime_data.size() > D3D12::RuntimeDataDwords * 4);
		uint32_t constants[D3D12::RuntimeDataDwords] {};
		std::memcpy(constants, runtime_data.data(), runtime_data.size());
		if (graphics) {
			list->SetGraphicsRoot32BitConstants(parameter++, D3D12::RuntimeDataDwords, constants, 0);
		} else {
			list->SetComputeRoot32BitConstants(parameter++, D3D12::RuntimeDataDwords, constants, 0);
		}
	}
	for (const auto& [count, table]: {std::pair {view_count, views.gpu},
	                                  std::pair {sampler_count, samplers.gpu}}) {
		if (count == 0) {
			continue;
		}
		if (graphics) {
			list->SetGraphicsRootDescriptorTable(parameter++, table);
		} else {
			list->SetComputeRootDescriptorTable(parameter++, table);
		}
	}
}

void RenderExecutor::RecordDispatch(CommandBuffer& buffer, const PipelineCache::Pipeline& pipeline,
                                    PreparedBindings&             bindings,
                                    const ShaderComputeInputInfo& /*input_info*/,
                                    uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) {
	if (pipeline.pipeline == nullptr) {
		return; // could not be made (reported where it failed)
	}
	m_statistics.dispatches++;
	// spirv_to_dxil's compute runtime data: group counts, padding, base group.
	const uint32_t    runtime[7] = {groups_x, groups_y, groups_z, 0, 0, 0, 0};
	PreparedBindings* stage      = &bindings;
	CommitBindings(buffer, false, pipeline, {&stage, 1},
	               {reinterpret_cast<const uint8_t*>(runtime), sizeof(runtime)});
	auto&      trace = D3D12::GetGpuTrace();
	const auto slot  = trace.Begin(buffer.Handle(), &pipeline.trace);
	buffer.Handle()->SetPipelineState(pipeline.pipeline);
	buffer.Handle()->Dispatch(groups_x, groups_y, groups_z);
	trace.End(buffer.Handle(), slot);
	// Later work reads what this dispatch wrote.
	buffer.GlobalMemoryBarrier();
}

ID3D12CommandSignature* RenderExecutor::DispatchSignature(const PipelineCache::Pipeline& pipeline) {
	const bool constants = pipeline.layout.runtime_data;
	auto&      signature = m_dispatch_signatures[constants ? pipeline.root_signature : nullptr];
	if (signature == nullptr) {
		D3D12_INDIRECT_ARGUMENT_DESC arguments[2] {};
		UINT                         count = 0;
		if (constants) {
			arguments[count].Type                             = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
			arguments[count].Constant.RootParameterIndex      = pipeline.layout.RuntimeDataParameter();
			arguments[count].Constant.DestOffsetIn32BitValues = 0;
			arguments[count].Constant.Num32BitValuesToSet     = 3;
			count++;
		}
		arguments[count++].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
		D3D12_COMMAND_SIGNATURE_DESC desc {};
		desc.ByteStride       = count * 3 * sizeof(uint32_t);
		desc.NumArgumentDescs = count;
		desc.pArgumentDescs   = arguments;
		D3D12::Check(m_context.GetGraphics().device->CreateCommandSignature(
		                 &desc, constants ? pipeline.root_signature : nullptr,
		                 IID_PPV_ARGS(&signature)),
		             "create dispatch command signature");
	}
	return signature.Get();
}

void RenderExecutor::RecordDispatchIndirect(CommandBuffer&                 buffer,
                                            const PipelineCache::Pipeline& pipeline,
                                            PreparedBindings&              bindings,
                                            const ShaderComputeInputInfo& /*input_info*/,
                                            const Buffer& args, uint64_t args_offset) {
	if (pipeline.pipeline == nullptr) {
		return;
	}
	m_statistics.dispatches++;
	// spirv_to_dxil reads the group counts from its runtime data. The command signature sets
	// them from arguments that precede the dispatch's own: [x y z][x y z].
	constexpr uint64_t CountsSize = 3 * sizeof(uint32_t);
	const bool         constants  = pipeline.layout.runtime_data;
	if (m_indirect_args == nullptr) {
		m_indirect_args = std::make_unique<Buffer>(m_context.GetGraphics(),
		                                           m_context.GetCommandScheduler(),
		                                           MemoryUsage::DeviceLocal, 0, 2 * CountsSize);
	}
	m_indirect_args->CopyFrom(buffer, args, args_offset, 0, CountsSize);
	if (constants) {
		m_indirect_args->CopyFrom(buffer, args, args_offset, CountsSize, CountsSize);
	}
	m_indirect_args->Use(buffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

	const uint32_t    runtime[7] = {}; // the counts come from the arguments
	PreparedBindings* stage      = &bindings;
	CommitBindings(buffer, false, pipeline, {&stage, 1},
	               {reinterpret_cast<const uint8_t*>(runtime), sizeof(runtime)});
	auto&      trace = D3D12::GetGpuTrace();
	const auto slot  = trace.Begin(buffer.Handle(), &pipeline.trace);
	buffer.Handle()->SetPipelineState(pipeline.pipeline);
	buffer.Handle()->ExecuteIndirect(DispatchSignature(pipeline), 1, m_indirect_args->Resource(), 0,
	                                 nullptr, 0);
	trace.End(buffer.Handle(), slot);
	buffer.GlobalMemoryBarrier();
}

} // namespace Libs::Graphics
