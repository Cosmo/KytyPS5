#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DESCRIPTORHEAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DESCRIPTORHEAP_H_

#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class CommandScheduler;
struct GraphicContext;

// A contiguous run of descriptors in a shader-visible heap.
struct DescriptorRange {
	D3D12_CPU_DESCRIPTOR_HANDLE cpu {};
	D3D12_GPU_DESCRIPTOR_HANDLE gpu {};
	uint32_t                    increment = 0;

	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const noexcept {
		return {cpu.ptr + static_cast<SIZE_T>(index) * increment};
	}
	[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const noexcept {
		return {gpu.ptr + static_cast<UINT64>(index) * increment};
	}
};

// Shader-visible view and sampler heaps. Descriptors are written per use into a ring and stay
// valid until the GPU completed the tick that allocated them. The heaps are bound to each
// command list on its first allocation.
//
// The first BindlessViews entries of the view heap hold persistent descriptors that shaders
// index directly (the buffers guest memory addresses resolve to, see BdaPageTable). Entry 0 is a
// null raw buffer view, so an unresolved address reads zeros.
class DescriptorHeap {
public:
	DescriptorHeap(GraphicContext& graphics, CommandScheduler& scheduler);
	~DescriptorHeap();
	KYTY_CLASS_NO_COPY(DescriptorHeap);

	static constexpr uint32_t BindlessViews = 1u << 16;

	[[nodiscard]] DescriptorRange AllocateViews(CommandBuffer& command, uint32_t count);
	// A persistent view slot (never 0); free it once no recorded work can use it any more.
	[[nodiscard]] uint32_t                    AllocateBindlessView();
	void                                      FreeBindlessView(uint32_t index);
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE BindlessView(uint32_t index) const noexcept;
	[[nodiscard]] DescriptorRange AllocateSamplers(CommandBuffer& command, uint32_t count);
	// A non-shader-visible view slot for clear operations, which read it while recording.
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE ScratchView() const noexcept;

	// Persistent non-shader-visible descriptors (views of images). Their contents are consumed
	// when recorded or copied into the shader-visible heap, so a freed slot is reusable at once.
	[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE AllocateCpu(D3D12_DESCRIPTOR_HEAP_TYPE type);
	void FreeCpu(D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE handle);

private:
	class Ring {
	public:
		// Per-use descriptors are allocated after the first `reserved` entries.
		Ring(GraphicContext& graphics, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity,
		     uint32_t reserved = 0);
		~Ring();
		KYTY_CLASS_NO_COPY(Ring);

		[[nodiscard]] ID3D12DescriptorHeap*  Heap() const noexcept { return m_heap; }
		[[nodiscard]] const DescriptorRange& Base() const noexcept { return m_base; }
		[[nodiscard]] DescriptorRange       Allocate(CommandScheduler& scheduler, uint32_t count);

	private:
		struct Use {
			uint64_t tick  = 0;
			uint32_t begin = 0;
			uint32_t end   = 0;
		};

		ID3D12DescriptorHeap* m_heap = nullptr;
		DescriptorRange       m_base;
		uint32_t              m_capacity = 0;
		uint32_t              m_first    = 0;
		uint32_t              m_head     = 0;
		std::deque<Use>       m_uses;
	};

	class CpuPool {
	public:
		CpuPool() = default;
		~CpuPool();
		KYTY_CLASS_NO_COPY(CpuPool);

		[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Allocate(ID3D12Device*              device,
		                                                   D3D12_DESCRIPTOR_HEAP_TYPE type);
		void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle) { m_free.push_back(handle); }

	private:
		std::vector<ID3D12DescriptorHeap*>       m_heaps;
		std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_free;
	};

	void Bind(CommandBuffer& command);

	GraphicContext&       m_graphics;
	CommandScheduler&     m_scheduler;
	Ring                  m_views;
	Ring                  m_samplers;
	ID3D12DescriptorHeap* m_scratch    = nullptr;
	uint64_t              m_bound_tick = 0;
	struct FreedView {
		uint32_t index = 0;
		uint64_t tick  = 0;
	};
	Common::Mutex         m_bindless_mutex;
	uint32_t              m_bindless_next = 1;
	std::deque<FreedView> m_bindless_freed;
	Common::Mutex         m_cpu_mutex;
	std::array<CpuPool, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> m_cpu_pools;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_DESCRIPTORHEAP_H_
