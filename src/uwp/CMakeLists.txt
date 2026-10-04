# The UWP app (Windows and Xbox Dev Mode): the launcher UI. This directory is its own CMake project
# (`uwp.ps1 configure`); it may only import APIs available to UWP apps (WindowsApp.lib, the store C
# runtime), which `uwp.ps1 build` checks. The emulator is not part of this build yet.
cmake_minimum_required(VERSION 3.23)
project(kyty_uwp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# The JSON library upstream keeps as a submodule (3rdparty/nlohmann_json); a different folder can be given.
set(KYTY_JSON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/nlohmann_json/include" CACHE PATH "Folder with nlohmann/json.hpp")
if(NOT EXISTS "${KYTY_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
	message(FATAL_ERROR "nlohmann/json.hpp not found: run 'git submodule update --init 3rdparty/nlohmann_json' or set KYTY_JSON_INCLUDE_DIR")
endif()

# C++/WinRT projection headers for the Windows SDK and WinUI 2, generated with the SDK's cppwinrt.
string(REGEX REPLACE "[\\/]$" "" kyty_uwp_sdk_version "$ENV{WindowsSDKVersion}")
find_program(KYTY_CPPWINRT cppwinrt PATHS "$ENV{WindowsSdkVerBinPath}/x64" NO_DEFAULT_PATH)
if(NOT KYTY_CPPWINRT OR kyty_uwp_sdk_version STREQUAL "")
	message(FATAL_ERROR "cppwinrt not found; configure from a Visual Studio developer shell")
endif()
set(kyty_uwp_projection "${CMAKE_CURRENT_BINARY_DIR}/cppwinrt")
set(kyty_uwp_winui_winmd "${KYTY_WINUI_ROOT}/lib/uap10.0/Microsoft.UI.Xaml.winmd")
add_custom_command(
	OUTPUT "${kyty_uwp_projection}/winrt/Microsoft.UI.Xaml.Controls.h"
	COMMAND "${KYTY_CPPWINRT}" -input ${kyty_uwp_sdk_version} -input "${kyty_uwp_winui_winmd}"
	        -input "${KYTY_WEBVIEW2_ROOT}/lib/Microsoft.Web.WebView2.Core.winmd"
	        -output "${kyty_uwp_projection}"
	DEPENDS "${kyty_uwp_winui_winmd}"
	COMMENT "Generating C++/WinRT headers (Windows SDK ${kyty_uwp_sdk_version}, WinUI 2)")
add_custom_target(kyty_uwp_projection DEPENDS "${kyty_uwp_projection}/winrt/Microsoft.UI.Xaml.Controls.h")

# No desktop system libraries (kernel32.lib, user32.lib, ...) for this target.
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")

file(GLOB kyty_uwp_src CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

# C++/WinRT reports errors as exceptions; windows.h needs NOMINMAX for the standard algorithms.
set_source_files_properties(${kyty_uwp_src} PROPERTIES COMPILE_OPTIONS "/EHsc" COMPILE_DEFINITIONS "NOMINMAX")

add_executable(kyty_uwp WIN32 ${kyty_uwp_src})
add_dependencies(kyty_uwp kyty_uwp_projection)
target_include_directories(kyty_uwp SYSTEM PRIVATE "${kyty_uwp_projection}" "${KYTY_JSON_INCLUDE_DIR}")
target_include_directories(kyty_uwp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# UWP apps use the store C++ runtime (vcruntime140_app.dll from the VCLibs framework package) and
# the WindowsApp umbrella library, which comes before the desktop system libraries.
target_link_options(kyty_uwp PRIVATE "/APPCONTAINER" "/LIBPATH:$ENV{VCToolsInstallDir}lib/x64/store"
	"/DEBUG:FULL" "/PDB:${CMAKE_CURRENT_BINARY_DIR}/kyty_uwp.pdb")
target_link_libraries(kyty_uwp PRIVATE WindowsApp.lib)

# The package layout next to the executable: register it with `uwp.ps1 deploy`.
set(KYTY_UWP_LAYOUT "${CMAKE_BINARY_DIR}/uwp-layout")
set(kyty_uwp_winui_version "8.2501.31001.0")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/AppxManifest.xml.in" "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml" @ONLY)
add_custom_target(kyty_uwp_layout ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory "${KYTY_UWP_LAYOUT}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:kyty_uwp> "${KYTY_UWP_LAYOUT}/"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml" "${KYTY_UWP_LAYOUT}/"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/Assets" "${KYTY_UWP_LAYOUT}/Assets"
	COMMENT "Updating the UWP package layout")
add_dependencies(kyty_uwp_layout kyty_uwp)
