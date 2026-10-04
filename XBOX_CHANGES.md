# Xbox changes to KytyPS5

This branch (`xbox-clean`) is KytyPS5 plus the changes needed to run on the Xbox Series X. It is the only branch meant to be shared.

## Rules

- It always builds and passes the CPU tests (`src/xbox/tests`).
- One commit does one thing and says it in plain words.
- Nothing goes in that has not been verified, and what was verified is written below.
- No development switches, no local paths, no game names or title ids.
- New code lives in `src/xbox/`. Changes to upstream files are small and listed here, so that rebasing on upstream stays easy.
- Experiments live on other branches (`exp/...`) and never build on each other. A working experiment is rewritten as a small clean commit here.

## Changes

| Commit | What | Upstream files touched | How it was checked |
| --- | --- | --- | --- |
| UWP app: launcher, settings, controllers, overlay | New folder `src/uwp` (its own CMake project, no emulator yet): WinUI 2 launcher with game library, settings page, game menu, overlay, Xbox controllers for four players with PS5 mapping, packaging and Device Portal script | none | built with clang-cl, import check against WindowsApp.lib clean; ran on a PC (Developer Mode): library finds games in a game folder, controller detected; installed on a Series X (signed .appx with VCLibs and WinUI 2.8 through the Device Portal) and started: launcher shows, controller listed, focus ring visible. The console needs a signed-in user for the app to be registered and launched for it |
| UWP app: `.zar` game archives in the library | `src/uwp/src/gameSource.*`: reads `eboot.bin`, `param.json` and the cover and background out of a `.zar` in place (ZArchive and zstd, as upstream builds them); the scan lists archives next to game folders | none (`src/uwp/CMakeLists.txt` uses upstream's `3rdparty/patches/zarchive-reader.patch`) | a standard C++ stream opens a `.zar` on a permitted drive from inside the app; scan of a folder of 8 archives finds all 8 with title ID and name in 0.15 s; import check clean |
