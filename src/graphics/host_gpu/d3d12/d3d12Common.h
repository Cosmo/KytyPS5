#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_D3D12COMMON_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_D3D12COMMON_H_

// Windows and D3D12 headers for the backend's implementation files only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace Libs::Graphics::D3D12 {

using Microsoft::WRL::ComPtr;

// Exits with the failing call and HRESULT; D3D12 failures here are unrecoverable.
void Check(HRESULT result, const char* operation);

// After the device was removed: why, the GPU operations that didn't finish and, for a page fault,
// the address and the resources around it (DRED, enabled when the device is created).
[[nodiscard]] std::string DeviceRemovedReport();

} // namespace Libs::Graphics::D3D12

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_D3D12COMMON_H_
