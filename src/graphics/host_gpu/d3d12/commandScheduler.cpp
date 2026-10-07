#include "graphics/host_gpu/d3d12/commandScheduler.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"

#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

CommandBuffer::CommandBuffer(CommandScheduler& scheduler): m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

ID3D12GraphicsCommandList* CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());
	return m_list;
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug = {op, submit_id, {arg0, arg1, arg2, arg3}, arg4};
}

void CommandBuffer::GlobalMemoryBarrier() const {
	// Resource state transitions order everything else; a UAV barrier without a resource orders the writes of all unordered-access resources.
	D3D12_RESOURCE_BARRIER barrier {};
	barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = nullptr;
	Handle()->ResourceBarrier(1, &barrier);
}

CommandScheduler::AllocatorPool::AllocatorPool(GraphicContext& graphics, MasterSemaphore& master): m_graphics(graphics), m_master(master) {}

CommandScheduler::AllocatorPool::~AllocatorPool() {
	for (auto* allocator: m_allocators) {
		allocator->Release();
	}
}

ID3D12CommandAllocator* CommandScheduler::AllocatorPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
        for (size_t index = begin; index < end; ++index) {
            if (gpu_tick >= m_ticks[index]) {
                return index;
            }
        }
        return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (found) {
		D3D12::Check(m_allocators[*found]->Reset(), "reset command allocator");
	} else {
		ID3D12CommandAllocator* allocator = nullptr;
		D3D12::Check(m_graphics.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
		m_allocators.push_back(allocator);
		m_ticks.push_back(0);
		found = m_allocators.size() - 1;
	}
	m_ticks[*found] = m_master.CurrentTick();
	m_hint          = (*found + 1) % m_ticks.size();
	return m_allocators[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics), m_allocators(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {}

CommandScheduler::~CommandScheduler() {
	Shutdown();
	if (m_list != nullptr) {
		m_list->Release();
	}
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			// A priority callback cannot join its own runner; the owning thread finishes the shutdown.
			EXIT_IF(m_operation_state == OperationState::Open);
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() || m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);
	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::Flush() {
	Submit();
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	KYTY_PROFILER_FUNCTION();
	const auto tick = Submit();
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	KYTY_PROFILER_FUNCTION();
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick);
		BeginNext();
	} else {
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() || !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_pending_operations.push({std::move(operation), CurrentTick()});
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock, [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_priority_operations.push({std::move(operation), CurrentTick()});
		lock.unlock();
		m_operation_available.notify_one();
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock, [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] { return stop.stop_requested() || !m_priority_operations.empty(); });
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at = !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	auto* allocator = m_allocators.Commit();
	if (m_list == nullptr) {
		D3D12::Check(m_graphics.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&m_list)), "CreateCommandList");
	} else {
		D3D12::Check(m_list->Reset(allocator, nullptr), "reset command list");
	}
	m_command.m_list                  = m_list;
	m_command.m_contains_encoded_draw = false;
	return m_command;
}

uint64_t CommandScheduler::Submit() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(m_command.IsInvalid());
	D3D12::Check(m_list->Close(), "close command list");

	uint64_t tick = 0;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		tick                     = m_master.NextTick();
		ID3D12CommandList* lists = m_list;
		m_graphics.queue->ExecuteCommandLists(1, &lists);
		D3D12::Check(m_graphics.queue->Signal(m_master.Handle(), tick), "signal the GPU timeline");
	}

	m_command.m_list = nullptr;
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
