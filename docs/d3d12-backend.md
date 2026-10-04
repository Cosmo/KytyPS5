# D3D12 backend

The UWP app (Windows and Xbox Dev Mode) renders through Direct3D 12 at feature level 11_0. The Vulkan renderer stays the reference on the desktop. One CMake option picks the renderer; each build contains exactly one.

| Build | `KYTY_GPU_BACKEND` |
|---|---|
| Linux, macOS, Windows desktop | `Vulkan` (default) |
| UWP (`KYTY_BUILD_UWP`) | `D3D12` (forced) |

Everything above the host GPU API is shared: the Sony libraries, PM4 command processing, the guest register state, the shader recompiler (guest shader to IR to SPIR-V), VideoOut. Shared code reaches the renderer through `graphics/host_gpu/gpuBackend.h`, which includes the backend's `RenderContext`, `CommandBuffer`, `RenderExecutor` and caches. There is no runtime abstraction layer. `graphics/host_gpu/d3d12/` is the D3D12 implementation; the Vulkan renderer's sources are not compiled in the D3D12 build, except the parts that do not call Vulkan.

## Shaders

The recompiler's SPIR-V is translated to DXIL with Mesa's `spirv_to_dxil` and signed with the Windows SDK's `dxil.dll` (3rdparty/spirv_to_dxil describes the translator and how to build it). A guest shader is translated to SPIR-V once per program, static state and resource specialization by `graphics/shader/programCache.*`, which both backends share; the D3D12 pipeline cache (`d3d12/pipelineCache.*`) turns each program into DXIL once, so nothing of this runs per draw. A shader that cannot be translated or fails validation is reported once in the log and skipped: the pipelines that need it are not created and the draws and dispatches that use them do nothing.

The device sets the shader model: 6.8 on a PC, 6.4 on the Series X. The DXIL validator that signs the shaders is the SDK's `dxil.dll`, loaded from the package (`LoadPackagedLibrary` in the UWP app).

Register mapping (the SPIR-V emitter sets descriptor set and binding for D3D12):

| Guest data | D3D12 register |
|---|---|
| descriptor binding | `space = IR::NativeBinding(stage, kind)`, register 0 |
| push constants | root constants, `b0 space1000` |
| spirv_to_dxil runtime data | root CBV, `b0 space1001` |

Compute shaders may declare at most 32 KiB of group shared memory in D3D12; the shader cache clamps the guest's LDS to it and warns once.

## Self test

`"d3d12_selftest": true` in `LocalState\kyty-uwp.json` makes the app, before it starts a game, translate and sign the renderer's built-in compute shaders (the tiling kernels) on a device of its own, create compute pipelines for them and log the result with timings (`D3D12 self test:` lines in `LocalState\kyty-emulator.txt`). It needs no guest, so it shows whether the shader route works on a console.

## Buffers and guest memory

Guest memory is cached in host buffers by the shared buffer cache (`renderer/cache/bufferCache.*`: page tracking, GPU-modified ranges, upload and download through staging rings, a least-recently-used collection of buffers). `d3d12/buffer.*` is the D3D12 `Buffer`: every buffer is suballocated from pooled heaps with AMD's D3D12 Memory Allocator (`3rdparty/D3D12MemoryAllocator`, a submodule; custom pools of upload and readback heaps that also allow unordered access, so shaders can bind them directly). A buffer that mirrors guest memory has a persistent raw view in the first 65,536 slots of the shader-visible descriptor heap (`descriptorHeap.*`).

DXIL has no pointers, so a shader that reads guest memory by address reads a page table (`bdaPageTable.*`): per cached 16 KiB guest page, the descriptor slot of the page's buffer (bits 32 to 55) and the page's offset in it (bits 0 to 31); `spirv_to_dxil` turns the address into a view from the heap and an offset. A page that is not cached reads zeros and is recorded in a fault buffer, which a compute pass collects at the next garbage collection so that the page is cached for later work. The table is flat (8 bytes per page of the 1.5 TiB guest address space, 768 MiB of GPU memory); a two-level table would need a change in the shader emitter.

Command lists and fences: `commandScheduler.*` records into one direct command list and signals a timeline fence (`masterSemaphore.*`) at each submission; deferred operations run when their tick completes. Buffers decay to the COMMON state after every submission and are promoted implicitly, so each buffer tracks its state per tick. After a device removal the report (`D3D12::DeviceRemovedReport`) gives the reason, the unfinished operations (DRED) and, from the GPU trace (`gpuTrace.*`: a slot written before and after each draw and dispatch), the pipeline the GPU stopped in, whose shaders are saved.
