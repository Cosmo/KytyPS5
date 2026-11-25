#include "graphics/host_gpu/renderer/image/tiler.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "gpu_tiler_shaders/gfx7_tiling_macro_128_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_macro_16_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_macro_32_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_macro_64_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_macro_8_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_macro_96_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_micro_128_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_micro_16_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_micro_32_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_micro_64_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_micro_8_spv.h"
#include "gpu_tiler_shaders/gfx7_tiling_micro_96_spv.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

namespace Libs::Graphics {

TileManager::TileManager(GraphicContext& graphics, CommandScheduler& scheduler,
                         StreamBuffer& stream_buffer)
    : m_graphics(graphics), m_scheduler(scheduler), m_stream_buffer(stream_buffer) {
	static_assert(TileShaders::FamilyCount == 9);
	static_assert(sizeof(TileShaders::TilingParams) <= UINT16_MAX && PipelineCount <= UINT16_MAX);
	static_assert(sizeof(Tiling) == 16);
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings {};
	for (uint32_t index = 0; index < 2; index++) {
		bindings[index] = {index, vk::DescriptorType::eStorageBuffer, 1,
		                   vk::ShaderStageFlagBits::eCompute, nullptr};
	}
	bindings[2] = {2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute,
	               nullptr};

	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
	descriptor_info.pBindings    = bindings.data();
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create TileManager descriptor layout");

	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                         sizeof(Conversion)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &m_descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create TileManager pipeline layout");
}

TileManager::~TileManager() {
	for (auto pipeline: m_pipelines) {
		m_graphics.device.destroyPipeline(pipeline, nullptr);
	}
	for (auto pipeline: {m_d16_to_d24, m_d16_to_d32, m_d24_to_d16, m_d32_to_d16, m_color_transform,
	                     m_color_clear}) {
		m_graphics.device.destroyPipeline(pipeline, nullptr);
	}
	for (const auto& [key, pipeline]: m_gfx7_pipelines) {
		m_graphics.device.destroyPipeline(pipeline, nullptr);
	}
	m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

TileManager::Tiling TileManager::Prepare(bool tile, uint64_t tiled_capacity,
                                         uint64_t linear_capacity, const ImageInfo& info,
                                         uint32_t levels, uint64_t source_offset,
                                         uint64_t target_offset) {
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const auto tiling = TileShaders::Prepare(tile, tiled_capacity, linear_capacity, info, levels,
	                                         source_offset & (descriptor_alignment - 1),
	                                         target_offset & (descriptor_alignment - 1));
	const auto offset = m_stream_buffer.Copy(
	    &tiling.params, tiling.params_size,
	    std::max<uint64_t>(limits.minUniformBufferOffsetAlignment, alignof(TileShaders::TilingParams)));
	return {offset, tiling.params.num_texels, tiling.params_size, tiling.pipeline_slot};
}

vk::Pipeline TileManager::GetPipeline(uint32_t slot) {
	EXIT_IF(slot >= m_pipelines.size());
	if (m_pipelines[slot] != nullptr) {
		return m_pipelines[slot];
	}
	const auto                       shader = TileShaders::PipelineShader(slot);
	const uint32_t                   values[] {shader.element_bytes, shader.tile};
	const vk::SpecializationMapEntry entries[] {{0, 0, 4}, {1, 4, 4}};
	const vk::SpecializationInfo     specialization {2, entries, sizeof(values), values};
	m_pipelines[slot] = CreatePipeline(shader.spirv, &specialization);
	return m_pipelines[slot];
}

vk::Pipeline TileManager::CreatePipeline(std::span<const uint32_t>     code,
                                         const vk::SpecializationInfo* specialization) const {
	const auto                        module = CompileSPV(code, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage               = vk::ShaderStageFlagBits::eCompute;
	stage.module              = module;
	stage.pName               = "main";
	stage.pSpecializationInfo = specialization;
	vk::ComputePipelineCreateInfo create {};
	create.stage          = stage;
	create.layout         = m_pipeline_layout;
	vk::Pipeline pipeline = nullptr;
	const auto   result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create TileManager pipeline");
	return pipeline;
}

void TileManager::Record(vk::Buffer source, uint64_t source_offset, uint64_t source_capacity,
                         vk::Buffer target, uint64_t target_offset, uint64_t target_capacity,
                         const Tiling& tiling) {
	const auto&    limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t descriptor_alignment =
	    std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const uint64_t source_descriptor_offset = Common::AlignDown(source_offset, descriptor_alignment);
	const uint64_t target_descriptor_offset = Common::AlignDown(target_offset, descriptor_alignment);
	const uint64_t source_base              = source_offset - source_descriptor_offset;
	const uint64_t target_base              = target_offset - target_descriptor_offset;
	const uint64_t source_range             = Common::AlignUp(source_base + source_capacity, 4);
	const uint64_t target_range             = Common::AlignUp(target_base + target_capacity, 4);
	EXIT_NOT_IMPLEMENTED(source_range > limits.maxStorageBufferRange ||
	                     target_range > limits.maxStorageBufferRange || target_offset % 4 != 0 ||
	                     target_capacity % 4 != 0);

	m_scheduler.EndRendering();
	auto                    command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[3] {};
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = source;
	barriers[0].offset              = source_offset;
	barriers[0].size                = source_capacity;
	barriers[1]                     = barriers[0];
	barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	barriers[1].buffer        = target;
	barriers[1].offset        = target_offset;
	barriers[1].size          = target_capacity;
	barriers[2]               = barriers[0];
	barriers[2].srcAccessMask = vk::AccessFlagBits::eHostWrite;
	barriers[2].dstAccessMask = vk::AccessFlagBits::eUniformRead;
	barriers[2].buffer        = m_stream_buffer.Handle();
	barriers[2].offset        = tiling.params_offset;
	barriers[2].size          = tiling.params_size;
	command.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	    vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 3, barriers, 0, nullptr);

	const vk::DescriptorBufferInfo source_info {source, source_descriptor_offset, source_range};
	const vk::DescriptorBufferInfo target_info {target, target_descriptor_offset, target_range};
	const vk::DescriptorBufferInfo params_info {m_stream_buffer.Handle(), tiling.params_offset,
	                                            tiling.params_size};
	const vk::DescriptorBufferInfo infos[] {source_info, target_info, params_info};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType =
		    index == 2 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo = &infos[index];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	command.bindPipeline(vk::PipelineBindPoint::eCompute, GetPipeline(tiling.pipeline_slot));
	const uint64_t groups   = (uint64_t {tiling.num_texels} + 63u) / 64u;
	const uint32_t groups_y = static_cast<uint32_t>(
	    (groups + limits.maxComputeWorkGroupCount[0] - 1u) / limits.maxComputeWorkGroupCount[0]);
	const uint32_t groups_x = static_cast<uint32_t>((groups + groups_y - 1u) / groups_y);
	EXIT_NOT_IMPLEMENTED(groups_y > limits.maxComputeWorkGroupCount[1]);
	command.dispatch(groups_x, groups_y, 1);

	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eMemoryRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 1,
	                        &barriers[1], 0, nullptr);
}

TileManager::Result TileManager::Detile(vk::Buffer tiled, uint64_t tiled_offset,
                                        const ImageInfo& info) {
	if (!info.IsTiled()) {
		const Result source {tiled, tiled_offset, info.data.size};
		return info.GetColorTransform() == ColorTransform::None
		           ? source
		           : TransformColor(source, info.GetColorTransform(), true);
	}
	const auto linear_capacity = info.LinearSize();
	const auto tiling  = Prepare(false, info.data.size, linear_capacity, info, 0, tiled_offset, 0);
	auto scratch = GetScratchBuffer(linear_capacity, tiled);
	Record(tiled, tiled_offset, info.data.size, scratch.buffer, 0, scratch.size, tiling);
	return {scratch.buffer, 0, linear_capacity};
}

// The uniform block of gfx7_tiling.comp (scalar layout).
struct Gfx7TilingInfo {
	uint32_t      bank_swizzle;
	uint32_t      micro_tiled_mips;
	uint32_t      num_mips;
	Gfx7::MipInfo mips[Gfx7::MAX_MIPS];
	uint32_t      num_texels;
};
static_assert(sizeof(Gfx7::MipInfo) == 12 && sizeof(Gfx7TilingInfo) == 12 + 16 * 12 + 4);

// shadPS4's TileManager::GetTilingPipeline: the shader of the texel size, specialised for the tile mode.
vk::Pipeline TileManager::GetGfx7Pipeline(const ImageInfo& info, bool tiler) {
	const auto& layout = info.gfx7;
	const auto  key    = uint32_t {layout.tile} | (layout.num_bits << 8u) | (tiler ? 1u << 24u : 0u);
	if (const auto it = m_gfx7_pipelines.find(key); it != m_gfx7_pipelines.end()) {
		return it->second;
	}
	const auto mode     = Gfx7::TileMode(layout.tile);
	const auto array    = Gfx7::GetArrayMode(mode);
	const auto micro    = Gfx7::GetMicroTileMode(mode);
	const bool is_macro = Gfx7::IsMacroTiled(array);
	std::array<uint32_t, 12> spec {1u, uint32_t(micro), Gfx7::GetMicroTileThickness(array), tiler ? 1u : 0u};
	if (is_macro) {
		const auto macro_mode = Gfx7::CalculateMacrotileMode(mode, layout.num_bits, 1u);
		spec[4]  = uint32_t(array);
		spec[5]  = uint32_t(Gfx7::GetPipeConfig(mode));
		spec[6]  = Gfx7::GetBankWidth(macro_mode);
		spec[7]  = Gfx7::GetBankHeight(macro_mode);
		spec[8]  = Gfx7::GetNumBanks(macro_mode);
		spec[9]  = static_cast<uint32_t>(std::bit_width(spec[8])) - 1u;
		spec[10] = Gfx7::CalculateTileSplit(mode, array, micro, layout.num_bits);
		spec[11] = Gfx7::GetMacrotileAspect(macro_mode);
	}
	std::span<const uint32_t> code;
	switch (layout.num_bits) {
		case 8: code = is_macro ? std::span<const uint32_t> {GFX7_TILING_MACRO_8_SPV} : std::span<const uint32_t> {GFX7_TILING_MICRO_8_SPV}; break;
		case 16: code = is_macro ? std::span<const uint32_t> {GFX7_TILING_MACRO_16_SPV} : std::span<const uint32_t> {GFX7_TILING_MICRO_16_SPV}; break;
		case 32: code = is_macro ? std::span<const uint32_t> {GFX7_TILING_MACRO_32_SPV} : std::span<const uint32_t> {GFX7_TILING_MICRO_32_SPV}; break;
		case 64: code = is_macro ? std::span<const uint32_t> {GFX7_TILING_MACRO_64_SPV} : std::span<const uint32_t> {GFX7_TILING_MICRO_64_SPV}; break;
		case 96: code = is_macro ? std::span<const uint32_t> {GFX7_TILING_MACRO_96_SPV} : std::span<const uint32_t> {GFX7_TILING_MICRO_96_SPV}; break;
		case 128: code = is_macro ? std::span<const uint32_t> {GFX7_TILING_MACRO_128_SPV} : std::span<const uint32_t> {GFX7_TILING_MICRO_128_SPV}; break;
		default: EXIT("gfx7 tiling: unsupported texel size %u\n", layout.num_bits);
	}
	std::array<vk::SpecializationMapEntry, 12> entries {};
	for (uint32_t i = 0; i < entries.size(); i++) {
		entries[i] = {i, i * 4u, 4u};
	}
	const uint32_t               count = is_macro ? 12u : 4u;
	const vk::SpecializationInfo specialization {count, entries.data(), count * 4u, spec.data()};
	const auto                   pipeline = CreatePipeline(code, &specialization);
	m_gfx7_pipelines.emplace(key, pipeline);
	return pipeline;
}

TileManager::Result TileManager::DetileGfx7(vk::Buffer tiled, uint64_t tiled_offset,
                                            const ImageInfo& info) {
	const auto scratch = GetScratchBuffer(info.gfx7.guest_size, tiled);
	RecordGfx7(false, tiled, tiled_offset, scratch.buffer, 0, info);
	return {scratch.buffer, 0, info.gfx7.guest_size};
}

void TileManager::TileGfx7(vk::Buffer linear, uint64_t linear_offset, vk::Buffer tiled,
                           uint64_t tiled_offset, const ImageInfo& info) {
	RecordGfx7(true, tiled, tiled_offset, linear, linear_offset, info);
}

// One dispatch of shadPS4's tiling shader between a surface in its gfx7 layout (`tiled`) and its
// levels one after another at their guest offsets (`linear`); `tiler` writes the gfx7 layout.
void TileManager::RecordGfx7(bool tiler, vk::Buffer tiled, uint64_t tiled_offset, vk::Buffer linear,
                             uint64_t linear_offset, const ImageInfo& info) {
	const auto& layout = info.gfx7;
	const auto& limits = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t size      = layout.guest_size;
	const uint64_t alignment = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	EXIT_NOT_IMPLEMENTED(size == 0 || size > limits.maxStorageBufferRange ||
	                     tiled_offset % alignment != 0 || linear_offset % alignment != 0);

	Gfx7TilingInfo params {};
	params.bank_swizzle     = layout.bank_swizzle;
	params.micro_tiled_mips = layout.micro_tiled_mips;
	params.num_mips         = info.resources.levels;
	for (uint32_t mip = 0; mip < params.num_mips; mip++) {
		params.mips[mip] = layout.mips[mip];
		if (layout.block) {
			params.mips[mip].pitch  = static_cast<uint16_t>(std::max((params.mips[mip].pitch + 3u) / 4u, 1u));
			params.mips[mip].height = static_cast<uint16_t>(std::max((params.mips[mip].height + 3u) / 4u, 1u));
		}
	}
	params.num_texels = static_cast<uint32_t>(size / (layout.num_bits / 8u));
	const auto params_offset = m_stream_buffer.Copy(
	    &params, sizeof(params), std::max<uint64_t>(limits.minUniformBufferOffsetAlignment, 4));
	const auto pipeline = GetGfx7Pipeline(info, tiler);

	// The shader reads one side and writes the other.
	const vk::Buffer source        = tiler ? linear : tiled;
	const uint64_t   source_offset = tiler ? linear_offset : tiled_offset;
	const vk::Buffer target        = tiler ? tiled : linear;
	const uint64_t   target_offset = tiler ? tiled_offset : linear_offset;

	m_scheduler.EndRendering();
	auto                    command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[3] {};
	barriers[0].srcAccessMask       = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite;
	barriers[0].dstAccessMask       = vk::AccessFlagBits::eShaderRead;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = source;
	barriers[0].offset              = source_offset;
	barriers[0].size                = size;
	barriers[1]                     = barriers[0];
	barriers[1].srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	barriers[1].dstAccessMask       = vk::AccessFlagBits::eShaderWrite;
	barriers[1].buffer              = target;
	barriers[1].offset              = target_offset;
	barriers[1].size                = size;
	barriers[2]                     = barriers[0];
	barriers[2].srcAccessMask       = vk::AccessFlagBits::eHostWrite;
	barriers[2].dstAccessMask       = vk::AccessFlagBits::eUniformRead;
	barriers[2].buffer              = m_stream_buffer.Handle();
	barriers[2].offset              = params_offset;
	barriers[2].size                = sizeof(params);
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 3, barriers, 0,
	                        nullptr);

