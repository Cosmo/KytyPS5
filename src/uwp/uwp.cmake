# The UWP app (Windows and Xbox Dev Mode): the launcher UI and the emulator in one package. Included by the root CMakeLists.txt when KYTY_BUILD_UWP is
# ON (`uwp.ps1 configure`), after the emulator's source lists and libraries are set up; it may only import APIs available to UWP apps
# (WindowsApp.lib, the store C runtime), which `uwp.ps1 build` checks.
set(KYTY_UWP_DIR "${KYTY_SOURCE_DIR}/uwp")

# The package identity. A development build can use other names to be installed next to another build of the app.
set(KYTY_UWP_PACKAGE_NAME "KytyPS5" CACHE STRING "Package identity name")
set(KYTY_UWP_DISPLAY_NAME "KytyPS5" CACHE STRING "Name shown to the player")
set(KYTY_UWP_PROTOCOL "kyty" CACHE STRING "URI scheme the app handles (<scheme>://run?title=<title ID>)")

set(KYTY_WINUI_ROOT "" CACHE PATH "Extracted Microsoft.UI.Xaml 2.8 NuGet package (WinUI 2)")
if(NOT EXISTS "${KYTY_WINUI_ROOT}/lib/uap10.0/Microsoft.UI.Xaml.winmd")
	message(FATAL_ERROR "Set KYTY_WINUI_ROOT to the extracted Microsoft.UI.Xaml 2.8 NuGet package")
endif()
# WinUI 2 declares the WebView2 package as a dependency; its metadata is only needed to generate the
# headers (nothing of it is packaged; the app doesn't use WebView2).
set(KYTY_WEBVIEW2_ROOT "" CACHE PATH "Extracted Microsoft.Web.WebView2 NuGet package (WinUI 2 dependency)")
if(NOT EXISTS "${KYTY_WEBVIEW2_ROOT}/lib/Microsoft.Web.WebView2.Core.winmd")
	message(FATAL_ERROR "Set KYTY_WEBVIEW2_ROOT to the extracted Microsoft.Web.WebView2 NuGet package")
endif()

# The guest's pthreads run on winpthread. Upstream's DLL (3rdparty/winpthread) imports msvcrt.dll, which UWP apps do not have; the UCRT build of the same
# library (mingw-w64's, from MSYS2's ucrt64 package) imports only the universal CRT. The import library upstream has links against either.
set(KYTY_WINPTHREAD_DLL "" CACHE FILEPATH "libwinpthread-1.dll of the UCRT build (mingw-w64), packaged with the app")
if(NOT EXISTS "${KYTY_WINPTHREAD_DLL}")
	message(FATAL_ERROR "Set KYTY_WINPTHREAD_DLL to the UCRT build of libwinpthread-1.dll")
endif()

# C++/WinRT projection headers for the Windows SDK and WinUI 2, generated with the SDK's cppwinrt.
string(REGEX REPLACE "[\\/]$" "" kyty_uwp_sdk_version "$ENV{WindowsSDKVersion}")
find_program(KYTY_CPPWINRT cppwinrt PATHS "$ENV{WindowsSdkVerBinPath}/x64" NO_DEFAULT_PATH)
if(NOT KYTY_CPPWINRT OR kyty_uwp_sdk_version STREQUAL "")
	message(FATAL_ERROR "cppwinrt not found; configure from a Visual Studio developer shell")
endif()
set(kyty_uwp_projection "${CMAKE_CURRENT_BINARY_DIR}/uwp/cppwinrt")
set(kyty_uwp_winui_winmd "${KYTY_WINUI_ROOT}/lib/uap10.0/Microsoft.UI.Xaml.winmd")
add_custom_command(
	OUTPUT "${kyty_uwp_projection}/winrt/Microsoft.UI.Xaml.Controls.h"
	COMMAND "${KYTY_CPPWINRT}" -input ${kyty_uwp_sdk_version} -input "${kyty_uwp_winui_winmd}"
	        -input "${KYTY_WEBVIEW2_ROOT}/lib/Microsoft.Web.WebView2.Core.winmd"
	        -output "${kyty_uwp_projection}"
	DEPENDS "${kyty_uwp_winui_winmd}"
	COMMENT "Generating C++/WinRT headers (Windows SDK ${kyty_uwp_sdk_version}, WinUI 2)")
