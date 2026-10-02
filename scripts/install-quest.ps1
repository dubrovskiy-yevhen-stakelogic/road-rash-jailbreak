[CmdletBinding()]
param(
    [string]$Adb,
    [string]$Serial,
    [string]$Apk,
    [string]$Runtime,
    [string]$Package = 'com.rrjb.vr',
    [switch]$Run,
    [int]$Seconds = 40
)
# The Quest install, one path (docs\QUEST.md): this is scripts\install-quest-player.ps1 - the player installer the
# player package's INSTALL.bat calls through install-player.ps1 - under a shorter name for development use. It
# takes the same parameters (-Adb -Serial -Apk -Runtime -Package) and does exactly what that script does: adb install
# -r of the APK (the app and its data are never removed), the prepared disc image from the PC game folder
# (<Runtime>\runtime\disc, checked against its disc-manifest.json) to
# /sdcard/Android/data/<package>/files/disc.bin (platform/app_paths.cpp FindDiscImage takes disc.bin first), verified
# on the headset by size and SHA-1. The game is not started.
# -Run (DEVELOPMENT) then starts it and collects its log for -Seconds (scripts\run-quest-proof.ps1 -NoInstall).
$ErrorActionPreference = 'Stop'
$forward = @{}
foreach ($k in 'Adb', 'Serial', 'Apk', 'Runtime', 'Package') {
    $v = Get-Variable -Name $k -ValueOnly
    if ($v) { $forward[$k] = $v }
}
& (Join-Path $PSScriptRoot 'install-quest-player.ps1') @forward
if ($Run) {
    $proof = @{ NoInstall = $true; Seconds = $Seconds }
    if ($Serial) { $proof['Serial'] = $Serial }
    if ($Adb) { $proof['Adb'] = $Adb }
    & (Join-Path $PSScriptRoot 'run-quest-proof.ps1') @proof
}
