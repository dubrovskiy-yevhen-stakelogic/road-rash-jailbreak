param(
    [string]$Adb,
    [string]$Serial,
    [string]$Apk,
    [string]$Runtime,
    [string]$Package = 'com.rrjb.vr',
    [switch]$NoHd # leave the HD media pack (docs\HD-MEDIA.md) on the PC even when one is prepared
)
# The player's Quest 3 installer (INSTALL.bat of the player package calls it through install-player.ps1;
# scripts\install-quest.ps1 is the same install under the older name, with -Run to start the game and collect its
# log; the APK comes from scripts\build-quest.ps1).
# Installs the APK over USB and copies the player's disc image, prepared on the PC by install.ps1
# (<Runtime>\runtime\disc), to the app's external files folder on the headset:
#   /sdcard/Android/data/<package>/files/disc.bin   (platform/app_paths.cpp FindDiscImage takes disc.bin first)
# The copy is verified on the headset by size and SHA-1 (adb shell sha1sum). The game is not started, the app
# is never uninstalled and its data (the memory card and settings in its internal storage) is never cleared:
# adb install -r only.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Apk) { $Apk = Join-Path $repo 'android/app/build/outputs/apk/release/app-release.apk' }
if (!$Runtime) {
    $hint = Join-Path $repo 'install-location.txt'
    $Runtime = if (Test-Path -LiteralPath $hint) { (Get-Content -LiteralPath $hint -Raw).Trim() } else { Join-Path $repo 'install' }
}
if ($Package -notmatch '^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$') { throw 'Invalid package name.' }
if (!(Test-Path -LiteralPath $Apk -PathType Leaf)) { throw "Quest APK not found: $Apk. Build it with scripts\build-quest.ps1 (the VR build) or pass -Apk." }
$discDir = Join-Path $Runtime 'runtime\disc'
$images = @(Get-ChildItem -LiteralPath $discDir -Filter '*.bin' -File -ErrorAction SilentlyContinue)
if ($images.Count -ne 1) { throw "Prepare the disc with INSTALL.bat (or INSTALL-PC.bat) first: expected one image in $discDir." }
$image = $images[0]
$manifestPath = Join-Path $discDir 'disc-manifest.json'
if (!(Test-Path -LiteralPath $manifestPath)) { throw "Missing $manifestPath. Re-run the PC installation." }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.exeSha1 -ne '67ed165a2c517d4e6106fb0dfa324d66dd9a76f1' -or $manifest.image -ne $image.Name -or $manifest.imageBytes -ne $image.Length) {
    throw 'The installed disc does not match its manifest. Re-run the PC installation.'
}

. (Join-Path $PSScriptRoot 'platform-tools.ps1')
$Adb = Get-Adb
$devices = @(& $Adb devices)
if ($LASTEXITCODE -ne 0) { throw 'Cannot query ADB devices.' }
$connected = @($devices | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
if (!$Serial) {
    if ($connected.Count -ne 1) { throw 'Connect one Quest over USB and accept USB debugging inside the headset, or specify -Serial.' }
    $Serial = $connected[0]
}
if ($Serial -notmatch '^[A-Za-z0-9._:-]+$') { throw 'Invalid ADB serial.' }
if ($Serial -notin $connected) { throw 'The selected Quest is not connected and authorized.' }
function Invoke-Adb([string[]]$Arguments) {
    # adb reports push/pull summaries on stderr; under PowerShell 5.1 with 'Stop' (or when the caller merges
    # stderr) those lines become terminating errors, so collect both streams as text and judge by the exit code.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = @(& $Adb -s $Serial @Arguments 2>&1 | ForEach-Object { "$_" }) } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($Arguments -join ' ')`r`n$($output -join "`r`n")" }
    return $output
}

$external = "/sdcard/Android/data/$Package/files"
$remote = "$external/disc.bin"
Write-Host "Installing $Apk (update, app data kept)..."
Invoke-Adb @('install', '-r', $Apk) | Write-Host
Invoke-Adb @('shell', 'mkdir', '-p', $external) | Out-Null
$expected = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA1).Hash.ToLowerInvariant()
$present = (& $Adb -s $Serial shell sha1sum $remote 2>$null) -join ' '
if ($present -match "^$expected\s") { Write-Host 'The disc image on the headset is already identical; not copied again.' }
else {
    Write-Host "Copying $($image.Name) ($([math]::Round($image.Length / 1MB)) MiB) to the headset as disc.bin..."
    Invoke-Adb @('push', $image.FullName, $remote) | Write-Host
}
$size = ((Invoke-Adb @('shell', 'stat', '-c', '%s', $remote)) -join '').Trim()
if ($size -ne [string]$image.Length) { throw "Disc size differs on the headset ($size, expected $($image.Length))." }
$actual = (Invoke-Adb @('shell', 'sha1sum', $remote)) -join ' '
if ($actual -notmatch "^$expected\s") { throw 'Disc SHA-1 differs on the headset.' }
Write-Host "Disc verified on the headset: $remote ($size bytes, SHA-1 $expected)."
$info = (Invoke-Adb @('shell', 'dumpsys', 'package', $Package)) -join "`n"
if ($info -match 'versionName=(\S+)') { Write-Host "Installed $Package $($Matches[1])." }
# The HD media pack, when one is prepared on the PC (PREPARE-HD.bat): scripts\install-quest-hd.ps1 copies it to files/hd.
if (!$NoHd -and (Test-Path -LiteralPath (Join-Path $Runtime 'runtime\hd\index.txt'))) {
    & (Join-Path $PSScriptRoot 'install-quest-hd.ps1') -Runtime $Runtime -Adb $Adb -Serial $Serial -Package $Package
}
Write-Host 'Road Rash: Jailbreak VR installed. Launch it from Unknown Sources on the headset. Existing saves were retained.'
