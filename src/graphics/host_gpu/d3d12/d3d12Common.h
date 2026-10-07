#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_D3D12COMMON_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_D3D12COMMON_H_

// Windows and D3D12 headers for the backend's implementation files only (the backend's public headers declare the D3D12 types they use).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace Libs::Graphics::D3D12 {

using Microsoft::WRL::ComPtr;

// Exits with the failing call and HRESULT; a D3D12 failure here is unrecoverable.
void Check(HRESULT result, const char* operation);

} // namespace Libs::Graphics::D3D12

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_D3D12COMMON_H_