	const vk::DescriptorBufferInfo infos[] {{tiled, tiled_offset, Common::AlignUp(size, 4)},
	                                        {linear, linear_offset, Common::AlignUp(size, 4)},
	                                        {m_stream_buffer.Handle(), params_offset, sizeof(params)}};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType =
		    index == 2 ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo = &infos[index];
	}
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
	const uint64_t groups   = (uint64_t {params.num_texels} + 63u) / 64u;
	const uint32_t groups_y = static_cast<uint32_t>(
	    (groups + limits.maxComputeWorkGroupCount[0] - 1u) / limits.maxComputeWorkGroupCount[0]);
	const uint32_t groups_x = static_cast<uint32_t>((groups + groups_y - 1u) / groups_y);
	command.dispatch(groups_x, groups_y, 1);

	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eMemoryRead |
	                            vk::AccessFlagBits::eHostRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, 1, &barriers[1], 0, nullptr);
}

void TileManager::Tile(vk::Buffer linear, uint64_t linear_offset, uint64_t linear_capacity,
                       vk::Buffer tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                       const ImageInfo& info, uint32_t levels) {
	const auto tiling =
	    Prepare(true, tiled_capacity, linear_capacity, info, levels, linear_offset, tiled_offset);
	Record(linear, linear_offset, linear_capacity, tiled, tiled_offset, tiled_capacity, tiling);
}

