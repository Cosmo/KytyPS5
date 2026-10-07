# Xbox changes to KytyPS5

KytyPS5 (upstream `KytyPS5/KytyPS5` at `df5b9b92`) plus what it takes to run on the Xbox Series X: a UWP app with a D3D12 renderer. This branch (`xbox-on-upstream-2026-10-12-df5b9b92`) has one commit per topic, and each commit adds its row below, so the series can be read, tried and moved to a newer upstream one commit at a time.

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
