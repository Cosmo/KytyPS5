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

## Images

The shared texture cache (`renderer/cache/textureCache.*`) tracks guest images, uploads them when the guest wrote them and downloads them when the GPU did. `d3d12/image.cpp` is the D3D12 `Image` (the host resource, its views and its state): images are placed in the pooled heaps, and render and depth targets are discarded on first use as D3D12 requires. D3D12 views must stay in the format family of their resource, so an image seen in formats of different families keeps one resource per family (created typeless, so that the formats of a family cast freely) and copies between them when another family is used, through buffers (a footprint of the source format out, one of the target format in). The shared view cache normalizes sampled and attachment views to one description; D3D12 creates the descriptor each use needs.

Guest surfaces are tiled. The tiling kernels (`host_gpu/shaders/gpu_tiler_*.comp`, the same ones the Vulkan renderer uses) turn SPIR-V to DXIL once, through `computeKernels.*` with one root signature for all of them (a read-only buffer, a buffer, a constant buffer, push constants as root constants, the translator's runtime data as root constants). `tiler.*` is the D3D12 tile manager: it detiles guest data into linear buffer data and back on the GPU and copies rows for the images that are linear.

Samplers (`samplerCache.*`) are descriptors in a sampler heap, one per distinct guest sampler; unnormalized coordinates are not supported (reported once). Reinterpreting color as multisampled depth is not supported yet.

## Pipelines

`d3d12/pipelineCache.*` turns the shared program cache's SPIR-V into DXIL (`dxilCompiler.*`, translations kept by content) and DXIL into pipelines. A graphics pipeline is made from the vertex and pixel stage translated together with `spirv_to_dxil_linked` (the DXIL signatures of consecutive stages only match when the stages are linked); rect lists get a generated geometry shader, merged geometry stages run as mesh shaders where the device has shader model 6.5. The root signature follows the stages' bindings: root constants for the push data and for the translator's runtime data (`b0` in spaces 1000 and 1001), a table of views (raw buffer views, image views, one space per binding kind) and a table of samplers; a sampler array that is also used for depth-compare sampling is declared a second time as comparison samplers, filled with the same descriptors. Pipelines are kept per key (programs, targets, blend, depth and stencil state, vertex layout); Release builds save the translations and an `ID3D12PipelineLibrary` of the pipelines in `_PipelineCache`, so a later run skips translation, validation and compilation. Whatever cannot be translated, linked or created (an unsupported vertex format, a root signature the device refuses, tessellation) is reported once and the draws and dispatches that need it are skipped.

## Draws, dispatches and presentation

`renderer/renderDraw.cpp` and `renderCompute.cpp` (shared) prepare guest draws and dispatches: resources, targets, the pipeline state; `d3d12/renderCommands.cpp` and `renderExecutor.cpp` record them: render and depth targets in their states (a depth target that the same draw samples is bound read-only, a limit D3D12 puts on feedback loops), vertex and index buffers, the descriptor tables, the viewport (D3D12 has no negative height: the vertex stage flips Y instead, and clip-space depth in [-w, w] is mapped to [0, w] there), triangle fans and legacy quads as triangle lists built from the guest indices, indirect dispatches through a command signature whose first arguments set the translator's runtime data. Each draw and dispatch is bracketed by the GPU trace.

The presenter (`presenter.cpp`) copies the guest's flip image into a texture of its own and draws it onto the composition swap chain with a fullscreen triangle (`gpu_blit_present.frag`; D3D12 has no blit); a second guest layer is blended over it as premultiplied alpha. The swap chain is clamped to 3840 x 2160 (a console loses its device when asked for more). A file `d3d12-dump-frames.txt` in LocalState makes every 600th presented frame be saved as `frame-<n>.bmp`, to see what a run shows.
