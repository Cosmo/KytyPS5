#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDERCONTEXT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDERCONTEXT_H_

#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/d3d12/bufferCache.h"
#include "graphics/host_gpu/d3d12/commandScheduler.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/d3d12/pipelineCache.h"
#include "graphics/host_gpu/d3d12/render.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "kernel/eventQueue.h"

#include <memory>
#include <shared_mutex>
#include <vector>

namespace Libs::VideoOut {
class VideoOutDriver;
}

namespace Libs::Graphics {

class GuestGpu;

// Owns the D3D12 renderer's scheduler, executor and caches, and receives the guest memory events.
class RenderContext {
public:
	explicit RenderContext(GraphicContext& graphics);
	~RenderContext();
	KYTY_CLASS_NO_COPY(RenderContext);

	[[nodiscard]] GraphicContext&           GetGraphics() const noexcept { return m_graphics; }
	void                                    InitializeGpu(VideoOut::VideoOutDriver* video_out);
	void                                    ShutdownGpu();
	[[nodiscard]] GuestGpu&                 GetGpu() const;
	[[nodiscard]] VideoOut::VideoOutDriver& GetVideoOut() const;

	Common::Mutex&    GetMutex() { return m_mutex; }
	CommandScheduler& GetCommandScheduler() { return m_command_scheduler; }
	RenderExecutor&   GetRenderExecutor() { return m_render_executor; }
	BufferCache&      GetBufferCache() { return m_buffer_cache; }
	TextureCache&     GetTextureCache() { return m_texture_cache; }
	PipelineCache&    GetPipelineCache() { return m_pipeline_cache; }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] bool InvalidateMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector() {}

	void AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id);
	void TriggerInterrupt(int event_id, uint32_t context_id);

private:
	struct InterruptEqRegistration {
		LibKernel::EventQueue::KernelEqueue eq       = LibKernel::EventQueue::KERNEL_EQUEUE_INVALID;
		int                                 event_id = 0;
	};

	GraphicContext&           m_graphics;
	Common::Mutex             m_mutex;
	RenderExecutor            m_render_executor;
	CommandScheduler          m_command_scheduler;
	PipelineCache             m_pipeline_cache;
	BufferCache               m_buffer_cache;
	TextureCache              m_texture_cache;
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	std::unique_ptr<GuestGpu> m_gpu;
	VideoOut::VideoOutDriver* m_video_out = nullptr;

	Common::Mutex                        m_interrupt_mutex;
	std::vector<InterruptEqRegistration> m_interrupt_eqs;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_RENDERCONTEXT_H_