void TileManager::TileImage(Image& image, std::span<const vk::BufferImageCopy> regions,
                            vk::Buffer tiled, uint64_t tiled_offset, uint64_t tiled_capacity,
                            uint32_t levels) {
	EXIT_IF(regions.empty());
	const auto linear_capacity = image.info.LinearSize(levels);
	// Reserve stream parameters before recording the image download.
	const auto tiling =
	    Prepare(true, tiled_capacity, linear_capacity, image.info, levels, 0, tiled_offset);
	auto linear = GetScratchBuffer(linear_capacity, tiled);
	image.Download(regions, linear.buffer, 0, linear.size);
	Record(linear.buffer, 0, linear_capacity, tiled, tiled_offset, tiled_capacity, tiling);
}

TileManager::Result TileManager::GetScratchBuffer(uint64_t size, vk::Buffer input) {
	EXIT_IF(size == 0);
	size = Common::AlignUp(size, 4);
	auto& buffer = m_scratch[m_scratch[0] && m_scratch[0]->Handle() == input ? 1 : 0];
	if (!buffer || buffer->Size() < size) {
		if (buffer) {
			m_scheduler.DeferOperation([old = std::move(buffer)]() mutable { old.reset(); });
		}
		buffer = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
		        vk::BufferUsageFlagBits::eTransferDst, size);
	}
	return {buffer->Handle(), 0, size};
}

