#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/d3d12/masterSemaphore.h"
#include "graphics/host_gpu/d3d12/render.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

struct ID3D12CommandAllocator;

namespace Libs::Graphics {

// Records guest work into one command list and submits it to the direct queue; each submission signals the next MasterSemaphore tick. Deferred operations
// run once their tick completed; priority operations (interrupts, flips) run on a thread of their own as soon as it did.
class CommandScheduler {
public:
	CommandScheduler(RenderContext& context, GraphicContext& graphics);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void EndRendering() {}
	void Flush();
	void FlushAndWait();
	void Finish();

	CommandBuffer&            BeginCommand();
	uint64_t                  Submit();
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool             Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }

private:
	// Command allocators, each reusable once the GPU completed the tick that last used it.
	class AllocatorPool {
	public:
		AllocatorPool(GraphicContext& graphics, MasterSemaphore& master);
		~AllocatorPool();
		KYTY_CLASS_NO_COPY(AllocatorPool);

		ID3D12CommandAllocator* Commit();

	private:
		GraphicContext&                      m_graphics;
		MasterSemaphore&                     m_master;
		std::vector<ID3D12CommandAllocator*> m_allocators;
		std::vector<uint64_t>                m_ticks;
		size_t                               m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	void BeginNext();
	void PriorityOperationsThread(std::stop_token stop);
	void RunOperation(Common::UniqueFunction<void>&& operation);

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	AllocatorPool                m_allocators;
	ID3D12GraphicsCommandList*   m_list = nullptr;
	CommandBuffer                m_command;
	std::queue<PendingOperation> m_pending_operations;
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMMANDSCHEDULER_H_
