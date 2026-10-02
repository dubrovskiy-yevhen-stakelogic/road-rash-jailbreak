# Validation

## The acceptance run

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_gates.ps1 [-Build <dir>] [-Disc <image>] [-Quick]
```

`tests\run_gates.ps1` runs every acceptance check the project has and prints `N/N gates passed`
(the count grows as functions are ported, so trust the run). `-Quick` skips the
gates that take minutes.

What is checked:

- **Ported gameplay, function by function.** Every native replacement of an original function is
  accepted only when the bench (`rrverify`) shows 0 mismatches over the whole guest RAM outside the
  guest stack, across dump-derived and randomized inputs. The original code runs for comparison in the
  project's own R3000 + GTE interpreter, which is a development oracle only and is never linked into
  the game.
- **Negative controls.** The heavy checks carry mutations that must make them fail (a flipped bit, a
  swallowed oracle call, a corrupted output buffer). A check that cannot fail proves nothing.
- **Asset parsers against the disc**: every model parses and assembles, the road network's routes close
  and its distance identity holds, scene cells, race routes and the assembled world of all 100 races,
  the HUD layout, textures pixel for pixel against an independent Python probe, the audio decoder sample
  for sample.
- **Renderer and game**: frames and draw packets against captures of the original, the front end's
  screens and memory-card layout, scripted races.

Many gates compare against local research captures (RAM dumps, savestates, goldens) kept under the
git-ignored `work\` folder. They are not distributable, so on a fresh source kit only the disc checks
run; the full run is for the project's development machine.

## Installation checks

Every installation (`scripts\install.ps1`, used by `INSTALL.bat` and the player installers) runs these
against the installed copy of your disc, and stops without replacing anything when one fails:

| Check | Expected |
|---|---|
| `rrtool identify` | `verdict  SUPPORTED` (SLUS_010.53 and the three overlays match by SHA-1) |
| copy integrity | SHA-256 of the copy equals the original image |
| `rrtool geoscan` | `112 .GEO files, 0 failed` |
| `rrtool asmcheck` | `0 without a program` |
| `rrtool roadcheck` | `all invariants hold` |
| `rrtool cellcheck ... DATA/RACE1_20.STP` | `0 failed` |
| launch check | `rrgame.exe --race 1 20 --frames 60` from the game folder, hidden and silent, with no disc argument, reports `disc: ...\runtime\disc\... (from runtime\disc)` and writes its frame |

The results are in `runtime\disc\install-check.log` and `.install-*\launch-check.log` in the game folder.

## Source and package checks

- `scripts\audit-source.ps1` checks the file list from `scripts\source-files.ps1`: no work\, build,
  install or save folders; no disc images, game files (every extension found on the SLUS-01053 disc
  and its well-known names), BIOS, memory cards, savestates, RAM/VRAM dumps, images, audio/video,
  executables, libraries, packages, keys or Ghidra databases; text files only (no NUL bytes), none over
  2 MiB; no decompiler output, disassembly listings, hex dumps or large numeric tables; and no
  development-process text (internal section tags, iteration names, dated work notes, machine paths,
  pointers to the internal research notes, development build folders, non-English text). It runs on
  the repository and again on every exported kit. The VR hand assets and the README screenshot
  `docs/images/gameplay.png` are the only binary exceptions, pinned by path and SHA-256.
  For a local source kit initialized as a Git checkout, `-AllowLocalState` permits Git metadata only;
  exported ZIPs are audited without that switch.
- `scripts\package-source.ps1` copies the audited list, records the SHA-256 of every file in
  `SOURCE-MANIFEST.json`, audits the exported folder, then re-hashes every ZIP entry against it.
- `scripts\package-player.ps1` packages only named files, rejects game-data file types, records
  `release-manifest.json` and verifies the ZIP the same way; the player installer re-verifies every file
  before it installs.

## Not yet tested

- PCVR play over Link / SteamVR / Virtual Desktop, and Quest visual quality, comfort, controls, haptics
  and frame rate while wearing the headset remain manual acceptance checks. Desktop VR mock checks
  do not establish these results. The release save provider also needs a transfer check on a headset.
- A clean machine: the WinGet dependency installation (CMake, Ninja, Visual Studio 2022 Build Tools) has
  not been run from scratch; installs on the development machine used `-SkipDependencies`.