TileManager::StorageBinding TileManager::BindStorage(Result buffer, uint64_t size) const {
	const auto& limits            = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto  alignment         = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const auto  descriptor_offset = Common::AlignDown(buffer.offset, alignment);
	const auto  base              = buffer.offset - descriptor_offset;
	EXIT_IF(buffer.buffer == nullptr || size == 0 || buffer.size < size || base > UINT32_MAX ||
	        size > UINT64_MAX - base || base + size > UINT64_MAX - 3);
	const auto range = Common::AlignUp(base + size, 4);
	EXIT_IF(range > limits.maxStorageBufferRange || range > UINT32_MAX);
	return {{buffer.buffer, descriptor_offset, range}, static_cast<uint32_t>(base)};
}

uint32_t TileManager::ConversionRows(uint64_t offset, uint64_t row_stride, uint64_t active,
                                     uint32_t remaining, uint64_t alignment, uint64_t max_range,
                                     uint32_t max_groups) noexcept {
	if (row_stride == 0 || active == 0 || remaining == 0 || alignment == 0 || max_groups == 0) {
		return 0;
	}
	const auto prefix = offset % alignment;
	if (prefix >= max_range || active > max_range - prefix) {
		return 0;
	}
	const auto descriptor_rows = 1 + (max_range - prefix - active) / row_stride;
	return static_cast<uint32_t>(std::min<uint64_t>({remaining, descriptor_rows, max_groups}));
}

