# Xbox changes to KytyPS5

KytyPS5 (upstream `KytyPS5/KytyPS5` at `c528324d`) plus what it takes to run on the Xbox Series X: a UWP app with a D3D12 renderer. This branch (`xbox-on-upstream-2026-10-28-c528324d`) has one commit per topic, and each commit adds its row below, so the series can be read, tried and moved to a newer upstream one commit at a time.

The "Checked" column says how a change was checked when it was made, before the work was regrouped into this series.

## Rules

- One commit per topic: it says what it changes and adds its row here.
- No local paths, game names or title ids.
- Where in doubt, upstream is right.

## Changes

| Change | What | Upstream files touched | Checked |
| --- | --- | --- | --- |
| UWP app: launcher | New folder `src/uwp`, its own CMake project: WinUI 2 launcher with a game library (game folders and `.zar` archives, also on the Xbox's USB drive through the `*FromApp` file calls), settings page, game menu, overlay, four Xbox controllers with the PS5 mapping (listed by model, with the Xbox's button symbols on a page of their own), a trophies page, `uwp.ps1` for building, packaging and the Device Portal | none | PC and Series X: works with the controller |
