# DEVELOPMENT: runs the Quest build of rrgame on the USB-connected headset and collects its log and screenshots:
#   - adb install -r android/app/build/outputs/apk/release/app-release.apk unless -NoInstall (nothing else changes)
#   - the player's own disc image once to /sdcard/Android/data/com.rrjb.vr/files/disc.bin (skipped when the headset
#     has a file of the same size and SHA-256)
#   - files/rrgame_args.txt = -GameArgs (more rrgame switches, e.g. '--race','1','20' or '--vr-mock','--race','1',
#     '20','--frames','600','--shot','quest_mock.png'); without -GameArgs the file is removed (the game starts on its
#     front end)
#   - am start, -Seconds of logcat (tag RRJB.VR) into work/xr/quest_logcat.txt, the files named by -Pull fetched to
#     work/xr/quest_<name>, then am force-stop.
# It never wakes the headset or fakes its proximity: a session nobody wears stays IDLE, and the log says so.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run-quest-proof.ps1 [-NoInstall] [-Seconds 40] [-GameArgs ...] [-Pull a.png]
param(
    [string]$Adb = (Join-Path (Split-Path $PSScriptRoot | Split-Path) 'android-toolchain\sdk\platform-tools\adb.exe'),
    [string]$Serial,
    [string]$Disc,
    [string[]]$GameArgs,
    [string[]]$Pull,
    [int]$Seconds = 40,
    [switch]$NoInstall,
    [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
# `powershell -File` hands an array over as one comma-joined string: split it again (no rrgame switch has a comma)
$GameArgs = @($GameArgs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Pull = @($Pull | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repo = Split-Path $PSScriptRoot
if (!$Disc) { $Disc = $env:RRJB_DISC }
if (!$Disc -and (Test-Path (Join-Path $repo 'disc.txt'))) { $Disc = (Get-Content (Join-Path $repo 'disc.txt') -TotalCount 1).Trim() }
$package = 'com.rrjb.vr'
$apk = Join-Path $repo 'android\app\build\outputs\apk\release\app-release.apk'
if (!$Serial) {
    $connected = @(& $Adb devices | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if ($connected.Count -ne 1) { throw 'Connect one authorised Quest, or pass -Serial.' }
    $Serial = $connected[0]
}
function Adb([string[]]$a) {
    $out = & $Adb -s $Serial @a
    if ($LASTEXITCODE -ne 0) { throw "adb $($a -join ' ') failed" }
    $out
}
if (!$NoInstall) {
    if (!(Test-Path $apk)) { throw 'Build the APK first: scripts\build-quest.ps1' }
    Adb @('install', '-r', $apk) | Write-Host
}
$files = "/sdcard/Android/data/$package/files"
Adb @('shell', 'mkdir', '-p', $files) | Out-Null
$local = Get-Item -LiteralPath $Disc
$name = 'disc.bin'
$remoteSize = "$(& $Adb -s $Serial shell "stat -c %s '$files/$name' 2>/dev/null || echo 0")".Trim()
$push = $true
if ($remoteSize -eq "$($local.Length)") {
    $expected = (Get-FileHash -LiteralPath $Disc -Algorithm SHA256).Hash.ToLowerInvariant()
    $push = -not "$(Adb @('shell', 'sha256sum', "$files/$name"))".StartsWith($expected)
}
if ($push) {
    Write-Host "pushing the disc image ($([math]::Round($local.Length / 1MB)) MB) ..."
    Adb @('push', $Disc, "$files/$name") | Write-Host
} else { Write-Host 'disc image already on the headset (same size and SHA-256)' }
& $Adb -s $Serial shell "rm -f '$files/rrvrtest.txt'" | Out-Null # the OpenXR test app's configuration
if ($GameArgs) {
    $tmp = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($tmp, ($GameArgs -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
        Adb @('push', $tmp, "$files/rrgame_args.txt") | Out-Null
    } finally { Remove-Item -LiteralPath $tmp }
} else {
    & $Adb -s $Serial shell "rm -f '$files/rrgame_args.txt'" | Out-Null
}
foreach ($p in $Pull) { & $Adb -s $Serial shell "rm -f '$files/$p'" | Out-Null }
Adb @('logcat', '-c') | Out-Null
Adb @('shell', 'am', 'start', '-n', "$package/.RoadRashActivity") | Write-Host
Start-Sleep -Seconds $Seconds
$out = Join-Path $repo 'work\xr'
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'quest_logcat.txt'
& $Adb -s $Serial logcat -d > $log
$ErrorActionPreference = 'Continue' # a file the run did not write is reported by adb, not fatal
foreach ($p in $Pull) { & $Adb -s $Serial pull "$files/$p" (Join-Path $out "quest_$p") 2>&1 | Out-Null }
$ErrorActionPreference = 'Stop'
if (!$KeepRunning) { Adb @('shell', 'am', 'force-stop', $package) | Out-Null }
& $Adb -s $Serial shell "rm -f '$files/rrgame_args.txt'" | Out-Null # the next plain start is the player's game
Select-String -Path $log -Pattern 'RRJB.VR' | Select-Object -Last 80 | ForEach-Object { $_.Line }
