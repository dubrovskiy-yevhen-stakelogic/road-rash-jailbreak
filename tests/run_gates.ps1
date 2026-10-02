# Runs every acceptance gate the project has, in one go, and reports pass/fail.
#
# The project's rule: "done" means an observable oracle result. This script is where those results
# live, so the state of the project can be checked rather than remembered.
#
#   powershell -ExecutionPolicy Bypass -File tests\run_gates.ps1 [-Build <dir>] [-Disc <path>] [-Quick]
#
# -Quick skips the gates that take minutes (the deep road verify and the audio cross-check).

param(
    [string]$Build = "build_m0",
    [string]$Disc = "",
    [switch]$Quick
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
# The disc image: -Disc, else RRJB_DISC, else the first line of disc.txt in the repository root (a local file,
# not part of the source kit).
if (!$Disc) { $Disc = $env:RRJB_DISC }
if (!$Disc -and (Test-Path (Join-Path $root "disc.txt"))) { $Disc = (Get-Content (Join-Path $root "disc.txt") -TotalCount 1).Trim() }
if (!$Disc) { Write-Host "No disc image: pass -Disc <image.bin>, set RRJB_DISC or write its path into disc.txt" -ForegroundColor Red; exit 1 }
# The directories the gates write (work\gate_render7, work\gate_game, work\gate_sweep) are per build:
# two gate runs on two builds at once must not regenerate or delete each other's inputs.
$gateTag = Split-Path -Leaf $Build

# The gates open GL windows (the renderer checks and the game gate). None of them needs to be seen,
# and a run that takes minutes should not cover the screen while it does. `hidden` renders
# and reads back exactly as a visible window does - verified byte for byte, same SHA-256 - so this
# costs nothing. Minimizing instead gives a pure black frame; see window_win32.cpp.
$env:RRJB_WINDOW = "hidden"
# The scout probes (pairs.py and the probes that import it, spawn.py, camera.py) start rrverify
# themselves; this makes -Build reach them too instead of their default build directory.
$env:RRJB_PROBE_BUILD = $Build
$tool = Join-Path $root "$Build\rrtool.exe"
$results = @()

function Gate {
    param([string]$Name, [scriptblock]$Body, [string]$Expect)
    Write-Host "== $Name" -ForegroundColor Cyan
    $global:LASTEXITCODE = 0
    $output = & $Body 2>&1 | Out-String
    $code = $global:LASTEXITCODE
    $ok = $output -match $Expect
    $script:results += [pscustomobject]@{ Gate = $Name; Passed = $ok }
    if ($ok) { Write-Host "   pass" -ForegroundColor Green }
    else {
        Write-Host "   FAIL - expected to match: $Expect" -ForegroundColor Red
        # An empty output is a process that died before its stdout reached the pipe: say how it ended.
        if (-not $output -or -not $output.Trim()) {
            Write-Host ("   (no output at all; the last program's exit code 0x{0:X8})" -f $code) -ForegroundColor Red
        }
        else { Write-Host ($output.Trim() -split "`n" | Select-Object -Last 6 | Out-String) }
    }
}

if (-not (Test-Path $tool)) {
    Write-Host "rrtool not found at $tool - build first: cmd /c build.cmd $Build" -ForegroundColor Red
    exit 2
}
if (-not (Test-Path $Disc)) {
    Write-Host "disc image not found: $Disc" -ForegroundColor Red
    exit 2
}

# --- C++ parsers against the disc -------------------------------------------------------------
Gate "models: every *.GEO parses, byte for byte" {
    & $tool geoscan $Disc
} "112 \.GEO files, 0 failed"

Gate "models: every multi-part group assembles" {
    & $tool asmcheck $Disc
} "0 without a program"

Gate "road network: routes close and the distance identity holds" {
    & $tool roadcheck $Disc
} "all invariants hold"

Gate "road geometry: the slice frame agrees with the positions" {
    & $tool roadgeo $Disc "DATA/RACE1_20.STP"
} "the slice frame and the chord agree"

Gate "scene cells: every cell parses and its indices stay in range" {
    & $tool cellcheck $Disc "DATA/RACE1_20.STP"
} "0 failed"

Gate "race routes: every leg chains and resolves to a stream range" {
    & $tool raceplan $Disc
} "every leg chains"

Gate "race world: the assembled centre line has no jump at a junction" {
    & $tool raceworld $Disc 1 20
} "jumps over 60 units: 0 of"

Gate "HUD: the shipped layout parses and matches its own declared counts" {
    $out = Join-Path $root "work\dash"
    New-Item -ItemType Directory -Force $out | Out-Null
    & $tool dash $Disc $out
} "match the declared counts"

# --- independent Python probes ----------------------------------------------------------------
Gate "probe: road network invariants (independent implementation)" {
    python (Join-Path $root "tools\scout\road.py") verify (Join-Path $root "work\disc_us\DATA")
} "ALL CHECKS PASSED"

Gate "probe: chunk invariants (independent implementation)" {
    python (Join-Path $root "tools\scout\chunk.py") verify
} "VERIFY OK"

Gate "probe: scene cell invariants (independent implementation)" {
    python (Join-Path $root "tools\scout\cell.py") verify
} "0 not|0 bad|ok "

Gate "probe: models parse (independent implementation)" {
    python (Join-Path $root "tools\scout\rmd3.py") scan (Join-Path $root "work\disc_us\DATA")
} "112 pass"

# A documented command that cannot be copied and run is wrong the same way a failing test is wrong.
# A heredoc can eat the backslash out of a Windows path, and whatever reads the text
# afterwards turns the surviving `\f` or `\r` into a control character - silently, because a diff
# viewer renders it as nothing.
Gate "docs: no path in a document has lost its backslash escape" {
    python (Join-Path $root "tools\scout\docpaths.py") verify
} "0 broken path"

# The controllers: synthetic XInput / WinMM / DualSense USB and Bluetooth reports, one
# per physical button, through the decoders and the default bindings to the original's pad word; the negative
# control swaps R1 / L1 in the bindings and must FAIL. Needs no disc and no controller.
$padGame = Join-Path $root "$Build\rrgame.exe"
Gate "input: the gamepad bindings give the original's pad bits (XInput, WinMM, DualSense USB / BT)" {
    & $padGame --padmapcheck
} "padmapcheck: \d+ checks, 0 failures\s+padmapcheck verdict PASS"
Gate "input: the pad binding check catches R1 / L1 swapped" {
    & $padGame --padmapcheck-mutate
} "(?s)MUTATED.*padmapcheck verdict FAIL"

# Fight damage: a scripted fight on 1/20 (17 rivals with clubs, health and its
# regeneration ceiling 10, the player punching every 20 frames): a LANDED blow (ReachTest's reach bit, not just the
# last-blow stamp NoteHit writes on a miss too) takes a rival to 0 - KnockOff - and a rival's blow lowers the
# player's health. About 30 s. The control: the same run with no punches hurts no rival.
$fightRun = @("--race", "1", "20", "--hold", "T", "--autosteer", "--opponents", "17", "--opponent-weapons", "1:5",
              "--opponent-health", "10", "--frames", "3900", "--no-sfx")
Gate "the game: a scripted fight lowers health both ways and knocks a rival off at 0 (1/20)" {
    & $padGame $Disc @fightRun --punch 20
} "the player's blows \d+, landed [1-9]\d*, missed \d+; riders hurt: rivals [1-9]\d* \(lowest health left 0\), the player [1-9]\d*; knocked to 0: [1-9]"
Gate "the game: the fight check's control - no punches, no rival hurt (1/20)" {
    & $padGame $Disc @fightRun
} "the player's blows 0, landed 0, missed 0; riders hurt: rivals 0 "

# The hit sound: the animation's sound events AnimSounds SLUS 0x80018E54 / AnimSoundFire 0x80017DA0
# (ANIMNOIZ.DAT), PORTED - the only sound a blow makes in the original. The rows against the original, whole RAM,
# with the mutate control; then the scripted fight WITH sound: on the frame the player's punch lands the SPU model
# keys exactly ONE voice on, with the registers the original keys for it in the oracle's punch-cop run (sound 56 of
# bank 0: @0x168B0, pitch 0x514, volume 0x1B80 both sides, ADSR 0x80FF/0x5FDE), and a rival's blow on the player
# keys exactly one. The control (RRJB_ANIMSOUNDS=off, no animation sound events): the same blows start nothing.
# About a minute each.
$verifyExe = Join-Path $root "$Build\rrverify.exe"
Gate "sound: the animation's sound events (the hit sound) match the original, bit for bit" {
    & $verifyExe phys --only anim_sounds; & $verifyExe phys --only anim_sound_fire
} "(?s)anim_sounds\s+0x80018E54 .*PASS.*anim_sound_fire 0x80017DA0 .*PASS"
Gate "sound: the hit-sound rows fail when the answer is wrong (--mutate)" {
    & $verifyExe phys --only anim_sounds --mutate; & $verifyExe phys --only anim_sound_fire --mutate
} "(?s)anim_sounds\s+0x80018E54 .*FAIL.*anim_sound_fire 0x80017DA0 .*FAIL"
$hitRun = @("--race", "1", "20", "--hold", "T", "--autosteer", "--opponents", "17", "--opponent-weapons", "1:5",
            "--punch", "20", "--frames", "3900")
Gate "the game: a landed punch keys the original's hit sound once (1/20, scripted fight)" {
    & $padGame $Disc @hitRun
} "AnimSounds SLUS 0x80018E54 [1-9]\d* call\(s\); the player's landed blows 1, key-ons on their frames 1: f\d+ ch\d+@168B0 p0514 v1B80/1B80 a80FF/5FDE; other blows that hurt [1-9]\d*, key-ons on their frames [1-9]"
Gate "the game: the hit-sound check's control - AnimSounds off, a landed punch is silent (1/20)" {
    $env:RRJB_ANIMSOUNDS = "off"
    try { & $padGame $Disc @hitRun }
    finally { Remove-Item Env:RRJB_ANIMSOUNDS }
} "AnimSounds SLUS 0x80018E54 0 call\(s\); the player's landed blows 1, key-ons on their frames 0: f\d+;"

# The sound service's clock: on the console AudioVSyncTick SLUS 0x80019990 (and SoundService under it) is
# the VSync callback's (SLUS 0x8001B700: game_state+0x0C += 5, then the call, every vblank) - 60 a second, one per 5
# ticks of the race clock, whatever the game's frame rate. The product runs them per 5 ticks of game time with the
# remainder carried; a 4-tick step (a 72 Hz display) for 600 frames is 2400 ticks = 8 s -> exactly 480. The control
# (RRJB_SOUND_VSYNC=frame: two a frame) gives 1200 = 150 a second and must be CAUGHT.
$paceRun = @("--race", "1", "20", "--hold", "T", "--ticks", "4", "--frames", "600")
Gate "sound: AudioVSyncTick / SoundService run 60 times a simulated second, as the console's vblank (1/20, 72 Hz step)" {
    & $padGame $Disc @paceRun
} "the sound service's clock: 480 vblank\(s\) through AudioVSyncTick/SoundService \(480 asked by the loop\) in 2400 tick\(s\) of game time \(8\.000 s\) = 60\.00 per simulated second; .* -> PASS"
Gate "sound: the service clock check catches two vblanks a frame (RRJB_SOUND_VSYNC=frame)" {
    $env:RRJB_SOUND_VSYNC = "frame"
    try { & $padGame $Disc @paceRun }
    finally { Remove-Item Env:RRJB_SOUND_VSYNC }
} "the sound service's clock: 1200 vblank\(s\) .* = 150\.00 per simulated second; .* -> FAIL"

# Maximum detail's full animation at every distance (src\game\anim_detail.h): the drawn pose of every rival
# and pedestrian the game animates coarsely (LodChoice's part mask / no interpolation) is recomputed at LOD 0 from the
# frame's copy of its clock; the arena is byte-equal around every computation and, where the game itself posed at LOD
# 0, the two poses are equal. The control (RRJB_ANIM_DETAIL=off): nothing computed. ~45 s each.
$detailRun = @("--race", "1", "1", "--hold", "T", "--autosteer", "--frames", "600", "--gfx", "modern")
Gate "graphics: maximum detail animates riders and pedestrians in full at every distance, the arena untouched (1/1)" {
    $env:RRJB_ANIM_DETAIL_CHECK = "1"
    try { & $padGame $Disc @detailRun }
    finally { Remove-Item Env:RRJB_ANIM_DETAIL_CHECK }
} "ANIM DETAIL \(anim_detail\.h[^\n]*: [1-9]\d* frame copies [^\n]*, [1-9]\d* full poses computed [^\n]*part-masked by LodChoice [1-9]\d*,[^\n]* refused 0; check: arena unchanged (\d+) of \1, pose equal to the game's where it posed at LOD 0 [1-9]\d* of \d+ \(differ 0\)"
Gate "graphics: the full-animation control - RRJB_ANIM_DETAIL=off computes nothing (1/1)" {
    $env:RRJB_ANIM_DETAIL = "off"
    try { & $padGame $Disc @detailRun }
    finally { Remove-Item Env:RRJB_ANIM_DETAIL }
} "ANIM DETAIL \(anim_detail\.h[^\n]*: 0 frame copies [^\n]*, 0 full poses computed"

# The cheat menu (src\game\cheats.h, tools\rrgame\cheat_menu.h): every cheat against a control - the same
# run without it (--cheat-report prints the cheats' tally with all of them off). The fight run is the fight gates';
# the police run a stopped player on 1/4 (a cop is released at ~2200); the sparring run rides, then brakes at 600 and
# punches every 20 frames. The time limit through the menus (venue 3, race type 0x21, 240 s). The career on a card the
# scripted menus save (Save Game), then the menu's two career cheats, the backup, the card check, and Load Game shows
# the new venue. About 30 s each; the menu runs ~20 s.
$cheatFight = @("--race", "1", "20", "--hold", "T", "--autosteer", "--opponents", "17", "--opponent-weapons", "1:5",
                "--opponent-health", "10", "--frames", "3900", "--no-sfx", "--punch", "20")
Gate "cheats: god mode - the rivals' blows land, the player is at his race-start health after every seated frame (1/20)" {
    & $padGame $Disc @cheatFight --cheat-god
} "(?s)riders hurt: rivals \d+ \(lowest health left -?\d+\), the player [1-9]\d*;.*god: health / ceiling put back [1-9]\d* time\(s\) [^\n]*the player at his race-start health after (\d+) of \1 seated frame"
Gate "cheats: rivals passive - no rival engages the player, none hits him (1/20)" {
    & $padGame $Disc @cheatFight --cheat-passive
} "(?s)riders hurt: rivals \d+ \(lowest health left -?\d+\), the player 0;.*passive: engagements refused [1-9]\d*"
Gate "cheats: the fight's control - no cheat, the rivals hit the player and his health drops (1/20)" {
    & $padGame $Disc @cheatFight --cheat-report
} "(?s)the player [1-9]\d*;.*passive: engagements refused 0,.*the player at his race-start health after (\d+) of (?!\1 seated)\d+ seated"
$cheatPolice = @("--race", "1", "4", "--frames", "3000", "--autosteer", "--brake-from", "300", "--no-sfx")
Gate "cheats: no police - no cop is released on a stopped player (1/4)" {
    & $padGame $Disc @cheatPolice --cheat-no-police
} "(?s)^(?=.*cops out at most 0\b)(?=.*cop bikes released 0 \(at most 0 live\))"
Gate "cheats: no police's control - the same stopped player draws a cop (1/4)" {
    & $padGame $Disc @cheatPolice --cheat-report
} "(?s)^(?=.*cops out at most [1-9])(?=.*cop bikes released [1-9]\d* \(at most [1-9])"
$cheatTraffic = @("--race", "1", "20", "--frames", "2000", "--hold", "T", "--autosteer", "--no-sfx")
Gate "cheats: no traffic - no car is spawned (1/20)" {
    & $padGame $Disc @cheatTraffic --cheat-no-traffic
} "pool 3 live at most 0\b"
Gate "cheats: no traffic's control - cars on the road (1/20)" {
    & $padGame $Disc @cheatTraffic --cheat-report
} "pool 3 live at most [1-9]"
$cheatWeapon = @("--race", "1", "20", "--frames", "1200", "--hold", "T", "--autosteer", "--opponents", "17", "--punch", "15",
                 "--chase", "-1", "--no-sfx")
Gate "cheats: a weapon of the disc's list from the start - the swings take that weapon's object (1/20, weapon 1)" {
    & $padGame $Disc @cheatWeapon --cheat-weapon 1
} "(?s)the disc's weapons \([^)]*\): 0=\w{4} 1=\w{4} 2=\w{4} 3=\w{4} 4=\w{4} 5=\w{4} 6=\w{4} 7=\w{4} 8=\w{4}.*the player's hand held the weapon objects \(frames\): \w{4} \(weapon 1\) x[1-9]\d*\r?\n"
Gate "cheats: put in the hand now - weapon 8 at frame 300, its swing takes its object (1/20)" {
    & $padGame $Disc @cheatWeapon --punch-action 5 --cheat-weapon-now 300:8 --cheat-report
} "put in the hand 1 time\(s\) \(first at frame 301: \w{4} \(weapon 8\)\)[^\n]*the player's hand held the weapon objects \(frames\): \w{4} \(weapon 8\) x[1-9]\d*\r?\n"
Gate "cheats: the weapon's control - fists, no weapon object in the hand (1/20)" {
    & $padGame $Disc @cheatWeapon --cheat-report
} "the player's hand held the weapon objects \(frames\): none"
$cheatSpar = @("--race", "1", "20", "--frames", "2400", "--autosteer", "--brake-from", "600", "--punch", "20", "--no-sfx")
Gate "cheats: sparring partners stay in reach of the standing player, his punches land, they do not hit back (1/20)" {
    & $padGame $Disc @cheatSpar --cheat-sparring passive
} "(?s)landed [1-9]\d*, missed \d+; riders hurt: rivals [1-9]\d* \(lowest health left \d+\), the player 0;.*the player stood \d+ frame\(s\), a partner inside FightUpdate's reach on [1-9]\d{3} of them"
Gate "cheats: sparring's control - the same stopped player, nobody within reach, no blow (1/20)" {
    & $padGame $Disc @cheatSpar --cheat-report
} "the player's blows 0, landed 0"
$cheatDir = Join-Path $root "work\gate_cheats_$gateTag"
New-Item -ItemType Directory -Force $cheatDir | Out-Null
$cheatTimer = @("--shell-frames", "60", "--shell-script", "2:g4;6:x;14:x;20:x;30:x;37:v3;38:down;40:up;44:x",
                "--shell-card", (Join-Path $cheatDir "timer_card.mcr"))
Gate "cheats: freeze the time limit - the 240 s race keeps its ticks left (venue 3 through the menus)" {
    & $padGame $Disc @cheatTimer --shell-race "--frames 900 --no-sfx --hold T --cheat-freeze-timer"
} "time limit frozen 900 frame\(s\), ticks left 72000 at the start and 72000 after the last frame"
Gate "cheats: the time limit's control - the ticks left run down (venue 3 through the menus)" {
    & $padGame $Disc @cheatTimer --shell-race "--frames 900 --no-sfx --hold T --cheat-report"
} "time limit frozen 0 frame\(s\), ticks left 72000 at the start and (?!72000 )\d+ after the last frame"
$shellExe = Join-Path $root "$Build\rrshell.exe"
Gate "cheats: career - the next venue and max nitros / weapons on a saved series: backup, verified card, Load Game shows it" {
    $card = Join-Path $cheatDir "card.mcr"
    foreach ($f in $card, "$card.before-cheats.bak", "$card.cheat-tmp") { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f } }
    & $padGame $Disc --shell-frames 320 --shell-script "2:g4;6:x;14:x;20:x;30:x;40:g45;170:x;190:x;220:x;250:x" --shell-card $card | Out-Null
    $before = (Get-FileHash -LiteralPath $card).Hash
    & $padGame $Disc --cheat-career venue $card
    & $padGame $Disc --cheat-career max $card
    "backup equal to the card before: " + ((Get-FileHash -LiteralPath "$card.before-cheats.bak").Hash -eq $before)
    & $shellExe cardcheck $Disc $card
    $log = Join-Path $cheatDir "load_log.txt"
    & $padGame $Disc --shell-frames 150 --shell-script "2:g43;140:x" --shell-card $card --shell-log $log | Out-Null
    Get-Content -LiteralPath $log | Select-String "^f140 " | ForEach-Object { $_.Line }
} "(?s)cheat-career: Career slot 0: series venue 0 -> 1 - saved and verified.*cheat-career: Career slot 0: nitros \d+, 9 weapons at level \d+ - saved and verified.*backup equal to the card before: True.*PASS.*f140 +screen +\d+ +sel +\d+ +mode 0x20 venue 1 "
Gate "cheats: career's control - a career record that fails its checksum is refused, the card left as it was" {
    $card = Join-Path $cheatDir "card.mcr"
    $bad = Join-Path $cheatDir "card_bad.mcr"
    $bytes = [System.IO.File]::ReadAllBytes("$card.before-cheats.bak")
    $bytes[8192 + 0x834 + 16] = $bytes[8192 + 0x834 + 16] -bxor 0x5A # a byte of career slot 0's player record
    [System.IO.File]::WriteAllBytes($bad, $bytes)
    $before = (Get-FileHash -LiteralPath $bad).Hash
    & $padGame $Disc --cheat-career venue $bad
    "card unchanged: " + ((Get-FileHash -LiteralPath $bad).Hash -eq $before)
} "(?s)career slot 0 fails its checksum \(nothing written\).*card unchanged: True"

# VR: the handlebars in the hands (tools\rrgame\vr_handlebars.h), on the desktop VR mock with scripted
# controllers (--vr-bars-script): both hands take the grips, the right wrist twists the throttle open, the bars turn 20
# degrees left, back, 20 right, back; then the right hand lets go and swings. The bike must turn LEFT while the bars
# steer left and RIGHT while they steer right (the heading, from the bike's own frame), reach speed on the twist alone,
# and the swing must punch. Controls: the twist throttle off (no speed), the Stick mode (the bars do nothing). Then a
# fight: the right hand off the bars as a fist swinging every 40 frames beside the rivals (1/20 --autosteer) must LAND
# blows (FightStat) with the haptic thump; its control with motion punches off throws none. About 10 s + 10 s + 10 s,
# 60 s + 60 s.
# (--vr-combat gesture: the swing gesture throws blows only in the Gesture combat mode; Physical is VR's default)
$barsRun = @("--vr-mock", "--vr-steering", "handlebars", "--vr-combat", "gesture", "--race", "1", "20", "--frames", "700",
             "--no-sfx")
$barsScript = "20 grab both; 20 twist 30 30; 300 turn 20 20; 360 turn 0 20; 420 turn -20 20; 480 turn 0 20; " +
              "560 release right; 572 move right 0.30 0.10 -0.30 8; 600 home right; 620 grab right"
Gate "vr: the handlebars in the hands steer both ways, the twist opens the throttle, a swing punches (mock, 1/20)" {
    & $padGame $Disc @barsRun --vr-bars-script $barsScript
} "vr bars: steering HANDLEBARS [^\n]* - grabs left 1 right 2, [^\n]*bar angle -20\.0 \.\. \+20\.0 deg, [^\n]*the heading turned -([5-9]|[1-9]\d+)\.\d deg while the bars steered left, \+([5-9]|[1-9]\d+)\.\d deg while they steered right; top speed held [2-9]\d\.\d; throttle up to 0\.8\d \(twist up to \+30\.0 deg\), punches right 1 left 0"
Gate "vr: the handlebars' control - the twist throttle off, no speed (mock, 1/20)" {
    & $padGame $Disc @barsRun --vr-twist-throttle 0 --vr-bars-script $barsScript
} "vr bars: steering HANDLEBARS \(full lock 30 deg, dead zone 3%, twist throttle off[^\n]*top speed held 0\.0; throttle up to 0\.00"
Gate "vr: the handlebars' control - the Stick mode, the scripted hands do nothing (mock, 1/20)" {
    & $padGame $Disc @barsRun --vr-steering stick --vr-bars-script $barsScript
} "vr bars: steering stick - grabs left 0 right 0, [^\n]*the analogue device on 0 of 0 pad frame\(s\)[^\n]*punches right 0 left 0"
$vrGateDir = Join-Path $root "work\gate_vr_$gateTag"
New-Item -ItemType Directory -Force $vrGateDir | Out-Null
$fistScript = Join-Path $vrGateDir "fist.txt"
$fistLines = @("10 grip right 1")
for ($f = 300; $f -lt 6000; $f += 40) { $fistLines += "$f move right 0.30 0.05 -0.30 6"; $fistLines += "$($f + 14) home right" }
Set-Content -LiteralPath $fistScript -Value $fistLines -Encoding ascii
$fistRun = @("--vr-mock", "--vr-steering", "handlebars", "--vr-combat", "gesture", "--vr-bars-script", $fistScript, "--race",
             "1", "20", "--hold", "T", "--autosteer", "--opponents", "17", "--opponent-weapons", "1:5", "--frames", "6000",
             "--no-sfx")
Gate "vr: a fist swung off the bars lands the player's blows (mock, 1/20, scripted fight)" {
    & $padGame $Disc @fistRun
} "(?s)punches right [1-9]\d* left 0, kicks 0, landed pulses [1-9].*the player's blows [1-9]\d*, landed [1-9]"
Gate "vr: the fist check's control - motion punches off, no blow (mock, 1/20)" {
    & $padGame $Disc @fistRun --vr-motion-punches 0
} "(?s)punches right 0 left 0.*the player's blows 0, landed 0"

# VR physical combat (tools\rrgame\vr_melee.h): a blow is a CONTACT of the tracked fist / the held weapon
# with a rider's posed body, applied on that rider through the ORIGINAL's fight code (src\game\fight_physical.h). The
# desktop VR mock with scripted hands (--vr-melee-script: blows aimed at the nearest rider), race 1/20 with the sparring
# partners (--cheat-sparring passive: a rival held beside the player). A fist driven through the torso at
# 5 m/s: exactly one blow, FIGHT.BIN record 0 node 1, the victim's drop equal to ApplyHit's formula, his hit stance, and
# the original's hit-sound voice (@168B0) keyed on that frame. Controls: the same path at 0.8 m/s (touches, too
# slow), 0.8 m above the head (no touch), Buttons mode (nothing sent), the swinging hand on the bars (touches, no blow);
# the club's tip across the head beyond fist reach is a weapon blow (record 4) and the same sweep with bare fists is
# none; strength from swing speed scales the node's damage. About 10 s each.
# (--vr-holsters 0 on the club's gate: the weapon in the hand from the start - with the holsters the race starts
# with it on the hip; the holsters' own gates follow these)
# (--vr-bike-lean 100: the scene these checks are set in - the head view's visual lean moves the player's
# body with the drawn bike, and at 50 % the resting open left hand brushes the partner riding alongside)
$meleeRun = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--autosteer", "--cheat-sparring", "passive", "--opponents",
              "17", "--frames", "800", "--vr-bike-lean", "100")
