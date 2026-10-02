# Dependencies and provenance

The game, its tools and the development oracle are the project's own code (MIT, see `LICENSE`). The
source tree vendors no third-party libraries except the OpenXR headers, the VR hand assets, stb_image / stb_image_write
and the xBR provenance copy below. PNG writing,
SHA-1, the audio mixer and decoders, the MDEC decoder, the R3000 + GTE interpreter and the OpenGL function loader
are newly written for this project; the PNG reader (`src/platform/png_read.*`, with its DEFLATE decoder) and the VR
hands' drawing come from the authors' own Gran Turismo 2 PC & VR port (MIT).

- `third_party/vrhands`: the VR player's hands. `BigHandLeft.uxrh`, `BigHandRight.uxrh` (meshes with four baked
  finger poses) and `BigHandsAlbedo.png` are built from the BigHands assets of
  [UltimateXR](https://github.com/VRMADA/ultimatexr-unity) (MIT, `ULTIMATEXR_LICENSE.txt`; provenance in
  `SOURCE.md`), as converted for the Gran Turismo 2 PC & VR port. The finger-pose blend and the hand basis from the
  OpenXR grip / aim poses (`tools/rrgame/vr_hands_draw.cpp`) follow MiamiVR's VRHandModel (MIT,
  `MIAMIVR_LICENSE.txt`). The three binary files are embedded into rrgame (and the Quest's librrgame.so) by
  `cmake/vr-hand-assets.cmake`; `scripts/audit-source.ps1` allows exactly these three, by path and SHA-256.
- `third_party/stb`: unmodified stb_image 2.30 (PNG / JPEG decoding of the optional HD media pack) and stb_image_write
  1.16 (the offline preparation's PNG / JPEG encoding, `tools/rrhd`), Sean Barrett and contributors, MIT or public
  domain; this project uses the MIT option. Copied from the authors' Gran Turismo 2 PC & VR port; retrieval URLs and
  SHA-256 in `third_party/stb/SOURCE.md`, licence in `LICENSE.txt`.
- `third_party/xbr`: Hyllian's xBR-lv3 shader (MIT, `LICENSE.txt`, `SOURCE.md`), kept for provenance.
  `src/rrformats/hd_contours.h` adapts its edge rules to offline processing of palette indices (the HD pack's font and
  HUD contour atlases), after the Gran Turismo 2 port's `ui_contours.h`. The shader itself is not executed.
- **Real-ESRGAN** (optional HD media preparation only, `scripts/prepare-hd.ps1`): `realesrgan-ncnn-vulkan` 0.2.5.0 and
  its `realesrgan-x4plus` model (BSD-3-Clause, [xinntao/Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)). Installed
  by you from the official release; the scripts check its SHA-256 and never download, bundle or redistribute it.
  The HD pack it helps make is your own and stays on your machines.
- `third_party/openxr`: the Khronos OpenXR API headers 1.1.58, unchanged. Every header carries
  `SPDX-License-Identifier: Apache-2.0 OR MIT`; this project uses them under the MIT option. The SDK's
  `LICENSE` file is kept beside them. Used only by the VR build.
- **OpenXR loader.** The Windows player package bundles the official x64
  Khronos `openxr_loader.dll` (built from [Khronos OpenXR SDK Source](https://github.com/KhronosGroup/OpenXR-SDK-Source),
  Apache-2.0; its statically linked JsonCpp keeps its MIT/public-domain notice). The Quest APK
  packages the loader from the Khronos `org.khronos.openxr:openxr_loader_for_android` AAR, Apache-2.0.
  Neither binary is part of the source kit; `scripts/package-player.ps1` copies the loader and its
  license texts into the player package's `LICENSES/`.
- **Windows system components**, linked or loaded at run time, not redistributed: OpenGL (`opengl32`),
  WinMM audio and joystick (`winmm`), XInput (`xinput1_4.dll` / `xinput9_1_0.dll`, loaded dynamically).
- **Build tools**: CMake, Ninja and Microsoft Visual Studio 2022 C++ Build Tools, installed by
  `INSTALL.bat` through WinGet under their own licenses (`Kitware.CMake`, `Ninja-build.Ninja`,
  `Microsoft.VisualStudio.2022.BuildTools` with the
  [Microsoft.VisualStudio.Workload.VCTools](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022)
  workload). 7-Zip (`7zip.7zip`) is installed only when a `.7z` disc archive is selected. None of their
  binaries is part of the source kit.
- **Android Platform Tools** (Quest installation and save transfer only): an existing `adb.exe` is used
  when found; otherwise `scripts/platform-tools.ps1` downloads Google's Platform Tools 36.0.2 for Windows
  from `dl.google.com`, verifies the archive and each extracted file against pinned SHA-256 hashes and
  keeps Google's `NOTICE.txt` and `source.properties` in its cache (`%LOCALAPPDATA%\RoadRashJailbreak\Tools`).
  SDK terms: https://developer.android.com/studio/terms. Not bundled.
- **Python 3.12** (standard library only) runs the verification probes in `tools/scout` that the acceptance run uses.
  **Ghidra** runs the helper scripts in `re/ghidra`; their output (a reading aid derived from the game's
  code) stays under the git-ignored `work\` and is never part of this source.

No disc image, game file, executable or overlay of the game, BIOS, memory card, savestate, RAM or VRAM
dump, screenshot of game art, extracted string or table, or decompiled game code is included in the
source distribution, apart from the documentation preview `docs/images/gameplay.png`. This single
screenshot is shown in the README, is never loaded by the game and is allowed by exact path and
SHA-256 in `scripts/audit-source.ps1`; the asset checks run before every export.

Facts about the original program (addresses, formats, behaviour) were measured on a legally owned disc
and recorded with the executable/overlay SHA-1 in the code comments and `docs/formats/`. Facts seen in
unlicensed third-party projects are used only as hints and re-derived from the bytes; no such code is
copied. The sibling project Gran Turismo 2 PC & VR (MIT, same authors) supplied engineering patterns
(installer, packaging, audit scripts and the release save provider), not game data.

This file is a dependency inventory, not a grant of rights to third-party game content or a
replacement for the project's own distribution license.
