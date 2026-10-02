# PCVR on Windows

Road Rash: Jailbreak PCVR runs on a Windows PC through OpenXR, with the same `rrgame.exe`, the same
installed disc and the same saves as the desktop game: `rrgame.exe --vr`.

> **Status.** `--vr` is built into rrgame.exe: the whole game - the menus on a theatre screen, the races in
> stereo from the rider's head or the chase cameras, the VR settings menu, the Touch controls - through OpenXR on
> OpenGL (`XR_KHR_opengl_enable`). Checked without a headset: the same VR frames through the desktop VR mock
> (`rrgame --vr-mock`), and the runtimes found and opened (Meta Oculus 1.208.0, Virtual Desktop VDXR 1.0.10). With no
> headset connected in Link / Virtual Desktop the game stops with `XR_ERROR_FORM_FACTOR_UNAVAILABLE` (no headset);
> a play session over Link / VD has not been run yet.

1. Connect your headset with your usual PCVR connection and start its software (SteamVR, Meta Quest
   Link, or Virtual Desktop's VDXR).
2. Run **INSTALL-PCVR.bat** from the player package (or **INSTALL.bat** from the source kit) and select
   your disc image. The player package includes the Khronos OpenXR loader; nothing else is needed.
3. Open **PLAY-PCVR-META.bat** for Meta Quest Link / Air Link, **PLAY-PCVR-STEAMVR.bat** for SteamVR /
   Steam Link, or **PLAY-PCVR-VD.bat** for Virtual Desktop (VDXR). **PLAY-PCVR.bat** uses the system
   default OpenXR runtime. **PLAY.bat** starts the desktop game with the same saves.

Requirements: Windows x64, an OpenGL 3.3 driver, and an active OpenXR runtime with a headset connected.

## Runtime selection

Every PCVR launcher runs `rrgame.exe --vr`. The META, STEAMVR and VD launchers clear an inherited
`XR_RUNTIME_JSON` and set `RRJB_XR_RUNTIME=meta`, `steamvr` or `vdxr` for the game process only (the game finds that
runtime's manifest from its running server process or its install folder). The Windows OpenXR default in the registry
is never changed. `RRJB_XR_RUNTIME=system` uses the current Windows default; an explicit `XR_RUNTIME_JSON` set by you
is respected by `PLAY-PCVR.bat`.

## Controls and settings

The Touch controls, the VR settings menu (L3 + R3, or both grips + Menu; F10 on the keyboard) and their defaults are
those of the Quest: see [Quest 3](QUEST.md#controls-touch). In PCVR the keyboard and a desktop controller work as well.
The VR settings are the `[vr]` section of `saves\rrgame_settings.ini`, the Touch bindings `[vr_controls]` of
`controls.ini`. The two steering modes - the left **Stick** and the **Handlebars** (default) in your hands (grab the
grips with the grip buttons, turn the bars, twist the right grip for the throttle, swing a free hand to punch) - are the
Quest's: see [Steering with the handlebars](QUEST.md#steering-with-the-handlebars-vr-menu---controls---steering-handlebars).
Command line: `--vr-steering stick|handlebars`, `--vr-bars-sensitivity P`, `--vr-bars-deadzone P`,
`--vr-twist-throttle 0|1`, `--vr-motion-punches 0|1`, `--vr-one-hand 0|1`, `--vr-bars-height CM`.

## Without a headset (developers)

```powershell
rrgame.exe <disc> --vr-mock --race 1 20 --frames 900 --shot work\xr\mock.png            # both eyes + the quad
rrgame.exe <disc> --vr-mock --vr-mock-yaw 70 --race 1 20 --frames 900 --shot work\xr\yaw.png
rrgame.exe <disc> --vr-mock --vr-menu-shot 1 --race 1 20 --frames 300 --shot work\xr\menu.png
rrgame.exe <disc> --vr-mock --shell-frames 600 --shell-shot work\xr\menus.png              # the front end
```

A scripted run (a frame count, a shot) is hidden and silent and reads no settings file.

The Handlebars mode in the mock takes scripted controllers instead of a headset's (`--vr-bars-script <file>` or the
script itself, `;`-separated `<frame> <command>`: `grab left|right|both`, `release H`, `turn DEG [FRAMES]` - the held
bars, positive left -, `twist DEG [FRAMES]` - the right wrist -, `trigger H V`, `grip H V`, `move H DX DY DZ FRAMES` -
a free hand, metres, x right / y up / z toward you -, `home H`). The run's `vr bars:` lines log the bars, the analogue
bytes, the heading, the swings, and a totals line at the end (tools\rrgame\vr_handlebars.h, tests\run_gates.ps1):

```powershell
rrgame.exe <disc> --vr-mock --vr-mock-pitch -35 --vr-steering handlebars --race 1 20 --frames 700 ^
    --vr-bars-script "20 grab both; 20 twist 30 30; 300 turn 20 20; 360 turn 0 20; 420 turn -20 20" --shot work\xr\bars.png
```

## Source builds

The source build compiles the VR code into `rrgame.exe`. Place the official x64 Khronos `openxr_loader.dll` beside the
executable (the build copies an existing one there; the installer copies any DLL found in the build folder).
`scripts\package-player.ps1 -OpenXRLoader <path>` bundles it and its Apache-2.0 license into the player package.