$meleeHit = "100 grip right 1; 660 swing right torso 5"
Gate "vr: a tracked fist through a rider's torso lands one blow through the original's fight code (mock, 1/20)" {
    & $padGame $Disc @meleeRun --vr-melee-script $meleeHit
} "(?s)(?=.*vr melee: combat PHYSICAL [^\n]* blows 1 \(fist 1, weapon 0\))(?=.*physical blow: f\d+ the right fist on rival b\d+ [^\n]*command 32, FIGHT record 0 node 1 [^\n]*the rival's stance (\d+) \(the node's hit stance \1\), mount 1; health b\d+ \d+->\d+)(?=.*the player's landed blows 1, key-ons on their frames [1-9]: f\d+ [^;]*@168B0 p0514 v1B80/1B80 a80FF/5FDE;)(?=.*the physical blows: 1 contact\(s\) sent, 1 applied through ApplyHit 0x800C17B0 \(landed 1, riders hurt 1, the drop equal to the original's formula 1,)"
Gate "vr: the physical blow's control - the same fist at 0.8 m/s touches, no blow (mock, 1/20)" {
    & $padGame $Disc @meleeRun --no-sfx --vr-melee-script "100 grip right 1; 660 swing right torso 0.8"
} "(?s)(?=.*blows 0 \(fist 0, weapon 0\), touches without a blow: too slow [1-9][^\n]*script blows 1 \(no rider in reach 0\))(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr: the physical blow's control - a swing 0.8 m above the torso touches nothing (mock, 1/20)" {
    & $padGame $Disc @meleeRun --no-sfx --vr-melee-script "100 grip right 1; 650 offset 0 0.8 0; 660 swing right torso 5"
} "(?s)(?=.*blows 0 \(fist 0, weapon 0\), touches without a blow: too slow 0, grazing 0, not armed / open hand 0, the same rider again 0, a hand on the bars 0;[^\n]*script blows 1 \(no rider in reach 0\))(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr: the physical blow's control - combat mode Buttons, the contact sends nothing (mock, 1/20)" {
    & $padGame $Disc @meleeRun --no-sfx --vr-combat buttons --vr-melee-script $meleeHit
} "(?s)(?=.*vr melee: combat buttons - [^\n]*blows 0 )(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr: the physical blow's control - the swinging hand holds the handlebars, no blow (mock, 1/20)" {
    & $padGame $Disc @meleeRun --no-sfx --vr-steering handlebars --vr-one-hand 0 --vr-bars-script "20 grab right" --vr-melee-script $meleeHit
} "(?s)(?=.*blows 0 \(fist 0, weapon 0\), [^\n]*a hand on the bars [1-9]\d*;)(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr: the club's tip across a rider's head beyond fist reach is a weapon blow (mock, 1/20, --weapon 1:3)" {
    & $padGame $Disc @meleeRun --no-sfx --weapon 1:3 --vr-holsters 0 --vr-melee-script "100 grip right 1; 660 sweep right head 4 0.45"
} "(?s)(?=.*blows 1 \(fist 0, weapon 1\))(?=.*physical blow: f\d+ the right weapon on rival b\d+ \(part 4,[^\n]*command 142, FIGHT record 4 )(?=.*the drop equal to the original's formula 1,)"
Gate "vr: the weapon check's control - the same sweep with bare fists touches nothing (mock, 1/20)" {
    & $padGame $Disc @meleeRun --no-sfx --vr-melee-script "100 grip right 1; 660 sweep right head 4 0.45"
} "(?s)(?=.*blows 0 \(fist 0, weapon 0\), touches without a blow: too slow 0, grazing 0,[^\n]*script blows 1 \(no rider in reach 0\))(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr: strength from swing speed scales the blow's base damage, ApplyHit's formula holds (mock, 1/20)" {
    & $padGame $Disc @meleeRun --no-sfx --vr-melee-strength 1 --vr-melee-script $meleeHit
} "(?s)(?=.*physical blow: f\d+ [^\n]*damage 20 x1\.[1-5]\d = 2[2-9]\))(?=.*the drop equal to the original's formula 1,)"

# VR weapon snatching, the nunchaku's chain, the sparring partners' attack (tools\rrgame\vr_snatch.h,
# vr_nunchaku.h, src\game\fight_physical.h PhysicalSnatchRun, src\game\cheats.h). The partners attack a STANDING player
# with the club (cheat rule on, god mode so he stays on the bike); the mock's left hand tracks the nearest partner's
# club, open, and closes on it when he swings it (`snatch left`): the grab goes through the original's WeaponSteal - the
# club changes hands (the player's weapon 9 -> 1, the partner's 1 -> 9, the cheat's log: the partner lost it).
# Controls: snatching off (the same hand closes on the club: nothing sent, nothing taken); the grip closed 45 cm
# above the club (no grab). The partners' attack on the desktop, and its control without the cheat rule (the original never
# attacks a standing player). The nunchaku (weapon 4, not 0: the rap sheet's order) in the right hand swept
# past a partner: its chain hangs (the far stick 0.4 m and more below the handle's end at rest) and its far stick lands
# the blow (FIGHT record 8, the original's for weapon 4); control: the chain physics off. About 15 s each.
$snatchRun = @("--vr-mock", "--race", "1", "20", "--autosteer", "--brake-from", "300", "--opponents", "17", "--frames", "1100",
               "--cheat-sparring", "passive", "--cheat-sparring-attack", "--cheat-sparring-weapon", "1", "--cheat-sparring-standing",
               "--cheat-god", "--no-sfx")
Gate "vr: a free hand closing on a partner's club mid-swing takes it through WeaponSteal (mock, 1/20, the standing player attacked)" {
    & $padGame $Disc @snatchRun --vr-melee-script "300 snatch left 3"
} "(?s)^(?=.*vr snatch: frame \d+ - the left hand closed on rival b\d+'s weapon 1 \(he swings it;)(?=.*weapon snatch: f\d+ the left hand on rival b(\d+)'s weapon 1 \(his clip frame \d+ of \d+\): command 32, FIGHT record 0 node 1, [^\n]*-> STOLEN by WeaponSteal; the player's weapon 9 -> 1 \(owned [0-9A-F]{3} -> [0-9A-F]{3}\), rival b\1's 1 -> 9)(?=.*vr snatch: frame \d+ - the left hand TAKEN rival b\d+'s weapon 1)(?=.*sparring partner b\d+ lost its \w{4} \(weapon 1\) \(stolen\))"
Gate "vr: the snatch's control - snatching off, the same hand closes on the club, nothing is taken (mock, 1/20)" {
    & $padGame $Disc @snatchRun --vr-melee-snatch 0 --vr-melee-script "300 snatch left 3"
} "(?s)^(?=.*vr melee script: frame \d+ snatch left closes on rival b\d+'s weapon 1)(?=.*grabs sent 0, weapons TAKEN 0)(?=.*the weapon snatches: 0 grab\(s\) sent)(?!.*lost its)"
Gate "vr: the snatch's control - the grip closed 45 cm above the club takes nothing (mock, 1/20)" {
    & $padGame $Disc @snatchRun --vr-melee-script "300 snatch left miss 3"
} "(?s)^(?=.*vr melee script: frame \d+ snatch left closes on rival b\d+'s weapon 1)(?=.*closed on a rival's weapon 0, grabs sent 0, weapons TAKEN 0)(?!.*lost its)"
$partnerRun = @("--race", "1", "20", "--autosteer", "--brake-from", "300", "--opponents", "17", "--frames", "1500", "--no-sfx",
                "--cheat-sparring", "passive", "--cheat-sparring-attack", "--cheat-sparring-weapon", "1")
Gate "cheats: partners attack - with the club, even the standing player (the cheat rule), his health drops (1/20)" {
    & $padGame $Disc @partnerRun --cheat-sparring-standing
} "(?s)^(?=.*riders hurt: rivals \d+ \(lowest health left -?\d+\), the player [1-9]\d*;)(?=.*partners attack with \w{4} \(weapon 1\), even a standing player \(cheat rule\))(?=.*partners attacking: armed [1-9]\d* time\(s\), [^;]*frames in a fight [1-9]\d*, the standing-player lifts [1-9]\d*)"
Gate "cheats: partners attack's control - without the cheat rule nobody attacks the standing player (1/20)" {
    & $padGame $Disc @partnerRun
} "(?s)^(?=.*riders hurt: rivals \d+ \(lowest health left -?\d+\), the player 0;)(?=.*frames in a fight 0, the standing-player lifts 0)"
$nunRun = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--autosteer", "--cheat-sparring", "passive", "--opponents", "17",
            "--frames", "800", "--vr-bike-lean", "100", "--no-sfx", "--cheat-weapon", "4", "--vr-holsters", "0")
$nunSweep = "100 grip right 1; 660 sweep right torso 5 0.45"
Gate "vr: the nunchaku's chain hangs and swings in the hand, its far stick lands the blow (mock, 1/20, weapon 4)" {
    & $padGame $Disc @nunRun --vr-melee-script $nunSweep
} "(?s)^(?=.*vr nunchaku: model 800 group 0 \(chain, weapon 0\): 4 parts[^\n]*group 4 \(nunchaku, weapon 4\): 4 parts, lengths 277 101 97 419)(?=.*vr melee: frame \d+ - the right nunchaku end hits rival b\d+ part \d+ at [3-9]\.\d+ m/s)(?=.*physical blow: f\d+ the right weapon on rival b\d+ [^\n]*command 142, FIGHT record 8 )(?=.*vr nunchaku: the chain simulated in [1-9]\d* frame\(s\)[^\n]*the tip hung at most 0\.[4-9]\d m below the handle's end)"
Gate "vr: the nunchaku's control - the chain physics off, the straight collider, no chain (mock, 1/20)" {
    & $padGame $Disc @nunRun --vr-melee-nunchaku 0 --vr-melee-script $nunSweep
} "(?s)^(?=.*the chain simulated in 0 frame\(s\))(?!.*nunchaku end hits)"
# VR holsters, the inventory and the swing's rules (tools\rrgame\vr_holsters.h, vr_weapon_calib.h): the
# player's weapons live on the hips and are drawn with a hand; what is owned is the game's own record (riderDef +0x2C).
# The desktop mock's hands (--vr-melee-script) with holster actions over them (--vr-holster-script), race 1/20 with the
# sparring partner (as above). The race starts with the club (--weapon 1:3) in its holster (the quick race's chain bit
# with no swings stays hidden); drawn from the right hip it lands a 4 m/s sweep across the partner's head through the
# original's fight code. Controls: the same sweep at 1.6 m/s (under the weapon swing speed 3 m/s), with 60 cm of swing
# travel asked (a short swing), with the grip calibrated 30 cm back (the collider is the drawn weapon: nothing touched),
# and with the club left in its holster (bare hands). About 10 s each.
$holRun = @($meleeRun) + @("--no-sfx", "--weapon", "1:3")
$holSweep = "100 grip right 1; 660 sweep right head 4 0.45"
Gate "vr holsters: the club drawn from the right hip lands a 4 m/s sweep through the original's fight code (mock, 1/20)" {
    & $padGame $Disc @holRun --vr-melee-script $holSweep --vr-holster-script "300 draw right right"
} "(?s)\A(?=.*the race starts with weapon 1 \(CLUB[^\n]*goes to its holster)(?=.*the right hand DRAWS weapon 1 \(CLUB[^\n]*from the right holster)(?=.*the record's weapon in hand 9 -> 1 \(\+0x2F 3\))(?=.*the right hand's weapon swing: [2-9]\d cm of travel, 3\.\d+ m/s at the contact)(?=.*blows 1 \(fist 0, weapon 1\))(?=.*physical blow: f\d+ the right weapon on rival b\d+ \(part 4,[^\n]*command 142, FIGHT record 4 )(?=.*the drop equal to the original's formula 1,)(?=.*holsters left -1 right 1;)"
Gate "vr holsters: the drawn club's control - the same sweep at 1.6 m/s is too slow for a weapon (mock, 1/20)" {
    & $padGame $Disc @holRun --vr-melee-script "100 grip right 1; 660 sweep right head 1.6 0.45" --vr-holster-script "300 draw right right"
} "(?s)\A(?=.*DRAWS weapon 1 )(?=.*no blow, too slow for a weapon \(1\.\d+ m/s at the contact)(?=.*blows 0 \(fist 0, weapon 0\))(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr holsters: the drawn club's control - 60 cm of swing travel asked, its 43 cm are a short swing (mock, 1/20)" {
    & $padGame $Disc @holRun --vr-weapon-travel 60 --vr-melee-script $holSweep --vr-holster-script "300 draw right right"
} "(?s)\A(?=.*DRAWS weapon 1 )(?=.*no blow, a short swing \(4\d cm of travel, the weapon swing travel 60 cm\))(?=.*blows 0 \(fist 0, weapon 0\))(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr holsters: the drawn club's control - its grip calibrated 30 cm back, the same sweep touches nothing (mock, 1/20)" {
    & $padGame $Disc @holRun --vr-weapon-grip "1:0,0,-300,0,0,0" --vr-melee-script $holSweep --vr-holster-script "300 draw right right"
} "(?s)\A(?=.*DRAWS weapon 1 )(?=.*blows 0 \(fist 0, weapon 0\), touches without a blow: too slow 0, grazing 0, not armed / open hand 0,)(?=.*the physical blows: 0 contact\(s\) sent)"
Gate "vr holsters: the drawn club's control - left in its holster, the same sweep is bare-handed and touches nothing (mock, 1/20)" {
    & $padGame $Disc @holRun --vr-melee-script $holSweep
} "(?s)\A(?=.*draws left 0 right 0, )(?=.*blows 0 \(fist 0, weapon 0\), touches without a blow: too slow 0, grazing 0,)(?=.*the physical blows: 0 contact\(s\) sent)"

# The holster cycle (no rival needed, the player riding straight): draw the club from the right hip, put it back there,
# draw the chain (weapon 0) from the left hip (its no-swing bit shown: --vr-holster-hide-empty 0), let go of it away from the
# holsters - it goes back to its own. Each change writes the record's weapon in hand (the race start's too: 5). Control:
# holsters off - the scripted hands draw nothing and the club stays in the rider's hand. About 5 s each.
$holCycle = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--frames", "520", "--no-sfx", "--weapon", "1:3", "--vr-holster-hide-empty", "0",
              "--vr-melee-script", "100 grip right 1", "--vr-holster-script", "150 draw right right; 300 stow right; 380 draw right left; 460 release right 12")
Gate "vr holsters: draw, put back, draw from the other hip, let go away - the record's weapon in hand follows (mock, 1/20)" {
    & $padGame $Disc @holCycle
} "(?s)\A(?=.*DRAWS weapon 1 [^\n]*from the right holster)(?=.*lets go of weapon 1 at a holster: back to the right holster)(?=.*DRAWS weapon 0 [^\n]*from the left holster)(?=.*lets go of weapon 0 away from the holsters: back to the left holster)(?=.*the record written 5 \(deferred \d+\))(?=.*owned at the end 0,1, in hand 9,)"
Gate "vr holsters: the cycle's control - holsters off, nothing drawn, the club stays in the rider's hand (mock, 1/20)" {
    & $padGame $Disc @holCycle --vr-holsters 0
} "(?s)\A(?=.*draws left 0 right 0, put back 0 [^\n]*the record written 0 )(?=.*owned at the end 0,1, in hand 1,)"

# The combat buttons (vr_holsters.h FilterButtons; the combat mode alone decides, in both steering modes):
# A pressed ten times beside the sparring partner through the VR bindings (as a headset's A) - with Physical only no blow
# and the suppressed frames counted; the Stick mode's control (the default Buttons + physical) lands blows. The prod (--weapon 6:15) drawn and swept slowly past the partner's head with B pressed every 4 frames: B
# discharges it on the rider its far end touches - blows through the fight code (command 148, FIGHT record 18), the
# blow cooldown between them; control: the prod button off (B is the kick again, no discharge). About 10 s each.
$holButtons = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--autosteer", "--cheat-sparring", "passive", "--opponents", "17",
                "--frames", "880", "--vr-bike-lean", "100", "--no-sfx", "--vr-melee-script", "100 grip right 0", "--vr-holster-script",
                ((680..860 | Where-Object { $_ % 20 -eq 0 } | ForEach-Object { "$_ button A 3" }) -join "; "))
Gate "vr holsters: combat Physical only - in the Handlebars mode the A button does not attack (mock, 1/20)" {
    & $padGame $Disc @holButtons --vr-steering handlebars --vr-combat physical
} "(?s)\A(?=.*combat mode physical: button frames suppressed [1-9]\d*,)(?=.*the fight totals: the player's blows 0,)"
Gate "vr holsters: the buttons' control - the Stick mode's A attacks (mock, 1/20)" {
    & $padGame $Disc @holButtons --vr-steering stick
} "(?s)\A(?=.*combat mode buttons\+physical: button frames suppressed 0,)(?=.*the fight totals: the player's blows [1-9]\d*, landed [1-9])"

# The buttons are VR's default combat (Buttons + physical) in both steering modes. The Handlebars mode with
# BOTH hands on the bars (taken at frame 670, once the autosteer has the sparring partner alongside): A ten times lands
# the original's punches (command 32, the partner hurt); the club drawn from the right hip before the bars were taken
# (the weapon hand takes its grip too where the buttons attack) - A swings the CLUB (command 142, FIGHT record 4); the
# stun gun drawn, B on the bars - its original attack lands; A with the stun gun - its own attack (action 6,
# command 148). Controls: Physical only - the same A with bare hands and with the club: suppressed, no blow (the club's
# hand does not take the bars there). A settings file without combat_defaults (combat=physical,
# bars_buttons_attack=0, as older builds wrote it) is read as Buttons + physical, logged; control: a file already
# carrying combat_defaults=1cr keeps its Physical only. About 10 s each.
$btnA = (680..860 | Where-Object { $_ % 20 -eq 0 } | ForEach-Object { "$_ button A 3" }) -join "; "
$btnB = (680..860 | Where-Object { $_ % 20 -eq 0 } | ForEach-Object { "$_ button B 3" }) -join "; "
$btnRun = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--autosteer", "--cheat-sparring", "passive", "--opponents", "17",
            "--frames", "880", "--vr-bike-lean", "100", "--no-sfx", "--vr-steering", "handlebars",
            "--vr-bars-script", "670 grab left; 670 grab right")
Gate "vr combat: Buttons + physical by default - A with both hands on the bars lands the original's punches (mock, 1/20)" {
    & $padGame $Disc @btnRun --vr-melee-script "10 grip right 0" --vr-holster-script $btnA
} "(?s)\A(?=.*vr melee: combat PHYSICAL \+ BUTTONS )(?=.*grabs left 1 right 1, frames held with both hands [1-9]\d* )(?=.*combat mode buttons\+physical: button frames suppressed 0,)(?=.*the fight totals: the player's blows [1-9]\d*, landed [1-9]\d*[^\n]*riders hurt: rivals [1-9])"
Gate "vr combat: the buttons' control - Physical only, the same A on the bars is no blow (mock, 1/20)" {
    & $padGame $Disc @btnRun --vr-combat physical --vr-melee-script "10 grip right 0" --vr-holster-script $btnA
} "(?s)\A(?=.*grabs left 1 right 1, frames held with both hands [1-9]\d* )(?=.*combat mode physical: button frames suppressed [1-9]\d*,)(?=.*the fight totals: the player's blows 0,)"
Gate "vr combat: the club drawn, both hands on the bars - A swings the club, command 142 (mock, 1/20)" {
    $log = Join-Path $vrGateDir "btn_club.log"
    & $padGame $Disc @btnRun --weapon 1:3 --log $log --vr-melee-script "100 grip right 1" --vr-holster-script "300 draw right right; $btnA"
    Get-Content $log | Select-String -Pattern "landed=[1-9]\d* [^\n]*player cmd=\d+ " | Select-Object -First 1 | ForEach-Object { "first landed: " + $_.Line.Trim() }
} "(?s)\A(?=.*the right hand DRAWS weapon 1 \(CLUB)(?=.*vr bars: frame 670 - the right hand takes its grip)(?=.*grabs left 1 right 1,)(?=.*the player's blows [1-9]\d*, landed [1-9])(?=.*owned at the end 0,1, in hand 1,)(?=.*first landed: [^\n]*player cmd=142 [^\n]*rec=4 )"
Gate "vr combat: the club's control - Physical only, the club's hand keeps off the bars and A is no blow (mock, 1/20)" {
    & $padGame $Disc @btnRun --vr-combat physical --weapon 1:3 --vr-melee-script "100 grip right 1" --vr-holster-script "300 draw right right; $btnA"
} "(?s)\A(?=.*the right hand DRAWS weapon 1 \(CLUB)(?=.*grabs left 1 right 0,)(?=.*combat mode physical: button frames suppressed [1-9]\d*,)(?=.*the player's blows 0,)"
Gate "vr combat: the stun gun drawn, both hands on the bars - B's original attack lands, A its own attack (mock, 1/20)" {
    & $padGame $Disc @btnRun --weapon 7:15 --vr-melee-script "100 grip right 1" --vr-holster-script "300 draw right right; $btnB"
    & $padGame $Disc @btnRun --weapon 7:15 --vr-melee-script "100 grip right 1" --vr-holster-script "300 draw right right; $btnA"
} "(?s)\A(?=.*grabs left 1 right 1,.*grabs left 1 right 1,)(?=.*the stun gun's original attack \(B at frame \d+\) LANDS on rival b\d+[^\n]*\(health \d+ -> \d+\))(?=.*A \(R1\) with weapon 7 in hand: its own attack, combat action 6)(?=.*A / X as a weapon's own attack [1-9]\d*.*the player's blows [1-9]\d*, landed [1-9])"
$oldIni = Join-Path $vrGateDir "settings_0_1_6.ini"
Set-Content -LiteralPath $oldIni -Encoding ascii -Value @("[vr]", "steering=handlebars", "combat=physical", "bars_buttons_attack=0", "prod_button=1")
$newIni = Join-Path $vrGateDir "settings_1cr.ini"
Set-Content -LiteralPath $newIni -Encoding ascii -Value @("[vr]", "steering=handlebars", "combat=physical", "combat_defaults=1cr")
Gate "vr combat: a settings file without combat_defaults starts from Buttons + physical once, logged" {
    & $padGame --vr-settings-check $oldIni
} "(?s)has no combat_defaults - the combat starts from Buttons \+ physical \(was physical\).*combat buttons\+physical \(the buttons attack\).*combat_defaults=1cr"
Gate "vr combat: the migration's control - a file with combat_defaults keeps Physical only" {
    & $padGame --vr-settings-check $newIni
} "(?s)\A(?!.*has no combat_defaults)(?=.*combat physical \(the buttons do not attack\))"
# (--vr-prod-reach 0: the contact-only B these two check - otherwise a B touching no rider is the
# original's own attack, its gates follow the holster menu's)
$holProd = @($meleeRun) + @("--no-sfx", "--weapon", "6:15", "--vr-prod-reach", "0", "--vr-melee-script", "100 grip right 1; 660 sweep right head 0.8 0.30",
                            "--vr-holster-script", ("300 draw right right; " + ((700..880 | Where-Object { $_ % 4 -eq 0 } | ForEach-Object { "$_ button B 2" }) -join "; ")))
Gate "vr holsters: B discharges the prod held in the hand on the rider it touches (mock, 1/20)" {
    & $padGame $Disc @holProd
} "(?s)\A(?=.*DRAWS weapon 6 \(PROD)(?=.*DISCHARGES on rival b\d+)(?=.*physical blow: f\d+ the right weapon on rival b\d+ [^\n]*command 148, FIGHT record 18 )(?=.*the prod's discharges \d+ \(blows [1-9]\d*, in the air [1-9]\d*, recharging [1-9]\d*\))(?=.*the drop equal to the original's formula [1-9]\d*,)"
Gate "vr holsters: the prod's control - the prod button off, B discharges nothing (mock, 1/20)" {
    & $padGame $Disc @holProd --vr-prod-button 0
} "(?s)\A(?=.*DRAWS weapon 6 \(PROD)(?=.*the prod's discharges 0 )(?=.*the physical blows: 0 contact\(s\) sent)"

# The holster menu through the real VR menu (--vr-mock-pad: the chord, the stick, A, the right trigger, Menu): the prod
# given in the hand at frame 50 (--cheat-weapon-now) is let go of (the chain's no-swing bit shown on the left hip:
# no holster has it - stowed, still owned); the menu's Weapons and holsters (a main-page row nine
# rows down, below Riding position) -> Right holster (its fourth row: Calibrate the grip is first),
# one trigger: the prod; the right hand then draws the PROD from the right hip. Control: the same walk without the
# trigger - the club.
$holMenuWalk = @("200 chord")
$t = 204; for ($k = 0; $k -lt 9; $k++) { $holMenuWalk += "$t down"; $t += 3 }
$holMenuWalk += "$($t + 1) a"; $t += 5
for ($k = 0; $k -lt 3; $k++) { $holMenuWalk += "$t down"; $t += 3 }
$holMenuTrigger = "$($t + 1) rtrigger"; $t += 5
$holMenuClose = "$t menu"
$holMenu = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--frames", "420", "--no-sfx", "--weapon", "1:3", "--cheat-weapon-now", "50:6", "--vr-holster-hide-empty", "0",
             "--vr-melee-script", "100 grip right 1", "--vr-holster-script", "150 release right 10; 300 draw right right")
Gate "vr holsters: the VR menu's Right holster row moves the prod there, the hand draws it from the right hip (mock, 1/20)" {
    & $padGame $Disc @holMenu --vr-mock-pad ((@($holMenuWalk) + @($holMenuTrigger, $holMenuClose)) -join "; ")
} "(?s)\A(?=.*weapon 6 \(PROD[^\n]*arrived in the player's hand)(?=.*lets go of weapon 6 away from the holsters: stowed)(?=.*the right hand DRAWS weapon 6 \(PROD[^\n]*from the right holster)(?=.*holsters left 0 right 6;)"
Gate "vr holsters: the menu's control - the same walk without the trigger, the right hip still gives the club (mock, 1/20)" {
    & $padGame $Disc @holMenu --vr-mock-pad ((@($holMenuWalk) + @($holMenuClose)) -join "; ")
} "(?s)\A(?=.*the right hand DRAWS weapon 1 \(CLUB[^\n]*from the right holster)(?=.*holsters left 0 right 1;)"

