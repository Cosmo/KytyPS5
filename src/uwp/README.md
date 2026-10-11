# UWP app (Windows and Xbox Dev Mode)

`src/uwp` is the KytyPS5 app for UWP: the launcher, the game menu, the settings, the trophies, the controller page, the overlay and the controller input. It runs on Windows with Developer Mode and is packaged for Xbox Dev Mode.

**Status:** the emulator runs in the app with a Direct3D 12 renderer (`docs/d3d12-backend.md`), on a PC and on the Xbox Series X, with sound through XAudio2. Shaders go through Mesa's `spirv_to_dxil`. Where the Series X's driver lacks something, the renderer works around it or skips it: mesh programs run as compute shaders, shaders read guest memory through a table of buffers below shader model 6.6, and tessellation, 64-bit image atomics and 96-bit textures are reported once and skipped (`XBOX_CHANGES.md` lists the changes and the known limits). Guest memory is committed on first touch, and above a budget the parts a game has not used lately go to a swap file (by default when the app may use 8 GB or less, as on the Xbox). `kyty://run?title=<title ID>` (or `?game=<folder>`) starts a game.

## The app

- **Home screen.** The games of the game folders as a row of covers over the selected game's background. A starts a game, Menu opens its options (its details, Play, Hide game), Y scans the folders again. The icons at the top right open the controller page, the trophies and the settings.
- **Game menu.** View + Menu in a game: resume, show or hide the overlay, save a frame dump, quit to the library.
- **Overlay.** Over a game: the run time, the frame rate and frame times, draws, skipped draws and GPU time per frame, the resolutions, CPU and disk reads, the app's RAM in parts, the swap file, the shaders and pipelines with the ones new in this run, the time spent waiting for compiles and for the swap file, the controllers, and the end of the emulator's log.
- **Trophies.** Each game's trophies, how many of each grade are earned, and when.
- **Look.** Laid out and sized like the Xbox's own screens (`src/uwp/src/xboxLook.*`): one bar of controller hints, one title size, the same margins everywhere, and on the Xbox, the Xbox's text size.

## Design

- **One XAML app.** WinUI 2.8 (the newest UI stack UWP and Xbox run), written in C++/WinRT. The markup is loaded at runtime (`XamlReader`), so there is no XAML compiler and no MSBuild project: the app is a CMake target built with clang-cl. `src/uwp/uwp.cmake` is included by the root `CMakeLists.txt` when `KYTY_BUILD_UWP` is on (`uwp.ps1 configure` sets it).
- **UWP APIs only.** The Xbox has none of the desktop-only DLLs, and an import it cannot resolve stops the app from starting while Windows still runs it. The target links `WindowsApp.lib` and the store C++ runtime ahead of the desktop libraries, and `uwp.ps1 build` checks every import of the package's binaries against `WindowsApp.lib`.
- **Game library.** The app searches its game folders for games (folders with `eboot.bin`, up to two levels deep, and `.zar` archives in those folders) and indexes them by title ID from `sce_sys\param.json`, in `LocalState\library.json`. Archives are read in place with ZArchive (the same library and patch as upstream, plus `zarchive-open-from-stream.patch`, which lets the app open the file itself: the Xbox allows a USB drive to be read only through the `*FromApp` functions); the covers and backgrounds come out of the archive too. The library is read from that file at start; Y (or F5) in the launcher scans again.
- **Files** outside the package are opened with the `*FromApp` functions. Game folders must grant UWP apps read access (below), so files are read directly. The `broadFileSystemAccess` capability goes through a file broker at about 20 ms per file call and is not used.
- **Pad host** (`padHost.h`). The input code maps the Xbox controllers to a PS5 pad state per player and hands it through the pad host to the emulator's controller library.
- **Emulator host** (`emulatorHost.*`). Starts the emulator on its own thread when a game starts, with the settings page's choices, and holds the game's page, the exit and restart logic and the frame statistics the overlay shows.

## Controllers

Xbox controllers (`Windows.Gaming.Input`, the same on a PC and on Xbox), up to four at once: each is a local player, and a controller that connects takes the lowest free player. The keyboard is for the menus.

| Xbox controller | PS5 controller |
|---|---|
| A, B, X, Y | Cross, Circle, Square, Triangle |
| Menu | Options |
| LB, RB | L1, R1 |
| LT, RT | L2, R2 (analog) |
| Left stick, right stick, their clicks | Left stick, right stick, L3, R3 |
| D-pad | D-pad |
| View + left stick, moved or clicked | A finger on the touchpad where the stick points, which also presses the touchpad (until View is released) |
| View + right stick | The controller's angle for the motion sensors: up/down tips it forward/back, left/right leans it, up to 180 degrees each way |
| View + right stick pressed in | Left/right turns the controller instead of leaning it |
| View + Menu | The game menu (Escape on a keyboard) |
| View + Menu + LT + RT | Show or hide the overlay (F3 on a keyboard) |

While View is held, the sticks do not reach the game. The View + Menu chords act when they are released, so the order of the presses does not matter.

