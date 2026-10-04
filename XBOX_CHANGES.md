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
| (none yet) | | | |