# VR weapons (tools\rrgame\vr_menu.cpp, vr_weapon_calib.h, vr_holsters.cpp). (1) The main VR page carries "Weapons and
# holsters" (9 rows down, below the Riding position row; its first row opens the grip calibration) and "Combat
# options" (10 down: the right trigger changes its first row, page 4). Controls: 8 rows down (Vibration) - no
# calibration page; 12 down (Controls) - the trigger changes Steering (page 2). (2) The left hand holds the MIRROR image of a grip calibrated in the right: the
# a club calibration taken on a headset (-30 0 +65 mm, +60 deg) with a yaw and a roll added (+10 up, 20 deg, 30 deg):
# the club's axis at the palm against the drawn fist's centre and against the aim ray, in each hand's frame, differ by
# under 1.5 mm / 0.1 deg once the left's right and yaw are negated, and the two weapons are world mirror images across the
# seat's mid-plane (the mock's rest hands are); control RRJB_GRIP_MIRROR=off (the grip unmirrored): 60 mm / 20 deg.
# (3) B with the stun gun (--weapon 7:15, drawn from the right hip) beside the sparring partner, touching no one: the
# ORIGINAL's attack (combat action 6 -> command 148, the fight code's own target and ReachTest) lands - the partner's
# health drops, a landed blow, the hit sound keyed on its frame. Controls: no partner (out of the original's reach: no
# blow, a buzz) and "Prod / stun gun reach" off (contact only: into the air). About 5 s / 10 s each.
function Weap2Walk([int]$downs, [string[]]$tail) {
    $s = @("200 chord"); $tt = 204
    for ($k = 0; $k -lt $downs; $k++) { $s += "$tt down"; $tt += 3 }
    foreach ($x in $tail) { $tt += 3; $s += "$tt $x" }
    $tt += 5; $s += "$tt menu"
    return ($s -join "; ")
}
$weap2Menu = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--frames", "320", "--no-sfx", "--weapon", "1:3", "--vr-melee-script", "100 grip right 1")
Gate "vr weapons: the main VR page's Weapons and holsters opens the grip calibration, its Combat options changes the combat page (mock, 1/20)" {
    & $padGame $Disc @weap2Menu --vr-mock-pad (Weap2Walk 9 @("a", "a"))
    & $padGame $Disc @weap2Menu --vr-mock-pad (Weap2Walk 10 @("a", "rtrigger"))
} "(?s)\A(?=.*vr weapons: the grip calibration page opened - weapon 1 in the right hand)(?=.*vr menu: 4/Combat mode: [^\n]*\(by a trigger\))"
Gate "vr weapons: the main page's control - Vibration's row opens no calibration, Controls' changes Steering (mock, 1/20)" {
    & $padGame $Disc @weap2Menu --vr-mock-pad (Weap2Walk 8 @("a", "a"))
    & $padGame $Disc @weap2Menu --vr-mock-pad (Weap2Walk 12 @("a", "rtrigger"))
} "(?s)\A(?!.*calibration page opened)(?=.*vr menu: 2/Steering: [^\n]*\(by a trigger\))(?!.*vr menu: 4/)"
$weap2Mirror = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--frames", "240", "--no-sfx", "--weapon", "1:3",
                 "--vr-weapon-grip", "1:-30,10,65,60,20,30", "--vr-melee-script", "100 grip right 1", "--vr-holster-script", "200 gripcheck 1")
Gate "vr weapons: a grip calibrated in the right hand sits as its mirror image in the left (mock, 1/20, the club)" {
    & $padGame $Disc @weap2Mirror
} "(?s)\A(?=.*grip check - weapon 1, [^\n]*in the right hand its axis at the palm -3\d\.\d \+\d+\.\d \+6\d\.\d mm [^\n]*\+60\.00 deg up and \+20\.00 deg left[^\n]*in the left hand \+[23]\d\.\d \+\d+\.\d \+6\d\.\d mm, \+60\.00 deg up, -20\.00 deg left; mirror difference [^\n]*\) [01]\.\d\d mm, 0\.0\d\d deg; the hands mirror images [^\n]*within [01]\.\d\d mm / 0\.0\d\d deg, the weapons' ends \(handle, tip\) within [01]\.\d\d / [01]\.\d\d mm)"
Gate "vr weapons: the mirror's control - RRJB_GRIP_MIRROR=off, the left hand holds it 6 cm and 20 deg off the mirror (mock, 1/20)" {
    $env:RRJB_GRIP_MIRROR = "off"
    try { & $padGame $Disc @weap2Mirror } finally { $env:RRJB_GRIP_MIRROR = $null }
} "(?s)\A(?=.*grip check \(RRJB_GRIP_MIRROR=off\) - weapon 1, [^\n]*mirror difference [^\n]*\) [5-6]\d\.\d\d mm, 1\d\.\d+ deg)"
$weap2Stun = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--autosteer", "--opponents", "17", "--frames", "950", "--vr-bike-lean", "100",
               "--weapon", "7:15", "--vr-melee-script", "100 grip right 1", "--vr-holster-script", "300 draw right right; 800 button B 2")
Gate "vr weapons: B with the stun gun beside the partner, touching no one, is the original's attack and lands (mock, 1/20)" {
    & $padGame $Disc @weap2Stun --cheat-sparring passive
} "(?s)\A(?=.*B: the right hand's stun gun \(weapon 7\) touches no rider - the ORIGINAL's attack)(?=.*the stun gun's original attack \(B at frame \d+\) LANDS on rival b\d+: [^\n]*\(health (\d+) -> (?!\1\))\d+\))(?=.*the fight totals: the player's blows [1-9]\d*, landed [1-9]\d*, [^\n]*riders hurt: rivals [1-9])(?=.*the player's landed blows [1-9]\d*, key-ons on their frames [1-9])(?=.*the original's attack on B 1 \(landed 1, missed 0, no blow 0\))"
Gate "vr weapons: the stun gun's control - no partner beside, out of the original's reach: no blow, a buzz (mock, 1/20)" {
    & $padGame $Disc @weap2Stun
} "(?s)\A(?=.*the ORIGINAL's attack)(?=.*the stun gun's original attack \(B at frame \d+\): no blow within 1\.5 s)(?=.*the player's blows 0, landed 0,)"
Gate "vr weapons: the stun gun's control - Prod / stun gun reach off, B beside the partner discharges into the air (mock, 1/20)" {
    & $padGame $Disc @weap2Stun --cheat-sparring passive --vr-prod-reach 0
} "(?s)\A(?=.*weapon 7 discharges into the air)(?!.*touches no rider - the ORIGINAL's attack)(?=.*the original's attack on B 0 )(?=.*the player's blows 0, landed 0,)"

# The inventory is the game's (weapon_inventory.h): bare-fisted rivals with a grudge (--opponent-weapons 9) against the
# player swinging the drawn club (--punch 8, the pad's armed swing): at frame 3746 a rival's punch steals it through the
# ORIGINAL's WeaponSteal RASHCDG 0x800BFF04 - the record's mask loses bit 1, the club leaves the hand and the right
# holster (nothing is drawn there any more), and it is in that rival's hand. Control: the same run without the brawlers -
# nothing taken. About 60 s each.
$holSteal = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--autosteer", "--opponents", "17", "--frames", "3800", "--no-sfx",
              "--weapon", "1:15", "--punch", "8", "--vr-melee-script", "100 grip right 1", "--vr-holster-script", "100 draw right right")
Gate "vr holsters: a rival's bare-fisted steal takes the drawn club out of the hand, the inventory and its holster (mock, 1/20)" {
    & $padGame $Disc @holSteal --opponent-weapons 9
} "(?s)\A(?=.*DRAWS weapon 1 \(CLUB)(?=.*weapon 1 \(CLUB[^\n]*in the right hand was TAKEN by a rival \(the record's mask 0x003 -> 0x001)(?=.*weapon 1 is now in rival b\d+'s hand)(?=.*taken by rivals 1, )(?=.*owned at the end 0, in hand 9,)"
Gate "vr holsters: the steal's control - no brawlers, the club stays owned (mock, 1/20)" {
    & $padGame $Disc @holSteal
} "(?s)\A(?=.*DRAWS weapon 1 \(CLUB)(?=.*taken by rivals 0, )(?=.*owned at the end 0,1, )"

# The punch's travel (a fist too needs a swing): the physical combat gates' fist through the torso asked for 30 cm
# of travel is a short swing (its positive is the gate above, 10 cm by default). The grip calibration page's other
# hand (calib 1 right + align: it takes the club 10 cm before the palm, moves it 8 cm up and 10 cm forward and turns
# it 35 deg down, 25 deg left) sets the grip to exactly that motion's result and saves it; control: the page open,
# no move. About 10 s / 5 s.
Gate "vr holsters: the punch's control - 30 cm of punch travel asked, the 5 m/s punch is a short swing (mock, 1/20)" {
    & $padGame $Disc @meleeRun --no-sfx --vr-fist-travel 30 --vr-melee-script $meleeHit
} "(?s)\A(?=.*no blow, a short swing \(1\d cm of travel, the punch travel 30 cm\))(?=.*blows 0 \(fist 0, weapon 0\))(?=.*the physical blows: 0 contact\(s\) sent)"
$holCalib = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--frames", "340", "--no-sfx", "--weapon", "1:3", "--vr-melee-script", "100 grip right 1")
Gate "vr holsters: the calibration page's other hand moves the club's grip by its own motion and saves it (mock, 1/20)" {
    & $padGame $Disc @holCalib --vr-holster-script "150 calib 1 right; 200 align 0 0.08 0.10 -35 25 60"
} "(?s)\A(?=.*the left hand takes hold of weapon 1 to move its grip)(?=.*weapon 1's grip set to \+40 \+146 \+130 mm, -35 \+25 \+0 deg)(?=.*grip calibrations saved 1;)"
Gate "vr holsters: the calibration's control - the page open, no move, nothing saved (mock, 1/20)" {
    & $padGame $Disc @holCalib --vr-holster-script "150 calib 1 right"
} "(?s)\A(?=.*the grip calibration page opened - weapon 1 in the right hand)(?=.*grip calibrations saved 0;)"


# VR: the held hands ride the bike as it is drawn (tools\rrgame\vr_handlebars.cpp HeldGlove): a slalom at
# full lock both ways with both hands on the bars (the original leans past 40 degrees each way; the head view draws the
# bike with 50 % of it by default - vr_visual_lean.h), and per frame the drawn fist's centre (vr_hands_draw.h
# kFistCentre through the hand's own model matrix) against the grip of the bike exactly as RenderView drew it
# (race_render.h DrawnMachine: the model matrix with the drawn lean, the part slots), and the palm's across axis against
# that bar. Under a millimetre and a tenth of a degree every frame, at horizon lock 100 and 0; the renderer's matrix
# equal to this frame's own read (no lag). Controls: the legacy placement (RRJB_BARS_HANDS=legacy) is off by over a
# centimetre, and "hands follow bike tilt" off keeps the controller's levelled wrist - tens of degrees across the fully
# leaning bar. About 6 s each.
$handsRun = @("--vr-mock", "--vr-steering", "handlebars", "--race", "1", "20", "--frames", "700", "--no-sfx", "--hold", "T",
              "--vr-bars-script", ("20 grab both; 20 twist 30 30; 200 turn -30 15; 260 turn 30 25; 330 turn -30 25; " +
                                   "400 turn 30 25; 470 turn -30 25; 540 turn 30 25; 610 turn 0 20"))
$handsOk = "vr hands on the bars: 1[0-9]{3} held hand-frame\(s\) over 700 drawn frame\(s\) - the fist's centre to the drawn grip max 0\.\d+ mm [^\n]*the palm across the drawn bar max 0\.0\d deg; the bike leaning up to 2\d\.\d deg, its fork turned up to 3\d\.\d deg; [^\n]*read max 0, part slots differing on 0 frame\(s\); the original's roll up to [4-8]\d\.\d deg drawn at 50%"
Gate "vr: the held hands sit on the drawn bike's grips through a leaning slalom, horizon lock 100 (mock, 1/20)" {
    & $padGame $Disc @handsRun --vr-horizon-lock 100
} $handsOk
Gate "vr: the held hands sit on the drawn bike's grips through a leaning slalom, horizon lock 0 (mock, 1/20)" {
    & $padGame $Disc @handsRun --vr-horizon-lock 0
} $handsOk
Gate "vr: the held hands' control - the legacy placement (RRJB_BARS_HANDS=legacy) is off the drawn grips by over a centimetre (mock, 1/20)" {
    $env:RRJB_BARS_HANDS = "legacy"
    try { & $padGame $Disc @handsRun }
    finally { Remove-Item Env:RRJB_BARS_HANDS }
} "vr hands on the bars: [^\n]*the fist's centre to the drawn grip max [1-9]\d\.\d+ mm"
Gate "vr: the held hands' control - hands follow bike tilt off, the levelled wrist lies across the leaning bar (mock, 1/20)" {
    & $padGame $Disc @handsRun --vr-bars-follow-tilt 0 --vr-bike-lean 100
} "vr hands on the bars: [^\n]*the fist's centre to the drawn grip max 0\.\d+ mm [^\n]*the palm across the drawn bar max [3-8]\d\.\d+ deg"

# VR: the drawn bike's visual lean and the eye on the bike (vr_visual_lean.h, [vr] bike_visual_lean /
# eye_on_bike): the same slalom at horizon lock 100 - the tracked hands (fixed in the seat's space, as a player's) come
# nearer their drawn grips with the bike drawn at 50 % of the original's roll than at 100 %, the drawn lean half the
# original's; the eye fixed on the bike travels on it by millimetres while held, the animated head (the control) by tens
# of centimetres. About 6 s each.
Gate "vr: the visual lean keeps the drawn grips nearer the tracked hands than the full lean, horizon lock 100 (mock, 1/20)" {
    $full = & $padGame $Disc @handsRun --vr-horizon-lock 100 --vr-bike-lean 100 | Select-String "vr hands on the bars" | ForEach-Object { $_.Line } | Out-String -Width 4096
    $half = & $padGame $Disc @handsRun --vr-horizon-lock 100 --vr-bike-lean 50 | Select-String "vr hands on the bars" | ForEach-Object { $_.Line } | Out-String -Width 4096
    $f = if ($full -match "tracked hand to its drawn grip max (\d+) mm") { [int]$Matches[1] } else { -1 }
    $h = if ($half -match "tracked hand to its drawn grip max (\d+) mm") { [int]$Matches[1] } else { -1 }
    $fullLean = $full -match "the bike leaning up to ([4-8]\d)\.\d deg"
    $halfLean = $half -match "the bike leaning up to (2\d)\.\d deg"
    "visual lean: 100 % $f mm (drawn at the original's roll $fullLean), 50 % $h mm (drawn at half $halfLean): nearer " + ($fullLean -and $halfLean -and $h -gt 0 -and $f -gt 0 -and $h -lt $f)
} "nearer True"
Gate "vr: the eye fixed on the bike stays put on it while the bars are held (mock, 1/20)" {
    & $padGame $Disc @handsRun --vr-horizon-lock 100
} "vr hands on the bars: [^\n]*the eye's travel on the drawn bike while held max \d mm"
Gate "vr: the eye check's control - the rider's animated head travels on the bike (mock, 1/20)" {
    & $padGame $Disc @handsRun --vr-horizon-lock 100 --vr-eye-on-bike 0
} "vr hands on the bars: [^\n]*the eye's travel on the drawn bike while held max [1-9]\d\d+ mm"

# VR: the lock levels the roll only by default and the fork is drawn at the hands' angle ([vr]
# horizon_mode, BarGrips::SetForkTurn). The same slalom: on frame 200 (on the start's slope, no roll yet) the
# tracked hands stay within 5 cm of their drawn grips with the roll-only lock and are over 15 cm off with the
# roll + pitch lock (the per-frame log, RRJB_HANDS_LOG); the drawn fork turns with the held bars past 25 degrees
# (above) and its control - the game's own fork slot (RRJB_BARS_FORK=game) - turns under 5 degrees and leaves the hands
# farther from the grips. About 6 s each.
Gate "vr: the roll-only horizon lock keeps the drawn grips by the hands on a slope; roll + pitch does not (mock, 1/20)" {
    $at = {
        param($mode)
        $csv = Join-Path $vrGateDir "hands_$mode.csv"
        $env:RRJB_HANDS_LOG = $csv
        try { & $padGame $Disc @handsRun --vr-horizon-lock 100 --vr-horizon-mode $mode | Out-Null }
        finally { Remove-Item Env:RRJB_HANDS_LOG }
        $row = Import-Csv $csv | Where-Object { $_.frame -eq "200" }
        if ($row) { [Math]::Max([double]$row.left_real_mm, [double]$row.right_real_mm) } else { -1 }
    }
    $roll = & $at "roll"
    $full = & $at "full"
    "frame 200: roll only $roll mm, roll + pitch $full mm: " + ($roll -ge 0 -and $roll -lt 50 -and $full -gt 150)
} "roll \+ pitch [\d.]+ mm: True"
Gate "vr: the drawn fork's control - the game's own fork slot barely turns, the hands farther off (mock, 1/20)" {
    $mine = & $padGame $Disc @handsRun --vr-horizon-lock 100 | Select-String "vr hands on the bars" | ForEach-Object { $_.Line } | Out-String -Width 4096
    $env:RRJB_BARS_FORK = "game"
    try { $game = & $padGame $Disc @handsRun --vr-horizon-lock 100 | Select-String "vr hands on the bars" | ForEach-Object { $_.Line } | Out-String -Width 4096 }
    finally { Remove-Item Env:RRJB_BARS_FORK }
    $m = if ($mine -match "tracked hand to its drawn grip max (\d+) mm") { [int]$Matches[1] } else { -1 }
    $g = if ($game -match "tracked hand to its drawn grip max (\d+) mm") { [int]$Matches[1] } else { -1 }
    $small = $game -match "its fork turned up to [0-4]\.\d deg"
    "the game's fork: turned under 5 degrees $small, hands $g mm; the hands' fork: $m mm: nearer " + ($small -and $m -gt 0 -and $m -lt $g)
} "nearer True"

# VR: taking the grip with a fist already clenched (vr_handlebars.cpp Solve): the right hand clenched off
# the bars, brought slowly (0.3 m/s) into the grip's reach, takes it; the same fist swung in at 5 m/s does not (a swing,
# not a reach). Within a second of letting go the reach is 28 cm: the grip squeezed again 24 cm below the bar a quarter
# of a second after the release takes it; the same squeeze two seconds later does not. About 4 s each.
$grabRun = @("--vr-mock", "--vr-steering", "handlebars", "--race", "1", "20", "--frames", "400", "--no-sfx")
Gate "vr: a clenched fist brought slowly to the bar takes its grip (mock, 1/20)" {
    & $padGame $Disc @grabRun --vr-bars-script "10 grip right 1; 100 move right 0 0.25 -0.15 60"
} "vr bars: steering HANDLEBARS [^\n]* - grabs left 0 right 1, [^\n]*grabs with the fist already clenched 1, "
Gate "vr: the clenched grab's control - the same fist swung into the reach takes nothing (mock, 1/20)" {
    & $padGame $Disc @grabRun --vr-bars-script "10 grip right 1; 100 move right 0 0.25 -0.15 4"
} "vr bars: steering HANDLEBARS [^\n]* - grabs left 0 right 0, [^\n]*grabs with the fist already clenched 0, in the re-grab reach 0, clenched fists entering too fast [1-9]"
Gate "vr: a squeeze 24 cm below the bar just after letting go re-takes the grip (mock, 1/20)" {
    & $padGame $Disc @grabRun --vr-bars-script "20 grab right; 150 release right; 152 move right 0 -0.24 0 4; 170 grip right 1"
} "vr bars: steering HANDLEBARS [^\n]* - grabs left 0 right 2, [^\n]*in the re-grab reach 1, "
Gate "vr: the re-grab's control - the same squeeze two seconds later takes nothing (mock, 1/20)" {
    & $padGame $Disc @grabRun --vr-bars-script "20 grab right; 150 release right; 152 move right 0 -0.24 0 4; 300 grip right 1"
} "vr bars: steering HANDLEBARS [^\n]* - grabs left 0 right 1, [^\n]*in the re-grab reach 0, "

# The handling (src\game\handling_modern.h): Original / Modern (GTA SA-style riding). Original must be the
# game without the layer - the per-frame log and the last frame of an autosteer race byte-identical to a run with the
# layer not called at all (RRJB_HANDLING_LAYER=off); the check's control: Modern changes that log. Modern on the same
# scripted keys (a slalom of 0.2 / 0.4 s taps, 1/20, no fall in either mode): the heading-rate state reaches 10 deg/s
# sooner after a key goes down and the drawn roll's acceleration (rms) falls below half of Original's; the control -
# Modern with every layer at 0 (the bare analogue pad, the turn layer off) - turns nothing itself and keeps the roll's
# acceleration within half of Original's. Fair: the scripted driver (--autosteer) covers 0.9..1.1 x Original's distance
# in its first 1500 frames (before any fall). VR: under Modern the held hands still sit on the drawn grips (the slalom
# of the held-hands gates) and the bike is drawn at Modern's whole SA lean (30..45 deg). About 5 s .. 30 s each.
$handDir = Join-Path $root "work\gate_handling_$gateTag"
New-Item -ItemType Directory -Force $handDir | Out-Null
$handAuto = @("--race", "1", "20", "--frames", "3000", "--hold", "T", "--autosteer", "--no-sfx")
Gate "handling: Original is the game without the layer - log and frame byte-identical (1/20, autosteer, 3000 frames)" {
    & $padGame $Disc @handAuto --log (Join-Path $handDir "orig.log") --shot (Join-Path $handDir "orig.png") | Out-Null
    $env:RRJB_HANDLING_LAYER = "off"
    try { & $padGame $Disc @handAuto --log (Join-Path $handDir "off.log") --shot (Join-Path $handDir "off.png") | Out-Null }
    finally { Remove-Item Env:RRJB_HANDLING_LAYER }
    $h = { param($f) (Get-FileHash -LiteralPath (Join-Path $handDir $f)).Hash }
    "log identical " + ((& $h "orig.log") -eq (& $h "off.log")) + ", frame identical " + ((& $h "orig.png") -eq (& $h "off.png"))
} "log identical True, frame identical True"
Gate "handling: the identity check's control - Modern changes the same race's log (1/20, autosteer, 3000 frames)" {
    & $padGame $Disc @handAuto --handling modern --log (Join-Path $handDir "modern.log") | Out-Null
    "log differs " + ((Get-FileHash -LiteralPath (Join-Path $handDir "modern.log")).Hash -ne (Get-FileHash -LiteralPath (Join-Path $handDir "off.log")).Hash)
} "log differs True"
$handSlalom = @("--race", "1", "20", "--frames", "530", "--hold", "T", "--no-sfx", "--steer-script",
                "300 L; 312 0; 340 R; 364 0; 390 L; 414 0; 440 R; 464 0; 490 L; 502 0")
function HandlingNumbers($line) {
    $n = [ordered]@{ ok = $false }
    if ($line -match "turned (\d+);[^\n]*its acceleration rms (\d+) max \d+ deg/s\^2; a 10 deg/s turn (-?\d+) ms after the input \((\d+)\)") {
        $n.turned = [int]$Matches[1]; $n.roll = [int]$Matches[2]; $n.onset = [int]$Matches[3]; $n.steps = [int]$Matches[4]; $n.ok = $true
    }
    if ($line -match "riding (\d+) of") { $n.riding = [int]$Matches[1] }
    return $n
}
Gate "handling: Modern on the key slalom turns sooner and its drawn roll is over twice as smooth (1/20)" {
    $o = HandlingNumbers ((& $padGame $Disc @handSlalom --handling original | Select-String "^handling: original") -join "")
    $m = HandlingNumbers ((& $padGame $Disc @handSlalom --handling modern | Select-String "^handling: modern") -join "")
    "original: onset $($o.onset) ms over $($o.steps), roll accel rms $($o.roll), riding $($o.riding); modern: onset $($m.onset) ms, roll accel rms $($m.roll), turned $($m.turned), riding $($m.riding)"
    "sooner and smoother " + ($o.ok -and $m.ok -and $m.turned -gt 0 -and $m.steps -ge 3 -and $m.onset -gt 0 -and $m.onset -lt $o.onset -and 2 * $m.roll -lt $o.roll -and $m.riding -ge $o.riding - 20)
} "sooner and smoother True"
Gate "handling: the slalom's control - Modern with every layer at 0 turns nothing itself, the roll as rough as Original's (1/20)" {
    $o = HandlingNumbers ((& $padGame $Disc @handSlalom --handling original | Select-String "^handling: original") -join "")
    $c = HandlingNumbers ((& $padGame $Disc @handSlalom --handling modern --handling-steer-lag 0 --handling-curve 0 --handling-turn-lag 0 --handling-lean-lag 0 | Select-String "^handling: modern") -join "")
    "original roll accel rms $($o.roll); the bare analogue pad: turned $($c.turned), roll accel rms $($c.roll)"
    "control holds " + ($o.ok -and $c.ok -and $c.turned -eq 0 -and 2 * $c.roll -ge $o.roll)
} "control holds True"
Gate "handling: fair - Modern's scripted driver covers 0.9..1.1 x Original's distance in 1500 frames (1/20, autosteer)" {
    $d = @{}
    foreach ($mode in "original", "modern") {
        $line = (& $padGame $Disc --race 1 20 --frames 1500 --hold T --autosteer --no-sfx --handling $mode --handling-log (Join-Path $handDir "fair_$mode.csv") | Select-String "^handling: $mode") -join ""
        $d[$mode] = if ($line -match "distance ([\d.]+),") { [double]$Matches[1] } else { -1 }
    }
    $r = if ($d.original -gt 0) { $d.modern / $d.original } else { 0 }
    "distance original $($d.original) modern $($d.modern) ratio {0:N3}; fair {1}" -f $r, ($r -ge 0.9 -and $r -le 1.1)
} "fair True"
Gate "vr: under Modern handling the held hands stay on the drawn grips, the bike drawn at SA's whole lean (mock, 1/20)" {
    & $padGame $Disc @handsRun --handling modern
} "(?s)(?=.*vr hands on the bars: [^\n]*the fist's centre to the drawn grip max 0\.\d+ mm [^\n]*the bike leaning up to (3\d|4[0-5])\.\d deg)(?=.*handling: modern - [^\n]*turned [1-9])"

