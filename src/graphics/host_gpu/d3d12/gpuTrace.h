#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GPUTRACE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GPUTRACE_H_

#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::D3D12 {

// A pipeline as the trace knows it: its name and its shaders, saved when the GPU hangs in it.
struct TracedPipeline {
	std::wstring                                               name;
	std::vector<const DxilShader*>                             dxil;
	std::vector<std::shared_ptr<const std::vector<uint32_t>>> spirv;
};

// Breadcrumbs for GPU hangs. Around each draw and dispatch the GPU writes to a slot in
// CPU-visible memory: "started" before, "finished" after. After a device removal the oldest slot
// still at "started" names the work the GPU was stuck in; DRED only says which kind of operation
// it was. Later work can show as started too, since it may begin while the stuck work runs.
class GpuTrace {
public:
	void Create(ID3D12Device* device);
	void Destroy();

	// Around a draw or dispatch with `pipeline`, which must outlive the trace.
	[[nodiscard]] uint32_t Begin(ID3D12GraphicsCommandList* list, const TracedPipeline* pipeline);
	void                   End(ID3D12GraphicsCommandList* list, uint32_t slot);

	// The pipelines of the work started but not finished, oldest first.
	[[nodiscard]] std::vector<const TracedPipeline*> Unfinished(size_t max_count) const;

private:
	static constexpr uint32_t SLOTS    = 1u << 16u;
	static constexpr uint32_t STARTED  = 1;
	static constexpr uint32_t FINISHED = 2;

	void Write(ID3D12GraphicsCommandList* list, uint32_t slot, uint32_t value,
	           D3D12_WRITEBUFFERIMMEDIATE_MODE mode);

	ID3D12Resource*                                     m_buffer = nullptr;
	const volatile uint32_t*                            m_values = nullptr;
	std::atomic<uint32_t>                               m_next   = 0;
	std::unique_ptr<std::atomic<const TracedPipeline*>[]> m_pipelines;

	std::mutex m_lists_mutex;
	std::unordered_map<ID3D12GraphicsCommandList*, ComPtr<ID3D12GraphicsCommandList2>> m_lists;
};

GpuTrace& GetGpuTrace();

} // namespace Libs::Graphics::D3D12

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_GPUTRACE_H_