Rumble goes to the controller's motors. The DualSense's adaptive triggers rumble in the Impulse Triggers while a trigger resists, vibrates or gives way, and its haptic feedback gives soft bumps on the small motor; the settings can turn both off. Player 1's DualSense speaker plays on a headset (on the Xbox, the chat headset), and with the setting on, the headset's microphone is player 1's DualSense microphone. Players 2 to 4 are called Player 2, Player 3 and Player 4 in the games.

## Setting up your games folder

The app looks for games in `LocalState\Games` (always) and in the folders listed in its settings (the settings page, or `uwp.ps1 folders -Add <folder>` and `-Remove <folder>`).

UWP apps may only read folders that allow it. Give apps read access to each game folder you add, once:

- **PC**, in Explorer: right-click the folder > Properties > Security > Edit > Add, enter `ALL APPLICATION PACKAGES`, OK, keep "Read & execute", OK. Or run:

  ```
  icacls "<games folder>" /grant "*S-1-15-2-1:(OI)(CI)RX"
  ```

  (`S-1-15-2-1` is ALL APPLICATION PACKAGES on every Windows language.)
- **Xbox** (Dev Mode): format a USB drive as NTFS, give ALL APPLICATION PACKAGES access to it on a PC as above, plug it into the Xbox and choose **Media** (Dev Mode shows it as drive E:), then put the games in a folder on it. Set the app's type to **Game** in Dev Home for the larger memory budget.

This lets every UWP app read that folder. To grant this app alone, use its package SID instead of `S-1-15-2-1`. The launcher lists the folders it cannot read together with the command that fixes them.

Saves, patches and trophies can live on the USB drive too: choose a folder under **User Data Path** in the settings. The app needs write access to it (`(OI)(CI)M` instead of `RX` in the command above); the settings page says whether it has it. When the folder can't be written as a game starts, that game uses the app's own folder.

## Settings

The settings page (the gear at the top right) keeps its choices in `LocalState\kyty-uwp.json`, in categories, as the Xbox's own Settings: Display (TV resolution, V-Sync, the overlay's size and log lines), User (name, console language), Controller (Impulse Triggers, haptics, the headset's speaker and microphone), Game folders, User Data Path, Memory and quality (game memory paging, compact GPU memory, video decoding, and the textures' detail, sharpness and filtering) and Developer (debug layer, detailed log, memory report, GPU crash diagnostics, keeping and removing compiled shaders). Each setting explains itself next to it. Game settings apply to the next game you start. A few settings for debugging exist only in the file (`src/uwp/src/settings.h`).

## Building

Requirements:

- Windows SDK 10.0.26100 or newer (C++/WinRT and the packaging tools) and the store C++ runtime from Visual Studio's C++ workload, with clang-cl.
- The WinUI 2 NuGet package `Microsoft.UI.Xaml` 2.8.7 and its declared dependency `Microsoft.Web.WebView2` 1.0.2849.39, extracted (a `.nupkg` is a zip). Only WebView2's metadata is used, to generate the C++ headers; nothing of it is packaged. `$env:KYTY_DEPS` is the folder that holds `nuget\<package>` (default `build\deps`), or pass `-WinUIRoot` and `-WebView2Root`.
- The submodules (`git submodule update --init`) and glslang's `glslangValidator` on the PATH, as for the desktop build.
- Mesa's `spirv_to_dxil` (its `include`, `lib` and `bin`) in `$env:KYTY_SPIRV_TO_DXIL_ROOT`; `docs/d3d12-backend.md` explains how to build it. The DXIL validator (`dxil.dll`) comes from the Windows SDK.
- The UCRT build of `libwinpthread-1.dll` (mingw-w64, MSYS2 package `mingw-w64-ucrt-x86_64-libwinpthread`) in `$env:KYTY_WINPTHREAD_DLL`.
- The libraries upstream fetches at configure time (opus, xbyak, zydis, zstd, ZArchive): downloaded, or taken from `$env:KYTY_DEPS\<name>-src` (opus, xbyak, zydis, zstd) and `$env:KYTY_ZARCHIVE_SOURCE` (ZArchive, with upstream's patch and `src/uwp/zarchive-open-from-stream.patch` applied) when they exist. `$env:KYTY_FFMPEG_DIR` names an unpacked FFmpeg package (upstream's prebuilt one) to use instead of downloading it.
- Developer Mode (Settings > System > For developers) to register the unsigned package.

`uwp.ps1` runs each step and builds into `build\uwp` (or `$env:KYTY_UWP_BUILD`):

```
src\uwp\uwp.ps1 configure      # CMake configure (Ninja, clang-cl)
src\uwp\uwp.ps1 build          # builds kyty_uwp and the package layout (uwp-layout)
src\uwp\uwp.ps1 deploy         # installs the WinUI framework package if missing, registers the layout
src\uwp\uwp.ps1 package        # a signed .msix for the Xbox (see "Xbox")
src\uwp\uwp.ps1 run [-Title <ID> | -Game <path>] [-Seconds N] [-Keep]
src\uwp\uwp.ps1                # build, deploy, run
```

The layout is registered in place, so after a rebuild the app runs the new binary without a new deploy (deploy again when the manifest changes).

