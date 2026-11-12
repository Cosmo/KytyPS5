# Build the public FSR 3.1.4 source with Kyty's UWP presentation and D3D12 entry points.
option(KYTY_ENABLE_FSR "Build FidelityFX FSR into the UWP app" ON)
set(KYTY_FSR_SDK_ROOT "$ENV{KYTY_FSR_SDK_ROOT}" CACHE PATH "Use a local FidelityFX SDK v1.1.4 source checkout")
if(NOT KYTY_ENABLE_FSR)
	return()
endif()

if(KYTY_FSR_SDK_ROOT)
	set(kyty_fidelityfx_source "${KYTY_FSR_SDK_ROOT}")
else()
	include(FetchContent)
	FetchContent_Declare(kyty_fidelityfx
		URL https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/zip/refs/tags/v1.1.4
		URL_HASH SHA256=9a19e1f20bf269163f052186ed563209c5a1d0f7b1cf7e567e643dee6cc61179
		DOWNLOAD_EXTRACT_TIMESTAMP FALSE
		SOURCE_SUBDIR sdk/include)
	FetchContent_MakeAvailable(kyty_fidelityfx)
	set(kyty_fidelityfx_source "${kyty_fidelityfx_SOURCE_DIR}")
endif()

function(kyty_add_fidelityfx target)
	set(sdk "${kyty_fidelityfx_source}/sdk")
	if(NOT EXISTS "${sdk}/tools/binary_store/FidelityFX_SC.exe" OR NOT EXISTS "${kyty_fidelityfx_source}/LICENSE.txt")
		message(FATAL_ERROR "KYTY_FSR_SDK_ROOT must point to the extracted FidelityFX SDK v1.1.4 source archive.")
	endif()

	set(staged "${CMAKE_BINARY_DIR}/fidelityfx/source")
	set(helpers "${staged}/include/FidelityFX/host/ffx_util.h")
	file(COPY "${sdk}/" DESTINATION "${staged}"
		PATTERN "ffx_util.h" EXCLUDE
		PATTERN "ffx_dx12.cpp" EXCLUDE
		PATTERN "ffx_fsr3.cpp" EXCLUDE
		PATTERN "CMakeCompileShaders.txt" EXCLUDE)
	file(READ "${sdk}/include/FidelityFX/host/ffx_util.h" source)
	string(REPLACE "#pragma once" "#pragma once\n\n#include <bit>" source "${source}")
	file(CONFIGURE OUTPUT "${helpers}" CONTENT "${source}" @ONLY)
	set(backend "${staged}/src/backends/dx12")

	# Keep PIX and AMD's desktop swapchain wrapper out of the app. Kyty owns the DXGI composition swapchain.
	file(READ "${sdk}/CMakeLists.txt" cmake)
	string(REPLACE "add_subdirectory(\${FFX_LIB_PATH}/pix)" "" cmake "${cmake}")
	string(REPLACE "\${CMAKE_HOME_DIRECTORY}/bin/ffx_sdk/" "\${CMAKE_CURRENT_BINARY_DIR}/lib/" cmake "${cmake}")
	file(CONFIGURE OUTPUT "${staged}/CMakeLists.txt" CONTENT "${cmake}" @ONLY)
	file(READ "${sdk}/src/backends/dx12/CMakeLists.txt" cmake)
	string(REGEX REPLACE "[^\n]*FrameInterpolationSwapchain/[^\n]*\n" "" cmake "${cmake}")
	string(REPLACE "\"/WX\" # warnings as errors" "" cmake "${cmake}")
	string(REPLACE " pixlib dxgi" " dxgi d3d12" cmake "${cmake}")
	file(CONFIGURE OUTPUT "${backend}/CMakeLists.txt" CONTENT "${cmake}" @ONLY)

	file(READ "${sdk}/src/backends/dx12/ffx_dx12.cpp" source)
	string(REPLACE "#define ENABLE_PIX_CAPTURES 1" "" source "${source}")
	string(REPLACE "= ffxSetFrameGenerationConfigToSwapchainDX12;"
		"= [](const FfxFrameGenerationConfig*) -> FfxErrorCode { return FFX_OK; };" source "${source}")
	string(FIND "${source}" "        //Query D3D12SerializeRootSignature" begin)
	string(FIND "${source}" "    ID3D12RootSignature* dx12RootSignature = reinterpret_cast<ID3D12RootSignature*>(outPipeline->rootSignature);" end)
	if(begin LESS 0 OR end LESS begin)
		message(FATAL_ERROR "Unsupported FidelityFX source. Use SDK v1.1.4.")
	endif()
	string(SUBSTRING "${source}" 0 ${begin} prefix)
	string(SUBSTRING "${source}" ${end} -1 suffix)
	file(CONFIGURE OUTPUT "${backend}/ffx_dx12.cpp" CONTENT "${prefix}
        HRESULT result = D3D12SerializeRootSignature(&dx12RootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, &outBlob, &errorBlob);
        if (errorBlob) errorBlob->Release();
        if (SUCCEEDED(result)) {
            result = dx12Device->CreateRootSignature(0, outBlob->GetBufferPointer(), outBlob->GetBufferSize(), IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature**>(&outPipeline->rootSignature)));
        }
        if (outBlob) outBlob->Release();
        if (FAILED(result)) return FFX_ERROR_BACKEND_API_ERROR;
    }