# Modern leans and turns like SA, and the VR menu's choice reaches the race. A standard bend on 1/20: the
# right key held one second from frame 400 (about 40 units/s), the VR mock's head view (horizon lock 100 %, roll only,
# Visual bike lean 50 % - the defaults). Original draws the bike at half the game's roll and keeps the view level;
# Modern draws SA's whole lean (asin of the asked turn's lateral g, 45 deg) - at least 15 deg more - and the view rolls
# with 40 % of it (at least 10 deg). The control: the layer not called at all (RRJB_HANDLING_LAYER=off) under
# --handling-vr modern draws Original's bend (the drawn lean within 1 deg of Original's, the view level). VR Original is
# the game without the layer (autosteer, the mock's frame and log byte-identical but for the log's timing lines). The
# desktop bend: Modern reaches a 10 deg/s turn at least 60 ms sooner and straightens in under half Original's time.
# The VR menu (Controls -> Handling, the right trigger at turn 233; up x3 from the top reaches Controls whatever rows
# are added above it) switches a scripted Original race to Modern from the next frame: the log names the switch, the
# race rides Modern, the view rolls; the control - the left trigger (Original) - switches nothing. About 5..10 s each.
$bendRun = @("--vr-mock", "--race", "1", "20", "--frames", "560", "--hold", "T", "--no-sfx", "--steer-script", "400 R; 460 0")
function BendView($text) {
    $n = [ordered]@{ ok = $false }
    if ($text -match "handling view: \d+ VR head-view frame\(s\) - the game's roll up to ([\d.]+) deg, the bike drawn leaning up to ([\d.]+) deg, the view rolled up to ([\d.]+) deg") {
        $n.game = [double]$Matches[1]; $n.drawn = [double]$Matches[2]; $n.view = [double]$Matches[3]; $n.ok = $true
    }
    return $n
}
Gate "handling: the VR bend - Modern draws SA's whole lean (15+ deg more than Original) and rolls the view (mock, 1/20)" {
    $o = BendView ((& $padGame $Disc @bendRun --handling-vr original | Out-String -Width 4096))
    $m = BendView ((& $padGame $Disc @bendRun --handling-vr modern | Out-String -Width 4096))
    "original: game $($o.game) drawn $($o.drawn) view $($o.view); modern: game $($m.game) drawn $($m.drawn) view $($m.view)"
    "SA lean and roll " + ($o.ok -and $m.ok -and $o.drawn -le 0.5 * $o.game + 0.5 -and $o.view -lt 1 -and $m.drawn -ge $o.drawn + 15 -and $m.drawn -le 45.05 -and $m.view -ge 10)
} "SA lean and roll True"
Gate "handling: the bend's control - Modern with the layer not called draws Original's bend (mock, 1/20)" {
    # (the layer off also drops the steering script - it is the layer's test input - so the scripted driver steers)
    $env:RRJB_HANDLING_LAYER = "off"
    try { $c = BendView ((& $padGame $Disc --vr-mock --race 1 20 --frames 560 --hold T --autosteer --no-sfx --handling-vr modern --handling-log (Join-Path $handDir "bend_off.csv") | Out-String -Width 4096)) }
    finally { Remove-Item Env:RRJB_HANDLING_LAYER }
    "modern, the layer off: game $($c.game) drawn $($c.drawn) view $($c.view)"
    "control holds " + ($c.ok -and $c.game -ge 20 -and $c.drawn -le 0.5 * $c.game + 0.5 -and $c.view -lt 1)
} "control holds True"
Gate "handling: VR Original is the game without the layer - the mock's frame and log byte-identical (1/20, autosteer)" {
    $vrAuto = @("--vr-mock", "--race", "1", "20", "--frames", "600", "--hold", "T", "--autosteer", "--no-sfx")
    & $padGame $Disc @vrAuto --log (Join-Path $handDir "vr_orig.log") --shot (Join-Path $handDir "vr_orig.png") | Out-Null
    $env:RRJB_HANDLING_LAYER = "off"
    try { & $padGame $Disc @vrAuto --log (Join-Path $handDir "vr_off.log") --shot (Join-Path $handDir "vr_off.png") | Out-Null }
    finally { Remove-Item Env:RRJB_HANDLING_LAYER }
    $logOf = { param($f) ((Get-Content (Join-Path $handDir $f)) | Where-Object { $_ -notmatch " us each| us per frame| ms " }) -join "`n" }
    $h = { param($f) (Get-FileHash -LiteralPath (Join-Path $handDir $f)).Hash }
    "log identical " + ((& $logOf "vr_orig.log") -eq (& $logOf "vr_off.log")) + ", frame identical " + ((& $h "vr_orig.png") -eq (& $h "vr_off.png"))
} "log identical True, frame identical True"
Gate "handling: the desktop bend - Modern turns in 60+ ms sooner and straightens in under half Original's time (1/20)" {
    $at = {
        param($mode)
        $l = (& $padGame $Disc --race 1 20 --frames 560 --hold T --no-sfx --steer-script "400 R; 460 0" --handling $mode | Select-String "^handling: $mode -") -join ""
        if ($l -match "a 10 deg/s turn (\d+) ms after the input \(1\), under 2 deg/s (\d+) ms after the release \(1\)") { @([int]$Matches[1], [int]$Matches[2]) } else { @(-1, -1) }
    }
    $o = & $at "original"
    $m = & $at "modern"
    "turn-in original $($o[0]) ms modern $($m[0]) ms; straightened original $($o[1]) ms modern $($m[1]) ms"
    "brisker " + ($o[0] -gt 0 -and $m[0] -gt 0 -and $m[0] -le $o[0] - 60 -and $m[1] -gt 0 -and 2 * $m[1] -lt $o[1])
} "brisker True"
$bendMenu = @("200 chord"); $t = 204
for ($k = 0; $k -lt 3; $k++) { $bendMenu += "$t up"; $t += 3 }
$bendMenu += "$($t + 1) a"; $t += 5
for ($k = 0; $k -lt 3; $k++) { $bendMenu += "$t down"; $t += 3 }
$bendMenu += "$($t + 1) a"; $t += 5
$bendMenuTrigger = "$($t + 1) rtrigger"; $t += 5
$bendMenuClose = "$t menu"
$bendMenuRun = @("--vr-mock", "--race", "1", "20", "--frames", "500", "--hold", "T", "--no-sfx", "--steer-script", "300 R; 360 0")
Gate "handling: the VR menu's Handling row switches the race to Modern from the next frame (mock, 1/20)" {
    & $padGame $Disc @bendMenuRun --vr-mock-pad ((@($bendMenu) + @($bendMenuTrigger, $bendMenuClose)) -join "; ")
} "(?s)\A(?=.*handling: frame 0 - this race rides original)(?=.*handling: the VR menu set the VR handling original -> modern)(?=.*handling: frame \d+ - the VR handling switched original -> modern)(?=.*handling: modern - [^\n]*shaped [1-9][^\n]*modes this race: original \d+ / modern [1-9]\d* frame\(s\), switched 1 time)(?=.*handling view: [^\n]*the view rolled up to [1-9]\d\.\d deg)"
Gate "handling: the menu's control - the left trigger keeps Original, nothing switches (mock, 1/20)" {
    & $padGame $Disc @bendMenuRun --vr-mock-pad ((@($bendMenu) + @($bendMenuTrigger.Replace("rtrigger", "ltrigger"), $bendMenuClose)) -join "; ")
} "(?s)\A(?=.*handling: the VR menu set the VR handling original -> original)(?!.*the VR handling switched)(?=.*handling: original - [^\n]*shaped 0,[^\n]*modes this race: original \d+ / modern 0 frame\(s\), switched 0 time)(?=.*handling view: [^\n]*the view rolled up to 0\.0 deg)"

# Wheelies (src\game\wheelie.h): OURS - the lean back (the stick pulled back / F with the throttle, the VR
# bars raised) holds the front up, drawn only (the drawn bike, rider, head camera, VR grips, hands and eye pitched about
# the rear wheel); a car hit in a wheelie runs the game's own launch 0x80084BE8 over the car instead of the original's
# reaction. Identity: under Original handling (the option's default "with Modern") and with the option off under Modern,
# the lean back changes nothing - log and frame byte-identical to the layer not called (RRJB_WHEELIE_LAYER=off); the
# control: Modern with wheelies draws another frame. Desktop: the first lift to 30 deg in under 600 ms, the throttle's
# release brings the front down within 800 ms (control: the option off, nothing lifts). VR (mock, Handlebars): both
# hands raised 32 cm on the bars (the full lift) lift the front, the held fists stay on the pitched grips, the view keeps about 30 % of
# the pitch (control: the same hands not raised). The car: aimed at the traffic of 1/20, a wheelie flies over the car and
# lands seated (control: no wheelie - knocked off at the car). The loop-over (off by default): held at full pull and full
# throttle the nose passes the balance point and the game's fall follows (control: off, the pitch stops under 53 deg).
# The VR menu's Wheelies row sets "Always" and the lean back lifts under Original handling (control: the left trigger
# steps to "Off"). About 10..40 s each.
$whDir = Join-Path $root "work\gate_wheelie_$gateTag"
New-Item -ItemType Directory -Force $whDir | Out-Null
$whId = @("--race", "1", "20", "--frames", "470", "--hold", "T", "--autosteer", "--no-sfx", "--wheelie-script", "400 W")
function WheelieSame($tag, [string[]]$extra) {
    & $padGame $Disc @whId @extra --log (Join-Path $whDir "$tag.log") --shot (Join-Path $whDir "$tag.png") | Out-Null
    $env:RRJB_WHEELIE_LAYER = "off"
    try { & $padGame $Disc @whId @extra --log (Join-Path $whDir "${tag}_off.log") --shot (Join-Path $whDir "${tag}_off.png") | Out-Null }
    finally { Remove-Item Env:RRJB_WHEELIE_LAYER }
    $logOf = { param($f) ((Get-Content (Join-Path $whDir $f)) | Where-Object { $_ -notmatch " us each| us per frame| ms " }) -join "`n" }
    $h = { param($f) (Get-FileHash -LiteralPath (Join-Path $whDir $f)).Hash }
    "log identical " + ((& $logOf "$tag.log") -eq (& $logOf "${tag}_off.log")) + ", frame identical " + ((& $h "$tag.png") -eq (& $h "${tag}_off.png"))
}
Gate "wheelie: under Original handling the lean back changes nothing - log and frame byte-identical to the layer not called (1/20)" {
    WheelieSame "orig" @()
} "log identical True, frame identical True"
Gate "wheelie: the option off under Modern - log and frame byte-identical to the layer not called (1/20)" {
    WheelieSame "off" @("--handling", "modern", "--wheelie", "off")
} "log identical True, frame identical True"
Gate "wheelie: the identity's control - Modern with wheelies draws the bike pitched, another frame (1/20)" {
    $o = & $padGame $Disc @whId --handling modern --shot (Join-Path $whDir "on.png") | Out-String -Width 4096
    $o
    "frame differs " + ((Get-FileHash -LiteralPath (Join-Path $whDir "on.png")).Hash -ne (Get-FileHash -LiteralPath (Join-Path $whDir "off_off.png")).Hash)
} "(?s)wheelie summary: wheelie settings: with Modern[^\n]*max pitch 3\d\.\d deg.*frame differs True"
$whDesk = @("--race", "1", "20", "--frames", "700", "--hold", "T", "--no-sfx", "--handling", "modern", "--wheelie-script", "300 W; 420 N; 460 0; 500 0.5; 600 0")
Gate "wheelie: desktop Modern - the lean back lifts the front to 30 deg in < 600 ms and the throttle's release drops it in < 800 ms (1/20)" {
    $l = (& $padGame $Disc @whDesk | Out-String -Width 4096) -split "`n" | Where-Object { $_ -match "^wheelie summary" }
    $l = $l -join ""
    $l
    if ($l -match "lifts (\d+) .*max pitch ([\d.]+) deg.*rider falls (\d+) .*the first lift to 30 deg in (-?\d+) ms, the first drop from 20\+ deg in (-?\d+) ms") {
        "lifts $($Matches[1]) max $($Matches[2]) falls $($Matches[3]) rise $($Matches[4]) drop $($Matches[5])"
        "wheelies " + ([int]$Matches[1] -ge 2 -and [double]$Matches[2] -ge 33 -and [double]$Matches[2] -le 40 -and [int]$Matches[3] -eq 0 -and [int]$Matches[4] -gt 0 -and [int]$Matches[4] -le 600 -and [int]$Matches[5] -gt 0 -and [int]$Matches[5] -le 800)
    }
} "wheelies True"
Gate "wheelie: the desktop control - the option off, the same lean back lifts nothing (1/20)" {
    & $padGame $Disc @whDesk --wheelie off | Out-String -Width 4096
} "wheelie summary: off this race - [^\n]*lifts 0 [^\n]*max pitch 0\.0 deg"
$whBars = @("--vr-mock", "--vr-steering", "handlebars", "--race", "1", "20", "--frames", "560", "--no-sfx", "--handling", "modern")
Gate "wheelie: VR Handlebars - both hands raised on the bars lift the front, the fists stay on the pitched grips, the view keeps ~30 % (mock, 1/20)" {
    $o = & $padGame $Disc @whBars --vr-bars-script "20 grab both; 20 twist 30 30; 400 lift 0.32 0 12" | Out-String -Width 4096
    $o
    if ($o -match "vr wheelie view: \d+ VR head-view frame\(s\), \d+ in a wheelie - the bike drawn pitched up to ([\d.]+) deg, the view pitched up by it at most ([\d.]+) deg") {
        "view kept " + ([double]$Matches[1] -ge 30 -and [double]$Matches[2] -ge 0.2 * [double]$Matches[1] -and [double]$Matches[2] -le 0.4 * [double]$Matches[1])
    }
} "(?s)(?=.*vr wheelie: both hands on the bars [^\n]*the pull up to 1\.00)(?=.*wheelie: frame \d+ - the first lean back [^\n]*the VR bars' gesture)(?=.*wheelie summary: wheelie settings[^\n]*lifts [1-9][^\n]*max pitch 3\d\.\d deg)(?=.*vr hands on the bars: [^\n]*the fist's centre to the drawn grip max 0\.\d+ mm)(?=.*view kept True)"
Gate "wheelie: the VR control - the hands on the bars not raised, nothing lifts (mock, 1/20)" {
    & $padGame $Disc @whBars --vr-bars-script "20 grab both; 20 twist 30 30" --wheelie-script "0 0" | Out-String -Width 4096
} "(?s)(?=.*vr wheelie: both hands on the bars [^\n]*the pull up to 0\.00)(?=.*wheelie summary: wheelie settings[^\n]*lifts 0 )"
$whCar = @("--race", "1", "20", "--frames", "1100", "--hold", "T", "--autosteer", "--no-sfx", "--handling", "modern", "--wheelie-aim-car", "300")
Gate "wheelie: a car hit in a wheelie - the game's launch over the car, landed seated, no fall (1/20, aimed at the traffic)" {
    & $padGame $Disc @whCar --wheelie-script "808 W" | Out-String -Width 4096
} "(?s)(?=.*wheelie: frame \d+ - a car hit at [\d.]+ deg of wheelie: the game's launch over it)(?=.*wheelie: frame \d+ - landed after \d+ frame\(s\), the rider seated - rides on)(?=.*wheelie summary: [^\n]*launches over a car [1-9][^\n]*landings seated [1-9]\d* / fallen 0[^\n]*rider falls 0 )"
Gate "wheelie: the car hit's control - no wheelie, the same aim knocks the rider off at the car (1/20)" {
    & $padGame $Disc @whCar --wheelie-script "808 0" | Out-String -Width 4096
} "wheelie summary: [^\n]*launches over a car 0 [^\n]*rider falls [1-9]\d* \(first at frame (8[5-9]\d|9[0-4]\d)\)"
$whLoop = @("--race", "1", "20", "--frames", "800", "--hold", "T", "--autosteer", "--no-sfx", "--handling", "modern", "--wheelie-script", "400 W")
Gate "wheelie: the loop-over (on) - held at full pull and throttle past the balance point, the game's fall (1/20)" {
    & $padGame $Disc @whLoop --wheelie-loop on | Out-String -Width 4096
} "(?s)(?=.*wheelie: frame \d+ - looped over at 8\d\.\d deg)(?=.*wheelie summary: [^\n]*loop-overs 1 \(the rider off after 1\))"
Gate "wheelie: the loop-over's control - off (the default), the pitch stops under 53 deg, no fall (1/20)" {
    $l = ((& $padGame $Disc @whLoop | Out-String -Width 4096) -split "`n" | Where-Object { $_ -match "^wheelie summary" }) -join ""
    $l
    if ($l -match "max pitch ([\d.]+) deg.*loop-overs (\d+) .*rider falls (\d+) ") { "held " + ([double]$Matches[1] -lt 53 -and [double]$Matches[1] -ge 33 -and $Matches[2] -eq "0" -and $Matches[3] -eq "0") }
} "held True"
# The VR menu: down x6 from the top reaches Riding position, Enter; down x7 the Wheelies row
$whMenu = @("200 chord"); $t = 204
for ($k = 0; $k -lt 6; $k++) { $whMenu += "$t down"; $t += 3 }
$whMenu += "$($t + 1) a"; $t += 5
for ($k = 0; $k -lt 7; $k++) { $whMenu += "$t down"; $t += 3 }
$whMenuTrigger = "$($t + 1) rtrigger"; $t += 5
$whMenuClose = "$t menu"
$whMenuRun = @("--vr-mock", "--race", "1", "20", "--frames", "560", "--hold", "T", "--no-sfx", "--wheelie-script", "420 W")
Gate "wheelie: the VR menu's Wheelies row - the right trigger sets Always, the lean back lifts under Original handling (mock, 1/20)" {
    & $padGame $Disc @whMenuRun --vr-mock-pad ((@($whMenu) + @($whMenuTrigger, $whMenuClose)) -join "; ") | Out-String -Width 4096
} "(?s)(?=.*wheelie: the VR menu set wheelies on)(?=.*handling: frame 0 - this race rides original)(?=.*wheelie summary: wheelie settings: always[^\n]*lifts [1-9])"
Gate "wheelie: the menu's control - the left trigger steps to Off, nothing lifts (mock, 1/20)" {
    & $padGame $Disc @whMenuRun --vr-mock-pad ((@($whMenu) + @($whMenuTrigger.Replace("rtrigger", "ltrigger"), $whMenuClose)) -join "; ") | Out-String -Width 4096
} "(?s)(?=.*wheelie: the VR menu set wheelies off)(?=.*wheelie summary: off this race - [^\n]*lifts 0 )"

# The deliberate start (src\game\wheelie.h WheelieSettings holdMs / liftCm, tools\rrgame\vr_wheelie.h): every lean-back
# input starts a wheelie only when held 200 ms with the throttle open and the bike under way; the VR Handlebars gesture
# is BOTH hands raised 14 cm (each at least 10.5) over where they hold the DRAWN grips, the stick and Down do not lean
# back in that mode, and the Stick mode asks for near full travel back. Normal riding (the mock's hands on the bars
# through a slalom, following the drawn grips and bobbing 3 cm together; or half following and bobbing 5 cm) starts
# nothing; the control RRJB_WHEELIE_INPUT=legacy (the input as it was: 4 cm from the grab in the seat's space, no hold,
# Down / the stick past a third a full lean back) lifts the front on the same riding. A thumb resting on the Touch stick
# (down-left) in the Handlebars mode, a stick pushed down-left in the Stick mode and a 100 ms tap of the stick lift
# nothing (control: legacy - each lifts). Deliberate: both hands raised 20 cm start a wheelie; 12 cm, a 50 ms jerk of
# 25 cm, one hand raised 30 cm and a pull toward the rider do not; the stick held full back starts one. About 15 s each.
$whBase = "20 grab both; 20 twist 30 30; 30 follow 1"
$whSlalom = @(); for ($f = 300; $f -lt 900; $f += 240) { $whSlalom += "$f turn 12 40"; $whSlalom += "$($f + 80) turn -12 60"; $whSlalom += "$($f + 160) turn 0 40" }
$whRide = "20 grab both; 20 twist 30 30; " + ($whSlalom -join "; ")
$whDeliberate = @("--vr-mock", "--vr-steering", "handlebars", "--race", "1", "20", "--frames", "900", "--hold", "T", "--autosteer", "--no-sfx", "--handling", "modern")
$whStickRun = @("--vr-mock", "--race", "1", "20", "--frames", "900", "--hold", "T", "--autosteer", "--no-sfx", "--handling", "modern", "--vr-mock-pad-race")
function WheelieStarts([string]$text) {
    $l = ($text -split "`n" | Where-Object { $_ -match "^wheelie summary" }) -join ""
    if ($l -match "lifts (\d+) .*max pitch ([\d.]+) deg.*wheelie starts (\d+) ") { return "lifts $($Matches[1]) max $($Matches[2]) starts $($Matches[3])" }
    return "no wheelie line"
}
Gate "wheelie input: normal riding with the hands on the bars (following the grips + 3 cm bob; half following + 5 cm bob) starts nothing (mock, 1/20)" {
    "bob3 " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whRide; 30 follow 1; 40 bob 0.03 45" --wheelie-log (Join-Path $whDir "ride_bob3.csv") | Out-String -Width 4096))
    "bob5 " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whRide; 30 follow 0.5; 40 bob 0.05 72" --wheelie-log (Join-Path $whDir "ride_bob5.csv") | Out-String -Width 4096))
} "(?s)\A(?=.*bob3 lifts 0 max 0\.0 starts 0)(?=.*bob5 lifts 0 max 0\.0 starts 0)"
Gate "wheelie input: the control - RRJB_WHEELIE_INPUT=legacy, the same riding lifts the front (mock, 1/20)" {
    $env:RRJB_WHEELIE_INPUT = "legacy"
    try {
        "bob3 " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whRide; 30 follow 1; 40 bob 0.03 45" --wheelie-log (Join-Path $whDir "ride_bob3_legacy.csv") | Out-String -Width 4096))
        "bob5 " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whRide; 30 follow 0.5; 40 bob 0.05 72" --wheelie-log (Join-Path $whDir "ride_bob5_legacy.csv") | Out-String -Width 4096))
    } finally { Remove-Item Env:\RRJB_WHEELIE_INPUT }
} "(?s)\A(?=.*bob3 lifts [1-9])(?=.*bob5 lifts [1-9])"
Gate "wheelie input: the Touch stick - a thumb on it in the Handlebars mode, down-left and a 100 ms tap in the Stick mode lift nothing (mock, 1/20)" {
    "bars " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script $whBase --vr-mock-pad-race --vr-mock-pad "600 stick -0.3 -0.6 150" --wheelie-log (Join-Path $whDir "bars_thumb.csv") | Out-String -Width 4096))
    "diag " + (WheelieStarts (& $padGame $Disc @whStickRun --vr-mock-pad "600 stick -0.5 -0.6 150" --wheelie-log (Join-Path $whDir "stick_diag.csv") | Out-String -Width 4096))
    "tap " + (WheelieStarts (& $padGame $Disc @whStickRun --vr-mock-pad "600 down 6" --wheelie-log (Join-Path $whDir "stick_tap.csv") | Out-String -Width 4096))
} "(?s)\A(?=.*bars lifts 0 max 0\.0 starts 0)(?=.*diag lifts 0 max 0\.0 starts 0)(?=.*tap lifts 0 max 0\.0 starts 0)"
Gate "wheelie input: the stick's control - RRJB_WHEELIE_INPUT=legacy, each of them lifts the front (mock, 1/20)" {
    $env:RRJB_WHEELIE_INPUT = "legacy"
    try {
        "bars " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script $whBase --vr-mock-pad-race --vr-mock-pad "600 stick -0.3 -0.6 150" --wheelie-log (Join-Path $whDir "bars_thumb_legacy.csv") | Out-String -Width 4096))
        "diag " + (WheelieStarts (& $padGame $Disc @whStickRun --vr-mock-pad "600 stick -0.5 -0.6 150" --wheelie-log (Join-Path $whDir "stick_diag_legacy.csv") | Out-String -Width 4096))
        "tap " + (WheelieStarts (& $padGame $Disc @whStickRun --vr-mock-pad "600 down 6" --wheelie-log (Join-Path $whDir "stick_tap_legacy.csv") | Out-String -Width 4096))
    } finally { Remove-Item Env:\RRJB_WHEELIE_INPUT }
} "(?s)\A(?=.*bars lifts [1-9])(?=.*diag lifts [1-9])(?=.*tap lifts [1-9])"
Gate "wheelie input: deliberate - both hands raised 20 cm start a wheelie after 200 ms, the stick held full back too (mock, 1/20)" {
    $o = & $padGame $Disc @whDeliberate --vr-bars-script "$whBase; 600 lift 0.20 0 12; 780 lift 0 0 12" --wheelie-log (Join-Path $whDir "lift20.csv") | Out-String -Width 4096
    ($o -split "`n" | Where-Object { $_ -match "^wheelie: frame \d+ - the lean back held" }) -join ""
    "lift20 " + (WheelieStarts $o)
    "full " + (WheelieStarts (& $padGame $Disc @whStickRun --vr-mock-pad "600 down 150" --wheelie-log (Join-Path $whDir "stick_full.csv") | Out-String -Width 4096))
} "(?s)\A(?=.*wheelie: frame 6[12]\d - the lean back held 2\d\d ms [^\n]*a wheelie starts \(the VR bars' gesture\))(?=.*lift20 lifts 1 max 3\d\.\d starts 1)(?=.*full lifts [1-9] max 3\d\.\d starts [1-9])"
Gate "wheelie input: deliberate's controls - 12 cm, a 50 ms jerk of 25 cm, one hand 30 cm, a 18 cm pull toward the rider start nothing (mock, 1/20)" {
    "lift12 " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whBase; 600 lift 0.12 0 12" --wheelie-log (Join-Path $whDir "lift12.csv") | Out-String -Width 4096))
    "jerk " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whBase; 600 lift 0.25 0 3; 606 lift 0 0 3" --wheelie-log (Join-Path $whDir "jerk.csv") | Out-String -Width 4096))
    "right " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whBase; 600 lift 0.30 0 12 right" --wheelie-log (Join-Path $whDir "right30.csv") | Out-String -Width 4096))
    "pull " + (WheelieStarts (& $padGame $Disc @whDeliberate --vr-bars-script "$whBase; 600 lift 0.04 0.18 12" --wheelie-log (Join-Path $whDir "pull18.csv") | Out-String -Width 4096))
} "(?s)\A(?=.*lift12 lifts 0 max 0\.0 starts 0)(?=.*jerk lifts 0 max 0\.0 starts 0)(?=.*right lifts 0 max 0\.0 starts 0)(?=.*pull lifts 0 max 0\.0 starts 0)"

