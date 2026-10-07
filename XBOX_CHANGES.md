# Xbox changes to KytyPS5

KytyPS5 (upstream `KytyPS5/KytyPS5` at `f90a8b15`) plus what it takes to run on the Xbox Series X: a UWP app with a D3D12 renderer. This branch (`xbox-on-upstream-2026-10-26-f90a8b15`) has one commit per topic, and each commit adds its row below, so the series can be read, tried and moved to a newer upstream one commit at a time.

The "Checked" column says how a change was checked when it was made, before the work was regrouped into this series.

## Rules

- One commit per topic: it says what it changes and adds its row here.
- No local paths, game names or title ids.
- Where in doubt, upstream is right.

## Changes

| Change | What | Upstream files touched | Checked |
| --- | --- | --- | --- |
| UWP app: launcher | New folder `src/uwp`, its own CMake project: WinUI 2 launcher with a game library (game folders and `.zar` archives, also on the Xbox's USB drive through the `*FromApp` file calls), settings page, game menu, overlay, four Xbox controllers with the PS5 mapping (listed by model, with the Xbox's button symbols on a page of their own), a trophies page, `uwp.ps1` for building, packaging and the Device Portal | none | PC and Series X: works with the controller |
| UWP app: the emulator inside the app | `KYTY_BUILD_UWP` compiles the emulator into the app: `*FromApp` file and memory functions (never read-write-execute; a look-up or read the Xbox's USB drive refuses for a moment is asked again), a backend seam with a placeholder D3D12 renderer, the Xbox controller as the guest's pad (View + right stick sets the pad's angle for its motion sensors, up to 180 degrees), guest audio through XAudio2, Roboto in the package for the games' text, `kyty://run` activation, the frame rate in the log | root and `3rdparty` `CMakeLists.txt`, `common/platform/sysWindows*`, `common/{archive,hostException}.cpp`, `kernel/memory*`, `libs/{audio,agc,controller}.*`, `libs/libFont.cpp`, `libs/libAmpr.cpp`, `common/emulatorConfig.*`, the command processor's and renderer's entry points | PC and Series X: games boot, with sound |
| Lazy guest memory | Guest direct and flexible memory is reserved and each 64 KiB window committed on first touch, instead of one 13.8 GiB section the Series X cannot reserve (`src/xbox/lazyRegions.h`, `HandleCommitFault`, `EnsureGuestCommitted` before host calls). Always on in the UWP app, `KYTY_LAZY_MEMORY=1` on the desktop | `kernel/memory*`, `kernel/pthread.cpp`, `libs/{libKernel,network,agc}.cpp`, `loader/runtimeLinker.cpp`, `common/platform/sysWindowsVirtual.*`, `host_gpu/pageManager.*` | PC and Series X: games run with far less memory committed |
| Renderer: shared logic | The guest shader translation and cache (`graphics/shader/programCache.*`) and the renderer logic that does not call Vulkan (caches, bindings, render targets, draw and dispatch preparation, pipeline state, tiling dispatches) are shared by both backends; the Vulkan calls move to Vulkan-only files next to them. Upstream's later edits of the moved code are carried over | most of `host_gpu/renderer/`, `shader/rectListShader.*`, root `CMakeLists.txt` | Desktop: built, not run |
| D3D12 renderer | `host_gpu/d3d12/`: SPIR-V to DXIL through Mesa's `spirv_to_dxil`, pipelines, buffers in pooled heaps (D3D12 Memory Allocator), images, samplers, GPU tiling, draws, dispatches, presentation; upstream's user clip planes (in root constants), ordered-append dispatches (one group at a time, each with its group ID) and alpha-to-coverage. Follows the Series X's limits (shader model 6.4, no barycentrics, mesh shaders or 16-bit ops; `xbox_gpu_limits` applies them on a PC); two-level BDA table (32 MB instead of 768 MB); R8 and R16 de-tiling without atomics | `common/emulatorConfig.*`, D3D12-only paths of the SPIR-V emitter, `renderer/cache/{bdaPageTable,bufferCache}.*`, `shader/programCache.*`, root `CMakeLists.txt`, `.gitmodules` | PC and Series X: 3D games draw correctly, videos play |
| UWP app: running games | The controller keeps working in a game, and the game's rumble, adaptive triggers (on the Impulse Triggers) and haptic feedback (soft bumps on the small motor) come back to it; the overlay shows shaders, pipelines and the output and render resolution; the pipeline cache is saved when a game ends (and can be kept across builds: a developer setting); 16:9 picture on a PC; guest code is executable before it runs; the app restarts itself when host memory sits in a range the game asks for; the guest's red zone is protected (`--redzone`) | `common/platform/sysWindowsVirtual.*`, `kernel/memory*`, `loader/runtimeLinker.cpp`, `libs/{audio,controller}.*`, `common/emulatorConfig.*`, `renderer/pipeline/pipelineCacheFile.*` | Series X: games that crashed at start now start; rumble, triggers and haptics work |

## Notes on running the CPU tests

- The tests that start the guest memory (`virtual_memory_allocation`, `guest_red_zone_patcher`, the compute tests with a context) each reserve 13.8 GB of address space on the desktop. Run in parallel (`ctest -j 4`), several of them fail with "could not reserve 13824 MB" when the PC's commit limit is reached. This is not a bug in the code: run them one at a time (`ctest -j 1`, or `ctest --rerun-failed -j 1`) and they pass. The UWP app avoids this with lazy memory (`KYTY_LAZY_MEMORY=1` on the desktop).
- `shader_recompiler_compute` and `pm4_context_state` fail in `Pm4NativeTargetGeometry` ("legacy register holes"); they failed the same way before these changes.

## Research guards the D3D12 renderer does not apply

Guards G1 to G15 of the research are applied as `docs/d3d12-backend.md` lists, except these:

- **G6** (loops with `break` need `[loop]`): there is no HLSL to annotate, because shaders go through Mesa's `spirv_to_dxil`. Not applied, not seen to fail in the test game.
- **G3** (uint view of a BC texture) and **G7** (stencil plane copy to a buffer): not handled specially; not run on the console.
- **G5** and **G13**: nothing in the shaders of the test game needed them, so they are neither applied nor tried.
- **Mesh shaders, tessellation, 64-bit image atomics, 96-bit textures**: the renderer reports them once and skips the work; the attempt's mesh-as-compute path is not ported.
- **Shader model 6.6 heap indexing** (the guest page table's descriptor index) is not confirmed on the console, which reports shader model 6.4: the table is created and filled there, but no shader that reads guest memory by address has been seen to run on it. A pipeline the device refuses for this is reported once and skipped.