To install next to another build of the app, give this one other names: `$env:KYTY_UWP_PACKAGE_NAME`, `$env:KYTY_UWP_DISPLAY_NAME` and `$env:KYTY_UWP_PROTOCOL` at configure time (defaults `KytyPS5`, `KytyPS5`, `kyty`).

### The emulator in the app

`KYTY_BUILD_UWP=ON` (set by `uwp.ps1 configure`) builds the whole repository for UWP: the root `CMakeLists.txt` includes `src/uwp/uwp.cmake`, which compiles the emulator's sources into the app (without the SDL window and host input, which the app replaces) next to the launcher. The emulator code is compiled as ordinary desktop code with `KYTY_PLATFORM_UWP` defined; what separates it from a desktop build:

- **Libraries.** SDL has no UWP port and stops its own build at the UWP partition of the headers, so it is built as desktop code with its video, audio, input and other subsystems that need desktop DLLs switched off, and without its dynamic API table (so only the SDL code in use is linked). Tracy runs in its UWP mode (no callstacks, no ETW). imgui's Windows clipboard, IME and shell functions are off. ZArchive gets `zarchive-open-from-stream.patch`. FFmpeg and cpuinfo are used as they are.
- **Shims.** `src/uwp/src/desktopShims.cpp` defines the few desktop functions the SDL and cpuinfo objects reference (as `__imp_<name>` pointers), so the executable imports none of them.
- **Files** go through the `*FromApp` functions (`common/platform/sysWindowsFileIO.cpp`), on a thread with a large stack, because the file broker needs more stack than a guest thread has; a `.zar` archive is opened with them as well (`SysFileOpenCStreamR`, `ZArchiveReader::OpenFromStream`).
- **Memory** goes through `common/platform/sysWindowsVirtual.*`: the `*FromApp` variants of the allocation, protection and section functions; memory is never writable and executable at once (a page meant to be both switches on execute and write faults, `HandleWriteExecuteFault` from the exception handler). Guest memory is committed on first touch rather than up front, and above a budget the pager moves the parts a game has not used lately to a swap file in LocalState (`kernel/memoryAddressSpace.inc`, `xbox/`); the settings page's Game memory paging sets the budget.
- **pthreads** run on winpthread. Upstream's DLL imports `msvcrt.dll`, which UWP apps do not have; the UCRT build of the same library (mingw-w64's, from MSYS2's ucrt64 package) is packaged instead (`$env:KYTY_WINPTHREAD_DLL`).
- **Imports** of the package's binaries are checked against `WindowsApp.lib` by `uwp.ps1 build`.

## Xbox

`uwp.ps1 package` makes an installable package of the build: the signed `<name>_<version>_x64.msix` and, in `Dependencies`, the framework packages it needs (VCLibs from the Windows SDK, WinUI 2.8). Every package gets a newer version, so it installs over the previous one.

Packages are signed with a certificate for `CN=KytyPS5` (the manifest's publisher) from your user certificate store. Create one once, in PowerShell:

```
New-SelfSignedCertificate -Type Custom -Subject "CN=KytyPS5" -KeyUsage DigitalSignature `
  -FriendlyName "KytyPS5 package signing" -CertStoreLocation "Cert:\CurrentUser\My" `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
```

To install the first time, open the Xbox Device Portal (the address Dev Home shows), then **Add**: choose the `.msix`, add both packages from `Dependencies`, and start. After that, `uwp.ps1` works with the Xbox through the Device Portal's REST API:

```
src\uwp\uwp.ps1 xbox -Address <IP> -XboxDir <folder>   # once: the address, then its sign-in
src\uwp\uwp.ps1 install -XboxDir <folder>               # installs the newest package
src\uwp\uwp.ps1 launch -XboxDir <folder>                # starts the app (a game with -Title or -Game)
src\uwp\uwp.ps1 logs -XboxDir <folder>                  # downloads the app's logs, prints their ends
src\uwp\uwp.ps1 screenshot -XboxDir <folder>            # saves a screenshot of the Xbox
src\uwp\uwp.ps1 upload -XboxDir <folder> -Files <files> [-To <folder in LocalState>]   # e.g. -To _Patches
```

`<folder>` keeps the address, the sign-in (encrypted for your Windows user), the downloaded logs and the screenshots; `$env:KYTY_XBOX_DIR` can stand in for `-XboxDir`.

## Running and debugging

```
src\uwp\uwp.ps1 folders [-Add <folder>] [-Remove <folder>]   # the game folders
src\uwp\uwp.ps1 games                                        # the library: title IDs, names, versions
src\uwp\uwp.ps1 stacks                                       # the call stacks of the running app's threads (cdb)
src\uwp\uwp.ps1 debug                                        # attaches cdb to the running app
```

The app writes `LocalState\kyty-uwp.txt` (UWP apps have no console) and the emulator `LocalState\kyty-emulator.txt`; `run` and `logs` print them. The overlay shows the end of the emulator's log over the game, and the game menu's **Save frame dump** saves the render targets of one frame as PNG files and its draw log (in `LocalState\_FrameDump`).