void TileManager::ConvertD16(Result source, Result target, D16Direction direction, bool d32,
                             const D16Layout& layout) {
	vk::Pipeline*             pipeline_pointer = nullptr;
	std::span<const uint32_t> code;
	if (direction == D16Direction::Promote) {
		pipeline_pointer = d32 ? &m_d16_to_d32 : &m_d16_to_d24;
		code             = TileShaders::PromoteD16();
	} else {
		pipeline_pointer = d32 ? &m_d32_to_d16 : &m_d24_to_d16;
		code             = TileShaders::DemoteD16();
	}
	auto& pipeline = *pipeline_pointer;
	if (pipeline == nullptr) {
		const uint32_t                   value = d32 ? 1u : 0u;
		const vk::SpecializationMapEntry entry {0, 0, sizeof(value)};
		const vk::SpecializationInfo     specialization {1, &entry, sizeof(value), &value};
		pipeline = CreatePipeline(code, &specialization);
	}

	const uint64_t source_element =
	    direction == D16Direction::Promote ? sizeof(uint16_t) : sizeof(uint32_t);
	const uint64_t target_element =
	    direction == D16Direction::Promote ? sizeof(uint32_t) : sizeof(uint16_t);
	const uint64_t source_active = static_cast<uint64_t>(layout.width) * source_element;
	const uint64_t target_active = static_cast<uint64_t>(layout.width) * target_element;
	const auto     required      = [](uint32_t height, uint32_t layers, uint64_t row_stride,
	                                  uint64_t slice_stride, uint64_t active) {
		EXIT_IF(height == 0 || layers == 0 || row_stride < active ||
		        (height - 1) > (UINT64_MAX - active) / row_stride);
		const auto slice = static_cast<uint64_t>(height - 1) * row_stride + active;
		EXIT_IF(slice_stride < slice || (layers - 1) > (UINT64_MAX - slice) / slice_stride);
		return static_cast<uint64_t>(layers - 1) * slice_stride + slice;
	};
	EXIT_IF(layout.width == 0 || layout.source_row_stride > UINT32_MAX ||
	        layout.target_row_stride > UINT32_MAX);
	const auto source_required = required(layout.height, layout.layers, layout.source_row_stride,
	                                      layout.source_slice_stride, source_active);
	const auto target_required = required(layout.height, layout.layers, layout.target_row_stride,
	                                      layout.target_slice_stride, target_active);
	EXIT_IF(source_required > UINT64_MAX - 3 || target_required > UINT64_MAX - 3);
	const auto source_barrier_size = Common::AlignUp(source_required, 4);
	const auto target_barrier_size = Common::AlignUp(target_required, 4);
	EXIT_IF(source.size < source_barrier_size || target.size < target_barrier_size);

	m_scheduler.EndRendering();
	auto                    command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barriers[2] {};
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].buffer              = source.buffer;
	barriers[0].offset              = source.offset;
	barriers[0].size                = source_barrier_size;
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite |
	                            vk::AccessFlagBits::eTransferWrite |
	                            vk::AccessFlagBits::eShaderWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[1].buffer              = target.buffer;
	barriers[1].offset              = target.offset;
	barriers[1].size                = target_barrier_size;
	barriers[1].srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
	                            vk::AccessFlagBits::eHostWrite |
	                            vk::AccessFlagBits::eTransferWrite |
	                            vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	    vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 2, barriers, 0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
	const auto& limits              = m_graphics.GetPhysicalDeviceProperties().limits;
	const auto descriptor_alignment = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 4);
	const auto rows_for = [&](Result buffer, uint64_t relative, uint64_t stride, uint64_t active,
	                          uint32_t remaining) {
		EXIT_IF(relative > buffer.size || buffer.offset > UINT64_MAX - relative);
		const auto offset = buffer.offset + relative;
		return ConversionRows(offset, stride, active, remaining, descriptor_alignment,
		                      limits.maxStorageBufferRange, limits.maxComputeWorkGroupCount[1]);
	};
	const auto groups_x = (static_cast<uint64_t>(layout.width) + 63u) / 64u;
	EXIT_IF(groups_x == 0 || groups_x > limits.maxComputeWorkGroupCount[0]);
	for (uint32_t layer = 0; layer < layout.layers; layer++) {
		for (uint32_t row = 0; row < layout.height;) {
			const auto source_relative =
			    layout.source_slice_stride * layer + layout.source_row_stride * row;
			const auto target_relative =
			    layout.target_slice_stride * layer + layout.target_row_stride * row;
			const auto remaining = layout.height - row;
			const auto rows = std::min(rows_for(source, source_relative, layout.source_row_stride,
			                                    source_active, remaining),
			                           rows_for(target, target_relative, layout.target_row_stride,
			                                    target_active, remaining));
			EXIT_IF(rows == 0);
			const auto source_span =
			    static_cast<uint64_t>(rows - 1) * layout.source_row_stride + source_active;
			const auto target_span =
			    static_cast<uint64_t>(rows - 1) * layout.target_row_stride + target_active;
			const auto source_binding = BindStorage(
			    {source.buffer, source.offset + source_relative, source.size - source_relative},
			    source_span);
			const auto target_binding = BindStorage(
			    {target.buffer, target.offset + target_relative, target.size - target_relative},
			    target_span);
			const vk::DescriptorBufferInfo infos[] {
			    source_binding.info,
			    target_binding.info,
			};
			std::array<vk::WriteDescriptorSet, 2> writes {};
			for (uint32_t index = 0; index < writes.size(); index++) {
				writes[index].dstBinding      = index;
				writes[index].descriptorCount = 1;
				writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
				writes[index].pBufferInfo     = &infos[index];
			}
			command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
			                             static_cast<uint32_t>(writes.size()), writes.data());
			Conversion push {};
			push.src_base    = source_binding.base;
			push.dst_base    = target_binding.base;
			push.width       = layout.width;
			push.height      = rows;
			push.source_pitch = static_cast<uint32_t>(layout.source_row_stride);
			push.target_pitch = static_cast<uint32_t>(layout.target_row_stride);
			command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
			                      sizeof(push), &push);
			command.dispatch(static_cast<uint32_t>(groups_x), rows, 1);
			row += rows;
		}
	}
	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eMemoryRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 1,
	                        &barriers[1], 0, nullptr);
}