add_custom_target(kyty_uwp_projection DEPENDS "${kyty_uwp_projection}/winrt/Microsoft.UI.Xaml.Controls.h")

# The app's own sources (launcher, settings, input, overlay, the emulator host and the UWP counterparts of the emulator's desktop code).
file(GLOB kyty_uwp_src CONFIGURE_DEPENDS "${KYTY_UWP_DIR}/src/*.cpp" "${KYTY_UWP_DIR}/src/*.h")

# The emulator, without the code the app replaces: the SDL window and host input (a XAML page and the Xbox controllers are the app's).
set(kyty_uwp_emulator_src ${kyty_emulator_src})
list(FILTER kyty_uwp_emulator_src EXCLUDE REGEX "/presentation/window/(window|hostInput)\\.cpp$")
# The generated shader headers come with the target below.
list(FILTER kyty_uwp_emulator_src EXCLUDE REGEX "_spv\\.h$")

# C++/WinRT reports errors as exceptions; windows.h needs NOMINMAX for the standard algorithms.
set_source_files_properties(${kyty_uwp_src} PROPERTIES COMPILE_OPTIONS "/EHsc" COMPILE_DEFINITIONS "NOMINMAX")

add_executable(kyty_uwp WIN32 ${kyty_uwp_src} ${kyty_uwp_emulator_src}
	"${fault_buffer_shader_header}" ${gpu_tiler_shader_headers} ${gpu_blit_shader_headers})
add_dependencies(kyty_uwp kyty_uwp_projection)
target_include_directories(kyty_uwp SYSTEM PRIVATE "${kyty_uwp_projection}")
target_include_directories(kyty_uwp PRIVATE "${KYTY_UWP_DIR}/src" ${inc_headers})

# UWP apps use the store C++ runtime (vcruntime140_app.dll from the VCLibs framework package) and the WindowsApp umbrella library, which comes before
# the desktop system libraries. Whatever the emulator or a library still takes from a desktop DLL is found by the import check, not by the linker.
target_link_options(kyty_uwp PRIVATE "/APPCONTAINER" "/LIBPATH:$ENV{VCToolsInstallDir}lib/x64/store"
	"/DEBUG:FULL" "/PDB:${CMAKE_CURRENT_BINARY_DIR}/kyty_uwp.pdb")
target_link_libraries(kyty_uwp PRIVATE WindowsApp.lib ${kyty_emulator_link_libraries} cpuinfo spdlog::spdlog ZArchive::zarchive Tracy::TracyClient)

# The package layout next to the executable: register it with `uwp.ps1 deploy`.
set(KYTY_UWP_LAYOUT "${CMAKE_BINARY_DIR}/uwp-layout")
set(kyty_uwp_winui_version "8.2501.31001.0")
configure_file("${KYTY_UWP_DIR}/AppxManifest.xml.in" "${CMAKE_BINARY_DIR}/uwp/AppxManifest.xml" @ONLY)
add_custom_target(kyty_uwp_layout ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory "${KYTY_UWP_LAYOUT}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:kyty_uwp> "${KYTY_UWP_LAYOUT}/"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/uwp/AppxManifest.xml" "${KYTY_UWP_LAYOUT}/"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_WINPTHREAD_DLL}" "${KYTY_UWP_LAYOUT}/"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_SPIRV_TO_DXIL_ROOT}/bin/spirv_to_dxil.dll" "${KYTY_DXIL_DLL}" "${KYTY_UWP_LAYOUT}/"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${KYTY_UWP_DIR}/Assets" "${KYTY_UWP_LAYOUT}/Assets"
	COMMENT "Updating the UWP package layout")
add_dependencies(kyty_uwp_layout kyty_uwp)
