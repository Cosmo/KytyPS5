#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDER_H_

#include "common/common.h"
#include "graphics/host_gpu/renderArgs.h"

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12GraphicsCommandList6;

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
class RenderContext;
class CommandScheduler;

// The command list being recorded, with the guest register state it records from.
class CommandBuffer {
public:
	~CommandBuffer() = default;
	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const noexcept { return m_list == nullptr; }

	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0, uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	// D3D12 has no render passes; kept for the interface the scheduler shares with the Vulkan backend.
	void EndRendering() const {}
	// Makes all prior writes visible to all later reads and writes.
	void GlobalMemoryBarrier() const;

	[[nodiscard]] ID3D12GraphicsCommandList* Handle() const;
	// The same list, for recording mesh shader draws (GraphicContext::mesh_shaders).
	[[nodiscard]] ID3D12GraphicsCommandList6* MeshHandle() const;
	void                                     MarkEncodedDraw() noexcept { m_contains_encoded_draw = true; }
	[[nodiscard]] bool                       ContainsEncodedDraw() const noexcept { return m_contains_encoded_draw; }
	[[nodiscard]] GraphicContext&            GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&             GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&               GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&            GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&                GetShaders() const noexcept { return *m_shaders; }

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

// Runs the guest's draws and dispatches. For now it counts them; nothing is rendered.
class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(uint64_t submit_id, CommandBuffer& buffer, uint32_t thread_group_x, uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);
	void DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr, uint32_t mode);

	struct Statistics {
		uint64_t draws      = 0;
		uint64_t dispatches = 0;
	};
	[[nodiscard]] const Statistics& GetStatistics() const noexcept { return m_statistics; }

private:
	void DrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args);
	void DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args);

	RenderContext& m_context;
	Statistics     m_statistics;

	friend class CommandProcessor;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDER_H_