void TileManager::TransformColor(Result input, Result output, ColorTransform transform,
                                 bool to_host) {
	uint32_t element_bytes = 0;
	switch (transform) {
		case ColorTransform::SwapBgra16: element_bytes = 8; break;
		case ColorTransform::Reverse10_11_11:
		case ColorTransform::Rotate2_10_10_10: element_bytes = 4; break;
		default: EXIT("invalid color transform\n");
	}
	EXIT_IF(input.size == 0 || input.size % element_bytes != 0 ||
	        input.size / element_bytes > UINT32_MAX ||
	        output.size < input.size);
	const auto pixels = static_cast<uint32_t>(input.size / element_bytes);
	if (m_color_transform == nullptr) {
		m_color_transform = CreatePipeline(TileShaders::ColorTransformShader(), nullptr);
	}
	const auto input_binding  = BindStorage(input, input.size);
	const auto output_binding = BindStorage(output, input.size);

	const vk::DescriptorBufferInfo infos[] {
	    input_binding.info,
	    output_binding.info,
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	vk::BufferMemoryBarrier barriers[2] {};
	for (uint32_t index = 0; index < 2; index++) {
		barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[index].buffer              = infos[index].buffer;
		barriers[index].offset              = infos[index].offset;
		barriers[index].size                = infos[index].range;
	}
	barriers[0].srcAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostWrite |
	                            vk::AccessFlagBits::eShaderWrite;
	barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
	barriers[1].srcAccessMask = vk::AccessFlagBits::eMemoryRead;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderWrite;
	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	command.pipelineBarrier(
	    vk::PipelineStageFlagBits::eAllCommands | vk::PipelineStageFlagBits::eHost,
	    vk::PipelineStageFlagBits::eComputeShader, {}, 0, nullptr, 2, barriers, 0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_color_transform);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	Conversion push {};
	push.src_base        = input_binding.base;
	push.dst_base        = output_binding.base;
	push.width           = pixels;
	push.color_transform = static_cast<uint32_t>(transform);
	push.to_host         = to_host;
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch((pixels + 63u) / 64u, 1, 1);
	barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barriers[1].dstAccessMask = vk::AccessFlagBits::eTransferRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &barriers[1],
	                        0, nullptr);
}

