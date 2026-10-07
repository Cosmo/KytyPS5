#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUBACKEND_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUBACKEND_H_

// Renderer types used by shared (backend-independent) code. The backend is chosen at build time (KYTY_GPU_BACKEND in CMake); both provide the same class
// names.

#include "graphics/host_gpu/renderArgs.h"

#if defined(KYTY_GPU_BACKEND_D3D12)
#include "graphics/host_gpu/d3d12/renderContext.h"
#else
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#endif

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUBACKEND_H_
