#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/d3d12/masterSemaphore.h"
#include "graphics/host_gpu/d3d12/render.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

struct ID3D12CommandAllocator;
struct ID3D12QueryHeap;
struct ID3D12Resource;

namespace Libs::Graphics {

// Records guest work into one command list and submits it to the direct queue. Each submission
// signals the next MasterSemaphore tick. Deferred operations run once their tick completes;
// priority operations (interrupts, flips) run on a dedicated thread as soon as it does.
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
	// Batch thresholds for RELEASE_MEM writes, interrupts and draws; see the Vulkan scheduler.
	void CompleteReleaseMemWrite();
	void CompleteReleaseMemInterrupt();
	void CompleteDraw();

	CommandBuffer& BeginCommand();
	uint64_t       Submit();
	void           Shutdown();
	void           Wait(uint64_t tick);
	void           PopPendingOperations();
	void           DrainPriorityOperations();
	void           WaitPriorityOperations(uint64_t tick);
	void           DeferOperation(Common::UniqueFunction<void>&& operation);
	void           DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void               CheckActive() const;
	CommandBuffer&     Current();
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

	// The GPU's time for each command list (WorkCounters::gpu_ns): a timestamp at its start and one at its end, read once its tick completed.
	// A list submitted while all slots wait for their ticks goes untimed.
	struct TimedList {
		uint64_t tick = 0;
		uint32_t slot = 0;
	};
	static constexpr uint32_t TimingSlots = 256;

	void BeginNext();
	void PriorityOperationsThread(std::stop_token stop);
	void RunOperation(Common::UniqueFunction<void>&& operation);
	void StartTiming();
	void EndTiming();
	void ReadTimings();

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	AllocatorPool                m_allocators;
	ID3D12GraphicsCommandList*   m_list = nullptr;
	ID3D12GraphicsCommandList6*  m_mesh_list = nullptr;
	CommandBuffer                m_command;
	uint32_t                     m_recorded_release_mem_writes     = 0;
	uint32_t                     m_recorded_release_mem_interrupts = 0;
	uint32_t                     m_recorded_draws                  = 0;
	std::queue<PendingOperation> m_pending_operations;
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
	ID3D12QueryHeap*             m_timestamps           = nullptr;
	ID3D12Resource*              m_timestamp_results    = nullptr;
	const uint64_t*              m_timestamp_values     = nullptr;
	double                       m_ns_per_timestamp     = 0.0;
	std::queue<TimedList>        m_timed_lists;
	uint32_t                     m_next_timing_slot     = 0;
	std::optional<uint32_t>      m_timing_slot; // the current list's
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_COMMANDSCHEDULER_H_