TileManager::Result TileManager::TransformColor(Result input, ColorTransform transform,
                                                bool to_host) {
	auto result = GetScratchBuffer(input.size, input.buffer);
	TransformColor(input, result, transform, to_host);
	return result;
}

vk::Buffer TileManager::CheckColorClear(Result metadata, uint64_t stride, uint32_t slices,
                                        uint32_t keys, bool expand) {
	struct Check {
		uint32_t metadata_base;
		uint32_t words;
		uint32_t stride;
		uint32_t keys;
		uint32_t expand;
	};
	static_assert(sizeof(Check) <= sizeof(Conversion));
	const uint32_t max_slices =
	    m_graphics.GetPhysicalDeviceProperties().limits.maxFramebufferLayers;
	EXIT_IF(slices == 0 || slices > max_slices || metadata.size % 4 != 0 || stride % 4 != 0 ||
	        metadata.size / 4 > UINT32_MAX || stride * (slices - 1) >= metadata.size);
	if (m_color_clear == nullptr) {
		m_color_clear      = CreatePipeline(TileShaders::ColorClearShader(), nullptr);
		m_clear_predicates = std::make_unique<Buffer>(
		    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0,
		    vk::BufferUsageFlagBits::eStorageBuffer |
		        vk::BufferUsageFlagBits::eConditionalRenderingEXT,
		    uint64_t {max_slices} * ColorClearSlots * sizeof(uint32_t));
	}
	const auto source = BindStorage(metadata, metadata.size);
	const vk::DescriptorBufferInfo infos[] {source.info,
	                                        {m_clear_predicates->Handle(), 0, VK_WHOLE_SIZE}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); index++) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	// Orders the metadata writers and the previous check's predicate reads before this check.
	vk::MemoryBarrier barrier {
	    .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
	    .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite};
	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &barrier, 0, nullptr,
	                        0, nullptr);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_color_clear);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());
	const Check push {source.base,
	                  static_cast<uint32_t>((metadata.size - stride * (slices - 1)) / 4),
	                  static_cast<uint32_t>(stride / 4), keys, expand};
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch(slices, 1, 1);
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eAllCommands, {}, 1, &barrier, 0, nullptr,
	                        0, nullptr);
	return m_clear_predicates->Handle();
}

} // namespace Libs::Graphics
