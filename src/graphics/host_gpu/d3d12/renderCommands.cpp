// D3D12 command recording for draws prepared by the shared renderer code (renderer/renderDraw.cpp).

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/frameResolution.h"
#include "graphics/host_gpu/d3d12/drawLog.h"
#include "graphics/host_gpu/d3d12/gpuTrace.h"
#include "graphics/host_gpu/frameDump.h"
#include "graphics/host_gpu/d3d12/renderContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/shader/recompiler/ir/MeshEmulation.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace IR = ShaderRecompiler::IR;

namespace {

// The largest render target, which bounds rasterization without attachments.
constexpr uint32_t MaxTargetExtent = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;

void WarnOnce(std::atomic_bool& warned, const char* message) {
	if (!warned.exchange(true, std::memory_order_relaxed)) {
		LOGF("D3D12: %s\n", message);
		std::printf("Warning: D3D12: %s\n", message);
	}
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(vk::PrimitiveTopology topology) {
	switch (topology) {
		case vk::PrimitiveTopology::ePointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
		case vk::PrimitiveTopology::eLineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
		case vk::PrimitiveTopology::eLineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
		case vk::PrimitiveTopology::eTriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		case vk::PrimitiveTopology::eTriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		// Rect lists: a geometry shader expands each triangle.
		case vk::PrimitiveTopology::ePatchList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		default: EXIT("D3D12: unsupported topology %d\n", static_cast<int>(topology));
	}
	return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

bool IsTriangleTopology(vk::PrimitiveTopology topology) {
	return topology == vk::PrimitiveTopology::eTriangleList ||
	       topology == vk::PrimitiveTopology::eTriangleStrip ||
	       topology == vk::PrimitiveTopology::eTriangleFan;
}

// D3D12 has no triangle fans: each fan of `vertices` (split at `restart_index` when `restart`,
// and after every `fan_length` vertices when nonzero) becomes a triangle list. Fan triangle i is
// written as (v[i+1], v[i+2], v[0]): Vulkan's first-vertex convention makes v[i+1] the
// provoking vertex of fan triangles, and the rotation keeps the winding.
std::vector<uint32_t> TriangulateFans(std::span<const uint32_t> vertices, bool restart,
                                      uint32_t restart_index, uint32_t fan_length) {
	std::vector<uint32_t> list;
	size_t                start = 0;
	for (size_t i = 0; i <= vertices.size(); i++) {
		const bool cut = i < vertices.size() && restart && vertices[i] == restart_index;
		if (i < vertices.size() && !cut && (fan_length == 0 || i - start < fan_length)) {
			continue;
		}
		for (size_t v = start + 1; v + 1 < i; v++) {
			list.insert(list.end(), {vertices[v], vertices[v + 1], vertices[start]});
		}
		start = cut ? i + 1 : i;
	}
	return list;
}

// The vertices a fan draw references: the guest indices, or consecutive numbers.
std::vector<uint32_t> FanVertices(RenderContext& context, const DrawCallInfo& draw,
                                  const DrawIndexBufferSource& index_source) {
	std::vector<uint32_t> vertices(draw.index_count);
	if (!draw.IsIndexed()) {
		for (uint32_t i = 0; i < draw.index_count; i++) {
			vertices[i] = i;
		}
		return vertices;
	}
	const auto* data = static_cast<const uint8_t*>(index_source.host_data);
	uint32_t    size = index_source.type == vk::IndexType::eUint16 ? 2 : 4;
	if (data == nullptr) {
		auto& cache = context.GetBufferCache();
		if (cache.IsRegionGpuModified(index_source.address, index_source.size)) {
			cache.ReadMemory(index_source.address, index_source.size);
		}
		data = reinterpret_cast<const uint8_t*>(index_source.address);
		size = index_source.guest_element_size;
	}
	for (uint32_t i = 0; i < draw.index_count; i++) {
		uint32_t index = 0;
		std::memcpy(&index, data + static_cast<size_t>(i) * size, size);
		vertices[i] = index;
	}
	return vertices;
}

// Whether the pixel stage samples subresources of the depth target it renders to.
bool SamplesDepthTarget(const RenderDepthInfo&                 depth,
                        const std::optional<PreparedBindings>& pixel) {
	if (!depth.image_id || !pixel) {
		return false;
	}
	const auto& target = depth.desc.view_info;
	return std::ranges::any_of(pixel->images, [&](const TextureBinding& binding) {
		const auto& sampled = binding.desc.view_info;
		return binding.image_id == depth.image_id &&
		       binding.desc.type == TextureCache::BindingType::Texture &&
		       ImageRangeOverlaps(sampled.base_level, sampled.level_count, target.base_level,
		                          target.level_count) &&
		       ImageRangeOverlaps(sampled.base_layer, sampled.layer_count, target.base_layer,
		                          target.layer_count);
	});
}

// The workgroups of a mesh draw, one per `primitives_per_group` guest primitives, per instance;
// 0 when nothing is drawn.
uint32_t MeshGroupCount(CommandBuffer& buffer, const DrawCallInfo& draw,
                        const ShaderVertexInputInfo& info, bool primitive_restart_enable) {
	// D3D12 limits a mesh dispatch to 65535 groups per dimension and 2^22 in total.
	constexpr uint64_t MaxGroupsPerGrid = uint64_t {1} << 22u;
	const auto&        mesh             = info.mesh;
	if (primitive_restart_enable) {
		static std::atomic_bool warned = false;
		WarnOnce(warned, "primitive restart is not implemented for mesh shaders");
	}
	if (mesh.primitives_per_group == 0) {
		static std::atomic_bool unsupported = false;
		WarnOnce(unsupported, "a mesh draw of this primitive type is not supported; it is skipped");
		return 0;
	}
	const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
	if (primitives == 0 || draw.instance_count == 0) {
		return 0;
	}
	const auto groups = (primitives - 1u) / mesh.primitives_per_group + 1u;
	// (The compute emulation draws in chunks, so the limits of one dispatch do not apply to it.)
	if (!mesh.compute_emulation &&
	    (groups > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
	     draw.instance_count > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
	     uint64_t {groups} * draw.instance_count > MaxGroupsPerGrid)) {
		static std::atomic_bool warned = false;
		WarnOnce(warned, "mesh draws beyond the host dispatch limits are skipped");
		return 0;
	}
	return groups;
}

// Whether the stage may write memory other than its render targets.
bool WritesMemory(const ShaderStageRuntime& runtime) {
	const auto& info = runtime.program->info;
	return std::ranges::any_of(info.buffers, [](const auto& buffer) { return buffer.written; }) ||
	       std::ranges::any_of(info.images, [](const auto& image) {
		       return image.written && image.resource_class == IR::ImageResourceClass::Storage;
	       });
}

// Guest viewports and scissors, and the Y flips of the runtime data. D3D12 viewports have
// positive heights, so a guest viewport that maps clip-space +Y down flips Y in the last vertex
// stage instead.
void SetViewports(const CommandBuffer& buffer, ID3D12GraphicsCommandList* list,
                  const ShaderVertexInputInfo& vs_input_info, uint32_t width, uint32_t height,
                  D3D12::VertexRuntimeData& runtime) {
	const auto& ctx     = buffer.GetRegisters();
	const auto& vp      = ctx.GetScreenViewport();
	const auto& clip    = ctx.GetClipControl();
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed = std::ranges::any_of(outputs, [](const auto& output) {
        return output.kind == IR::StageOutputKind::ViewportIndex;
    });
	constexpr uint32_t Slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<D3D12_VIEWPORT, Slots> viewports {};
	std::array<D3D12_RECT, Slots>     scissors {};
	const uint32_t                    count = indexed ? Slots : 1;
	for (uint32_t i = 0; i < count; i++) {
		const auto& guest = vp.viewports[i];
		auto&       out   = viewports[i];
		float       x = guest.xoffset - guest.xscale, w = guest.xscale * 2.0f;
		float       y_center = guest.yoffset, y_scale = guest.yscale;
		if (clip.clip_disable) {
			x = 0.0f;
			w = static_cast<float>(MaxTargetExtent);
			y_center = y_scale = static_cast<float>(MaxTargetExtent) / 2.0f;
		}
		const bool flip = y_scale >= 0.0f;
		if (i == 0) {
			runtime.yz_flip_mask = flip ? 1u : 0u;
		} else if (flip != (runtime.yz_flip_mask != 0)) {
			static std::atomic_bool warned = false;
			WarnOnce(warned, "viewports with different Y directions are unsupported");
		}
		out.TopLeftX = x;
		out.Width    = w;
		out.TopLeftY = flip ? y_center - y_scale : y_center + y_scale;
		out.Height   = std::abs(y_scale) * 2.0f;
		out.MinDepth = guest.zoffset - (clip.dx_clip_space ? 0.0f : guest.zscale);
		out.MaxDepth = guest.zscale + guest.zoffset;
		if (out.MinDepth < 0.0f || out.MinDepth > 1.0f || out.MaxDepth < 0.0f ||
		    out.MaxDepth > 1.0f) {
			static std::atomic_bool warned = false;
			WarnOnce(warned, "viewport depth ranges outside [0, 1] are clamped");
			out.MinDepth = std::clamp(out.MinDepth, 0.0f, 1.0f);
			out.MaxDepth = std::clamp(out.MaxDepth, 0.0f, 1.0f);
		}

		const auto scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), vk::Extent2D {width, height}, i);
		scissors[i] = {scissor.left, scissor.top, scissor.right, scissor.bottom};
		if (out.Width == 0.0f) {
			// Keep empty slots at their guest index.
			out.Width   = 1.0f;
			scissors[i] = {};
		}
	}
	runtime.viewport_width  = viewports[0].Width;
	runtime.viewport_height = viewports[0].Height;
	list->RSSetViewports(count, viewports.data());
	list->RSSetScissorRects(count, scissors.data());
}

void SetDynamicState(const CommandBuffer& buffer, ID3D12GraphicsCommandList* list,
                     const RenderDepthInfo& depth, bool depth_bounds) {
	const auto&       ctx   = buffer.GetRegisters();
	const auto&       blend = ctx.GetBlendColor();
	const float       blend_constants[] {blend.red, blend.green, blend.blue, blend.alpha};
	list->OMSetBlendFactor(blend_constants);
	if (depth.stencil_test_enable) {
		if (depth.stencil_front.reference != depth.stencil_back.reference) {
			static std::atomic_bool warned = false;
			WarnOnce(warned, "stencil references differ per face; using the front reference");
		}
		list->OMSetStencilRef(depth.stencil_front.reference);
	}
	if (depth_bounds) {
		D3D12::ComPtr<ID3D12GraphicsCommandList1> list1;
		D3D12::Check(list->QueryInterface(IID_PPV_ARGS(&list1)), "query ID3D12GraphicsCommandList1");
		list1->OMSetDepthBounds(ctx.GetDepthBoundsMin(), ctx.GetDepthBoundsMax());
	}
	if (ctx.GetLineWidth() != 1.0f) {
		static std::atomic_bool warned = false;
		WarnOnce(warned, "wide lines are unsupported; drawing 1 pixel wide lines");
	}
}

// Debugging (frame_dump_snapshots): the draw log gets the statistics of the first color target after a draw that writes a large floating-point one.
void LogSnapshot(RenderContext& context, const DrawRenderState& state) {
	if (!Config::FrameDumpSnapshots() || !FrameDump::LogActive() || state.color_count == 0 || !state.color_info[0].image_id) {
		return;
	}
	const auto& desc = state.color_info[0].desc;
	if (desc.info.extent.width < 1920) {
		return;
	}
	const auto stats = context.GetTextureCache().SnapshotStats(state.color_info[0].image_id);
	if (!stats.empty()) {
		FrameDump::LogEvent(fmt::format("    after that draw, the average of 0x{:x} (every 4th texel): {}", desc.info.data.address, stats));
	}
}

// Debugging (the frame dump's draw log): what a draw is made of, as text. The pipeline's name starts with its number ("pipeline 57: ...").
std::string DescribeDraw(const GraphicContext& graphics, const CommandBuffer& buffer, const DrawCallInfo& draw, const DrawRenderState& state, vk::PrimitiveTopology topology,
                         bool primitive_restart_enable, bool mesh, bool emulated_mesh, const PipelineCache::Pipeline& pipeline, const PreparedBindings& vertex_bindings,
                         const PreparedBindings* pixel_bindings) {
	std::string pipeline_name(pipeline.trace.name.begin(), pipeline.trace.name.end());
	pipeline_name.resize(pipeline_name.find(':') != std::string::npos ? pipeline_name.find(':') : pipeline_name.size());
	const auto& vs_program = *state.vertex_info[0].stage.program;
	std::string text = fmt::format("{} idx={} inst={} guest-prim={} topology={}{}{} pipeline=\"{}\" vs=0x{:016x}", draw.Name(), draw.index_count, draw.instance_count,
	                               static_cast<uint32_t>(buffer.GetUserConfig().GetPrimType()), vk::to_string(topology), primitive_restart_enable ? " restart" : "",
	                               mesh ? (emulated_mesh ? " mesh(emulated)" : " mesh(native)") : "", pipeline_name, vs_program.shader_hash);
	if (state.ps_active) {
		text += fmt::format(" ps=0x{:016x}", state.ps_input_info.stage.program->shader_hash);
	} else {
		text += " ps=none";
	}
	text += "\n";

	const std::array<uint64_t, 3> vertex_ids {state.programs.vertex[0].id, state.programs.vertex[1].id, state.programs.vertex[2].id};
	const auto key = MakeGraphicsPipelineKey(graphics, std::span {state.color_info, state.color_count}, state.depth_info, std::span {state.vertex_info.data(), 1}, buffer,
	                                         state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable, vertex_ids,
	                                         state.ps_active ? state.programs.pixel.id : 0);
	const auto& params = key.static_params;
	for (uint32_t i = 0; i < state.color_count; i++) {
		const auto& color = state.color_info[i];
		const auto  slot  = color.target_slot;
		text += fmt::format("    color target {}: {} | write mask 0x{:x} blend {}", slot, DrawLog::Target(color.desc, color.guest_mip_level, color.guest_array_layer), params.color_mask[slot],
		                    params.blend_enable[slot] ? "on" : "off");
		if (params.blend_enable[slot]) {
			text += fmt::format(" (color src {} dst {} fn {}; alpha src {} dst {} fn {}{})", params.color_srcblend[slot], params.color_destblend[slot], params.color_comb_fcn[slot],
			                    params.alpha_srcblend[slot], params.alpha_destblend[slot], params.alpha_comb_fcn[slot], params.separate_alpha_blend[slot] ? "" : ", alpha like color");
		}
		text += "\n";
	}
	if (state.depth_info.image_id) {
		const auto& depth = state.depth_info;
		text += fmt::format("    depth target: {} | test {} {} write {} stencil {}{}\n", DrawLog::Target(depth.desc, depth.desc.view_info.base_level, depth.desc.view_info.base_layer),
		                    depth.depth_test_enable ? "on" : "off", vk::to_string(depth.depth_compare_op), depth.depth_write_enable ? "on" : "off", depth.stencil_test_enable ? "on" : "off",
		                    depth.depth_load_clear_enable ? " | cleared by this draw" : "");
	} else {
		text += "    no depth target\n";
	}
	text += fmt::format("    cull front {} back {} | samples {}\n", params.cull_front ? "on" : "off", params.cull_back ? "on" : "off", params.samples);
	text += DrawLog::Textures("vs", vertex_bindings);
	text += DrawLog::WrittenBuffers("vs", vertex_bindings);
	if (pixel_bindings != nullptr) {
		text += DrawLog::Textures("ps", *pixel_bindings);
		text += DrawLog::WrittenBuffers("ps", *pixel_bindings);
	}
	return text;
}

} // namespace