# The settings file (tools\rrgame\settings_file.h, settings_check.h): the headset's file shape - a UTF-8
# byte-order mark and CR line ends (as a copy through PowerShell carries), a [handling] section of an older build (no
# lean_model, vr_mode=original), [cheats], a [vr] with combat_defaults - read as an interactive start reads it, one value
# changed on every menu (the VR Riding position page for [vr] and [handling], the cheat page, the F10 graphics save),
# read back: every key of every section present once with the value held; the handling migration fires on the first start
# and marks itself (the section written back complete), so the second start fires nothing - also when no menu is
# opened ("start": the reads alone). Controls: RRJB_SETTINGS_MARK=off (the marker not written at the start: two starts
# with no menu opened fire it twice) and RRJB_INI_BOM=keep (the reader blind to the
# first header: the [handling] section read as absent). About 1 s each.
$iniDir = Join-Path $root "work\gate_settings_$gateTag"
New-Item -ItemType Directory -Force $iniDir | Out-Null
function SettingsFixture([string]$name) {
    $f = Join-Path $iniDir $name
    $lines = @("[handling]", "mode=original", "vr_mode=original", "steer_lag=100", "curve=100", "turn_lag=100", "lean_lag=100", "max_lean=45", "",
               "[cheats]", "weapon=off", "no_police=0", "", "[vr]", "eye_scale=130", "seat_height_cm=-15", "steering=handlebars", "combat=buttons+physical",
               "combat_defaults=1cr", "holster_left=1", "holster_right=7", "weapon_grip_1=-30 0 65 60 0 0")
    $enc = New-Object System.Text.UTF8Encoding($true) # with the byte-order mark
    [IO.File]::WriteAllText($f, (($lines -join "`r`n") + "`r`n"), $enc)
    return $f
}
Gate "settings: the headset's file shape - every menu's change kept, every key present once, the handling migration once" {
    $f = SettingsFixture "roundtrip.ini"
    "first:"; & $padGame --settings-roundtrip-check $f
    "second:"; & $padGame --settings-roundtrip-check $f
} "(?s)\Afirst:(?=.*?the VR handling starts from Modern again.*second:)(?=.*settings round trip: PASS.*second:)(?!.*second:.*the VR handling starts from Modern again)(?=.*second:.*wheelie lift 15 cm, VR seat -10 cm)(?=.*second:.*\[handling\] 17 key\(s\) written, 17 in the file, 17 with the held value, 1 header\(s\) -> KEPT)(?=.*second:.*\[vr\][^\n]*1 header\(s\) -> KEPT)(?=.*second:.*\[cheats\][^\n]*KEPT)(?=.*second:.*\[graphics\][^\n]*KEPT)(?=.*second:.*settings round trip: PASS)"
Gate "settings: two starts with no menu opened - the handling migration fires on the first only" {
    $f = SettingsFixture "startonly.ini"
    "first:"; & $padGame --settings-roundtrip-check $f start; "second:"; & $padGame --settings-roundtrip-check $f start
} "(?s)\Afirst:(?=.*?the VR handling starts from Modern again.*second:)(?!.*second:.*the VR handling starts from Modern again)(?=.*second:.*settings round trip: read - handling VR modern)"
Gate "settings: the migration marker's control - RRJB_SETTINGS_MARK=off, two starts with no menu opened fire the handling migration twice" {
    $f = SettingsFixture "nomark.ini"
    $env:RRJB_SETTINGS_MARK = "off"
    try { "first:"; & $padGame --settings-roundtrip-check $f start; "second:"; & $padGame --settings-roundtrip-check $f start } finally { Remove-Item Env:\RRJB_SETTINGS_MARK }
} "(?s)second:.*the VR handling starts from Modern again"
Gate "settings: the reader's control - RRJB_INI_BOM=keep, the first section behind the byte-order mark is lost (two [handling] headers)" {
    $f = SettingsFixture "bom.ini"
    $env:RRJB_INI_BOM = "keep"
    try { & $padGame --settings-roundtrip-check $f } finally { Remove-Item Env:\RRJB_INI_BOM }
} "(?s)\A(?!.*the VR handling starts from Modern again).*\[handling\] [^\n]*2 header\(s\) -> LOST.*settings round trip: FAIL"
# The product's defaults (tools\rrgame\vr_settings.h ProductDefaults, docs\QUEST.md "Default settings"): a fresh
# install (no file) starts on the handlebars, the seat 15 cm lower, VR handling Modern, the hand lift 14 cm. Control: a
# file's own values win (the stick, the seat at 0).
Gate "settings: a fresh install starts from the product defaults - handlebars, seat -15 cm, VR Modern, hand lift 14 cm" {
    $f = Join-Path $iniDir "fresh.ini"
    if (Test-Path $f) { Remove-Item $f }
    & $padGame --settings-roundtrip-check $f start
} "settings round trip: read - handling VR modern, wheelie lift 14 cm, VR seat -15 cm, steering handlebars"
Gate "settings: the defaults' control - a file's own [vr] values win over them (the stick, the seat at 0)" {
    $f = Join-Path $iniDir "own.ini"
    [IO.File]::WriteAllText($f, "[vr]`nseat_height_cm=0`nsteering=stick`ncombat_defaults=1cr`n")
    & $padGame --settings-roundtrip-check $f start
} "settings round trip: read - handling VR modern, wheelie lift 14 cm, VR seat \+0 cm, steering stick"

Gate "shell: the frontend's own loop, input, dispatch and handover" {
    python (Join-Path $root "tools\scout\shell.py") verify
} "shell: \d+ checks, 0 failures"

Gate "sound: the voice allocator, the bank format and the 3D volume law" {
    python (Join-Path $root "tools\scout\sound.py") verify
} "sound: \d+ checks, 0 failures"

Gate "collision: the broad-phase grid, the boxes and the rider state category" {
    python (Join-Path $root "tools\scout\collide.py") verify
} "collide: \d+ checks, 0 failures"

Gate "race loop: frame order, grid, camera and the end-of-race path" {
    python (Join-Path $root "tools\scout\raceloop.py") verify
} "raceloop: \d+ checks, 0 failures"

Gate "frontend: screen graph, widget tables and the save layout" {
    python (Join-Path $root "tools\scout\frontend.py") verify
} "0 failures"

Gate "bike step: the per-bike step's block map, call sites, fields and cuts" {
    python (Join-Path $root "tools\scout\bike_step.py") verify
} "bike_step: \d+ checks, 0 failures"

Gate "sound: the engine note, replayed against the interpreter" {
    python (Join-Path $root "tools\scout\engine_note.py") verify
} "engine_note: \d+ checks, 0 failures"

Gate "textures: C++ decoder against the Python probe, pixel for pixel" {
    $out = Join-Path $root "work\tex_cpp"
    New-Item -ItemType Directory -Force $out | Out-Null
    & $tool tex $Disc "DATA/CARSC.TEX" $out | Out-Null
    & $tool tex $Disc "DATA/BBLEVEL1.TEX" $out | Out-Null
    python (Join-Path $root "tests\compare_tex.py") $out (Join-Path $root "work\tex")
} "images differing  : 0"

