// Vulkan command recording for draws and dispatches prepared by the shared renderer code.

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>

namespace Libs::Graphics {

static void SetGraphicsDynamicParams(const CommandBuffer& buffer, vk::CommandBuffer vk_buffer,
                                     const ShaderVertexInputInfo& vs_input_info,
                                     const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	vk_buffer.setViewportWithCount(viewport_count, viewports.data());
	vk_buffer.setScissorWithCount(viewport_count, scissors.data());

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	vk_buffer.setLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	vk_buffer.setBlendConstants(blend_constants.data());
	vk_buffer.setDepthTestEnable(depth.depth_test_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthWriteEnable(depth.depth_write_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthCompareOp(depth.depth_compare_op);

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	vk_buffer.setDepthBiasEnable(depth_bias_enable ? VK_TRUE : VK_FALSE);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = PolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		vk_buffer.setDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	vk_buffer.setStencilTestEnable(depth.stencil_test_enable ? VK_TRUE : VK_FALSE);
	if (depth.stencil_test_enable) {
		const auto set_stencil = [&](vk::StencilFaceFlagBits face, const vk::StencilOpState& state) {
			vk_buffer.setStencilOp(face, state.failOp, state.passOp, state.depthFailOp, state.compareOp);
			vk_buffer.setStencilCompareMask(face, state.compareMask);
			vk_buffer.setStencilWriteMask(face, state.writeMask);
			vk_buffer.setStencilReference(face, state.reference);
		};
		set_stencil(vk::StencilFaceFlagBits::eFront, depth.stencil_front);
		set_stencil(vk::StencilFaceFlagBits::eBack, depth.stencil_back);
	}

#if defined(__APPLE__)
	// MoltenVK has no VK_EXT_color_write_enable; the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
#else
	vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	for (uint32_t slot = 0; slot < rendering.num_color_attachments; slot++) {
		enable[slot] = rendering.color_attachments[slot].image_view != nullptr;
	}
	if (rendering.num_color_attachments != 0) {
		vk_buffer.setColorWriteEnableEXT(rendering.num_color_attachments, enable);
	}
#endif
}

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		auto&      image      = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		ConsumeDepthClearState(cache, depth);
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			EXIT("depth attachment feedback loop is not supported by the host\n");
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count, emit.first_vertex,
				               emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.IsIndexed()) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count, i + emit.first_vertex,
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable) {
	auto& ucfg = buffer.GetUserConfig();
	const auto vertex_stages =
	    std::span {state.vertex_info.data(), state.programs.VertexStageCount()};
	const bool mesh_active = state.vertex_info[0].stage.program->stage == ShaderType::Mesh;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vertex_info[0].mesh;
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.IsIndexed() || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed());
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed(), primitive_restart_enable);
		}
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			std::printf("[WARN] mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
			return;
			//EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     //draw.instance_count);
		}
	}

	if (mesh_active && draw.IsIndexed()) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		PrepareBindings(state.vertex_info[i].stage, bindings.vertex[i]);
		descriptor_stages[stage_count++] = &bindings.vertex[i];
	}
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.Name(), "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vertex_info[0]);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "CreatePipeline");
	}
	auto& pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    state.programs);
	vk::ImageAspectFlags feedback_aspects;
	const auto           rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         feedback_aspects, stages);

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	auto vk_buffer = buffer.Handle();
	SetDrawDebugPhase(buffer, submit_id, draw, draw.IsIndexed() ? 0x100u : 0x200u);
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (state.ps_active && !draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	if (mesh_active) {
		const uint32_t draw_data[] {
		    draw.index_count,
		    draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
		    emit.first_instance, index_source.guest_element_size,
		    static_cast<uint32_t>(index_source.address),
		    static_cast<uint32_t>(index_source.address >> 32u)};
		static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
		vk_buffer.pushConstants(pipeline.pipeline_layout,
		                        vk::ShaderStageFlagBits::eMeshEXT |
		                            vk::ShaderStageFlagBits::eFragment,
		                        0, sizeof(draw_data), draw_data);
	} else {
		CommitIndexBuffer(vk_buffer, index_binding);
	}

	SetGraphicsDynamicParams(buffer, vk_buffer, vertex_stages.back(), state.depth_info, rendering);
	if (m_context.GetGraphics().attachment_feedback_loop_enabled) {
		vk_buffer.setAttachmentFeedbackLoopEnableEXT(feedback_aspects);
	}

	LogDrawPhase(draw.Name(), "BeginRendering");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	if (mesh_active) {
		vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
	} else {
		EmitDrawPrimitives(ucfg, vk_buffer, draw, emit);
	}

	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		m_context.GetCommandScheduler().EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
	LogDrawPhase(draw.Name(), "DrawComplete");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
}

static bool HasStorageWrites(const ShaderComputeInputInfo& input_info) {
	const auto& images = input_info.stage.program->info.images;
	return HasShaderBufferWrites(input_info.stage) ||
	       std::any_of(images.begin(), images.end(), [](const auto& image) {
		       return image.written &&
		              image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Storage;
	       });
}

void RenderExecutor::RecordDispatch(CommandBuffer& buffer, const PipelineCache::Pipeline& pipeline,
                                    PreparedBindings&             bindings,
                                    const ShaderComputeInputInfo& input_info,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z) {
	auto              vk_buffer        = buffer.Handle();
	PreparedBindings* descriptor_stage = &bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	if (HasStorageWrites(input_info)) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);

	// The removed host fence also ordered read-only dispatches before later writers.
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
}

void RenderExecutor::RecordDispatchIndirect(CommandBuffer&                 buffer,
                                            const PipelineCache::Pipeline& pipeline,
                                            PreparedBindings&              bindings,
                                            const ShaderComputeInputInfo&  input_info,
                                            const Buffer& args, uint64_t args_offset) {
	PreparedBindings* descriptor_stage = &bindings;
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const auto vk_buffer = buffer.Handle();
	if (HasStorageWrites(input_info)) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eDrawIndirect, {}, 1, &barrier, 0, nullptr,
	                          0, nullptr);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatchIndirect(args.Handle(), args_offset);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
}

} // namespace Libs::Graphics