${suffix}" @ONLY)

	# The SDK's dependency list starts with the name of its output variable. Ninja treats it as a file.
	set(shader_cmake "${staged}/include/FidelityFX/gpu/CMakeCompileShaders.txt")
	file(READ "${sdk}/include/FidelityFX/gpu/CMakeCompileShaders.txt" cmake)
	string(REPLACE "list(APPEND PERMUTATION_OUTPUTS " "list(APPEND shader_headers " cmake "${cmake}")
	string(REPLACE "\${PERMUTATION_OUTPUTS} \${PERMUTATION_OUTPUTS} PARENT_SCOPE"
		"\${PERMUTATION_OUTPUTS} \${shader_headers} PARENT_SCOPE" cmake "${cmake}")
	file(CONFIGURE OUTPUT "${shader_cmake}" CONTENT "${cmake}" @ONLY)

	# The legacy FSR3 wrapper leaves its mode flags unset and always asks the upscaler for shared
	# images. Use the interpolation component's descriptions when no upscaler was created.
	set(fsr3 "src/components/fsr3/ffx_fsr3.cpp")
	file(READ "${sdk}/${fsr3}" source)
	string(REPLACE "    // ensure upscalingOnly"
		"    contextPrivate->upscalingOnly = upscalingOnly;\n    contextPrivate->interpolationOnly = interpolationOnly;\n\n    // ensure upscalingOnly" source "${source}")
	string(FIND "${source}" "    // set up FSR3Upscaler resources" begin)
	string(FIND "${source}" "        wchar_t Name[256]" end)
	if(begin LESS 0 OR end LESS begin)
		message(FATAL_ERROR "Unsupported FidelityFX source. Use SDK v1.1.4.")
	endif()
	string(SUBSTRING "${source}" 0 ${begin} prefix)
	string(SUBSTRING "${source}" ${end} -1 suffix)
	set(source "${prefix}    // Create the shared depth and motion images using the active component.
    {
        FfxFsr3UpscalerSharedResourceDescriptions fs3UpscalerResourceDescs = {};
        if (interpolationOnly) {
            FfxFrameInterpolationSharedResourceDescriptions interpolationDescs = {};
            FFX_VALIDATE(ffxFrameInterpolationGetSharedResourceDescriptions(&contextPrivate->fiContext, &interpolationDescs));
            fs3UpscalerResourceDescs.dilatedDepth = interpolationDescs.dilatedDepth;
            fs3UpscalerResourceDescs.dilatedMotionVectors = interpolationDescs.dilatedMotionVectors;
            fs3UpscalerResourceDescs.reconstructedPrevNearestDepth = interpolationDescs.reconstructedPrevNearestDepth;
        } else {
            FFX_VALIDATE(ffxFsr3UpscalerGetSharedResourceDescriptions(&contextPrivate->upscalerContext, &fs3UpscalerResourceDescs));
        }

${suffix}")
	string(REPLACE "    contextPrivate->backendInterfaceSharedResources.fpDestroyBackendContext("
		"    if (!(contextPrivate->description.flags & FFX_FSR3_ENABLE_UPSCALING_ONLY))\n        contextPrivate->backendInterfaceSharedResources.fpDestroyBackendContext(" source "${source}")
	string(REPLACE "    fsr3DispatchParams.renderSize"
		"    fsr3DispatchParams.upscaleSize = dispatchParams->upscaleSize;\n    fsr3DispatchParams.renderSize" source "${source}")
	file(CONFIGURE OUTPUT "${staged}/${fsr3}" CONTENT "${source}" @ONLY)

	# The parent has already selected x64 clang-cl. The SDK's standalone toolchain forces a Visual Studio generator platform.
	set(_TOOLCHAIN_ ON)
	set(CMAKE_EXE_LINKER_FLAGS "/machine:x64")
	set(FFX_API_BACKEND DX12_X64)
	set(FFX_API_DX12 ON)
	set(FFX_FSR3 ON)
	set(FFX_BUILD_AS_DLL OFF)
	add_subdirectory("${staged}" "${CMAKE_BINARY_DIR}/fidelityfx/build" EXCLUDE_FROM_ALL)
	target_link_libraries(${target} PRIVATE ffx_fsr3_x64 ffx_backend_dx12_x64)
	target_compile_definitions(${target} PRIVATE KYTY_HAS_FSR=1)
endfunction()