RenderExecutor::RenderTargets RenderExecutor::AcquireRenderTargets(CommandBuffer&   buffer,
                                                                   RenderColorInfo* colors,
                                                                   uint32_t         color_count,
                                                                   RenderDepthInfo& depth,
                                                                   bool read_only_depth) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	auto&         cache = m_context.GetTextureCache();
	RenderTargets targets;
	targets.width  = std::numeric_limits<uint32_t>::max();
	targets.height = std::numeric_limits<uint32_t>::max();
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		const auto  view  = cache.FindRenderTarget(target.image_id, target.desc);
		auto&       image = cache.GetImage(target.image_id);
		const auto& info  = target.desc.view_info;
		EXIT_IF(image.backing.samples != target.desc.info.samples);
		// Before the state change: an image the device cannot render to may not enter the render target state.
		const auto rtv = image.RenderTargetView(view);
		if (rtv.ptr == 0) {
			targets.unrenderable = true;
			continue;
		}
		image.Use(buffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
		          ImageSubresourceRange {info.base_level, info.level_count, info.base_layer,
		                                 info.layer_count},
		          view);
		const auto extent                      = target.Extent();
		targets.width                          = std::min(targets.width, extent.width);
		targets.height                         = std::min(targets.height, extent.height);
		targets.colors[target.target_slot]     = rtv;
		targets.color_count = std::max(targets.color_count, target.target_slot + 1);
	}
	if (targets.unrenderable) {
		return targets; // the draw is skipped: the depth target's pending clear stays for the next draw
	}
	for (uint32_t slot = 0; slot < targets.color_count; slot++) {
		if (targets.colors[slot].ptr != 0) {
			continue;
		}
		if (m_null_render_target.ptr == 0) {
			m_null_render_target = m_context.GetDescriptorHeap().AllocateCpu(
			    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			D3D12_RENDER_TARGET_VIEW_DESC desc {};
			desc.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			m_context.GetGraphics().device->CreateRenderTargetView(nullptr, &desc,
			                                                       m_null_render_target);
		}
		targets.colors[slot] = m_null_render_target;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto view = cache.FindDepthTarget(depth.image_id, depth.desc);
		ConsumeDepthClearState(cache, depth);
		auto&       image = cache.GetImage(depth.image_id);
		const auto& info  = depth.desc.view_info;
		EXIT_IF(image.backing.samples != depth.desc.info.samples);
		// A writable attachment is written even when guest depth/stencil tests do not write.
		image.Use(buffer, read_only_depth ? ReadOnlyDepthState : D3D12_RESOURCE_STATE_DEPTH_WRITE,
		          ImageSubresourceRange {info.base_level, info.level_count, info.base_layer,
		                                 info.layer_count},
		          view);
		const auto aspects = ImageViewOps::DepthAspectMask(info.format);
		targets.depth      = image.DepthStencilView(view, read_only_depth);
		targets.has_depth  = true;
		if (depth.depth_load_clear_enable && (aspects & vk::ImageAspectFlagBits::eDepth)) {
			targets.depth_clears |= D3D12_CLEAR_FLAG_DEPTH;
		}
		if (depth.stencil_clear_enable && (aspects & vk::ImageAspectFlagBits::eStencil)) {
			targets.depth_clears |= D3D12_CLEAR_FLAG_STENCIL;
		}
		if (read_only_depth && targets.depth_clears != 0) {
			static std::atomic_bool warned = false;
			WarnOnce(warned, "a depth target sampled by the same draw is not cleared");
			targets.depth_clears = {};
		}
		targets.depth_clear   = depth.depth_clear_value;
		targets.stencil_clear = depth.stencil_clear_value;
		targets.width         = std::min(targets.width, depth.desc.info.extent.width);
		targets.height        = std::min(targets.height, depth.desc.info.extent.height);
	}
	if (color_count == 0 && !depth.image_id) {
		targets.width  = MaxTargetExtent;
		targets.height = MaxTargetExtent;
	}
	EXIT_IF(targets.width == 0 || targets.height == 0 ||
	        targets.width == std::numeric_limits<uint32_t>::max() ||
	        targets.height == std::numeric_limits<uint32_t>::max());
	return targets;
}

