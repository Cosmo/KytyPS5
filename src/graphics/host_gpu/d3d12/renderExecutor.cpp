#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/d3d12/drawLog.h"
#include "graphics/host_gpu/d3d12/gpuTrace.h"
#include "graphics/host_gpu/frameDump.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace IR = ShaderRecompiler::IR;

namespace {

// Debugging (the frame dump's draw log): a dispatch, as text. `groups` is empty for an indirect dispatch.
std::string DescribeDispatch(const PipelineCache::Pipeline& pipeline, const PreparedBindings& bindings, const std::string& groups) {
	std::string name(pipeline.trace.name.begin(), pipeline.trace.name.end());
	name.resize(name.find(':') != std::string::npos ? name.find(':') : name.size());
	const auto* program = bindings.runtime != nullptr ? bindings.runtime->program : nullptr;
	std::string text = fmt::format("{} cs=0x{:016x} pipeline=\"{}\"\n", groups, program != nullptr ? program->shader_hash : 0, name);
	for (const auto& image: bindings.images) {
		if (image.desc.type == TextureCache::BindingType::Storage) {
			FrameDump::NoteWrittenImage(image.desc.info.data.address);
		}
	}
	text += DrawLog::Textures("cs", bindings);
	text += DrawLog::WrittenBuffers("cs", bindings);
	return text;
}

// A raw view of `binding`: a UAV, or an SRV used in `read_state` for buffers the shader only
// reads.
void WriteRawView(ID3D12Device* device, const BufferBinding& binding,
                  D3D12_CPU_DESCRIPTOR_HANDLE destination, CommandBuffer& command,
                  std::optional<D3D12_RESOURCE_STATES> read_state = {}) {
	EXIT_IF(binding.buffer == nullptr || binding.offset % 16 != 0);
	const auto& buffer    = *binding.buffer;
	const auto  available = buffer.Size() - binding.offset;
	const auto  size = binding.range == UINT64_MAX ? available : std::min(binding.range, available);
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

// An image binding without an image (the guest left the slot unbound) reads as zeros through a null view, as upstream binds Vulkan
// null descriptors; its format and dimension follow the shader's declaration.
void WriteNullImageView(ID3D12Device* device, const IR::ImageResource& resource, bool storage, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
	using Dimension    = ShaderRecompiler::Decoder::ImageDimension;
	DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
	if (resource.numeric_class == Prospero::TextureNumericClass::Uint) {
		format = resource.atomic64 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32_UINT;
	} else if (resource.numeric_class == Prospero::TextureNumericClass::Sint) {
		format = DXGI_FORMAT_R32_SINT;
	}
	if (storage) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC desc {};
		desc.Format = format;
		switch (resource.dimension) {
			case Dimension::Dim1D: desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; break;
			case Dimension::Dim1DArray:
				desc.ViewDimension            = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
				desc.Texture1DArray.ArraySize = 1;
				break;
			case Dimension::Dim3D:
				desc.ViewDimension   = D3D12_UAV_DIMENSION_TEXTURE3D;
				desc.Texture3D.WSize = 1;
				break;
			case Dimension::Dim2DArray:
			case Dimension::Dim2DMsaaArray:
				desc.ViewDimension            = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = 1;
				break;
			default: desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; break;
		}
		device->CreateUnorderedAccessView(nullptr, nullptr, &desc, destination);
		return;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC desc {};
	desc.Format                  = format;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	switch (resource.dimension) {
		case Dimension::Dim1D:
			desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE1D;
			desc.Texture1D.MipLevels = 1;
			break;
		case Dimension::Dim1DArray:
			desc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
			desc.Texture1DArray.MipLevels = 1;
			desc.Texture1DArray.ArraySize = 1;
			break;
		case Dimension::Dim3D:
			desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
			desc.Texture3D.MipLevels = 1;
			break;
		case Dimension::Dim2DArray:
			if (resource.cube) {
				desc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
				desc.TextureCubeArray.MipLevels = 1;
				desc.TextureCubeArray.NumCubes  = 1;
			} else {
				desc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MipLevels = 1;
				desc.Texture2DArray.ArraySize = 1;
			}
			break;
		case Dimension::Dim2DMsaa: desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; break;
		case Dimension::Dim2DMsaaArray:
			desc.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
			desc.Texture2DMSArray.ArraySize = 1;
			break;
		default:
			if (resource.cube) {
				desc.ViewDimension         = D3D12_SRV_DIMENSION_TEXTURECUBE;
				desc.TextureCube.MipLevels = 1;
			} else {
				desc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipLevels = 1;
			}
			break;
	}
	device->CreateShaderResourceView(nullptr, &desc, destination);
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
		// The samplers of all stages in binding order. An array that is also used for depth comparison is declared twice (the second at the comparison
		// space): the first range holds each sampler without comparison, the second with it, as D3D12 requires of the shader's declaration.
		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> handles;
		handles.reserve(sampler_count);
		for (auto* prepared: stages) {
			EXIT_IF(prepared == nullptr || prepared->runtime == nullptr || !*prepared->runtime);
			const auto& program = *prepared->runtime->program;
			constexpr auto kind = IR::DescriptorBindingKind::Samplers;
			if ((program.bindings.descriptor_mask & (uint64_t {1} << static_cast<uint32_t>(kind))) != 0) {
				const bool     both   = HasComparisonSamplers(program, kind);
				const uint32_t ranges = both ? 2 : 1;
				for (uint32_t range = 0; range < ranges; range++) {
					for (uint32_t resource = 0; resource < program.info.samplers.size(); resource++) {
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
						handles.push_back(handle);
					}
				}
			}
		}
		EXIT_IF(handles.size() != sampler_count);
		// The heap holds 2048 descriptors at most: draws that bind the same samplers share one table per command list.
		samplers = heap.WriteSamplers(buffer, handles);
	}
	uint32_t     view_index    = 0;
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
			if (!binding.image_id) {
				continue; // unbound: a null view below
			}
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

		// The bindings in kind order (RootLayout::Add). An image binding holds each image's mips at
		// the image's descriptor index; they are written after the bindings are laid out.
		std::array<uint32_t, IR::ImageBindingCount> image_starts {};
		for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
			const auto index = std::countr_zero(mask);
			const auto kind  = static_cast<IR::DescriptorBindingKind>(index);
			const auto count = program.bindings.descriptor_counts[index];
			if (kind == IR::DescriptorBindingKind::Samplers) {
				continue; // collected above
			}
			if (IR::ImageBindingResourceClass(kind) != IR::ImageResourceClass::None) {
				image_starts[IR::ImageBindingIndex(kind)] = view_index;
				view_index += count;
				continue;
			}
			const auto read_state = IR::IsReadOnlyBufferBinding(program.info, kind)
			                            ? std::optional {sampled_state}
			                            : std::nullopt;
			switch (kind) {
				case IR::DescriptorBindingKind::Buffers:
					// The prepared buffers are in descriptor index order.
					EXIT_IF(prepared->buffers.size() != count);
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
					const auto* table = kind == IR::DescriptorBindingKind::BdaPagetable
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
				case IR::DescriptorBindingKind::SharedMemory:
					// The group shared memory of a program that needs more than the device offers: a device-local scratch buffer that the
					// program reads and writes.
					EXIT_IF(prepared->shared_memory.buffer == nullptr);
					WriteRawView(device, prepared->shared_memory, views.Cpu(view_index++), buffer);
					break;
				default:
					EXIT("D3D12: unsupported descriptor binding kind %u\n",
					     static_cast<uint32_t>(kind));
			}
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& resource = program.info.images[i];
			auto&       texture  = images.at(i);
			const auto kind  = *IR::DescriptorBindingForImage(resource);
			const auto start = image_starts[IR::ImageBindingIndex(kind)] + resource.descriptor_index;
			if (!texture.image_id) {
				EXIT_IF(texture.image_view.index != UINT32_MAX || !texture.mip_views.empty());
				for (uint32_t mip = 0; mip < resource.mip_count; mip++) {
					WriteNullImageView(device, resource, IR::ImageBindingResourceClass(kind) == IR::ImageResourceClass::Storage,
					                   views.Cpu(start + mip));
				}
				continue;
			}
			auto&       image    = cache.GetImage(texture.image_id);
			EXIT_IF(resource.mip_count != (texture.mip_views.empty() ? 1u : texture.mip_views.size()));
			for (uint32_t mip = 0; mip < resource.mip_count; mip++) {
				const auto view   = texture.mip_views.empty() ? texture.image_view : texture.mip_views[mip];
				const auto source = texture.desc.type == TextureCache::BindingType::Storage
				                        ? image.UnorderedAccessView(view)
				                        : image.ShaderResourceView(view);
				device->CopyDescriptorsSimple(1, views.Cpu(start + mip), source,
				                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
		}
		if (program.bindings.UsesPushData()) {
			std::ranges::copy(prepared->shader_data,
			                  push.dwords.begin() + program.bindings.push_data_start_dword);
		}
		has_push = has_push || UsesPushConstants(program);
	}
	EXIT_IF(view_index != view_count || has_push != layout.push_constants);

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
	if (layout.guest_table) {
		// Guest memory is read through the persistent descriptors at the start of the view heap.
		EXIT_IF(parameter != layout.GuestTableParameter());
		const auto table = heap.BindlessTable(buffer);
		if (graphics) {
			list->SetGraphicsRootDescriptorTable(parameter++, table);
		} else {
			list->SetComputeRootDescriptorTable(parameter++, table);
		}
	}
}

void RenderExecutor::RecordDispatch(CommandBuffer& buffer, const PipelineCache::Pipeline& pipeline,
                                    PreparedBindings&             bindings,
                                    const ShaderComputeInputInfo& input_info,
                                    uint32_t groups_x, uint32_t groups_y, uint32_t groups_z,
                                    bool ordered_groups) {
	if (pipeline.pipeline == nullptr) {
		return; // could not be made (reported where it failed)
	}
	m_statistics.dispatches++;
	if (FrameDump::LogActive()) {
		FrameDump::LogDispatch(DescribeDispatch(pipeline, bindings, fmt::format("groups {}x{}x{}", groups_x, groups_y, groups_z)));
	}
	// spirv_to_dxil's compute runtime data: group counts, padding, base group.
	const uint32_t    runtime[7] = {groups_x, groups_y, groups_z, 0, 0, 0, 0};
	PreparedBindings* stage      = &bindings;
	CommitBindings(buffer, false, pipeline, {&stage, 1},
	               {reinterpret_cast<const uint8_t*>(runtime), sizeof(runtime)});
	if (input_info.lds_storage || WritesMemory(input_info.stage)) {
		// Memory writes wait for earlier accesses to the memory (as the Vulkan renderer's hazard barrier).
		buffer.GlobalMemoryBarrier();
	}
	auto&      trace = D3D12::GetGpuTrace();
	const auto slot  = trace.Begin(buffer.Handle(), &pipeline.trace);
	buffer.Handle()->SetPipelineState(pipeline.pipeline);
	if (ordered_groups) {
		// The groups one by one, in launch order (renderer/renderCompute.cpp): D3D12 has no
		// vkCmdDispatchBase, so each keeps its guest workgroup ID through the base group of the
		// runtime data, which spirv_to_dxil adds to SV_GroupID (a shader without runtime data
		// does not read the ID).
		for (uint32_t z = 0; z < groups_z; ++z) {
			for (uint32_t y = 0; y < groups_y; ++y) {
				for (uint32_t x = 0; x < groups_x; ++x) {
					if ((x | y | z) != 0u) {
						buffer.GlobalMemoryBarrier();
					}
					if (pipeline.layout.runtime_data) {
						const uint32_t base[3] = {x, y, z};
						buffer.Handle()->SetComputeRoot32BitConstants(
						    pipeline.layout.RuntimeDataParameter(), 3, base, 4);
					}
					buffer.Handle()->Dispatch(1, 1, 1);
				}
			}
		}
	} else {
		buffer.Handle()->Dispatch(groups_x, groups_y, groups_z);
	}
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
	if (FrameDump::LogActive()) {
		FrameDump::LogDispatch(DescribeDispatch(pipeline, bindings, "groups from an indirect buffer"));
	}
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
