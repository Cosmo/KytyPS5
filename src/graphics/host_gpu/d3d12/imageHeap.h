#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_IMAGEHEAP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_IMAGEHEAP_H_

namespace Libs::Graphics {

class RenderExecutor;

// The D3D12 renderer keeps no image tables resident (its ShaderProgramCache has no
// ImageHeapSource): shaders that index guest image tables by GPU-computed offsets search them as
// before, and no shader uses an image heap. The shared draw and dispatch code calls these.
class ImageHeap {
public:
	void BeginDraw() {}
	void Rebind(RenderExecutor& /*executor*/) {}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_IMAGEHEAP_H_