void RenderExecutor::RecordEmulatedMeshDraw(CommandBuffer&                 buffer,
                                            const ShaderVertexInputInfo&   vs_input_info,
                                            const PipelineCache::Pipeline& compute,
                                            const PipelineCache::Pipeline& graphics,
                                            std::span<const uint32_t> draw_data, uint32_t groups,
                                            uint32_t instances) {
	// What one draw may use of the device's memory; a bigger draw takes more chunks.
	constexpr uint64_t RecordBytes = uint64_t {32} << 20u;
	constexpr uint64_t IndexBytes  = uint64_t {8} << 20u;
	constexpr uint32_t MaxDispatch = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;

	const auto& mesh            = vs_input_info.mesh;
	const auto  layout          = IR::MeshEmulation::LayoutFor(vs_input_info.stage.program->info.outputs);
	const auto  primitive_slots = mesh.EmulatedPrimitiveSlots();
	const uint64_t group_records =
	    uint64_t {mesh.EmulatedVertexSlots()} * layout.StrideDwords() * sizeof(uint32_t);
	const uint64_t group_indices = uint64_t {primitive_slots} * 3 * sizeof(uint32_t);
	if (group_records == 0 || group_indices == 0 || group_records > RecordBytes ||
	    group_indices > IndexBytes) {
		static std::atomic_bool warned = false;
		WarnOnce(warned, "a mesh group is too big for the compute emulation; its draws are skipped");
		return;
	}
	EXIT_IF(draw_data.size() != IR::PushData::MeshDrawDwordCount);
	if (m_mesh_records == nullptr) {
		m_mesh_records = std::make_unique<Buffer>(m_context.GetGraphics(), m_context.GetCommandScheduler(),
		                                          MemoryUsage::DeviceLocal, 0, RecordBytes);
		m_mesh_indices = std::make_unique<Buffer>(m_context.GetGraphics(), m_context.GetCommandScheduler(),
		                                          MemoryUsage::DeviceLocal, 0, IndexBytes);
	}
	auto*      list    = buffer.Handle();
	auto&      trace   = D3D12::GetGpuTrace();
	const auto records = m_mesh_records->GpuAddress();
	const auto indices = m_mesh_indices->GpuAddress();

	// The chunk: `chunk_groups` groups of each of `chunk_instances` instances, as many as fit.
	const uint64_t budget = std::min(RecordBytes / group_records, IndexBytes / group_indices);
	const uint32_t chunk_instances_max =
	    static_cast<uint32_t>(std::min<uint64_t>({instances, budget, MaxDispatch}));
	const uint32_t chunk_groups_max = static_cast<uint32_t>(
	    std::min<uint64_t>({groups, std::max<uint64_t>(1, budget / chunk_instances_max), MaxDispatch}));

	const auto& compute_layout = compute.layout;
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	uint32_t parameters[IR::MeshEmulation::DrawDwordCount] {};
	std::ranges::copy(draw_data, parameters);
	for (uint32_t instance_base = 0; instance_base < instances; instance_base += chunk_instances_max) {
		const uint32_t chunk_instances = std::min(chunk_instances_max, instances - instance_base);
		for (uint32_t group_base = 0; group_base < groups; group_base += chunk_groups_max) {
			const uint32_t chunk_groups = std::min(chunk_groups_max, groups - group_base);
			parameters[IR::MeshEmulation::GroupBaseParameter]    = group_base;
			parameters[IR::MeshEmulation::InstanceBaseParameter] = instance_base;
			parameters[IR::MeshEmulation::ChunkWidthParameter]   = chunk_groups;

			// The compute shader writes the chunk's vertices and triangles.
			m_mesh_records->Use(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			m_mesh_indices->Use(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			// (Root descriptors are set after the transitions: the debug layer takes a buffer's state
			// at its first use from the first descriptor set for it.)
			list->SetComputeRootUnorderedAccessView(compute_layout.MeshScratchParameter(), records);
			list->SetComputeRootUnorderedAccessView(compute_layout.MeshScratchParameter() + 1, indices);
			list->SetComputeRoot32BitConstants(0, IR::MeshEmulation::DrawDwordCount, parameters, 0);
			if (compute_layout.runtime_data) {
				// spirv_to_dxil's compute runtime data: the group counts.
				const uint32_t runtime[7] = {chunk_groups, chunk_instances, 1, 0, 0, 0, 0};
				list->SetComputeRoot32BitConstants(compute_layout.RuntimeDataParameter(),
				                                   static_cast<UINT>(std::size(runtime)), runtime, 0);
			}
			auto slot = trace.Begin(list, &compute.trace);
			list->SetPipelineState(compute.pipeline);
			list->Dispatch(chunk_groups, chunk_instances, 1);
			trace.End(list, slot);
			m_statistics.dispatches++;
			buffer.GlobalMemoryBarrier();

			// One indexed draw of every slot of the chunk.
			m_mesh_records->Use(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			m_mesh_indices->Use(buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
			list->SetGraphicsRootShaderResourceView(graphics.layout.MeshScratchParameter(), records);
			const auto                     index_count = chunk_groups * chunk_instances * primitive_slots * 3u;
			const D3D12_INDEX_BUFFER_VIEW  view {indices, static_cast<UINT>(index_count * sizeof(uint32_t)),
			                                     DXGI_FORMAT_R32_UINT};
			list->IASetIndexBuffer(&view);
			slot = trace.Begin(list, &graphics.trace);
			list->SetPipelineState(graphics.pipeline);
			list->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
			trace.End(list, slot);
		}
	}
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
                                         bool                         primitive_restart_enable) {
	// Upstream's depth-to-color copy is in the Vulkan renderer only; here it stops as on a Vulkan device without it.
	EXIT_NOT_IMPLEMENTED(buffer.GetRegisters().GetRenderControl().copy_depth_to_color);
	const auto& vs_input_info = state.vertex_info[0];
	// (The draw log notes a draw that is skipped, and why.)
	const auto log_skip = [&draw](const char* reason) {
		if (FrameDump::LogActive()) {
			FrameDump::LogDraw(fmt::format("{} idx={} inst={} -> {}", draw.Name(), draw.index_count, draw.instance_count, reason), true);
		}
	};
	if (state.programs.VertexStageCount() != 1) {
		static std::atomic_bool warned = false;
		WarnOnce(warned, "tessellation is not supported; its draws are skipped");
		log_skip("tessellation is not supported");
		return;
	}
	const bool mesh = vs_input_info.stage.program->stage == ShaderType::Mesh;
	// A mesh program the device cannot run as a mesh shader runs as a compute shader whose output a
	// vertex stage draws (ShaderRecompiler::IR::MeshEmulation).
	const bool emulated_mesh = mesh && vs_input_info.mesh.compute_emulation;
	const auto& mode = buffer.GetRegisters().GetModeControl();
	if (mode.cull_front && mode.cull_back && (mesh || IsTriangleTopology(topology))) {
		// D3D12 cannot cull both faces, which draws nothing.
		log_skip("both faces are culled (draws nothing)");
		return;
	}
	const uint32_t mesh_groups =
	    mesh ? MeshGroupCount(buffer, draw, vs_input_info, primitive_restart_enable) : 0;
	if (mesh && mesh_groups == 0) {
		log_skip("a mesh draw without groups");
		return;
	}
	m_statistics.draws++;

	// What is drawn: the guest draw, or the triangle list replacing its fans. Reading guest
	// indices can finish GPU work, so this precedes all resource preparation.
	bool                  indexed       = draw.IsIndexed();
	uint32_t              count         = draw.index_count;
	int32_t               vertex_offset = emit.vertex_offset;
	DrawIndexBufferSource indices       = index_source;
	std::vector<uint32_t> fan_list;
	if (!mesh && topology == vk::PrimitiveTopology::eTriangleFan) {
		// Legacy quads are drawn as fans of 4 vertices each.
		const bool quads = buffer.GetUserConfig().GetPrimType() ==
		                   Prospero::PrimitiveType::kQuadListLegacy;
		if (quads && (draw.index_count & 0x3u) != 0) {
			static std::atomic_bool warned = false;
			WarnOnce(warned, "a quad list draw with a partial quad is skipped");
			log_skip("a quad list with a partial quad");
			return;
		}
		const bool restart_16 = index_source.host_data != nullptr ||
		                        index_source.guest_element_size == 2;
		fan_list      = TriangulateFans(FanVertices(m_context, draw, index_source),
		                                primitive_restart_enable,
		                                restart_16 ? 0xffffu : UINT32_MAX, quads ? 4 : 0);
		indices       = {.host_data          = fan_list.data(),
		                 .size               = fan_list.size() * sizeof(uint32_t),
		                 .type               = vk::IndexType::eUint32,
		                 .guest_element_size = sizeof(uint32_t)};
		indexed       = true;
		count         = static_cast<uint32_t>(fan_list.size());
		vertex_offset = draw.IsIndexed() ? emit.vertex_offset
		                                 : static_cast<int32_t>(emit.first_vertex);
		topology      = vk::PrimitiveTopology::eTriangleList;
		primitive_restart_enable = false;
		if (count == 0) {
			log_skip("the fans have no triangles");
			return;
		}
	}

	if (mesh && draw.IsIndexed()) {
		// The mesh program reads the guest indices by address; registering them here has
		// PrepareGraphicsBindings synchronize them.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address,
		    static_cast<uint64_t>(draw.index_count) * index_source.guest_element_size);
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 2> descriptor_stages {};
	uint32_t                         stage_count = 0;
	PrepareBindings(vs_input_info.stage, bindings.vertex[0]);
	descriptor_stages[stage_count++] = &bindings.vertex[0];
	if (state.ps_active) {
		if (!bindings.pixel) {
			bindings.pixel.emplace();
		}
		PrepareBindings(state.ps_input_info.stage, *bindings.pixel);
		descriptor_stages[stage_count++] = &*bindings.pixel;
	}
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh) {
		vertex_bindings = AcquireVertexBuffers(buffer, vs_input_info);
		index_binding   = PrepareIndexBuffer(buffer, indices);
	}

	// D3D12 has no feedback loops: a depth target the pixel stage also samples is bound
	// read-only, so it can be a depth-read target and a shader resource at once.
	// (The reused pixel bindings are the last pixel shader's when this draw has none.)
	const bool read_only_depth =
	    state.ps_active && SamplesDepthTarget(state.depth_info, bindings.pixel);
	if (read_only_depth) {
		auto& depth = state.depth_info;
		if (depth.depth_write_enable || depth.stencil_front.writeMask != 0 ||
		    depth.stencil_back.writeMask != 0) {
			static std::atomic_bool warned = false;
			WarnOnce(warned, "a depth target sampled by the same draw is bound read-only; "
			                 "its depth and stencil writes are dropped");
		}
		depth.depth_write_enable      = false;
		depth.stencil_front.writeMask = 0;
		depth.stencil_back.writeMask  = 0;
	}
	m_read_only_depth = read_only_depth ? state.depth_info.image_id : ImageId {};

	if (emulated_mesh) {
		// What is drawn is the list of triangles the compute shader wrote.
		topology                 = vk::PrimitiveTopology::eTriangleList;
		primitive_restart_enable = false;
	}
	const bool index16   = indices.type == vk::IndexType::eUint16;
	const auto strip_cut = !primitive_restart_enable ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED
	                       : index16 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
	                                 : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
	auto& pipeline = m_context.GetPipelineCache().GetGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info,
	    std::span {state.vertex_info.data(), 1}, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    strip_cut, state.programs);
	if (FrameDump::LogActive()) {
		FrameDump::LogDraw(DescribeDraw(m_context.GetGraphics(), buffer, draw, state, topology, primitive_restart_enable, mesh, emulated_mesh, pipeline, bindings.vertex[0],
		                                state.ps_active && bindings.pixel ? &*bindings.pixel : nullptr) +
		                   (pipeline.pipeline == nullptr ? "    -> SKIPPED: the pipeline could not be made\n" : ""));
	}
	if (pipeline.pipeline == nullptr) {
		// Could not be made (reported where it failed): this draw is skipped.
		m_read_only_depth = {};
		return;
	}
	const PipelineCache::Pipeline* mesh_compute = nullptr;
	if (emulated_mesh) {
		mesh_compute = &m_context.GetPipelineCache().GetMeshComputePipeline(vs_input_info,
		                                                                    state.programs.vertex[0]);
		if (mesh_compute->pipeline == nullptr) {
			log_skip("the compute pipeline of the mesh program could not be made");
			m_read_only_depth = {};
			return;
		}
	}
	const auto targets = AcquireRenderTargets(buffer, state.color_info, state.color_count,
	                                          state.depth_info, read_only_depth);
	if (targets.unrenderable) {
		log_skip("a color target's format cannot be rendered to on this device");
		m_read_only_depth = {};
		return;
	}

	// Resource preparation above may finish and restart the command list. From here on, every
	// operation records into the current list and cannot touch guest memory.
	auto* list = buffer.Handle();
	SetDrawDebugPhase(buffer, submit_id, draw, draw.IsIndexed() ? 0x100u : 0x200u);

	std::array<D3D12_VERTEX_BUFFER_VIEW, PreparedVertexBuffers::MaxBuffers> vertex_views {};
	for (uint32_t i = 0; i < vertex_bindings.count; i++) {
		const auto* vertex = vertex_bindings.buffers[i];
		EXIT_IF(vertex == nullptr);
		vertex->Use(buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		vertex_views[i] = {vertex->GpuAddress() + vertex_bindings.offsets[i],
		                   static_cast<UINT>(vertex_bindings.sizes[i]),
		                   vs_input_info.buffers[i].stride};
	}
	if (vertex_bindings.count != 0) {
		list->IASetVertexBuffers(0, vertex_bindings.count, vertex_views.data());
	}
	if (index_binding.buffer != nullptr) {
		index_binding.buffer->Use(buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
		const D3D12_INDEX_BUFFER_VIEW view {index_binding.buffer->GpuAddress() + index_binding.offset,
		                                    static_cast<UINT>(indices.size),
		                                    index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
		list->IASetIndexBuffer(&view);
	}

	// spirv_to_dxil's vertex index is SV_VertexID plus first_vertex.
	D3D12::VertexRuntimeData runtime {};
	// (The vertex stage of a mesh draw run as a compute shader reads its vertices from the records:
	// its draw has no vertex offset or first instance.)
	runtime.first_vertex =
	    emulated_mesh ? 0u : indexed ? static_cast<uint32_t>(vertex_offset) : emit.first_vertex;
	runtime.base_instance   = emulated_mesh ? 0u : emit.first_instance;
	runtime.is_indexed_draw = (draw.IsIndexed() || emulated_mesh) ? 1u : 0u;
	if (emulated_mesh) {
		// The compute shader is the mesh program's own stage: its bindings go to the compute root
		// signature; the pixel stage's go to the graphics one below.
		const uint32_t    compute_runtime[7] {};
		PreparedBindings* vertex_stage = &bindings.vertex[0];
		CommitBindings(buffer, false, *mesh_compute, {&vertex_stage, 1},
		               {reinterpret_cast<const uint8_t*>(compute_runtime), sizeof(compute_runtime)});
	}
	SetViewports(buffer, list, vs_input_info, targets.width, targets.height, runtime);
	// The scene's size for the overlay: draws with color targets and a depth target, within the targets.
	if (targets.color_count > 0 && targets.has_depth) {
		FrameResolution::NoteRender(static_cast<uint32_t>(std::clamp(runtime.viewport_width, 0.0f, static_cast<float>(targets.width))),
		                            static_cast<uint32_t>(std::clamp(runtime.viewport_height, 0.0f, static_cast<float>(targets.height))));
	}
	CommitBindings(buffer, true, pipeline, emulated_mesh ? stages.subspan(1) : stages,
	               {reinterpret_cast<const uint8_t*>(&runtime), sizeof(runtime)});
	// The draw parameters of a mesh program start its push constants, the first root parameter.
	const uint32_t draw_data[] {
	    draw.index_count,
	    draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
	    emit.first_instance, index_source.guest_element_size,
	    static_cast<uint32_t>(index_source.address),
	    static_cast<uint32_t>(index_source.address >> 32u)};
	static_assert(std::size(draw_data) == IR::PushData::MeshDrawDwordCount);
	if (mesh && !emulated_mesh) {
		list->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(std::size(draw_data)), draw_data,
		                                    0);
	}
	SetDynamicState(buffer, list, state.depth_info,
	                state.depth_info.depth_bounds_test_enable && targets.has_depth);

	LogDrawPhase(draw.Name(), "Draw");
	list->OMSetRenderTargets(targets.color_count, targets.colors.data(), FALSE,
	                         targets.has_depth ? &targets.depth : nullptr);
	if (targets.depth_clears != 0) {
		list->ClearDepthStencilView(targets.depth, targets.depth_clears, targets.depth_clear,
		                            targets.stencil_clear, 0, nullptr);
	}
	if (emulated_mesh) {
		RecordEmulatedMeshDraw(buffer, vs_input_info, *mesh_compute, pipeline, draw_data,
		                       mesh_groups, draw.instance_count);
		if (WritesMemory(vs_input_info.stage) ||
		    (state.ps_active && WritesMemory(state.ps_input_info.stage))) {
			buffer.GlobalMemoryBarrier();
		}
		m_read_only_depth = {};
		LogDrawPhase(draw.Name(), "DrawComplete");
		LogSnapshot(m_context, state);
		return;
	}
	auto&      trace = D3D12::GetGpuTrace();
	const auto slot  = trace.Begin(list, &pipeline.trace);
	list->SetPipelineState(pipeline.pipeline);
	if (mesh) {
		buffer.MeshHandle()->DispatchMesh(mesh_groups, draw.instance_count, 1);
	} else {
		list->IASetPrimitiveTopology(PrimitiveTopology(topology));
		if (indexed) {
			list->DrawIndexedInstanced(count, draw.instance_count, 0, vertex_offset,
			                           emit.first_instance);
		} else {
			list->DrawInstanced(count, draw.instance_count, emit.first_vertex,
			                    emit.first_instance);
		}
	}
	trace.End(list, slot);

	if (WritesMemory(vs_input_info.stage) ||
	    (state.ps_active && WritesMemory(state.ps_input_info.stage))) {
		// Later work reads what this draw wrote.
		buffer.GlobalMemoryBarrier();
	}
	m_read_only_depth = {};
	LogDrawPhase(draw.Name(), "DrawComplete");
	LogSnapshot(m_context, state);
}

} // namespace Libs::Graphics
