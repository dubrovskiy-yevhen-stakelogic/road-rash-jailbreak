# Changelog

## 0.1.0

First release for Windows PC, PCVR and Quest 3 standalone. The feature set:

- **Disc and installation.** `rrtool identify <image>` reads the executable and the three overlays through the
  project's disc reader and compares their SHA-1 with the supported release, Road Rash: Jailbreak (USA),
  SLUS-01053; other discs, other revisions and 2048-byte ISO images are rejected with the reason.
  `INSTALL.bat` / `scripts/install.ps1` installs missing CMake, Ninja and the Visual Studio 2022 C++ Build Tools
  through WinGet, builds, identifies the disc (BIN, CUE, ZIP or 7z), copies it into the game folder's
  `runtime\disc\` with a SHA-256 check, runs the disc self-checks, starts the game once hidden to prove it finds
  the disc, and creates `PLAY.bat` and the PCVR launchers. The previous disc and executables are kept in
  `backup-*\`; saves are never touched. rrgame finds its disc without a command-line path (`disc.txt` next to the
  executable, then `runtime\disc\*.bin`, then `disc\*.bin`; a `.cue` is followed to its `.bin`); saves live in
  `saves\rrjb_card.mcr`, a raw PlayStation memory card image.
- **Desktop game.** The ported front end (title, menus, career, memory-card saves), races with the ported bike
  physics, rider AI, combat and weapons, police, traffic, pedestrians, HUD, sound and music, two-player split
  screen. The original's hit / swing / kick sounds and its sound service at the console's 60 Hz.
- **PC graphics.** F10 settings overlay: render scale 50-200 % or 720p-4K, fullscreen, MSAA, smooth textures with
  mipmaps, precise vertices, depth-buffer draw order, maximum detail (LOD 0 everywhere, road markings at every
  distance) and draw distance, profiler, control bindings, cheats. Coplanar layering (tunnel, barriers, road
  markings, signs), full animation of pedestrians and riders at every distance. First-person head camera in the
  camera cycle.
- **Controls.** Keyboard, XInput and DirectInput / WinMM pads, native DualSense (USB / Bluetooth, rumble,
  adaptive triggers), rebindable controls, combat on R1 / L1 / Triangle.
- **Cheats** (F10 and the VR menu): any weapon, no police, no traffic, sparring partners (optionally armed),
  passive rivals, god mode, infinite nitro, frozen time limit, career unlocks with verified, backed-up save writes.
- **Handling.** Original (the ported physics) or Modern: smoothed analogue steering, spring-damped turn rate and a
  lean of up to 45 degrees, drawn in full in VR while the view rolls a share of it; the ported physics stay
  untouched. Modern is the VR default.
- **VR through OpenXR.** `rrgame --vr` on PCVR (OpenGL) and the standalone Quest 3 APK (GLES 3.2, single-pass
  multiview, foveation). Head view on the rider's bike with horizon lock, chase views, HUD panel, theatre screen for
  menus and movies, VR settings menu, comfort options; each race frame steps to the frame's predicted display time.
- **VR handlebars and melee.** Grab the grips with tracked hands, steer with both hands, twist throttle; fists and
  weapons hit on contact through the original's damage, reaction, knock-off and hit sound; hip holsters with the
  owned-weapon inventory, per-weapon grip calibration (mirrored for the left hand), snatching a rival's weapon
  mid-swing, nunchaku and chain simulated as physical chains, the prod and stun gun on B with the original's attack.
- **HD media** (`PREPARE-HD.bat`, optional): Real-ESRGAN pictures and movies, xBR font and HUD contours, a disc-bound
  HD pack for PC and Quest; "HD textures and media" in F10 and the VR menu.
- **Quest and PCVR tooling.** PCVR launchers (`PLAY-PCVR-META.bat`, `PLAY-PCVR-STEAMVR.bat`, `PLAY-PCVR-VD.bat`,
  `PLAY-PCVR.bat`), the Quest installer (`scripts/install-quest-player.ps1`, HD pack staged on shared storage) and
  PC <-> Quest save transfer.
- **Release packaging.** Optimized Windows and Quest builds, a non-debuggable Quest APK (version code 11),
  and USB career save transfer through a save provider with card validation, atomic replacement and backups.
- **Source kit tooling.** `scripts/source-files.ps1`, `scripts/audit-source.ps1` (rejects game data, dumps, images,
  binaries, keys, extracted tables, decompiled code and development-process text), `scripts/package-source.ps1`
  (folder, ZIP, SHA-256 and `SOURCE-MANIFEST.json`) and `scripts/package-player.ps1`.