# --- the oracle ---------------------------------------------------------------------------------
$verify = Join-Path $root "$Build\rrverify.exe"
if (Test-Path $verify) {
    # Run WITH --model-row-loop-quirk. Without it the bench reports 99/100 and calls it a FAIL, and
    # that one race is not a defect in our parser: it is the original's own intersection-row loop
    # running once past its declared count. Modelling that rule is
    # what makes the comparison apples to apples, so 100/100 is the meaningful gate.
    Gate "oracle: the original's race-graph parser, run in our interpreter" {
        & $verify road --disc $Disc --model-row-loop-quirk
    } "verdict\s+PASS"

    # The menus against the ORIGINAL's primitives: the product's front end is
    # driven to a page, its shell state planted into the retro-shell capture, the original's widget pass
    # run in the interpreter and its packets rasterised over the capture's VRAM; the product's frame must
    # match. The Bike page (the three stat bars, the chooser arrows only on the highlighted row) and the
    # Gang page; the control (RRJB_MENU_WIDGETS=off, the view's own layout: the stray '<' boxes, no stat
    # bars) must be CAUGHT.
    if (Test-Path (Join-Path $root "work\oracle\state\retro-shell\vram.bin")) {
        $mp = Join-Path $root "work\gate_menu\$gateTag"
        New-Item -ItemType Directory -Force $mp | Out-Null
        $shell = Join-Path $root "work\oracle\state\retro-shell"
        $pages = @(@("the Bike page", "2:g8;20:down;30:down", "200", "bike"),
                   @("the Bike page, Right pressed (the arrow's run)", "2:g8;20:down;30:down;40:right", "46", "bike_right"),
                   @("the Gang page", "2:g7;20:down", "60", "gang"),
                   @("the sound options' sliders", "2:g48;20:down", "60", "noise"))
        foreach ($p in $pages) {
            Gate "menus: $($p[0]) against the original's primitives" {
                & $verify menuprims --disc $Disc --state $shell --script $p[1] --frames $p[2] --out (Join-Path $mp $p[3])
            } "-> PASS"
        }
        Gate "menus: the menu check catches the view's own layout (RRJB_MENU_WIDGETS=off, the Bike page)" {
            $env:RRJB_MENU_WIDGETS = "off"
            & $verify menuprims --disc $Disc --state $shell --script "2:g8;20:down;30:down" --frames 200 --out (Join-Path $mp "bike_off")
            Remove-Item Env:RRJB_MENU_WIDGETS
        } "-> FAIL"
        # The panel films and logos (widget types 2..6 and 8): after 151 idle frames the
        # Bike page's turntable (the bike's .STR in the option's kind-4 record) starts; the film library calls of
        # the original's pass (start / picture at x, y) must be the product's. The control (RRJB_MENU_FILMS=off, the
        # view's own layout: no idle gate, no film calls) must be CAUGHT. The credits mid-scroll (the text arm's
        # draw area) and the jukebox (the highlighted label over its button: a text block's calls in table order).
        foreach ($p in @(@("the Bike page's turntable film starting (idle 151)", "2:g8;20:down;30:down", "181", "film_start", "start\(230,111\) \| ours: start\(230,111\) -> same"),
                         @("the Bike page's turntable film playing", "2:g8;20:down;30:down", "240", "film_play", "picture\(230,111\) \| ours: picture\(230,111\) -> same"),
                         @("the main menu's logo film playing", "2:g4;20:down", "240", "film_main", "picture\(230,111\) \| ours: picture\(230,111\) -> same"),
                         @("the credits mid-scroll", "2:g50", "240", "credits", "-> same"),
                         @("the jukebox", "2:g47;20:down", "60", "jukebox", "-> same"))) {
            Gate "menus: $($p[0]) against the original's pass" {
                & $verify menuprims --disc $Disc --state $shell --script $p[1] --frames $p[2] --out (Join-Path $mp $p[3])
            } "(?s)$($p[4]).*-> PASS"
        }
        Gate "menus: the menu check catches a menu without the panel films (RRJB_MENU_FILMS=off, the Bike page)" {
            $env:RRJB_MENU_FILMS = "off"
            try { & $verify menuprims --disc $Disc --state $shell --script "2:g8;20:down;30:down" --frames 240 --out (Join-Path $mp "film_off") }
            finally { Remove-Item Env:RRJB_MENU_FILMS }
        } "(?s)ours: none -> DIFFERENT.*-> FAIL"
    }

    # The MDEC: the ORIGINAL's DctVlc + DecDCTReset / DecDCTin / DecDCTout on the
    # interpreter's MDEC device against the product's decoders (the films' own VLC + rr::mdec; the sky's PORTED
    # DctVlc + rr::mdec) - 66 film frames of 10 files and every column pair of two captures' skies, word for word.
    # The control (the device on another arithmetic, --device-model legacy) must be CAUGHT.
    $mdecStates = @("--state", (Join-Path $root "work\oracle\state\rr-race"), "--state", (Join-Path $root "work\oracle\state\quick"))
    Gate "mdec: the product's decoders against the original's code on the MDEC device (films, the sky strips)" {
        & $verify mdec --disc $Disc @mdecStates
    } "(?s)films,.*3344128 of 3344128 pixels equal.*rr-race sky strips.*55 bit-exact.*quick sky strips.*55 bit-exact.*mdec verdict PASS"
    Gate "mdec: the check catches the device on another arithmetic (--device-model legacy)" {
        & $verify mdec --disc $Disc @mdecStates --device-model legacy
    } "mdec verdict FAIL"

    # The model -> LECT chunk -> palette binding, re-derived from the disc and diffed against the
    # captured console state. Needs work\disc_us and the
    # extracted states, so it is skipped when they are not there.
    if ((Test-Path (Join-Path $root "work\disc_us\DATA")) -and
        (Test-Path (Join-Path $root "work\oracle\state\rr-race\vram.bin"))) {
        foreach ($state in @("rr-race", "rr-pack", "quick")) {
            Gate "oracle: model -> texture -> palette against the disc ($state)" {
                & $verify texbind --state (Join-Path $root "work\oracle\state\$state") `
                                  --data (Join-Path $root "work\disc_us\DATA")
            } "verdict PASS"
        }
        # The cell -> texture page binding, primitive by primitive: the console's fix-up passes wrote
        # the resolved tpage into every band-0/1 primitive of its resident cells, and our rule
        # (rr::CellTextureSlot over the parser's key lists) must name the same page for each
        # (docs\formats\scene_cell.md 12.2a). The control searches the keys in disc order, without
        # the descending sort of SLUS 0x80033DAC - the defect that painted race 1/20's tree canopy
        # with a neighbouring cell's houses - and must be caught.
        foreach ($state in @("rr-race", "rr-pack", "quick")) {
            Gate "oracle: every cell primitive samples the page the console resolved ($state)" {
                & $verify cellbind --state (Join-Path $root "work\oracle\state\$state") `
                                   --data (Join-Path $root "work\disc_us\DATA")
            } "cellbind verdict PASS(?! \()"
        }
        Gate "oracle: the cell page check catches the disc-order key search (rr-race)" {
            & $verify cellbind --state (Join-Path $root "work\oracle\state\rr-race") `
                               --data (Join-Path $root "work\disc_us\DATA") --mutate
        } "cellbind verdict PASS \(negative control caught\)"
    }

    # render7: our frame against the ORIGINAL's frame for the rr-race state.
    # The interpreter runs the original from the capture and records its GPU words; psxgpu.py turns
    # them into one row per polygon; rrview draws the same state and --roadcheck compares the band-2
    # road (strips, UVs, palettes, colours, lane lines, the road page against VRAM) and bands 0/1
    # (texture windows, UVs, colour-table colours, the quad split) primitive by primitive. Each
    # negative control must be CAUGHT. Then --cellcheck with the colour-table modulation.
    $view = Join-Path $root "$Build\rrview.exe"
    if ((Test-Path $view) -and (Test-Path (Join-Path $root "work\oracle\state\rr-race\ram.bin"))) {
        $r7 = Join-Path $root "work\gate_render7\$gateTag"
        New-Item -ItemType Directory -Force $r7 | Out-Null
        & $verify trace --frames 2 --max-steps 30000000 --no-cop2 --out $r7 | Out-Null
        python (Join-Path $root "tools\scout\psxgpu.py") frame --gpu (Join-Path $r7 "gpu.bin") `
            --vram (Join-Path $root "work\oracle\state\rr-race\vram.bin") --frame 1 `
            --out (Join-Path $r7 "orig.png") --prims (Join-Path $r7 "orig_prims.csv") | Out-Null
        $stateArgs = @("--race", "1", "4", "--tex", "--state", (Join-Path $root "work\oracle\state\rr-race"),
                       "--size", "384", "240", "--orig-prims", (Join-Path $r7 "orig_prims.csv"))
        Gate "render: road, cell windows and colours against the original's packets (rr-race)" {
            & $view $Disc @stateArgs --shot (Join-Path $r7 "ours.png") --roadcheck (Join-Path $r7 "road.txt")
        } "roadcheck verdict PASS(?! \()"
        foreach ($m in @("kind", "palette", "depth", "window")) {
            Gate "render: the road check catches a wrong $m" {
                & $view $Disc @stateArgs --shot (Join-Path $r7 "ours_$m.png") --roadcheck (Join-Path $r7 "road_$m.txt") `
                    "--roadcheck-mutate-$m"
            } "roadcheck verdict PASS \(negative control"
        }
        # The sky layers (clouds RASHCDG 0x8006396C, panorama 0x800644F4 with its HORZ cut-outs) and the
        # bike's shadow (SLUS 0x80025EE0) against the same packets and the capture's VRAM.
        Gate "render: clouds and panorama against the original's packets and VRAM (rr-race)" {
            & $view $Disc @stateArgs --shot (Join-Path $r7 "sky.png") --skypacketcheck (Join-Path $r7 "sky.txt")
        } "skypacketcheck verdict PASS(?! \()"
        foreach ($m in @("rotate", "offs", "yaw", "slice")) {
            Gate "render: the sky check catches a wrong $m" {
                & $view $Disc @stateArgs --shot (Join-Path $r7 "sky_$m.png") --skypacketcheck (Join-Path $r7 "sky_$m.txt") `
                    --skypacketcheck-mutate $m
            } "skypacketcheck verdict PASS \(negative control"
        }
        Gate "render: the bike's shadow quads against the original's packets (rr-race)" {
            & $view $Disc @stateArgs --shot (Join-Path $r7 "shadow.png") --shadowcheck (Join-Path $r7 "shadow.txt")
        } "shadowcheck verdict PASS(?! \()"
        Gate "render: the shadow check catches a wrong light" {
            & $view $Disc @stateArgs --shot (Join-Path $r7 "shadow_m.png") --shadowcheck (Join-Path $r7 "shadow_m.txt") `
                --shadowcheck-mutate
        } "shadowcheck verdict PASS \(negative control"
        Gate "render: every cell pixel is its palette entry times its colour-table colour (1/20 at 6000)" {
            & $view $Disc --race 1 20 --drive --at 6000 --tex --shot (Join-Path $r7 "cells.png") `
                --cellcheck (Join-Path $r7 "cells.txt")
        } "verdict PASS\r?\n"
        Gate "render: the cell check catches the wrong palette row" {
            & $view $Disc --race 1 20 --drive --at 6000 --tex --shot (Join-Path $r7 "cells_m.png") `
                --cellcheck (Join-Path $r7 "cells_m.txt") --cellcheck-mutate
        } "verdict PASS \(negative control"
        # The effects: the sprite sheet the loader builds against the
        # captures' RAM and VRAM, and the PORTED effect pass run on rr-pack (the one capture with live
        # effect records) against the original's packets of that frame, texel by texel through the
        # renderer's rule. Each negative control must FAIL.
        $game = Join-Path $root "$Build\rrgame.exe"
        Gate "render: the effect sheet against the captures' RAM and VRAM (rr-race, rr-pack, rr-grid)" {
            foreach ($s in @("rr-race", "rr-pack", "rr-grid")) {
                & $game $Disc --fxsheetcheck (Join-Path $root "work\oracle\state\$s")
            }
        } "(?s)verdict PASS.*verdict PASS.*verdict PASS"
        Gate "render: the effect sheet check catches dropped sprite offsets" {
            & $game $Disc --fxsheetcheck-mutate (Join-Path $root "work\oracle\state\rr-pack")
        } "(?s)MUTATED.*verdict FAIL"
        $fxPack = Join-Path $r7 "pack"
        New-Item -ItemType Directory -Force $fxPack | Out-Null
        & $verify trace --state (Join-Path $root "work\oracle\state\rr-pack") --frames 2 --max-steps 30000000 `
            --no-cop2 --out $fxPack | Out-Null
        Gate "render: the effects against the original's packets and texels (rr-pack)" {
            & $game $Disc --fxcheck (Join-Path $root "work\oracle\state\rr-pack") (Join-Path $fxPack "prims.csv")
        } "equal word for word: 4 of 4[\s\S]*verdict PASS"
        foreach ($m in @(1, 2)) {
            Gate "render: the effect check catches mutation $m (1 sheet offsets, 2 camera row)" {
                & $game $Disc --fxcheck-mutate $m (Join-Path $root "work\oracle\state\rr-pack") (Join-Path $fxPack "prims.csv")
            } "(?s)MUTATED.*verdict FAIL"
        }
        # The roadside props (race_scene.cpp LoadProps, rrview --propcheck) against the original's packets
        # of the same rr-pack frame: placement, unit and rotation to 2.5 px, the texel of every corner, and
        # the face the model emitter's one-sided test keeps (the front/back plate pairs that flickered).
        python (Join-Path $root "tools\scout\psxgpu.py") frame --gpu (Join-Path $fxPack "gpu.bin") `
            --vram (Join-Path $root "work\oracle\state\rr-pack\vram.bin") --frame 0 `
            --out (Join-Path $fxPack "orig.png") --prims (Join-Path $fxPack "orig_prims.csv") | Out-Null
        $packArgs = @("--race", "1", "4", "--tex", "--state", (Join-Path $root "work\oracle\state\rr-pack"),
                      "--size", "384", "240", "--orig-prims", (Join-Path $fxPack "orig_prims.csv"))
        Gate "render: the roadside props against the original's packets (rr-pack)" {
            & $view $Disc @packArgs --shot (Join-Path $fxPack "props.png") --propcheck (Join-Path $fxPack "props.txt")
        } "propcheck verdict PASS(?! \()"
        foreach ($m in @("cull", "scale", "mirror", "uv")) {
            Gate "render: the prop check catches a wrong $m" {
                & $view $Disc @packArgs --shot (Join-Path $fxPack "props_$m.png") `
                    --propcheck (Join-Path $fxPack "props_$m.txt") --propcheck-mutate $m
            } "propcheck verdict PASS \(negative control"
        }
        # The WHOLE frame: the PRODUCT's frame of each capture - rrgame --parity makes the capture's RAM
        # the session's arena and draws it with the ordinary frame assembly, pixel for pixel on the 384 x 240 draw
        # area - against the original's frame of the same state (the interpreter's GPU words, psxgpu.py). rr-race and
        # quick: the RAM as the traced frame 1 starts to reach the GPU (--dump-ram-frame 1); rr-pack: the snapshot
        # itself (its trace stops at a CD-ROM read inside frame 0). Measured: the share of pixels within 24 and the
        # share of the original's primitives whose mean colour ours matches within 20. The negative control turns the
        # four of its fixes off (the level's sun, the model draw's +0x1B0 frame, the rivals' own models / palettes,
        # the original's model light) and must FAIL.
        $par = Join-Path $root "work\gate_parity"
        $scout = Join-Path $root "tools\scout\psxgpu.py"
        # The frame matched: the ported shadow, the machines from the ported model draw's vertices, the render
        # camera, the emitter's back-face test, the other objects' model light, HudFrame re-run on the capture (the
        # HUD walked from the capture's own slot at its drawing offset: a capture on the second buffer draws at
        # y 256), the 3D scene in the original's ordering-table order (the frame's two tables, SLUS 0x80035958), the
        # ported cell sort, the cells, band 2, the near polygons and the models at the GTE's own SXY (half a pixel
        # on, texels truncated), the sky from the PORTED 0x80064B9C / 0x80063C5C packets, and the console's fill
        # rule (a pixel's integer point inside or on a TOP / LEFT edge, edge_rule.h). The reference keeps the GPU's
        # mask bit (psxgpu.py E6: the shadow's packets darken a pixel once, as the console does) and fills by the
        # same rule. Measured (rr-race, quick, rr-pack): 99.3 / 100.0, 98.3 / 97.4, 98.0 / 100.0; shadow
        # 99.9 / 99.9 / 100.0 %, page 26 98.7 / 98.0 / 97.9 %, page 28 99.0 / 98.0 / 97.7 %, DASH1P
        # 91.0 / 93.0 / 92.6 %, road 100.0 / 99.8 / 100.0 %, the pedestrians (page 12) 66.7 % (rr-race, 9 pixels).
        # quick's black panorama tile is the capture's (a slot its VRAM had not refilled). rr-pack's sky holds
        # pixels an earlier frame of that buffer drew: its own 71 packets equal the capture's word for word, yet
        # the cloud edges sit 1-3 px off.
        $parityLimits = @{ "rr-race" = @("--min-close", "99.1", "--min-prims", "99.5", "--min-category", "shadow:99.5",
                                         "--min-category", "sky gradient:99.5",
                                         "--min-category", "HUD text:99.5", "--min-category", "DASH1P:89",
                                         "--min-category", "page 26:98.5", "--min-category", "page 28:98.5",
                                         "--min-category", "page 7):95.3", "--min-category", "panorama:96.8",
                                         "--min-category", "clouds:99.5", "--min-category", "road surface:99.8",
                                         "--min-category", "page 12:60");
                           "quick"   = @("--min-close", "97.9", "--min-prims", "96.8", "--min-category", "sky gradient:99.5",
                                         "--min-category", "shadow:99.5", "--min-category", "DASH1P:91",
                                         "--min-category", "HUD text:98.5", "--min-category", "page 26:97",
                                         "--min-category", "page 28:97.3", "--min-category", "page 7):91.3",
                                         "--min-category", "panorama:75", "--min-category", "clouds:99.4",
                                         "--min-category", "road surface:99.6");
                           "rr-pack" = @("--min-close", "97.8", "--min-prims", "99.5", "--min-category", "HUD text:98",
                                         "--min-category", "DASH1P:90", "--min-category", "sparks:90",
                                         "--min-category", "page 26:97.5", "--min-category", "page 7):99.4",
                                         "--min-category", "shadow:99.7", "--min-category", "page 11:99",
                                         "--min-category", "road surface:99.2") }
        foreach ($s in @("rr-race", "quick", "rr-pack")) {
            $d = Join-Path $par $s
            New-Item -ItemType Directory -Force $d | Out-Null
            $st = Join-Path $root "work\oracle\state\$s"
            & $verify trace --state $st --sav (Join-Path $root "work\oracle\vr_capture\$s.sav") --no-capture --frames 3 `
                --max-steps 45000000 --no-cop2 --dump-ram-frame 1 --out $d | Out-Null
            $frameArgs = if ($s -eq "rr-pack") { @("--frame", "0", "--gpujson", (Join-Path $st "gpu.json")) } else { @("--frame", "1") }
            python $scout frame --gpu (Join-Path $d "gpu.bin") --vram (Join-Path $st "vram.bin") @frameArgs `
                --out (Join-Path $d "orig.png") --prims (Join-Path $d "orig_prims.csv") --owners (Join-Path $d "orig_owners.bin") | Out-Null
            $ram = if ($s -eq "rr-pack") { $st } else { Join-Path $d "ram_frame1.bin" }
            Gate "render: the product's whole frame against the original's ($s)" {
                & $game $Disc --parity $ram --frames 1 --no-sfx --shot (Join-Path $d "ours.png") | Out-Null
                python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours.png") --prims (Join-Path $d "orig_prims.csv") `
                    --owners (Join-Path $d "orig_owners.bin") --diff (Join-Path $d "diff.png") @($parityLimits[$s])
            } "parity verdict PASS"
        }
        # The sky packets themselves - rr-race / quick against the traced frame's primitives (psxgpu.py's
        # CSV, vertex for vertex), rr-pack against the capture's own OT (word for word; its trace starts after the sky).
        foreach ($s in @("rr-race", "quick")) {
            Gate "render: the product's sky packets equal the original's ($s)" {
                $d = Join-Path $par $s
                $env:RRJB_SKY2_PRIMS = (Join-Path $d "orig_prims.csv")
                try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx }
                finally { Remove-Item Env:RRJB_SKY2_PRIMS }
            } "the traced frame's sky primitives: (\d+) of \1 equal"
        }
        # The sky gradient: the POLY_G4s of the PORTED 0x80063C5C against the traced frame's, vertex
        # and colour; RRJB_SKY3=off (the renderer's gradient, no packets) must show none equal; and the whole-frame check
        # catches the renderer's gradient in place of the original's (rr-race, "sky gradient" 99.0 % < 99.5).
        foreach ($s in @("rr-race", "quick")) {
            Gate "render: the product's gradient packets equal the original's ($s)" {
                $d = Join-Path $par $s
                $env:RRJB_SKY2_PRIMS = (Join-Path $d "orig_prims.csv")
                try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx }
                finally { Remove-Item Env:RRJB_SKY2_PRIMS }
            } "the traced frame's gradient quads: ([1-9]\d*) of \1 equal"
        }
        Gate "render: the gradient check catches the renderer's own gradient (rr-race, RRJB_SKY3=off)" {
            $d = Join-Path $par "rr-race"
            $env:RRJB_SKY2_PRIMS = (Join-Path $d "orig_prims.csv"); $env:RRJB_SKY3 = 'off'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx }
            finally { Remove-Item Env:RRJB_SKY2_PRIMS, Env:RRJB_SKY3 }
        } "the traced frame's gradient quads: 0 of [1-9]"
        Gate "render: the whole-frame check catches the renderer's own gradient (rr-race, RRJB_SKY3=off)" {
            $d = Join-Path $par "rr-race"
            $env:RRJB_SKY3 = 'off'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "ours_sky3_off.png") | Out-Null }
            finally { Remove-Item Env:RRJB_SKY3 }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_sky3_off.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-race"])
        } "parity verdict FAIL"
        # The MDEC tile cache: the sky's tiles the product decodes again for --parity, against
        # the capture's own VRAM halfword for halfword, with the MDEC arithmetic of the capture's emulator
        # (RRJB_MDEC=emuold: its default "old routines"): every tile bit-exact proves the product's whole path - the
        # PORTED DctVlc, the command bits, the tables, the macroblock layout, HorzCut, the slot walk. The product's own
        # arithmetic (the console's) differs from that emulator's by one 5-bit step on ~70 % of the texels.
        foreach ($s in @("rr-race", "quick")) {
            Gate "mdec: the product's sky tiles equal the capture's VRAM under the capture emulator's MDEC ($s)" {
                $env:RRJB_MDEC = "emuold"
                $env:RRJB_SKY2_VRAM = Join-Path $root "work\oracle\state\$s\vram.bin"
                try { & $game $Disc --parity (Join-Path (Join-Path $par $s) "ram_frame1.bin") --frames 1 --no-sfx }
                finally { Remove-Item Env:RRJB_MDEC; Remove-Item Env:RRJB_SKY2_VRAM }
            } "(\d+) tiles, \1 bit-exact, 0 not refilled"
        }
        Gate "render: the product's sky packets equal the original's (rr-pack)" {
            & $game $Disc --parity (Join-Path $root "work\oracle\state\rr-pack") --frames 1 --no-sfx
        } "the capture's own sky packets: (\d+) of \1 equal"
        Gate "render: the whole-frame check catches the renderer's own sky in place of the original's (rr-race)" {
            $d = Join-Path $par "rr-race"
            $env:RRJB_SKY2 = 'off'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "ours_sky2_off.png") | Out-Null }
            finally { Remove-Item Env:RRJB_SKY2 }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_sky2_off.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-race"])
        } "parity verdict FAIL"
        Gate "render: the whole-frame check catches the renderer fixes switched off (quick)" {
            $d = Join-Path $par "quick"
            $env:RRJB_SUN = 'off'; $env:RRJB_BIKE_FRAME = 'ground'; $env:RRJB_RIVALS = 'player'; $env:RRJB_MODEL_LIGHT = 'off'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "ours_off.png") | Out-Null }
            finally { Remove-Item Env:RRJB_SUN, Env:RRJB_BIKE_FRAME, Env:RRJB_RIVALS, Env:RRJB_MODEL_LIGHT }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_off.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["quick"])
        } "parity verdict FAIL"
        # The subdividers' control: without the original's affine mapping and subdivision (RRJB_AFFINE=off, a
        # perspective-correct renderer) rr-pack's near wall lands elsewhere and the frame must FAIL.
        Gate "render: the whole-frame check catches the subdividers switched off (rr-pack)" {
            $d = Join-Path $par "rr-pack"
            $env:RRJB_AFFINE = 'off'
            try { & $game $Disc --parity (Join-Path $root "work\oracle\state\rr-pack") --frames 1 --no-sfx --shot (Join-Path $d "ours_off.png") | Out-Null }
            finally { Remove-Item Env:RRJB_AFFINE }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_off.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-pack"])
        } "parity verdict FAIL"
        # The shadow and model-draw control: five switches off - our shadow approximation, the renderer's own
        # posing of the machines, the view record's camera, both sides of the machines, the other objects without the model
        # light, HudFrame on the capture - must FAIL rr-race's limits (measured 89.4 / 83.0 without the HUD switch).
        Gate "render: the whole-frame check catches the shadow and model-draw fixes switched off (rr-race)" {
            $d = Join-Path $par "rr-race"
            $env:RRJB_SHADOW = 'ours'; $env:RRJB_POSE_FROM = 'ours'; $env:RRJB_GL_CAMERA = 'view'; $env:RRJB_MACHINE_NCLIP = 'off'
            $env:RRJB_LOOK_LIGHT = 'off'; $env:RRJB_PARITY_HUD = 'off'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "ours_look_off.png") | Out-Null }
            finally { Remove-Item Env:RRJB_SHADOW, Env:RRJB_POSE_FROM, Env:RRJB_GL_CAMERA, Env:RRJB_MACHINE_NCLIP, Env:RRJB_LOOK_LIGHT, Env:RRJB_PARITY_HUD }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_look_off.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-race"])
        } "parity verdict FAIL"
        # The effect records: rr-pack's effect records drawn as the capture's own frame left them (its draw
        # cycle already aged them: the player bike's +9 bit 3 is set) and the effect packets rasterised at the GPU's
        # integer pixel corner - measured 92.2 / 90.0, the sparks 89.1 % close. Each fix
        # off alone must FAIL rr-pack's limits (measured 91.3 / 89.2 with sparks 25.4 %; 91.5 / 88.8 with 36.7 %).
        foreach ($ctl in @(@("RRJB_PARITY_FX_AGE", "again", "aged once more"), @("RRJB_FX_PIXEL", "centre", "sampled at pixel centres"))) {
            Gate "render: the whole-frame check catches the effect records $($ctl[2]) (rr-pack)" {
                $d = Join-Path $par "rr-pack"
                Set-Item -Path "Env:$($ctl[0])" -Value $ctl[1]
                try { & $game $Disc --parity (Join-Path $root "work\oracle\state\rr-pack") --frames 1 --no-sfx --shot (Join-Path $d "ours_fx_off.png") | Out-Null }
                finally { Remove-Item "Env:$($ctl[0])" }
                python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_fx_off.png") --prims (Join-Path $d "orig_prims.csv") `
                    --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-pack"])
            } "parity verdict FAIL"
        }
        # The HUD placement's control: the HUD walked from the product's placement 0x800D9CA0 at offset (0, 0) - rr-pack
        # was captured on the second buffer, so the top panels' text is clipped away by the capture's absolute draw area
        # (measured: HUD text 57.4 %, close 88.6 %) and must FAIL.
        # The ordering tables' control: the scene with the depth buffer instead of the ordering tables - the
        # bike's own polygons resolve by depth, the fork shows through the leg (measured page 26 76.9 %) - must FAIL.
        Gate "render: the whole-frame check catches the depth buffer in place of the ordering tables (rr-pack)" {
            $d = Join-Path $par "rr-pack"
            $env:RRJB_OT_ORDER = 'zbuffer'
            try { & $game $Disc --parity (Join-Path $root "work\oracle\state\rr-pack") --frames 1 --no-sfx --shot (Join-Path $d "ours_zbuffer.png") | Out-Null }
            finally { Remove-Item Env:RRJB_OT_ORDER }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_zbuffer.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-pack"])
        } "parity verdict FAIL"
        # The cell sort's control: the frame's cell sort and the cell emitters' back-face test off - quick's
        # far-left facade keeps the sign's side panel the original drops (measured page 7 76.8 %) - must FAIL.
        Gate "render: the whole-frame check catches the cells drawn without the original's back-face test (quick)" {
            $d = Join-Path $par "quick"
            $env:RRJB_CELL_SORT = 'off'; $env:RRJB_CELL_NCLIP = 'off'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "ours_cells_off.png") | Out-Null }
            finally { Remove-Item Env:RRJB_CELL_SORT, Env:RRJB_CELL_NCLIP }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_cells_off.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["quick"])
        } "parity verdict FAIL"
        # The GTE projection's control: every vertex through the float GL camera and texels rounded to the
        # nearest (measured 93.2 / 92.9 %) - must FAIL.
        Gate "render: the whole-frame check catches the float camera in place of the GTE's SXY (rr-race)" {
            $d = Join-Path $par "rr-race"
            $env:RRJB_PROJ = 'float'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "ours_float.png") | Out-Null }
            finally { Remove-Item Env:RRJB_PROJ }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_float.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-race"])
        } "parity verdict FAIL"
        # The model-draw SXY controls: the shadow's world points through the float camera (measured shadow 97.4 %), the
        # cars / props / pedestrians likewise (props 95.2 %), and the reference drawn without the mask bit (shadow 81.5 %) -
        # each must FAIL rr-pack's limits.
        foreach ($ctl in @(@("RRJB_GTE_SHADOW", "world", "the shadow at its world points"), @("RRJB_GTE_OBJECTS", "off", "the objects on the float camera"))) {
            Gate "render: the whole-frame check catches $($ctl[2]) (rr-pack)" {
                $d = Join-Path $par "rr-pack"
                Set-Item -Path "Env:$($ctl[0])" -Value $ctl[1]
                try { & $game $Disc --parity (Join-Path $root "work\oracle\state\rr-pack") --frames 1 --no-sfx --shot (Join-Path $d "ours_gte2_off.png") | Out-Null }
                finally { Remove-Item "Env:$($ctl[0])" }
                python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_gte2_off.png") --prims (Join-Path $d "orig_prims.csv") `
                    --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-pack"])
            } "parity verdict FAIL"
        }
        # The fill rule: GL's own coverage (RRJB_EDGE=gl) and a reference filled with
        # the mirrored rule (PSXGPU_EDGE=br) must both be CAUGHT.
        Gate "render: the whole-frame check catches GL's own fill rule (RRJB_EDGE=gl, rr-race)" {
            $d = Join-Path $par "rr-race"
            $env:RRJB_EDGE = 'gl'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "ours_edge_gl.png") | Out-Null }
            finally { Remove-Item Env:RRJB_EDGE }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_edge_gl.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-race"])
        } "parity verdict FAIL"
        Gate "render: the whole-frame check catches a reference with the mirrored fill rule (PSXGPU_EDGE=br, quick)" {
            $d = Join-Path $par "quick"
            $st = Join-Path $root "work\oracle\state\quick"
            $env:PSXGPU_EDGE = 'br'
            try { python $scout frame --gpu (Join-Path $d "gpu.bin") --vram (Join-Path $st "vram.bin") --frame 1 `
                      --out (Join-Path $d "orig_br.png") --prims (Join-Path $d "orig_br.csv") --owners (Join-Path $d "orig_br.bin") | Out-Null }
            finally { Remove-Item Env:PSXGPU_EDGE }
            python $scout parity (Join-Path $d "orig_br.png") (Join-Path $d "ours.png") --prims (Join-Path $d "orig_br.csv") `
                --owners (Join-Path $d "orig_br.bin") @($parityLimits["quick"])
        } "parity verdict FAIL"
        Gate "render: the whole-frame check catches a reference drawn without the GPU's mask bit (rr-pack)" {
            $d = Join-Path $par "rr-pack"
            $st = Join-Path $root "work\oracle\state\rr-pack"
            $env:PSXGPU_MASK = 'off'
            try { python $scout frame --gpu (Join-Path $d "gpu.bin") --vram (Join-Path $st "vram.bin") --frame 0 --gpujson (Join-Path $st "gpu.json") `
                      --out (Join-Path $d "orig_nomask.png") --prims (Join-Path $d "orig_nomask.csv") --owners (Join-Path $d "orig_nomask.bin") | Out-Null }
            finally { Remove-Item Env:PSXGPU_MASK }
            python $scout parity (Join-Path $d "orig_nomask.png") (Join-Path $d "ours.png") --prims (Join-Path $d "orig_nomask.csv") `
                --owners (Join-Path $d "orig_nomask.bin") @($parityLimits["rr-pack"])
        } "parity verdict FAIL"
        Gate "render: the whole-frame check catches the HUD drawn at the product's offset (rr-pack)" {
            $d = Join-Path $par "rr-pack"
            $env:RRJB_PARITY_HUD_ORIGIN = 'off'
            try { & $game $Disc --parity (Join-Path $root "work\oracle\state\rr-pack") --frames 1 --no-sfx --shot (Join-Path $d "ours_hud_off.png") | Out-Null }
            finally { Remove-Item Env:RRJB_PARITY_HUD_ORIGIN }
            python $scout parity (Join-Path $d "orig.png") (Join-Path $d "ours_hud_off.png") --prims (Join-Path $d "orig_prims.csv") `
                --owners (Join-Path $d "orig_owners.bin") @($parityLimits["rr-pack"])
        } "parity verdict FAIL"
        # The ported shadow SLUS 0x80025EE0: the product's shadow packets of each capture are the
        # original's 0x2A packets, all 65 of them, four screen points each; the control reads the ground normal from
        # the light vector (RRJB_SHADOW_MUTATE) and must FAIL.
        foreach ($s in @("rr-race", "quick", "rr-pack")) {
            Gate "render: the product's shadow packets are the original's ($s)" {
                $d = Join-Path $par $s
                $ram = if ($s -eq "rr-pack") { Join-Path $root "work\oracle\state\$s" } else { Join-Path $d "ram_frame1.bin" }
                & $game $Disc --parity $ram --frames 1 --no-sfx --shot (Join-Path $d "shadow.png") --shadowcheck (Join-Path $d "orig_prims.csv")
            } "shadowcheck verdict PASS"
        }
        Gate "render: the shadow packet check catches a wrong ground normal (quick)" {
            $d = Join-Path $par "quick"
            $env:RRJB_SHADOW_MUTATE = '1'
            try { & $game $Disc --parity (Join-Path $d "ram_frame1.bin") --frames 1 --no-sfx --shot (Join-Path $d "shadow_mut.png") --shadowcheck (Join-Path $d "orig_prims.csv") }
            finally { Remove-Item Env:RRJB_SHADOW_MUTATE }
        } "shadowcheck verdict FAIL"
        # A rival is drawn only within ModelVisible's range of its kind (cell_view.h InDrawRange):
        # held back, the player sees rivals 2..4 take the ramp near route 507 about 358 units ahead, where the
        # population window retires them in the air - otherwise they would "ride off into the sky".
        # (with the original grid - the player at the back of 18 - the brake comes at frame 300.)
        Gate "the game: no rival vanishes in the air while drawn (1/20, the player held back)" {
            & $game $Disc --race 1 20 --frames 1000 --autosteer --hold T --brake-from 300 --no-sfx
        } "vanished in the air while drawn: 0\r?\n"
        Gate "the game: the draw-range check catches rivals drawn at any range" {
            & $game $Disc --race 1 20 --frames 1000 --autosteer --hold T --brake-from 300 --no-sfx --draw-range-off
        } "SWITCHED OFF.*vanished in the air while drawn: [1-9]"
        # The model draw's SXY: the cars / props / pedestrians drawn at the model draw's SXY, the GPU's large-polygon rule
        # applied (race 1/1, 1000 frames: both counters non-zero), and two players at the GTE's own SXY (the views' vertex
        # passes counted; RRJB_GTE_2P=off: none).
        Gate "the game: the objects at the model draw's SXY and the GPU's large-polygon rule (1/1)" {
            & $game $Disc --race 1 1 --frames 1000 --autosteer --hold T --no-sfx
        } "drawn at their model-draw SXY [1-9]\d* time.*refused by the GPU's 1024 x 512 rule [1-9]\d* \(cells\)"
        Gate "the game: two players at the GTE's own SXY (1/1)" {
            & $game $Disc --race 1 1 --players 2 --frames 300 --hold T --hold2 T --no-sfx
        } "gteproj: the world's vertices through the GTE's own RTPS \(the cells' slot RT / TR, H 237\) in [1-9]\d* frame\(s\): [1-9]"
        # Two players' sky from the original's packets - the gradient 0x80063C5C and the
        # two-player sky 0x80064CC8 (GAMEBIN2.DAT's type-10 section, the DATA\FE TIMs) per view; RRJB_SKY3=off: none
        Gate "the game: two players' sky from the original's packets (1/1)" {
            & $game $Disc --race 1 1 --players 2 --frames 300 --hold T --hold2 T --no-sfx
        } "sky3: PORTED.* [1-9]\d* gradient packets \(0x80063C5C\), [1-9]\d* two-player sky packets"
        Gate "the game: the two-player sky check catches the renderer's sky (1/1, RRJB_SKY3=off)" {
            $env:RRJB_SKY3 = 'off'
            try { & $game $Disc --race 1 1 --players 2 --frames 300 --hold T --hold2 T --no-sfx } finally { Remove-Item Env:RRJB_SKY3 }
        } "sky3: OFF.* 0 gradient packets \(0x80063C5C\), 0 two-player sky packets"
        Gate "the game: the two-player check catches the float camera (1/1, RRJB_GTE_2P=off)" {
            $env:RRJB_GTE_2P = 'off'
            try { & $game $Disc --race 1 1 --players 2 --frames 300 --hold T --hold2 T --no-sfx } finally { Remove-Item Env:RRJB_GTE_2P }
        } "gteproj: the world's vertices through the GTE's own RTPS \(the cells' slot RT / TR, H 237\) in 0 frame\(s\)"
        # The grid: BuildGrid RASHCDI 0x80067B00 / SpawnBike 0x80065A94 PORTED on the arena;
        # race 1/4 is rr-race's race, whose two parked police bikes hold the grid's road coordinate unchanged.
        Gate "the game: the starting grid is the original's (BuildGrid PORTED; 1/4's parked police where the capture has them)" {
            & $game $Disc --race 1 4 --frames 1 --no-sfx
        } "(?s)the grid is the ORIGINAL's.*bike 16 slot 17 police class 2 live 0 road 9 dir -1 along  1056\.998.*bike 17 slot 18 police class 2 live 0 road 9 dir -1 along  1054\.070"
        Gate "the game: RRJB_GRID=ours brings back the session's own layout (the grid's negative control)" {
            $env:RRJB_GRID = 'ours'; try { & $game $Disc --race 1 4 --frames 1 --no-sfx } finally { Remove-Item Env:RRJB_GRID }
        } "grid \(OURS: the session's layout\): bike  0"
        # The streamer: the PORTED streamer replayed over the 14 VR dumps of race 1/4 holds
        # every piece, cell and loaded key the console holds; its control skips the release pass
        $vr = Join-Path $root "work\oracle\vr_capture\ramdumps"
        Gate "the game: the streamer holds what the console holds (--streamcheck, 13 VR images)" {
            & $game $Disc --streamcheck $vr
        } "streamcheck [^\n]*13 image\(s\) after the first compared, 0 mismatch\(es\)"
        Gate "the game: the stream check catches a streamer that never releases (--streamcheck-mutate)" {
            & $game $Disc --streamcheck-mutate $vr
        } "streamcheck-mutate [^\n]*, [1-9]\d* mismatch\(es\)"
        # Player 2's Start (mp_input.h): the pause test reads pad record 1 too - player 2 pauses, moves
        # the pause menu on its own record and resumes; the control (RRJB_P2_START=off) never pauses.
        Gate "the game: player 2 pauses a two-player race with its own Start and resumes from the menu" {
            & $game $Disc --race 2 1 --players 2 --frames 900 --hold T --hold2 T --no-sfx --pause-script "400:p2start;420:p2down;440:p2up;460:p2x"
        } "(?s)paused 1 time\(s\).*resumed 1.*pauses by pad 1: 0, by pad 2: 1"
        Gate "the game: player 2 Start's control (RRJB_P2_START=off) does not pause" {
            $env:RRJB_P2_START = 'off'; try { & $game $Disc --race 2 1 --players 2 --frames 900 --hold T --hold2 T --no-sfx --pause-script "400:p2start" } finally { Remove-Item Env:RRJB_P2_START }
        } "pauses by pad 1: 0, by pad 2: 0"
        # Side Car versus (screen 35 reads pad 1 only, frontend.md 2.2): player 2's Right picks its own sidecar (bike 15)
        # and the race builds player 2's own rig from it; the control presses the same Right on player 1's pad, which
        # screen 35 does not read (player 2 keeps bike 6).
        $sc35 = "2:g4;10:down;20:x;40:down;50:down;60:down;70:down;80:up;90:x;120:x;140:p2down;150:{0};160:p2up;170:p2x"
        Gate "the game: Side Car from the menus - player 2 picks its own sidecar on screen 35 and races it" {
            & $game $Disc --shell-frames 200 --shell-script ($sc35 -f "p2right") --shell-race "--frames 120 --no-sfx --hold T"
        } "(?s)HANDOVER: race set 2 id 1, race type 0x18.*player 1's sidecar \(bike 6\).*player 2's sidecar \(bike 15\)"
        Gate "the game: Side Car control - player 1's Right on screen 35 leaves player 2's sidecar" {
            & $game $Disc --shell-frames 200 --shell-script ($sc35 -f "right") --shell-race "--frames 120 --no-sfx --hold T"
        } "(?s)HANDOVER: race set 2 id 1, race type 0x18.*player 1's sidecar \(bike 6\).*player 2's sidecar \(bike 6\)"
        # Jailbreak (rules.md 16): the menus' own path hands the race type 0x2C, and the race loads player 1's
        # sidecar rig (CRUISES3.MRO model 108), spawns the passenger and reads both STARTJBA.BIN blocks.
        # rules.md 16.5: the Jailbreak bundle bblevJBD (LoadBikeBank 0x8005C45C), the rig in the model arena with its
        # own six part slots, animated from them (fork / wheels), its page and TSLP palette from the bundle.
        Gate "the game: Jailbreak from the menus - race type 0x2C, bblevJBD, the posed sidecar rig, the passenger, STARTJBA.BIN" {
            & $game $Disc --jailbreak --shell-frames 60 --shell-race "--frames 300 --no-sfx --hold T"
        } "(?s)(?=.*HANDOVER: race set 1 id 30, race type 0x2C)(?=.*sidecar: DATA/CRUISES3\.MRO model 108 [^\n]*sheet LECT 112 from DATA/BBLEVJBD\.TEX, palette TSLP block \d+ of DATA/BBLEVJBD\.TEX)(?=.*the model arena [^\n]*DATA/BBLEVJBD\.GEO models [^\n]*108@\d+ \(DATA/CRUISES3\.MRO\))(?=.*the sidecar rig model 108: player 1's bike has its own 6 part slots)(?=.*player 1's sidecar \(bike 8\): passenger bike)(?=.*STARTJBA\.BIN \(RASHCDI 0x80068740, (?:transcribed|PORTED in BuildRace)\): read; block 0 16 record\(s\), block 1 27)(?=.*the sidecar rig \(race_scene_sidecar\.cpp\): [1-9]\d* frame\(s\) drawn from its 6 part slots; frames a slot changed: fork 1 \d+, pitch 2 \d+, wheel 3 [1-9])"
    }
}

# --- HD media (docs\HD-MEDIA.md) -----------------------------------------------------------------
# The optional HD pack's loader. The pack a player prepares (scripts\prepare-hd.ps1, neural pictures of the player's own
# disc) is never needed here: rrhd writes SYNTHETIC pictures (colour bands) keyed to this disc's menu background and
# button font into work\, and the front end must (1) draw them, (2) refuse them entry by entry when their source hashes
# are wrong, (3) refuse another disc's pack whole, and (4) with HD media off draw exactly the frame it draws without a
# pack. The format and the film bound are checked on data rrhd makes up (selftest).
$rrhd = Join-Path $root "$Build\rrhd.exe"
$hdGame = Join-Path $root "$Build\rrgame.exe"
if ((Test-Path $rrhd) -and (Test-Path $hdGame)) {
    $hdWork = Join-Path $root "work\gate_hd_$gateTag"
    New-Item -ItemType Directory -Force -Path $hdWork | Out-Null
    $hdMenu = @('--shell-frames', '60', '--shell-script', '2:g4')
    Gate "HD media: the pack format, the contour pass and the film bound on synthetic data, negative controls included" {
        & $rrhd selftest (Join-Path $hdWork 'selftest')
    } "hd selftest: \d+ checks, 0 failed"
    Gate "HD media: the front end draws a pack keyed to this disc (menu background, button font) at scale 4" {
        & $rrhd synth-pack $Disc (Join-Path $hdWork 'good') | Out-Null
        $env:RRJB_HD_PACK = Join-Path $hdWork 'good'
        try { & $hdGame $Disc @hdMenu --hd-media 1 --shell-frame (Join-Path $hdWork 'good.png') } finally { Remove-Item Env:RRJB_HD_PACK }
    } "(?s)HD textures and media ON.*shell frame: 2048x960.*shell HD: scale 4, 5 HD pictures loaded, \d+ pictures without one, 1 contour fonts"
    Gate "HD media: NEGATIVE - a pack whose source hashes are wrong is refused entry by entry (the originals are drawn)" {
        & $rrhd synth-pack $Disc (Join-Path $hdWork 'wrong') --wrong-hash | Out-Null
        $env:RRJB_HD_PACK = Join-Path $hdWork 'wrong'
        try { & $hdGame $Disc @hdMenu --hd-media 1 } finally { Remove-Item Env:RRJB_HD_PACK }
    } "(?s)shell HD: scale 4, 0 HD pictures loaded, \d+ pictures without one, 0 contour fonts.*HD picture DATA/FE/BGRND2\.STR#0 refused: made from another source.*HD font DATA/FE/BTN_FONT\.PFN refused"
    Gate "HD media: NEGATIVE - another disc's pack is refused whole" {
        & $rrhd synth-pack $Disc (Join-Path $hdWork 'other') --wrong-profile | Out-Null
        $env:RRJB_HD_PACK = Join-Path $hdWork 'other'
        try { & $hdGame $Disc @hdMenu --hd-media 1 } finally { Remove-Item Env:RRJB_HD_PACK }
    } "(?s)hd: HD pack .* REJECTED: made from the disc whose SLUS_010\.53 SHA-1 is 0{40}"
    Gate "HD media: switched off, the menu frame is byte for byte the frame without a pack" {
        $env:RRJB_HD_PACK = Join-Path $hdWork 'good'
        try { & $hdGame $Disc @hdMenu --hd-media 0 --shell-frame (Join-Path $hdWork 'off.png') | Out-Null } finally { Remove-Item Env:RRJB_HD_PACK }
        $env:RRJB_HD_PACK = Join-Path $hdWork 'no-pack-here'
        try { & $hdGame $Disc @hdMenu --shell-frame (Join-Path $hdWork 'none.png') | Out-Null } finally { Remove-Item Env:RRJB_HD_PACK }
        $a = (Get-FileHash (Join-Path $hdWork 'off.png')).Hash
        $b = (Get-FileHash (Join-Path $hdWork 'none.png')).Hash
        if ($a -eq $b) { "HD off: identical ($a)" } else { "HD off: DIFFERENT ($a / $b)" }
    } "HD off: identical"
}

# --- slow gates ---------------------------------------------------------------------------------
# VR comfort (tools\rrgame\vr_comfort.h): the VR menu on the triggers, the menus hold the rumble, the view after a
# fall, smooth motion. The menu through --vr-mock-pad on race 1/20: the chord at turn 300, six rows down to Riding
# position and A (its first row: Seat height),
# the stick held right for 40 turns and drifting at 0.7 for 50, the left trigger, the right trigger twice, Menu closes it
# at 500 - only the triggers change the value; the Touch actuators are sent nothing while the menu is open, though the
# race's rumble runs under it (without the hold the hands' haptics re-send it every frame) and after it. Controls:
# RRJB_VR_MENU_STICK=values (the stick changes the value again), RRJB_VR_HAPTICS_HOLD=off (the rumble reaches the
# controllers under the menu). The pause (--pause-script) holds them too. About 10 s each.
$comfortMenu = @("--vr-mock", "--race", "1", "20", "--frames", "900", "--hold", "T", "--autosteer", "--no-sfx", "--vr-mock-pad",
                 "300 chord; 310 down; 314 down; 318 down; 322 down; 326 down; 330 down; 334 a; 340 stickright 40; 390 drift 0.7 50; 450 ltrigger; 460 rtrigger; 470 rtrigger; 500 menu")
Gate "vr comfort: the VR menu's values change only with the triggers - the stick held right and drifting changes nothing (mock, 1/20)" {
    & $padGame $Disc @comfortMenu
} "(?s)(?=.*vr menu: 9/Seat height: \+0 cm -> 9/Seat height: -5 cm \(by a trigger\))(?=.*value changes by the triggers 3, by the stick's left / right 0, )"
Gate "vr comfort: the menu rule's control - RRJB_VR_MENU_STICK=values, the stick changes the value (mock, 1/20)" {
    $env:RRJB_VR_MENU_STICK = "values"
    try { & $padGame $Disc @comfortMenu } finally { Remove-Item Env:\RRJB_VR_MENU_STICK }
} "value changes by the triggers 3, by the stick's left / right [1-9]\d*, "
Gate "vr comfort: nothing reaches the Touch actuators while the VR menu is open or the race is paused, the rumble runs again after (mock, 1/20)" {
    & $padGame $Disc @comfortMenu
    & $padGame $Disc --vr-mock --race 1 20 --frames 900 --hold T --autosteer --no-sfx --pause-script "300:start;500:start"
} "(?s)(?=.*\(200 with the VR menu, 0 paused\) - xrApplyHapticFeedback 0 \(VR menu 0, pause 0\), stop [1-9]\d*; \d+ racing turn\(s\) - apply [1-9])(?=.*\(0 with the VR menu, 200 paused\) - xrApplyHapticFeedback 0 \(VR menu 0, pause 0\))"
Gate "vr comfort: the haptics' control - RRJB_VR_HAPTICS_HOLD=off, the rumble reaches the controllers under the VR menu (mock, 1/20)" {
    $env:RRJB_VR_HAPTICS_HOLD = "off"
    try { & $padGame $Disc @comfortMenu } finally { Remove-Item Env:\RRJB_VR_HAPTICS_HOLD }
} "\(200 with the VR menu, 0 paused\) - xrApplyHapticFeedback [1-9]\d* \(VR menu [1-9]\d*, pause 0\)"

# The fall (race 1/11 with the autosteer: off the bike at frame 1235 into a truck, back on at 2441, off at 3025, on at
# 4223): the head view comes back after each re-seat with the eye where it was on the bike (re-latched on the first
# seated frame the eye sat 1.45 m right of the bike at hip height - the headset's "shifted" / "stayed in third person"
# view), off the bike a fixed level view (no turn) under a short fade. Control: RRJB_FALL_VIEW=legacy (the original's
# camera turning and jumping unfaded, the eye re-latched off the bike). About 50 s each.
$comfortFall = @("--vr-mock", "--race", "1", "11", "--frames", "5000", "--hold", "T", "--autosteer", "--no-sfx")
Gate "vr comfort: after a fall the head view returns with the eye kept on the bike; off it a level fixed view with a fade (mock, 1/11)" {
    & $padGame $Disc @comfortFall
} "falls 2, re-seats 2, \d+ frame\(s\) off the bike \(the anchor moves up to 0\.\d+ world units and turns up to 0\.00 deg a frame\), [1-9]\d* faded, the eye's point on the bike moved up to 0\.000 world units at a re-seat, the head view back after a re-seat [1-9]\d* frame\(s\) \(chase 0\), the re-latch an eye without the re-seat rule would have taken (?:0\.[5-9]|[1-9])"
Gate "vr comfort: the fall's control - RRJB_FALL_VIEW=legacy, the view turns and jumps unfaded, the eye re-latched off the bike (mock, 1/11)" {
    $env:RRJB_FALL_VIEW = "legacy"
    try { & $padGame $Disc @comfortFall } finally { Remove-Item Env:\RRJB_FALL_VIEW }
} "turns up to (?:[5-9]|\d\d+)\.\d+ deg a frame\), 0 faded, the eye's point on the bike moved up to (?:0\.[5-9]|[1-9])\d*\.\d+ world units at a re-seat"

# Smooth motion (the judder): the scripted mock on a synthetic 72 Hz display (--vr-mock-hz 72: the headset's 4, 4, 4,
# 4, 4, 5 ticks a frame), race 1/20, 1200 frames. With it (the default) the world 5 m ahead of the eye moves in the eye
# with a second difference of ~7 mm RMS and the eye's jerk is ~1200 m/s^3; control: --vr-smooth-motion 0 - 64 mm and
# ~41000 m/s^3. And the simulation is untouched: the race log's frame lines (the PORTED engine's fields after every
# step) are the same with it on and off - the arena is put back byte for byte after each drawn frame; the comparison's
# control: 72 Hz against 90 Hz differ. About 15 s each.
$comfortJudder = @("--vr-mock", "--race", "1", "20", "--frames", "1200", "--hold", "T", "--autosteer", "--no-sfx", "--vr-mock-hz", "72")
Gate "vr comfort: smooth motion at 72 Hz - the world's judder in the eye under 15 mm RMS, the eye's jerk under 3000 m/s^3 (mock, 1/20)" {
    & $padGame $Disc @comfortJudder
} "(?s)(?=.*the world 5 m ahead in the eye: second difference RMS (?:[0-9]|1[0-4])\.\d+ mm, [^\n]*the eye's jerk RMS (?:[0-9]{1,3}|[12][0-9]{3})\.\d m/s\^3)(?=.*0 cut\(s\), 1200 restore\(s\), 0 restore mismatch\(es\); the head view's bike stabilised 1200 frame\(s\))"
Gate "vr comfort: smooth motion's control - off at 72 Hz, the world judders over 40 mm RMS in the eye (mock, 1/20)" {
    & $padGame $Disc @comfortJudder --vr-smooth-motion 0
} "the world 5 m ahead in the eye: second difference RMS (?:[4-9]\d|\d{3,})\.\d+ mm"
$comfortLogs = {
    param([string[]]$a, [string[]]$b)
    $dir = Join-Path $root "work\gate_comfort\$gateTag"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $la = Join-Path $dir "a.log"
    $lb = Join-Path $dir "b.log"
    & $padGame $Disc --vr-mock --race 1 20 --frames 600 --hold T --autosteer --no-sfx --log $la @a | Out-Null
    & $padGame $Disc --vr-mock --race 1 20 --frames 600 --hold T --autosteer --no-sfx --log $lb @b | Out-Null
    $fa = @(Get-Content $la | Where-Object { $_ -match '^f\d+ ' })
    $fb = @(Get-Content $lb | Where-Object { $_ -match '^f\d+ ' })
    $same = $fa.Count -eq $fb.Count
    for ($i = 0; $same -and $i -lt $fa.Count; $i++) { if ($fa[$i] -ne $fb[$i]) { $same = $false } }
    if ($same) { "comfort logs: $($fa.Count) frame lines, all equal" } else { "comfort logs: $($fa.Count) / $($fb.Count) frame lines, they DIFFER" }
}
Gate "vr comfort: smooth motion leaves the simulation untouched - the 72 Hz race log's frame lines equal with it on and off (mock, 1/20)" {
    & $comfortLogs @("--vr-mock-hz", "72") @("--vr-mock-hz", "72", "--vr-smooth-motion", "0")
} "comfort logs: [1-9]\d\d frame lines, all equal"
Gate "vr comfort: the log comparison's control - 72 Hz against 90 Hz, the frame lines differ (mock, 1/20)" {
    & $comfortLogs @("--vr-mock-hz", "72") @("--vr-mock-hz", "90")
} "comfort logs: [1-9]\d\d / [1-9]\d\d frame lines, they DIFFER"

# The player's own bike in the head view (tools\rrgame\vr_bike_shake.h). The same 72 Hz run as the judder
# gates; the meter measures the drawn bike's vertices in the eye. The shake comes from the bike's orientation rows +0x1B0,
# whose length the game lets creep to 1.007 and snaps back every 3..7 steps (the drawn bike's SCALE pulsing ~0.6 %):
# the bar ends' second difference 0.24 deg RMS per frame. With [vr] bike_shake low (the default) the drawn rows are at
# unit length and the slots smoothed: ~0.06 deg; control --vr-bike-shake original: over 0.2 deg, the rows 0.99 .. 1.007.
# Off holds the body's pitch slot at rest. And the simulation is untouched (the race log's frame lines equal low
# against original). About 15 s each.
Gate "bike shake: the own bike's handlebars in the eye shake under 0.1 deg RMS a frame at 72 Hz, low (mock, 1/20)" {
    & $padGame $Disc @comfortJudder
} "(?s)\A(?=.*\n  bar ends drawn 0\.0\d+ / )(?=.*bike shake filter: 1200 head-view frame\(s\) \(\d+ without the step's part slots\), 1200 restore\(s\), 0 restore mismatch\(es\); the game's orientation rows' length 0\.99\d+ \.\. 1\.00[3-9])"
Gate "bike shake: the control - original, the handlebars shake over 0.2 deg RMS a frame with the game's rows (mock, 1/20)" {
    & $padGame $Disc @comfortJudder --vr-bike-shake original
} "\n  bar ends drawn 0\.[2-9]\d+ / "
Gate "bike shake: off holds the body's pitch slot at rest, the game's pitch spring moves it (mock, 1/20)" {
    & $padGame $Disc @comfortJudder --vr-bike-shake off
} "(?s)\A(?=.*\n  body +drawn [^\n]*slot 2 pitch 0\.0000 / 0\.0000)(?=.*the body's pitch slot - the game's RMS [1-9]\.\d+ deg \(max [1-9]\d*\.\d+\), drawn RMS 0\.000 deg)"
Gate "bike shake: the drawn bike leaves the simulation untouched - the 72 Hz race log equal with low and original (mock, 1/20)" {
    & $comfortLogs @("--vr-mock-hz", "72", "--vr-bike-shake", "low") @("--vr-mock-hz", "72", "--vr-bike-shake", "original")
} "comfort logs: [1-9]\d\d frame lines, all equal"

# The VR race stepped on the display's clock (tools\rrgame\vr_pacing.h). The Quest's loop is irregular
# (xrWaitFrame returns anywhere in the period: "interval avg 13.92 / max 20.55 ms"); stepped by the loop's wall clock the
# world judders (the headset: 49 mm, jerk 30417 m/s^3). The mock reproduces it with --vr-mock-timing "jitter 7" (the
# loop's clock up to 7 ms late, the display regular): on the display's clock the world 5 m ahead stays at the regular
# clock's 7 mm and the drawn game time follows the display time exactly; control RRJB_VR_PACING=wallclock (the loop's
# wall-clock step): over 30 mm. Missed displays, a 72 -> 90 Hz switch and a 2 s hold ("miss 97; switch 600 90; hold 800 2000"):
# still under 10 mm, the drawn time exact, the hold not stepped (no burst); control: over 30 mm. And the display clock is
# the wall-clock step when the loop is regular: the race log with the jitter equals the regular one (control: the wall
# clock's differs). About 15 s each.
$pacingMix = "jitter 7; miss 97; switch 600 90; hold 800 2000"
Gate "vr pacing: an irregular loop (jitter 7 ms) - the display's clock keeps the world under 10 mm RMS, the drawn time exact (mock, 1/20)" {
    & $padGame $Disc --vr-mock --race 1 20 --frames 1200 --hold T --autosteer --no-sfx --vr-mock-timing "jitter 7"
} "(?s)\A(?=.*the world 5 m ahead in the eye: second difference RMS [0-9]\.\d+ mm)(?=.*vr pacing: the race stepped on the display's clock [^\n]*the loop's interval avg 13\.\d+ / max (?:1[89]|2[01])\.\d+ ms[^\n]*error RMS 0\.000 ms, max 0\.000 ms \(1\d{3}\))"
Gate "vr pacing: the control - the loop's wall clock (RRJB_VR_PACING=wallclock) judders over 30 mm RMS with the same jitter (mock, 1/20)" {
    $env:RRJB_VR_PACING = "wallclock"
    try { & $padGame $Disc --vr-mock --race 1 20 --frames 1200 --hold T --autosteer --no-sfx --vr-mock-timing "jitter 7" }
    finally { Remove-Item Env:RRJB_VR_PACING }
} "(?s)\A(?=.*the world 5 m ahead in the eye: second difference RMS (?:[3-9]\d|\d{3,})\.\d+ mm)(?=.*the race stepped on the loop's wall clock [^\n]*error RMS [1-9]\.\d+ ms)"
Gate "vr pacing: missed displays, a 72 -> 90 Hz switch and a 2 s hold - under 10 mm, the drawn time exact, the hold not stepped (mock, 1/20)" {
    & $padGame $Disc --vr-mock --race 1 20 --frames 1200 --hold T --autosteer --no-sfx --vr-mock-timing $pacingMix
} "(?s)\A(?=.*the world 5 m ahead in the eye: second difference RMS [0-9]\.\d+ mm)(?=.*the race stepped on the display's clock [^\n]*missed displays 12 \(on 12 frame\(s\)\), refresh changes 1 \(72\.0 -> 90\.0 Hz at frame \d+\)[^\n]*resets 1, stalls not stepped 1 \(6\d\d\.\d ticks dropped\); [^\n]*error RMS 0\.000 ms, max 0\.000 ms)"
Gate "vr pacing: the control - the same missed displays, switch and hold on the wall clock judder over 30 mm RMS (mock, 1/20)" {
    $env:RRJB_VR_PACING = "wallclock"
    try { & $padGame $Disc --vr-mock --race 1 20 --frames 1200 --hold T --autosteer --no-sfx --vr-mock-timing $pacingMix }
    finally { Remove-Item Env:RRJB_VR_PACING }
} "the world 5 m ahead in the eye: second difference RMS (?:[3-9]\d|\d{3,})\.\d+ mm"
Gate "vr pacing: on a regular loop the display's clock is the wall-clock step - the race log with the jitter equals the regular one (mock, 1/20)" {
    & $comfortLogs @("--vr-mock-hz", "72") @("--vr-mock-timing", "jitter 7")
} "comfort logs: [1-9]\d\d frame lines, all equal"
Gate "vr pacing: the control - on the wall clock the jitter changes the race log (mock, 1/20)" {
    $env:RRJB_VR_PACING = "wallclock"
    try { & $comfortLogs @("--vr-mock-hz", "72") @("--vr-mock-timing", "jitter 7") }
    finally { Remove-Item Env:RRJB_VR_PACING }
} "comfort logs: [1-9]\d\d / [1-9]\d\d frame lines, they DIFFER"

# Rider placement (src\game\rider_pose.h RiderOwnFrame / MachineClimbSlots, tools\rrgame\race_render.cpp): a rider off his
# bike is drawn where the game has him. ModelVisible RASHCDG 0x80067AC4 hangs only a +0x48 == 1 rider on the seat; a
# thrown one (+0x48 = 3) is an object of its own at +0xB8 in its rows +0x1B0. Wherever the PORTED model draw's capture
# is not drawn (VR eyes, maximum detail, the head view, an object out of the console camera's view) the renderer
# places the rider from these. Race 1/11 with the autosteer (into a truck at 1235,
# thrown ~94 units, walks back, the climb, riding): the player's posed rider against the model draw's own vertices of him
# - off the bike ~0.03 world units RMS (seated: up to 94 units off), the climb ~0.13 (on the unturned bike
# ~0.8), riding unchanged. Control RRJB_RIDER_PLACE=seat (every rider on the seat). Rivals: the sparring partners knocked
# off on 1/20 are drawn off their bikes, some frames for their rider alone (the bike out of the view). About 40 s each.
$fallRun = @("--race", "1", "11", "--frames", "2620", "--hold", "T", "--autosteer", "--no-sfx")
$sparRun = @("--race", "1", "20", "--frames", "1800", "--hold", "T", "--weapon", "7:15", "--cheat-sparring", "passive",
             "--opponent-health", "1", "--punch", "30", "--punch-from", "800", "--punch-action", "6", "--no-sfx")
Gate "rider placement: the thrown player's rider is drawn where the game has him, against the PORTED model draw's vertices (1/11)" {
    & $padGame $Disc @fallRun
} "rider placement: the player's rider drawn off the bike 1\d{3} frame\(s\) in 1 stretch\(es\), the longest 1\d{3} \(up to [5-9]\d\.\d+ world units from the seat\), on the bike in the climb [1-9]\d+;[^\n]* - riding: [1-9]\d+ frame\(s\), RMS mean 0\.0\d+ max 0\.[01]\d+ [^\n]*; off the bike: 1\d{3} frame\(s\), RMS mean 0\.0\d+ max 0\.0\d+ \(pre-1cp placement [1-9]\d\.\d+ / [5-9]\d\.\d+\); climb: [1-9]\d+ frame\(s\), RMS mean 0\.[01]\d+ max 0\.[0-3]\d+ \(pre-1cp placement 0\.[5-9]\d+ / [1-9]\.\d+\)"
Gate "rider placement: the control - RRJB_RIDER_PLACE=seat, the rider on the seat is up to 90+ units from the model draw's (1/11)" {
    $env:RRJB_RIDER_PLACE = "seat"
    try { & $padGame $Disc @fallRun } finally { Remove-Item Env:\RRJB_RIDER_PLACE }
} "rider placement SEAT ONLY [^\n]*drawn off the bike 0 frame\(s\)[^\n]* - riding: [1-9]\d+ frame\(s\), RMS mean [1-9]\d*\.\d+ max (?:[5-9]\d|\d{3,})\.\d+"
Gate "rider placement: the rivals' thrown riders are drawn off their bikes, also when only the rider is in the view (1/20, sparring)" {
    & $padGame $Disc @sparRun
} "rivals' riders off the bike [1-9]\d* rider-frame\(s\) \([1-9]\d* drawn for their rider alone\)"
Gate "rider placement: the rivals' control - RRJB_RIDER_PLACE=seat, none drawn off the bike (1/20, sparring)" {
    $env:RRJB_RIDER_PLACE = "seat"
    try { & $padGame $Disc @sparRun } finally { Remove-Item Env:\RRJB_RIDER_PLACE }
} "rivals' riders off the bike 0 rider-frame\(s\) \(0 drawn for their rider alone\)"

# Triangle off the bike (the original's: the pad reader SLUS 0x8001D6AC sets rider +0x228 0x1000 on slot 6's
# press, RiderRecover RASHCDG 0x80092E04 then puts the bike back on the road beside the standing rider - no walk back).
# The binding "Back to the bike" (keyboard Space / E, the pad's Triangle, VR A) presses it; --to-bike N:F scripts it.
# 1/11: off the bike ~390 frames with it against ~1200 walking back (the first gate above is its control). On the bike
# slot 6 is read by nothing: the race log's first 1200 frames (before the fall) are equal with the presses and without;
# the comparison's control: over the fall they differ. VR: the Touch A does the same (the mock: --vr-holster-script's scripted A,
# which runs over --vr-melee-script's hands). About 40 s each.
Gate "to the bike: Triangle off the bike brings the bike back - off it ~390 frames instead of ~1200 (1/11)" {
    & $padGame $Disc @fallRun --to-bike 20:0
} "the player's rider drawn off the bike [2-5]\d\d frame\(s\) in 1 stretch\(es\)[^\n]*on the bike in the climb [1-9]\d+"
$toBikeLogs = {
    param([string]$frames)
    $dir = Join-Path $root "work\gate_tobike\$gateTag"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $la = Join-Path $dir "a.log"
    $lb = Join-Path $dir "b.log"
    & $padGame $Disc --race 1 11 --frames $frames --hold T --autosteer --no-sfx --log $la | Out-Null
    & $padGame $Disc --race 1 11 --frames $frames --hold T --autosteer --no-sfx --log $lb --to-bike 20:0 | Out-Null
    $fa = @(Get-Content $la | Where-Object { $_ -match '^f\d+ ' })
    $fb = @(Get-Content $lb | Where-Object { $_ -match '^f\d+ ' })
    $same = $fa.Count -eq $fb.Count
    for ($i = 0; $same -and $i -lt $fa.Count; $i++) { if ($fa[$i] -ne $fb[$i]) { $same = $false } }
    if ($same) { "to-bike logs: $($fa.Count) frame lines, all equal" } else { "to-bike logs: $($fa.Count) / $($fb.Count) frame lines, they DIFFER" }
}
Gate "to the bike: on the bike Triangle changes nothing - the race log before the fall is equal with the presses (1/11)" {
    & $toBikeLogs "1200"
} "to-bike logs: 1\d{3} frame lines, all equal"
Gate "to the bike: the log comparison's control - over the fall the presses change the race (1/11)" {
    & $toBikeLogs "1700"
} "to-bike logs: 1\d{3} / 1\d{3} frame lines, they DIFFER"
Gate "to the bike: VR - the Touch A off the bike brings the bike back (mock, 1/11)" {
    & $padGame $Disc --vr-mock @fallRun --vr-melee-script "100 grip right 0" --vr-holster-script ((1320..2080 | Where-Object { $_ % 40 -eq 0 } | ForEach-Object { "$_ button A 5" }) -join "; ")
} "the player's rider drawn off the bike [2-6]\d\d frame\(s\) in 1 stretch\(es\)"

# The head view's horizon (tools\rrgame\vr_horizon.h, vr_comfort.cpp) and the riders near it. (1) On 1/23's
# steep downhill the game's own pitch move (the stoppie) fires every ~1.05 s - a 9 deg nod of the whole
# view with the eye on the bike; [vr] view_pitch low (the default) draws the bike with the road's grade low-passed and a
# quarter of its own pitch, so the view's pitch high-passed (0.5 Hz) stays under 3 deg RMS and its change a frame under
# 0.5 deg (original, the control: 4.6 deg RMS, 27 deg, 1.75 deg a frame); road likewise. (2) The stoppie's start also
# hitches the bike along the road by 13..20 cm in one step: the stabiliser's leash along the bike's forward is 20 cm
# (RRJB_SMOOTH_ALONG=0.06, 6 cm: the control) - the eye's fore-aft second difference's worst frames halve.
# (3) The held hands stay on the grips of the bike drawn with the planned pitch (the 1/20 slalom on the display's
# clock; control original: nothing planned). (4) The rival bikes near the player go through the player's g-h filter
# (RivalSmooth): the sparring partner beside the player on 1/20, its drawn motion's second difference in the world under
# 5 m (control RRJB_RIVAL_SMOOTH=off with the 6 cm leash). (5) [vr] seat_back_cm 30: the eye 0.30 back on the bike, the
# hands still on the grips, the drawn bike no nearer than 0.6 m to the eye (control 0 cm: ~0.52 m). (6) The VR menu:
# Seat forward / back (main row 7) and Graphics -> Bike pitch (first person) change with the triggers. About 30 s each.
$horizonRun = @("--vr-mock", "--race", "1", "23", "--frames", "2800", "--hold", "T", "--autosteer", "--no-sfx", "--handling-vr", "modern",
                "--vr-mock-timing", "jitter 7")
Gate "vr horizon: 1/23's stoppies no longer nod the view - low draws the road's grade and a quarter of the nod (mock, jitter 7)" {
    & $padGame $Disc @horizonRun
} "view pitch: 2800 head-view frame\(s\), 2800 drawn with the planned pitch[^\n]*own pitch moves \([^)]*\) [5-9],[^\n]*high-passed \(0\.5 Hz\) RMS [0-2]\.\d+ deg max [0-7]\.\d+,[^\n]*the view RMS 0\.0\d+ max 0\.[0-4]\d deg"
Gate "vr horizon: the control - view_pitch original, the view nods with every stoppie (mock, jitter 7)" {
    & $padGame $Disc @horizonRun --vr-view-pitch original
} "view pitch: 2800 head-view frame\(s\), 0 drawn with the planned pitch[^\n]*high-passed \(0\.5 Hz\) RMS [3-9]\.\d+ deg max [1-9]\d\.\d+,[^\n]*the view RMS 0\.[1-9]\d+ max [1-9]\.\d+ deg"
Gate "vr horizon: view_pitch road - the grade only, no nod at all (mock, jitter 7)" {
    & $padGame $Disc @horizonRun --vr-view-pitch road
} "view pitch: 2800 head-view frame\(s\), 2800 drawn with the planned pitch[^\n]*high-passed \(0\.5 Hz\) RMS [0-2]\.\d+ deg max [0-6]\.\d+,[^\n]*the view RMS 0\.0\d+ max 0\.[0-4]\d deg"
Gate "vr horizon: the stoppies' hitch along the road spread by the 20 cm leash - the eye's fore-aft worst frames (mock, jitter 7)" {
    & $padGame $Disc @horizonRun
} "the eye's fore-aft second difference RMS \d+\.\d+ mm, p99 [0-7]\.\d+, max (?:[0-9]|[1-9]\d|[12]\d\d)\.\d \("
Gate "vr horizon: the leash's control - RRJB_SMOOTH_ALONG=0.06, the eye lurches with the hitch (mock, jitter 7)" {
    $env:RRJB_SMOOTH_ALONG = "0.06"
    try { & $padGame $Disc @horizonRun } finally { Remove-Item Env:\RRJB_SMOOTH_ALONG }
} "the eye's fore-aft second difference RMS \d+\.\d+ mm, p99 (?:[8-9]|[1-9]\d)\.\d+, max (?:[3-9]\d\d|\d{4,})\.\d \("
$horizonHands = @("--vr-mock", "--vr-steering", "handlebars", "--race", "1", "20", "--frames", "700", "--no-sfx", "--hold", "T",
                  "--vr-horizon-lock", "100", "--vr-mock-timing", "jitter 7",
                  "--vr-bars-script", ("20 grab both; 20 twist 30 30; 200 turn -30 15; 260 turn 30 25; 330 turn -30 25; " +
                                       "400 turn 30 25; 470 turn -30 25; 540 turn 30 25; 610 turn 0 20"))
Gate "vr horizon: the held hands stay on the grips of the bike drawn with the planned pitch (mock, 1/20 slalom, jitter 7)" {
    & $padGame $Disc @horizonHands
} "(?s)\A(?=.*vr hands on the bars: 1[0-9]{3} held hand-frame\(s\) over 700 drawn frame\(s\) - the fist's centre to the drawn grip max 0\.\d+ mm [^\n]*read max 0, part slots differing on 0 frame\(s\))(?=.*view pitch: 700 head-view frame\(s\), 700 drawn with the planned pitch[^\n]*the drawn pitch turned up to [1-9]\d\.\d+ deg)"
Gate "vr horizon: the hands' control - view_pitch original, nothing planned (mock, 1/20 slalom, jitter 7)" {
    & $padGame $Disc @horizonHands --vr-view-pitch original
} "view pitch: 700 head-view frame\(s\), 0 drawn with the planned pitch"
$horizonSpar = @("--vr-mock", "--race", "1", "20", "--frames", "1800", "--hold", "T", "--autosteer", "--no-sfx", "--handling-vr", "modern",
                 "--vr-mock-timing", "jitter 7", "--cheat-sparring", "passive")
Gate "vr horizon: the sparring partner beside the player drawn through the player's filter - its world jitter under 5 m (mock, 1/20)" {
    & $padGame $Disc @horizonSpar
} "(?s)\A(?=.*near riders: [^\n]*under 5 m - [1-9]\d+ sample\(s\)[^;]*in the world RMS \d+\.\d+ \(median [0-1]\.\d+, p90 [0-5]\.\d+\);)(?=.*rival smoothing: 1800 frame\(s\), [1-9]\d+ rival bike frame\(s\) drawn)"
Gate "vr horizon: the rivals' control - RRJB_RIVAL_SMOOTH=off and the 6 cm leash, the partner's world jitter (mock, 1/20)" {
    $env:RRJB_RIVAL_SMOOTH = "off"; $env:RRJB_SMOOTH_ALONG = "0.06"
    try { & $padGame $Disc @horizonSpar } finally { Remove-Item Env:\RRJB_RIVAL_SMOOTH; Remove-Item Env:\RRJB_SMOOTH_ALONG }
} "near riders: [^\n]*under 5 m - [1-9]\d+ sample\(s\)[^;]*in the world RMS \d+\.\d+ \(median [2-9]\.\d+, p90 (?:[7-9]|[1-9]\d)\.\d+\);"
Gate "vr seat: seat_back_cm 30 - the eye 0.30 back on the bike, the hands on the grips, the bike no nearer than 0.6 m (mock, 1/20 slalom)" {
    & $padGame $Disc @horizonHands --vr-seat-back 30
} "(?s)\A(?=.*the seat moved 0\.300 world units back along the bike: the eye at right [^\n]*fwd -0\.3\d\d)(?=.*vr hands on the bars: 1[0-9]{3} held hand-frame\(s\)[^\n]*the fist's centre to the drawn grip max 0\.\d+ mm)(?=.*the drawn bike's nearest vertex to the eye [6-9]\d\d mm)"
Gate "vr seat: the control - seat_back_cm 0, the drawn bike's nearest vertex ~0.52 m from the eye (mock, 1/20 slalom)" {
    & $padGame $Disc @horizonHands
} "(?s)\A(?!.*the seat moved)(?=.*the drawn bike's nearest vertex to the eye [3-5]\d\d mm)"
# The view pitch and the wheelie: ViewPitch filters only the game's own pitch; the part of it a HELD wheelie
# covers (the start's pop under the lift at frame 150, --hold T) is left to the wheelie layer, which draws its whole held
# pitch on the bike, the grips and the hands, the view keeping 30 % of it. Control RRJB_VIEW_PITCH_WHEELIE=filter (the
# covered part quartered with the rest): the held wheelie drawn down to a few degrees.
$horizonWheelie = @("--vr-mock", "--vr-steering", "handlebars", "--race", "1", "20", "--frames", "420", "--no-sfx", "--hold", "T",
                    "--handling", "modern", "--vr-mock-timing", "jitter 7", "--vr-bars-script", "20 grab both; 20 twist 30 30; 150 lift 0.32 0 12")
Gate "vr horizon + wheelie: a wheelie held over the game's pop is drawn whole (~35 deg), the view keeps ~30 %, the hands on the grips (mock, 1/20)" {
    & $padGame $Disc @horizonWheelie
} "(?s)\A(?=.*left to the wheelie layer on [1-9]\d* frame\(s\), up to [1-4]\d\.\d deg; the held wheelie \(30 deg or more\) [1-9]\d* frame\(s\): the drawn bike over the grade 3\d\.\d \.\. [34]\d\.\d deg, the view over the grade [5-9]\.\d \.\. 1[0-5]\.\d deg)(?=.*vr hands on the bars: [^\n]*the fist's centre to the drawn grip max 0\.\d+ mm)"
Gate "vr horizon + wheelie: the control - RRJB_VIEW_PITCH_WHEELIE=filter, the held wheelie quartered with the pop (mock, 1/20)" {
    $env:RRJB_VIEW_PITCH_WHEELIE = "filter"
    try { & $padGame $Disc @horizonWheelie } finally { Remove-Item Env:\RRJB_VIEW_PITCH_WHEELIE }
} "left to the wheelie layer on 0 frame\(s\)[^\n]*the held wheelie \(30 deg or more\) [1-9]\d* frame\(s\): the drawn bike over the grade [0-9]\.\d \.\."
$horizonMenu = @("--vr-mock", "--race", "1", "20", "--hold", "T", "--frames", "420", "--no-sfx")
Gate "vr menu: Riding position -> Seat forward / back and Bike pitch change with the right trigger (mock, 1/20)" {
    & $padGame $Disc @horizonMenu --vr-mock-pad (Weap2Walk 6 @("a", "down", "rtrigger"))
    & $padGame $Disc @horizonMenu --vr-mock-pad (Weap2Walk 6 (@("a") + @("down") * 5 + @("rtrigger")))
} "(?s)\A(?=.*vr menu: 9/Seat forward / back: 0 cm -> 9/Seat forward / back: 5 cm back \(by a trigger\))(?=.*vr menu: 9/Bike pitch \(first person\): Low -> 9/Bike pitch \(first person\): Original \(by a trigger\))"
Gate "vr menu: the control - the left trigger moves the seat forward and the bike pitch to Road only (mock, 1/20)" {
    & $padGame $Disc @horizonMenu --vr-mock-pad (Weap2Walk 6 @("a", "down", "ltrigger"))
    & $padGame $Disc @horizonMenu --vr-mock-pad (Weap2Walk 6 (@("a") + @("down") * 5 + @("ltrigger")))
} "(?s)\A(?=.*vr menu: 9/Seat forward / back: 0 cm -> 9/Seat forward / back: 5 cm forward \(by a trigger\))(?=.*vr menu: 9/Bike pitch \(first person\): Low -> 9/Bike pitch \(first person\): Road only \(by a trigger\))(?!.*5 cm back)"

# The distant traffic cars' brightness (tools\rrgame\carflick_probe.cpp; shaders.cpp vTexelPC / uSmoothWiden).
# The meter draws eye 0 again with the projection half a pixel off and compares each car's mean colour between the two
# (a sub-pixel move, as the car's own motion or the head makes it, and as the two eyes differ). Unfixed, a car 100..200
# units away changes by 6.9 of 255 on average (the "blinking"): with MSAA a partly covered pixel takes its texel outside
# the primitive (a neighbouring image of the atlas), and the trilinear footprint leaves the pixel frequency in. The
# texel is looked up at the centroid and an object's minified footprint widened by up to half a mip level: 3.96.
# Control RRJB_CENTROID=off + RRJB_SMOOTH_WIDEN=off: 6.92. About 20 s each (1/1, traffic from frame 667).
function CarFlick([string]$tag, [hashtable]$extra) {
    $dir = Join-Path $root "work\gate_carflick_$gateTag"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $csv = Join-Path $dir "$tag.csv"
    $env:RRJB_CARFLICK = $csv; $env:RRJB_CARFLICK_MSAA = "2"; $env:RRJB_CARFLICK_SHIFT = "0.5"
    foreach ($k in $extra.Keys) { Set-Item "env:$k" $extra[$k] }
    try { & $padGame $Disc --vr-mock --vr-multiview 0 --race 1 1 --hold T --autosteer --frames 900 --no-sfx | Out-Null }
    finally {
        Remove-Item Env:\RRJB_CARFLICK, Env:\RRJB_CARFLICK_MSAA, Env:\RRJB_CARFLICK_SHIFT
        foreach ($k in $extra.Keys) { Remove-Item "Env:\$k" }
    }
    $rows = @(Import-Csv $csv | Where-Object { [double]$_.dist -ge 100 -and [double]$_.dist -lt 200 -and [int]$_.pixels -ge 12 -and [int]$_.pixels2 -ge 12 })
    $sum = 0.0
    foreach ($r in $rows) { $sum += [math]::Abs([double]$r.luma - [double]$r.luma2) }
    $mean = if ($rows.Count) { $sum / $rows.Count } else { 99.0 }
    "carflick: $($rows.Count) car-frame(s) at 100..200 units, the mean brightness change under a half-pixel move " + $mean.ToString("F2", [Globalization.CultureInfo]::InvariantCulture)
}
Gate "distant cars: a traffic car 100..200 units away changes its mean brightness under 5 of 255 under a half-pixel move (mock, 1/1)" {
    CarFlick "after" @{}
} "carflick: [1-9]\d+ car-frame\(s\) at 100\.\.200 units, the mean brightness change under a half-pixel move [0-4]\.\d\d\r?$"
Gate "distant cars: the control - RRJB_CENTROID=off, RRJB_SMOOTH_WIDEN=off, the change over 6 (mock, 1/1)" {
    CarFlick "control" @{ RRJB_CENTROID = "off"; RRJB_SMOOTH_WIDEN = "off" }
} "carflick: [1-9]\d+ car-frame\(s\) at 100\.\.200 units, the mean brightness change under a half-pixel move (?:[6-9]|\d{2,})\.\d\d\r?$"

if (-not $Quick) {
    # The end-to-end one: the PRODUCT, not a harness. Two scripted runs of the same race, one with the
    # throttle held and one without, and the ported engine's own field (+0x1E0, read back out of the
    # entity) must differ. This is the only gate that exercises the whole chain at once - disc, world
    # assembly, the ported race spine, the ported engine, the renderer - and it is the one that would
    # catch "the game still builds but nothing actually drives".
    $game = Join-Path $root "$Build\rrgame.exe"
    if (Test-Path $game) {
        Gate "the game: holding the throttle moves the ported engine" {
            $dir = Join-Path $root "work\gate_game\$gateTag"
            New-Item -ItemType Directory -Force $dir | Out-Null
            & $game $Disc --race 1 20 --frames 450 --log (Join-Path $dir "idle.txt") | Out-Null
            & $game $Disc --race 1 20 --frames 450 --hold T --log (Join-Path $dir "throttle.txt") | Out-Null
            $idle = (Get-Content (Join-Path $dir "idle.txt") | Select-String "^f450 ").Line
            $open = (Get-Content (Join-Path $dir "throttle.txt") | Select-String "^f450 ").Line
            # Idle may be exactly 0: with the whole ported step running, a stopped bike stays stopped
            # (region E4 sets flagsB bit 13). Both lines must still parse, so a missing
            # log cannot pass as "idle 0".
            $a = -1; $b = -1
            if ($idle -match "spd=(\d+)") { $a = [int64]$Matches[1] }
            if ($open -match "spd=(\d+)") { $b = [int64]$Matches[1] }
            Write-Output "idle speed $a, throttle speed $b"
            if ($a -ge 0 -and $b -gt $a) { Write-Output "game verdict PASS" } else { Write-Output "game verdict FAIL" }
        } "game verdict PASS"

        # Every race of both sets, through the PRODUCT, for N frames with the throttle held and the
        # autosteer on: every run must exit 0 AND print its own "frames run N," line. The ids come
        # from the race graph (`rrtool races`) - set 2's are not 1..36. This is the check that the
        # ported code does not assume one race's data layout: a read that indexes by race data
        # (grid records, level records, model tables, the arena's pointers) goes wrong on some
        # races and not on others, and the single-race gates above cannot see that. Runs in
        # parallel; each run's output is kept in work\gate_sweep\<build>\r_<set>_<id>.txt.
        # A run that never returns is a failure too, named with the last frame it finished: each run
        # writes `--heartbeat hb_<set>_<id>.txt` ("frame <n>" after every frame, flushed), and a run
        # still going after $timeoutSec is stopped - by PID, the rrgame child of its own cmd.exe - and
        # reported as HANG. N is long enough to reach the falls, the walk back and the re-seat on most
        # races. A hidden run
        # goes at about 60 frames/s plus about 30 s of loading: 7200 frames take 1281 s for the 100
        # races on 12 slots, so 6600 keeps the gate near 20 min; 400 s is over twice one run.
        Gate "the game: every race of both sets runs N frames without a fault or a hang" {
            $frames = 6600
            $timeoutSec = 400
            $dir = Join-Path $root "work\gate_sweep\$gateTag"
            New-Item -ItemType Directory -Force $dir | Out-Null
            $races = @()
            foreach ($line in (& $tool races $Disc)) {
                if ($line -match "^set (\d+):((?: \d+)+)\s*$") {
                    $set = [int]$Matches[1]
                    foreach ($id in ($Matches[2].Trim() -split " ")) { $races += ,@($set, [int]$id) }
                }
            }
            $slots = [Math]::Max(1, [Math]::Min(12, [Environment]::ProcessorCount - 2))
            $running = @()
            $done = @()
            $queue = [System.Collections.Queue]::new()
            foreach ($r in $races) { $queue.Enqueue($r) }
            while ($queue.Count -gt 0 -or $running.Count -gt 0) {
                while ($queue.Count -gt 0 -and $running.Count -lt $slots) {
                    $r = $queue.Dequeue()
                    $log = Join-Path $dir ("r_{0}_{1}.txt" -f $r[0], $r[1])
                    $hb = Join-Path $dir ("hb_{0}_{1}.txt" -f $r[0], $r[1])
                    Remove-Item -Force $log, $hb -ErrorAction SilentlyContinue
                    $psi = New-Object System.Diagnostics.ProcessStartInfo
                    $psi.FileName = "cmd.exe"
                    $psi.Arguments = "/c `"`"$game`" `"$Disc`" --race $($r[0]) $($r[1]) --frames $frames --hold T --autosteer --no-sfx --heartbeat `"$hb`" > `"$log`" 2>&1`""
                    $psi.UseShellExecute = $false
                    $psi.CreateNoWindow = $true
                    $running += [pscustomobject]@{ Set = $r[0]; Id = $r[1]; Log = $log; Hb = $hb; Hung = $false
                                                   Start = [DateTime]::Now; P = [System.Diagnostics.Process]::Start($psi) }
                }
                Start-Sleep -Milliseconds 200
                $still = @()
                foreach ($j in $running) {
                    if ($j.P.HasExited) { $done += $j; continue }
                    if (([DateTime]::Now - $j.Start).TotalSeconds -gt $timeoutSec) {
                        # Only this run's own processes, by PID: the rrgame child of this cmd.exe, then the cmd.
                        $kids = Get-CimInstance Win32_Process -Filter "ParentProcessId = $($j.P.Id)" -ErrorAction SilentlyContinue
                        foreach ($k in $kids) { Stop-Process -Id $k.ProcessId -Force -ErrorAction SilentlyContinue }
                        Stop-Process -Id $j.P.Id -Force -ErrorAction SilentlyContinue
                        $j.P.WaitForExit(10000) | Out-Null
                        $j.Hung = $true
                        $done += $j
                        continue
                    }
                    $still += $j
                }
                $running = $still
            }
            $ok = 0
            foreach ($j in ($done | Sort-Object Set, Id)) {
                $last = "none"
                if (Test-Path $j.Hb) { $t = Get-Content $j.Hb -Tail 1; if ($t) { $last = $t } }
                if ($j.Hung) {
                    Write-Output ("  set {0} race {1}: HANG - no exit after {2} s, last heartbeat '{3}'" -f $j.Set, $j.Id, $timeoutSec, $last)
                    continue
                }
                $code = $j.P.ExitCode
                $ran = (Test-Path $j.Log) -and (Select-String -Path $j.Log -Pattern "^frames run $frames," -Quiet)
                if ($code -eq 0 -and $ran) { $ok++ }
                else { Write-Output ("  set {0} race {1}: exit 0x{2:X8}, frames line {3}, last heartbeat '{4}'" -f $j.Set, $j.Id, $code, $ran, $last) }
            }
            if ($races.Count -eq 0) { Write-Output "race sweep: no races enumerated" }
            else { Write-Output "race sweep: $ok of $($races.Count) races ran $frames frames" }
        } "race sweep: (\d+) of \1 races ran \d+ frames"
    }

    # Every route in the game, not just the one the quick gate checks. Minutes: 100 races, each
    # streaming about 1500 chunks off the disc image.
    Gate "race world: all 100 routes assemble, no jumps, lengths agree with the race graph" {
        & $tool raceworldall $Disc
    } "race world verdict PASS"

    # The same claim from the other side, in Python and without any of the C++: the arms of each
    # junction form ONE connected network, and every pair of roads meeting there is linked by a
    # chain of arms. Both implementations put the largest join at the same 42 world units.
    foreach ($roadSet in 1, 2) {
        Gate "probe: junction arms connect, set $roadSet (independent implementation)" {
            python (Join-Path $root "tools\scout\junction.py") verify --set $roadSet
        } "VERIFY OK"
    }

    # Three transcriptions checked against the original running in the interpreter, each with its
    # negative control: a probe that cannot fail proves nothing. The ground probe applies all of its
    # mutations in one run and must fail on exactly the eight checks that guard them; the other three
    # mutate one claim at a time and must detect every one.
    Gate "ground query: the transcription against 1270 live calls" {
        python (Join-Path $root "tools\scout\ground.py") verify
    } "ground: \d+ checks, 0 failures"
    Gate "ground query: the probe fails exactly where it was mutated" {
        python (Join-Path $root "tools\scout\ground.py") verify --mutate
    } "ground: \d+ checks, 8 failures"
    Gate "road query: six functions against the original, store for store" {
        python (Join-Path $root "tools\scout\roadq.py") verify
    } "roadq: 6 functions, \d+ invocations, 0 mismatches, 0 PyCpu/oracle differences, 0 static failures"
    Gate "road query: every planted mutant is caught" {
        python (Join-Path $root "tools\scout\roadq.py") verify --mutate
    } "roadq --mutate: (\d+) of \1 mutants caught"
    Gate "contact pairs: the collision pass and its resolvers against live contacts" {
        python (Join-Path $root "tools\scout\pairs.py") verify
    } "pairs: \d+ checks, 0 failures"
    Gate "contact pairs: the probe fails exactly where it was mutated" {
        python (Join-Path $root "tools\scout\pairs.py") verify --mutate
    } "pairs: \d+ checks, 11 failures"
    Gate "impact: the outcome solver, the rider layer and combat against 52 live runs" {
        python (Join-Path $root "tools\scout\impact.py") verify
    } "impact: \d+ checks, 0 failures"
    Gate "impact: the probe fails exactly where it was mutated" {
        python (Join-Path $root "tools\scout\impact.py") verify --mutate
    } "impact: \d+ checks, 39 failures"
    Gate "rider animation: the machine, the clip decoder and the pose, against the original" {
        python (Join-Path $root "tools\scout\anim.py") verify
    } "anim: \d+ checks, 0 failures"
    Gate "rider animation: the probe fails exactly where it was mutated" {
        python (Join-Path $root "tools\scout\anim.py") verify --mutate
    } "anim: \d+ checks, 54 failures"
    Gate "crash chain: the impact state pass, the bike-bike gate and combat, against 80 live runs" {
        python (Join-Path $root "tools\scout\crash.py") verify
    } "crash: \d+ checks, 0 failures"
    Gate "crash chain: the probe fails exactly where it was mutated" {
        python (Join-Path $root "tools\scout\crash.py") verify --mutate
    } "crash: \d+ checks, 42 failures"
    Gate "HUD: HudFrame's element choice, values and packets against the original" {
        python (Join-Path $root "tools\scout\hud.py") verify
    } "hud: \d+ checks, 0 failures"
    Gate "HUD: every mutated claim is caught by its own check" {
        python (Join-Path $root "tools\scout\hud.py") verify --mutate
    } "hud --mutate: (\d+) of \1 mutants caught by their own claim, 0 missed"
    Gate "road runtime: 24 functions under the road query, store for store" {
        python (Join-Path $root "tools\scout\roadrt.py") verify
    } "roadrt: 24 functions, \d+ invocations, 0 mismatches, 0 PyCpu/oracle differences, rsqrt 0 entry differences, 0 static failures, 0 unrun -> PASS"
    Gate "road runtime: every planted mutant is caught" {
        python (Join-Path $root "tools\scout\roadrt.py") verify --mutate
    } "roadrt --mutate: (\d+) of \1 mutants caught"
    Gate "road re-bind: 14 functions under 0x800374D4, store for store" {
        python (Join-Path $root "tools\scout\roadrb.py") verify
    } "roadrb: 14 functions, \d+ invocations, 0 mismatches, 0 PyCpu/oracle differences, 0 static failures, 0 unrun -> PASS"
    Gate "road re-bind: every planted mutant is caught" {
        python (Join-Path $root "tools\scout\roadrb.py") verify --mutate
    } "roadrb --mutate: (\d+) of \1 mutants caught"
    # spawn.py rebuilds its traces in work\spawn\probe on every run; two runs at once corrupt them,
    # which is why these two stay sequential.
    Gate "population: activation window, spawner and the Rand order, against the original" {
        python (Join-Path $root "tools\scout\spawn.py") verify
    } "spawn: \d+ checks, 0 failures"
    Gate "population: every mutated claim is detected" {
        python (Join-Path $root "tools\scout\spawn.py") verify --mutate
    } "spawn: (\d+) mutations, \1 detected"
    Gate "bike step: every mutated claim is detected" {
        python (Join-Path $root "tools\scout\bike_step.py") verify --mutate
    } "bike_step: (\d+) mutations, \1 detected"
    Gate "sound: every mutated engine-note claim is detected" {
        python (Join-Path $root "tools\scout\engine_note.py") verify --mutate
    } "engine_note: \d+ mutations, 0 undetected"
    # The SPU control registers cut out of the savestates for the bench's register file
    # checked against PMON/EON/SPUCNT facts from RAM; the shifted
    # read must fail.
    Gate "sound: the savestates' SPU control registers, cross-checked" {
        python (Join-Path $root "tools\scout\spu_regs.py") verify
    } "spu_regs: 4 state\(s\), 0 failure\(s\)"
    Gate "sound: a mis-read SPU register block is detected" {
        python (Join-Path $root "tools\scout\spu_regs.py") verify --mutate
    } "spu_regs: 4 state\(s\), [1-9]\d* failure\(s\)"
    # The race camera: the fifteen-function tree, CAMERA.CA and 17 traced
    # runs of data-edited snapshot copies. camera.py rebuilds its traces in work\camera\probe on every
    # run, so these two stay sequential; each mutation must fail the check that guards its own claim.
    Gate "camera: the view tree, CAMERA.CA and the traced runs, against the original" {
        python (Join-Path $root "tools\scout\camera.py") verify
    } "camera: \d+ checks, 0 failures"
    Gate "camera: every mutated claim is caught by its own check" {
        python (Join-Path $root "tools\scout\camera.py") verify --mutate
    } "camera --mutate: (\d+) of \1 mutations caught by their own check"
    # The race spine's seams: sixteen transcriptions run against the
    # original bytes on the probe's own integer executor; each mutant must fail its own function's cases.
    Gate "spine seams: sixteen transcriptions against the original, whole RAM" {
        python (Join-Path $root "tools\scout\spine.py") verify
    } "spine: \d+ checks, 0 failures"
    Gate "spine seams: every planted mutant is caught by its own function" {
        python (Join-Path $root "tools\scout\spine.py") verify --mutate --cases 150
    } "spine --mutate: (\d+) of \1 mutants caught by their own function, 0 missed"

    if (Test-Path $verify) {
        # The ported physics, function by function, against the original running in our interpreter.
        # Each variant below is one single-threaded pass over every row (over a thousand rows, hours
        # each), so all seven start together and the gates read their output: run one after another
        # they would take most of a day.
        $benchDir = Join-Path $root "work\gates_bench"
        New-Item -ItemType Directory -Force $benchDir | Out-Null
        $benchRuns = [ordered]@{
            plain = "phys"; mutate = "phys --mutate"; skip = "phys --skip-oracle"
            mfo = "phys --mutate-frame-out"; nospu = "phys --no-spu-file"
            mspu = "phys --mutate-spu-file"; splice = "phys --splice-control"
        }
        $benchProcs = foreach ($k in $benchRuns.Keys) {
            $o = Join-Path $benchDir "$k.txt"
            Start-Process -FilePath "cmd.exe" -WindowStyle Hidden -PassThru -WorkingDirectory $root `
                -ArgumentList "/c `"`"$verify`" $($benchRuns[$k]) > `"$o`" 2>&1`""
        }
        $benchProcs | Wait-Process
        function BenchOut($k) { Get-Content (Join-Path $benchDir "$k.txt") }

        Gate "physics: every ported function matches the original, bit for bit" {
            BenchOut plain
        } "verdict\s+PASS"

        # The negative control, and the more important of the two: flip one bit of the native answer
        # and every row must fail. A bench that cannot fail proves nothing about the rows that pass.
        Gate "physics: the bench itself fails when the answer is wrong" {
            BenchOut mutate
        } "rows PASS\s+0"

        # The second negative control, for the rows that lean on the oracle-supplied-callee seam:
        # swallow those calls instead of executing them - the rejected "stub the sound call" approach - and the
        # run must fail. If this ever passes, the seam is decorative and the rows that use it are not
        # proving what they claim. Matching on the verdict rather than a row count so that adding
        # rows does not quietly turn the control off.
        Gate "physics: the oracle seam is load-bearing (skipping it must fail)" {
            BenchOut skip
        } "verdict\s+FAIL"

        # The third control, for the rows whose callee writes an output buffer into the CALLER'S own
        # frame. Corrupt what the bench expects to find in that buffer
        # and those rows must fail - otherwise the buffer is not really being compared and the widest
        # part of the seam's trust boundary is unguarded. It fails on 0 bytes of RAM by design: the
        # buffer is the only thing under test here.
        Gate "physics: the caller-frame output buffer is really compared" {
            BenchOut mfo
        } "verdict\s+FAIL"

        # The two controls of the SPU control register file: with
        # the file OFF the rows that read libspu's register pair must fail, and a flipped bit in the
        # port's register file or SPU store list must fail the rows that compare them.
        Gate "physics: the SPU register file is load-bearing (switching it off must fail)" {
            BenchOut nospu
        } "verdict\s+FAIL"
        Gate "physics: the SPU registers the port writes are really compared" {
            BenchOut mspu
        } "verdict\s+FAIL"

        # The splice rows run the whole original bike step and swap native code
        # in at one region's entry. Corrupt the state the native region leaves and exactly those
        # rows must fail - otherwise the region's exit state is not really being compared.
        Gate "physics: a spliced region's exit state is really compared" {
            BenchOut splice
        } "verdict\s+FAIL"
    }

    $rraudio = Join-Path $root "$Build\rraudio.exe"
    if (Test-Path $rraudio) {
        # Pitch modulation and the live repeat address (the sound bench's stages B5/B6).
        Gate "audio: the SPU voice model's PMON and repeat-address rules" {
            & $rraudio spu-test
        } "spu-test: 0 failure\(s\)"
    }

    $report = Join-Path $root "work\av\xcheck\report.txt"
    if (Test-Path $report) {
        Gate "audio: C++ decoder against the Python probe, sample for sample" {
            Get-Content $report
        } "samples differing\s*:\s*0"
    }
}

# --- summary -------------------------------------------------------------------------------------
Write-Host ""
$passed = ($results | Where-Object { $_.Passed }).Count
$total = $results.Count
if ($passed -eq $total) {
    Write-Host "$passed/$total gates passed" -ForegroundColor Green
    exit 0
}
Write-Host "$passed/$total gates passed - failures:" -ForegroundColor Red
$results | Where-Object { -not $_.Passed } | ForEach-Object { Write-Host "  $($_.Gate)" -ForegroundColor Red }
exit 1
