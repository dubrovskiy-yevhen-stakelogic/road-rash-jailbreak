# Quest 3 standalone

The game runs on the headset itself - the same game as on the PC: the front end, the career, the races and the
saves, in stereo with the Touch controllers. The PC is needed only to build or
install it and to copy your disc image.

> **Status.** The APK (`com.rrjb.vr`, "Road Rash VR", `scripts\build-quest.ps1`) installs and starts on a
> Quest 3: the disc is found, the front end loads, the OpenXR session is created (OpenGL ES 3.2, 1680 x 1760 per eye,
> 72 Hz, MSAA 2x, fixed foveation), the sound opens on AAudio. The whole race path, sound included, has run on the
> headset's GPU through the VR mock. What still needs someone wearing the headset: the session going FOCUSED, the
> frame rate at 72 Hz, the Touch layout and the haptics by hand.

## Install from the player package

1. Enable developer mode on the Quest, connect it with USB and accept USB debugging inside the headset.
2. Run **INSTALL.bat** and select your Road Rash: Jailbreak (USA) image.
3. The installer:
   - verifies the package files and prepares your disc on the PC (identification by the executable and
     overlay SHA-1, a byte-for-byte copy check, the disc self-checks),
   - finds `adb.exe`: the one you name with `-Adb`, `adb.exe` on PATH, an Android SDK
     (`ANDROID_HOME`, `ANDROID_SDK_ROOT`, `%LOCALAPPDATA%\Android\Sdk`, `android-toolchain\sdk` next to the source folder),
     a verified cached copy, or Google's Platform Tools 36.0.2 downloaded from `dl.google.com` and checked
     against pinned SHA-256 hashes (terms: https://developer.android.com/studio/terms),
   - updates the APK with `adb install -r` (never uninstalls, never clears app data),
   - copies the disc image to `/sdcard/Android/data/com.rrjb.vr/files/disc.bin` (skipped when an
     identical image is already there) and checks its size and SHA-1 on the headset (`sha1sum`).
4. It does not start the game. Open it from **Unknown Sources** on the headset.

## Install from the source kit

```powershell
.\INSTALL.bat                                   # PC game folder and disc first
.\scripts\build-quest.ps1                       # the VR build: produces the APK
.\scripts\install-quest-player.ps1              # APK + disc to the connected headset
.\scripts\install-quest-player.ps1 -Apk 'D:\Builds\rrjb-vr.apk' -Runtime 'D:\Games\RoadRashJailbreak' -Serial YOUR_SERIAL
```

`install-quest-player.ps1` takes the disc from the PC game folder (`<Runtime>\runtime\disc`, checked
against its `disc-manifest.json`) and the APK from `android\app\build\outputs\apk\release\app-release.apk`
unless `-Apk` names another. `scripts\install-quest.ps1` is the same installer under a shorter name (same
parameters); with `-Run` it also starts the game and collects its log (`scripts\run-quest-proof.ps1`, the
developers' loop, which can also pass development switches in `files/rrgame_args.txt`).

## Controls (Touch)

| Touch | In the race | In the menus |
|---|---|---|
| right trigger | throttle | - |
| left trigger | brake | - |
| left stick | steer; up / down with an attack: the other combat moves | move |
| A | punch / swing (combat action 1) | confirm |
| X | combat action 2 | (Square) |
| B | kick (combat action 3) | back |
| Y | the view: rider's head -> the chase cameras -> head | (Circle) |
| left stick click | taunt | - |
| right stick click | look back | - |
| Menu | pause | - |
| L3 + R3, or both grips + Menu | the VR settings menu | the VR settings menu |
| hold the Meta button | recentre (the headset's own) | recentre |

Rebind in **VR menu -> Controls** (saved as `[vr_controls]` in controls.ini). The race starts on the rider's head
with the horizon locked (the bike's lean does not roll your view); the view is recentred at every race start.

### Steering with the handlebars (VR menu -> Controls -> Steering: Handlebars)

Two steering modes: **Stick** (the table above) and **Handlebars** (the default) - you ride with your hands on the
bike's own grips, as in a VR motorcycle game. In the Handlebars mode, in the rider's head view:

| Touch | Handlebars mode |
|---|---|
| grip button near a grip (within 20 cm) | take that grip (left hand the left grip, right the right); let go to release |
| both hands on the grips, turn them | steer - the angle of the bars (the right grip pushed forward = left); full lock at 30 degrees |
| one hand on its grip | steers alone (push / pull it), unless One-hand steering is off |
| twist the right grip toward you | throttle (Twist throttle on; the right trigger works too) |
| right trigger | throttle, proportional (analogue) |
| left trigger | brake (a lever), proportional |
| a free hand swung fast (forward, outward, an uppercut) | punch on that side: right = combat action 1, left = action 2; with a weapon in hand the weapon swing |
| a free hand swung down fast | kick (combat action 3) |
| grip button with the hand off the bars | a fist |
| A / X / B, Y, the stick clicks, Menu | as in the table above |
| left stick | steers while no hand holds the bars |

The rider's body is not drawn in this mode's head view: you see your own gloved hands, closed round the grips while you
hold them, open or a fist otherwise; a cyan marker shows a grip within reach (green: close enough to take). The weapon
you pick up appears in your hand. Haptics: a pulse when you take a grip, the road through the held grips (stronger
with speed), a thump when your blow lands, and the game's own rumble. In the chase view the hands float free and the
stick steers. **VR menu -> Controls -> Handlebar options**: bars sensitivity (full lock 60..15 degrees), dead zone,
throttle (twist grip + trigger or trigger only), motion punches on / off, one-hand steering on / off, hands follow the
bike's tilt. The bars height is on **Riding position** (below). Saved in `[vr]` of rrgame_settings.ini (`steering`,
`bars_sensitivity`, `bars_deadzone`, `twist_throttle`, `motion_punches`, `one_hand_steering`, `bars_height_cm`).

### Wheelies

With Modern handling (the VR default) the bike can ride a held wheelie. It needs a deliberate lift:

| Steering | How |
|---|---|
| Handlebars | throttle open, then raise **both** hands together about 15 cm above where they hold the grips and keep them there for a moment (200 ms); raise further (to 30 cm) for the full angle; lower the hands (or shut the throttle) to put the front down. One hand alone, a quick jerk, pulling the bars toward you or the thumb on the left stick does nothing |
| Stick | throttle open and the left stick held (almost) fully back for a moment |

**VR menu -> Riding position** holds everything about where you sit and what the bike does under you: seat height,
seat forward / back, eye (fixed on the bike or the rider's head), bars height, visual bike lean (Original handling),
bike pitch (first person), VR view roll with the lean, Wheelies (with Modern handling / always / off), hand lift to start
(5..40 cm), full-lift height, hold to start (0..1000 ms), wheelie angle, VR view pitch in a wheelie, wheelie over cars,
wheelie loop-over crash. The triggers change the values. The wheelie keys are in `[handling]` of rrgame_settings.ini
(`wheelie`, `wheelie_lift_cm`, `wheelie_full_cm`, `wheelie_hold_ms`, `wheelie_angle`, `wheelie_view_pitch`,
`wheelie_cars`, `wheelie_loop`).

## VR settings

**VR menu** (L3 + R3 or both grips + Menu; the game holds while it is open): Resume, Recentre view, View (rider's head
/ chase camera), Horizon lock 0-100 %, Horizon lock levels, Comfort vignette, **Riding position**, HUD size,
Vibration, **Weapons and holsters**, **Combat options**, **Graphics and performance** (eye resolution 50-200 % -
applies at the next start -, refresh rate 72 / 80 / 90 / 120 Hz, MSAA, draw distance, maximum detail, textures,
foveation, single-pass stereo, smooth motion, view off the bike, bike vibration, HD media), **Controls**, **Cheats**,
Quit game.

### Default settings

A fresh install (no `rrgame_settings.ini`, or a key the file lacks) starts from these - a seated Quest 3 setup:

| Section | Defaults |
|---|---|
| `[vr]` graphics | eye resolution 130 %, 72 Hz, MSAA 2x, extended draw distance, maximum detail, smooth textures, HD media on, foveation medium, single-pass stereo, smooth motion |
| `[vr]` view | rider's head, horizon lock 100 % (roll only), comfort vignette off, seat height -15 cm, seat forward / back 0, eye fixed on the bike, HUD medium, bike vibration low, bike pitch low, view off the bike fixed |
| `[vr]` controls | steering **Handlebars** (sensitivity 100 %, dead zone 3 %, twist throttle, motion punches, one-hand steering, bars height 0, hands follow the bike's tilt, visual lean 50 %), vibration 50 % |
| `[vr]` combat | Buttons + physical, the blow's minimum speed 1.5 m/s, fist 8 cm, weapon length 100 %, the weapon in the right hand, snatch and nunchaku on; swing speed 3.0 m/s, weapon travel 25 cm, fist travel 10 cm, 50 % into the body, cooldown 500 ms; the prod's B discharge and reach on |
| `[vr]` weapons | holsters on: the club (weapon 1) on the left hip, the stun gun (weapon 7) on the right; holster height 58 cm, spread 26 cm, forward 5 cm, reach 18 cm, markers on, empty weapons hidden; a calibrated grip for every weapon 0..8 (`weapon_grip_N`) |
| `[handling]` | VR: **Modern**; desktop: Original; SA tuning 100 % (steering smoothing, response, turn and lean smoothing), maximum lean 45 deg, lean from the turn, VR view roll 40 %; wheelies with Modern handling, hand lift to start 14 cm, full lift 30 cm, hold 200 ms, angle 35 deg, VR view pitch 30 %, over cars on, loop-over off |
| `[cheats]` | all off |

These are `VrSettings::ProductDefaults` (tools\rrgame\vr_settings.cpp) over the member initialisers; a scripted run
(a frame count, the desktop mock's checks and gates) keeps the member initialisers (Stick steering, 100 % eye
resolution, full vibration, seat 0) so the gates ride the races they always did. Nothing machine-specific is in them
(no paths, no device ids, no play-space calibration).

### The settings file

`rrgame_settings.ini` has one section per menu: `[vr]`, `[handling]`, `[cheats]` (and `[graphics]` on the PC). Every
save rewrites only its own section with its complete key list, keeps the other sections as they are, keeps a key of
its section it does not know (a newer build's), and merges a section that appears twice. A UTF-8 byte-order mark and
CR line ends (a file edited on Windows) are read fine. A one-time migration of an older build's section writes its
marker back at the start, so it runs once. Check a file without a headset: `rrgame --settings-roundtrip-check <copy of
the ini>` (reads it as the game starts, changes one value on every menu, reads it back - it CHANGES the file, so give it
a copy).

## Headset layout

| Where | Contents |
|---|---|
| `/sdcard/Android/data/com.rrjb.vr/files/disc.bin` | your disc image (else the first `*.bin` / `*.img` there) |
| app internal storage, `files/saves/` | the memory card `rrjb_card.mcr` (the same format as on PC), `rrgame_settings.ini` (the `[vr]` settings), `controls.ini` |
| `files/rrgame_args.txt` (external, optional) | DEVELOPMENT: more rrgame switches; absent for players |

The disc lookup is `FindDiscImage` and the saves folder `SavesDir` in `src\platform\app_paths.cpp`. The internal
storage stays private; USB save transfer uses the card-only save provider, which works in the release APK.
The provider permits the ADB shell only and refuses a transfer while the game holds its save lock.
The game's log is logcat's tag `RRJB.VR`.

Saves move between PC and Quest with **TRANSFER_PC_SAVES_TO_QUEST.bat** and
**TRANSFER_QUEST_SAVES_TO_PC.bat**; see [save transfer](SAVE-TRANSFER.md).
