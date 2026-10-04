#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDER_H_

#include "common/common.h"
#include "graphics/host_gpu/d3d12/buffer.h"
#include "graphics/host_gpu/d3d12/pipelineCache.h"
#include "graphics/host_gpu/renderArgs.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

struct ID3D12GraphicsCommandList;
struct ID3D12GraphicsCommandList6;

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct RenderColorInfo;
struct RenderDepthInfo;
struct ShaderStageRuntime;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
struct DrawRenderState;
class RenderContext;
class CommandScheduler;

// The command list currently being recorded, plus the guest register state it records from.
class CommandBuffer {
public:
	~CommandBuffer() = default;
	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const noexcept { return m_list == nullptr; }

	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	// D3D12 has no render passes; kept for the shared scheduler interface.
	void EndRendering() const {}
	// Makes all prior writes visible to all later reads and writes.
	void GlobalMemoryBarrier() const;

	[[nodiscard]] ID3D12GraphicsCommandList* Handle() const;
	// The same list, for recording mesh shader draws (GraphicContext::mesh_shaders).
	[[nodiscard]] ID3D12GraphicsCommandList6* MeshHandle() const;
	void                         MarkEncodedDraw() noexcept { m_contains_encoded_draw = true; }
	[[nodiscard]] bool           ContainsEncodedDraw() const noexcept {
		return m_contains_encoded_draw;
	}
	[[nodiscard]] GraphicContext& GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&  GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&    GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig& GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&     GetShaders() const noexcept { return *m_shaders; }

	struct DebugInfo {
		uint32_t op        = 0;
		uint64_t submit_id = 0;
		uint32_t args[4]   = {};
		uint64_t arg4      = 0;
	};
	[[nodiscard]] const DebugInfo& GetDebugInfo() const noexcept { return m_debug; }

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	RenderContext&             m_context;
	GraphicContext&            m_graphics;
	ID3D12GraphicsCommandList* m_list                  = nullptr;
	ID3D12GraphicsCommandList6* m_mesh_list            = nullptr;
	DebugInfo                  m_debug;
	bool                       m_contains_encoded_draw = false;
	HW::Context*               m_registers             = nullptr;
	HW::UserConfig*            m_user_config           = nullptr;
	HW::Shader*                m_shaders               = nullptr;

	friend class CommandScheduler;
};

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(uint64_t submit_id, CommandBuffer& buffer, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);
	void DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr,
	                      uint32_t mode);

	struct Statistics {
		uint64_t draws      = 0;
		uint64_t dispatches = 0;
	};
	[[nodiscard]] const Statistics& GetStatistics() const noexcept { return m_statistics; }

	// Shared with the Vulkan renderer (renderer/pipeline/descriptors.cpp).
	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void FindBuffers(PreparedBindings& bindings);
	void RebindBuffers(PreparedBindings& bindings);
	void RebindImages(PreparedBindings& bindings);

private:
	void DrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args);
	void DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		std::optional<PreparedBindings> pixel;
	};

	// The attachments of a draw, in the state it renders them.
	struct RenderTargets {
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
		uint32_t                    color_count = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE depth {};
		bool                        has_depth     = false;
		D3D12_CLEAR_FLAGS           depth_clears  = {};
		float                       depth_clear   = 0.0f;
		uint8_t                     stencil_clear = 0;
		uint32_t                    width         = 0;
		uint32_t                    height        = 0;
	};

	// Shared with the Vulkan renderer (renderer/pipeline/descriptors.cpp and
	// renderer/{color,depth}RenderTarget.cpp).
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource& resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value);
	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slice_offset, uint32_t render_target_slot,
	                              bool ignore_target_mask = false, bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	// Shared with the Vulkan renderer (renderer/renderDraw.cpp).
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                          uint32_t         render_target_slice_offset,
	                                          DrawRenderState& state);
	void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer, const DrawCallInfo& draw,
	                         DrawRenderState& state, vk::PrimitiveTopology topology,
	                         const DrawEmitInfo& emit, const DrawIndexBufferSource& index_source,
	                         bool primitive_restart_enable);
	[[nodiscard]] bool ResolveColorTargets(CommandBuffer& buffer,
	                                       uint32_t       render_target_slice_offset);
	// `read_only_depth` binds the depth target read-only, for a draw that also samples it.
	[[nodiscard]] RenderTargets AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                                 uint32_t color_count, RenderDepthInfo& depth,
	                                                 bool read_only_depth);
	void               BindImage(ImageId id, bool storage);
	void               BindRenderTarget(ImageId id);
	void               ResetBindings();
	// Shared with the Vulkan renderer (renderer/renderCompute.cpp).
	[[nodiscard]] bool TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                              const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
	                                              CommandBuffer& command, uint32_t group_x,
	                                              uint32_t group_y, uint32_t group_z, uint32_t mode);

	void RecordDispatch(CommandBuffer& buffer, const PipelineCache::Pipeline& pipeline,
	                    PreparedBindings& bindings, const ShaderComputeInputInfo& input_info,
	                    uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);
	// Group counts come from `args` (x, y, z at `args_offset`).
	void RecordDispatchIndirect(CommandBuffer& buffer, const PipelineCache::Pipeline& pipeline,
	                            PreparedBindings& bindings, const ShaderComputeInputInfo& input_info,
	                            const Buffer& args, uint64_t args_offset);
	// The command signature of indirect dispatches of `pipeline`.
	[[nodiscard]] ID3D12CommandSignature* DispatchSignature(const PipelineCache::Pipeline& pipeline);
	// Sets the root signature and arguments of `pipeline` for the prepared stages, in order.
	void CommitBindings(CommandBuffer& buffer, bool graphics, const PipelineCache::Pipeline& pipeline,
	                    std::span<PreparedBindings* const> stages,
	                    std::span<const uint8_t>           runtime_data);

	RenderContext&              m_context;
	Statistics                  m_statistics;
	std::vector<ImageId>        m_bound_images;
	// Reused across draws and dispatches (their vectors keep their capacity).
	GraphicsBindings            m_graphics_bindings;
	PreparedBindings            m_compute_bindings;
	D3D12_CPU_DESCRIPTOR_HANDLE m_null_render_target {}; // fills unused color slots
	// The depth target of the draw being recorded while the draw also samples it; its state
	// is then ReadOnlyDepthState for both uses.
	ImageId                                m_read_only_depth;
	static constexpr D3D12_RESOURCE_STATES ReadOnlyDepthState =
	    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
	    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	// Arguments of the indirect dispatch being recorded (see RecordDispatchIndirect).
	std::unique_ptr<Buffer> m_indirect_args;
	// By root signature; null for pipelines without runtime data.
	std::unordered_map<ID3D12RootSignature*, D3D12::ComPtr<ID3D12CommandSignature>>
	    m_dispatch_signatures;

	friend class CommandProcessor;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDER_H_
