# Road Rash: Jailbreak 0.1.0 player installation

The player package `RoadRashJailbreak-0.1.0.zip` contains prebuilt Windows tools and three installers.
No Visual Studio or compilation is needed. No game data is included: you select your own disc image.

| Installer | Plays | State |
|---|---|---|
| **INSTALL-PC.bat** | Windows desktop, on a monitor | playable |
| **INSTALL-PCVR.bat** | Windows PCVR through OpenXR | included |
| **INSTALL.bat** | Quest 3 standalone | included |

The ZIP includes the Windows game, the x64 OpenXR loader and a signed, non-debuggable ARM64 Quest APK.
Choose the installer for the platform you want to play on.

## Windows PC

1. Extract the ZIP into a normal writable folder.
2. Run **INSTALL-PC.bat** and select your **Road Rash: Jailbreak (USA)** image: the `.bin`, its `.cue`,
   or a `.zip` / `.7z` holding the `.bin`.
3. The installer verifies every package file against `release-manifest.json`, identifies your disc by
   the SHA-1 of `SLUS_010.53` and the three overlays, copies the image into the game folder, checks the
   copy byte for byte, runs the disc self-checks and starts the game once, hidden and silent, to prove it
   finds the disc. Unsupported images are rejected with the reason (another game, another revision, a
   modified image, a 2048-byte ISO).
4. Open **PLAY.bat** in the game folder, or **rrgame.exe** directly.

The default game folder is `%LOCALAPPDATA%\RoadRashJailbreak`. It holds `rrgame.exe`, the PLAY
launchers, `runtime\disc\` (your disc copy, about 560 MiB, plus `disc-manifest.json` with its hashes),
`saves\` and `backup-*\` folders with the files an update replaced. Your original image is only read.

To use an image in place instead of the copy, put its path on one line in `disc.txt` next to
`rrgame.exe`; rrgame reads `disc.txt` before `runtime\disc\`.

## Windows PCVR

Run **INSTALL-PCVR.bat** with the same image, then start your headset's PC connection and open
**PLAY-PCVR-META.bat** (Meta Quest Link / Air Link), **PLAY-PCVR-STEAMVR.bat** (SteamVR / Steam Link)
or **PLAY-PCVR-VD.bat** (Virtual Desktop, VDXR). See [PCVR](PCVR.md).

## Quest 3

1. Enable developer mode on the Quest, connect USB and accept USB debugging inside the headset.
2. Run **INSTALL.bat** and select your image.
3. The installer prepares the disc on the PC as above, finds ADB (or downloads Google's pinned
   Platform Tools), updates the APK with `adb install -r`, copies the disc image to
   `/sdcard/Android/data/com.rrjb.vr/files/disc.bin` and verifies its size and SHA-1 on the headset. It does not
   start the game, never uninstalls the app and never clears its data.
4. Open the game from **Unknown Sources** on the headset.

See [Quest](QUEST.md) and [save transfer](SAVE-TRANSFER.md).

## Optional HD media

At the end of the installation you are asked whether to prepare HD media (pictures, fonts, HUD; optionally the
films) from your disc; **PREPARE-HD.bat** in the package does it later. It needs Real-ESRGAN
(`realesrgan-ncnn-vulkan-20220424-windows.zip`, v0.2.5.0 from the official GitHub release, put into your Downloads
folder) and a Vulkan GPU; the scripts check its hash and never download it. The pack is written to `runtime\hd` in
the game folder; the Quest installer copies it to the headset (`scripts\install-quest-hd.ps1` does it alone).
Switch it with **HD textures and media** (F10 > Image quality / VR menu > Graphics). See [HD media](HD-MEDIA.md).

## Existing installations and saves

Saves are in `saves\` next to `rrgame.exe`: `saves\rrjb_card.mcr` is a raw 128 KiB PlayStation memory
card image. Reinstalling or updating never touches `saves\`. A card from an earlier build
(`rrjb_card.mcr` next to the exe) is copied into `saves\` on the first start and the old file is kept.

## Command line

```powershell
.\scripts\install-player.ps1 -Target PC -DiscImage 'D:\Discs\Road Rash - Jailbreak (USA).cue'
.\scripts\install-player.ps1 -Target PC -DiscImage 'D:\Discs\RRJB.zip' -InstallDir 'D:\Games\RoadRashJailbreak'
.\scripts\install-player.ps1 -Target Quest -DiscImage 'D:\Discs\Road Rash - Jailbreak (USA).bin' -Serial YOUR_SERIAL
.\scripts\install-player.ps1 -VerifyOnly
.\tools\rrtool.exe identify 'D:\Discs\Road Rash - Jailbreak (USA).bin'
```

`-Adb` selects an existing adb.exe, `-Serial` one of several connected headsets, `-SkipLaunchCheck`
skips the hidden test start, `-HdMedia Original|Menus|MenusAndMovies` answers the HD media question
(`-Upscaler <folder>` names an extracted Real-ESRGAN).
