[CmdletBinding()]
param(
    [string]$DiscImage,
    [ValidateSet('Quest','PC','PCVR')][string]$Target = 'Quest',
    [string]$InstallDir,
    [string]$Adb,
    [string]$Serial,
    [switch]$SkipLaunchCheck,
    [switch]$VerifyOnly,
    [ValidateSet('', 'Original', 'Menus', 'MenusAndMovies')][string]$HdMedia = '', # HD media (docs\HD-MEDIA.md): asked when not given
    [string]$Upscaler
)
# The player package's installer (INSTALL-PC.bat, INSTALL-PCVR.bat, INSTALL.bat for Quest). It verifies
# every package file against release-manifest.json, then prepares the disc with the prebuilt tools
# (install.ps1 -NoBuild; no compiler needed) and, for Quest, installs the APK and copies the disc to the
# headset (install-quest-player.ps1). PCVR and Quest require the VR build: a package without the OpenXR loader
# or the APK says so and stops.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
if (!$InstallDir) { $InstallDir = Join-Path $env:LOCALAPPDATA 'RoadRashJailbreak' }
$manifest = Get-Content -LiteralPath (Join-Path $root 'release-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -ne '0.1.0') { throw 'Unexpected release version.' }
$seen = @{}
foreach ($entry in $manifest.files) {
    if ($entry.path -match '(^|[\\/])\.\.([\\/]|$)|^[\\/]|:' -or $seen.ContainsKey($entry.path)) { throw 'Invalid release manifest path.' }
    $seen[$entry.path] = $true
    $path = Join-Path $root $entry.path
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Release file failed verification: $($entry.path)" }
}
foreach ($required in @('tools/rrgame.exe','tools/rrtool.exe','scripts/install-player.ps1','scripts/install.ps1','scripts/install-quest-player.ps1','scripts/platform-tools.ps1','scripts/transfer-saves.ps1')) {
    if (!$seen.ContainsKey($required)) { throw "Required release file is missing from the manifest: $required" }
}
if ($VerifyOnly) { Write-Host 'Release files verified.'; return }
$apk = @($seen.Keys | Where-Object { $_ -match '^RoadRashJailbreak-VR-.*\.apk$' })
if ($Target -eq 'PCVR' -and !$seen.ContainsKey('tools/openxr_loader.dll')) {
    throw 'This package has no PCVR support yet (no OpenXR loader): PCVR requires the VR build. Use INSTALL-PC.bat for the desktop game.'
}
if ($Target -eq 'Quest' -and $apk.Count -ne 1) {
    throw 'This package has no Quest APK yet: Quest requires the VR build. Use INSTALL-PC.bat for the desktop game.'
}
. (Join-Path $PSScriptRoot 'platform-tools.ps1')
$adbPath = $null
if ($Target -eq 'Quest') {
    $adbPath = Get-Adb
    # ADB daemon startup messages use stderr even when device discovery succeeds.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $devices = @(& $adbPath devices 2>&1 | ForEach-Object { "$_" }) } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "ADB device discovery failed:`r`n$($devices -join "`r`n")" }
    $connected = @($devices | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
    if (!$Serial) {
        if ($connected.Count -ne 1) { throw 'Connect one Quest and accept USB debugging inside the headset.' }
        $Serial = $connected[0]
    }
    if ($Serial -notin $connected) { throw 'The selected Quest is not connected and authorized.' }
}
$installArgs = @{ DiscImage = $DiscImage; InstallDir = $InstallDir; BuildDir = (Join-Path $root 'tools'); NoBuild = $true; SkipLaunchCheck = $SkipLaunchCheck }
if ($HdMedia) { $installArgs['HdMedia'] = $HdMedia }
if ($Upscaler) { $installArgs['Upscaler'] = $Upscaler }
& (Join-Path $PSScriptRoot 'install.ps1') @installArgs
if ($Target -eq 'Quest') {
    & (Join-Path $PSScriptRoot 'install-quest-player.ps1') -Adb $adbPath -Serial $Serial -Apk (Join-Path $root $apk[0]) -Runtime $InstallDir
} else {
    Write-Host "PC game ready: $InstallDir. Use PLAY.bat for desktop; PLAY-PCVR-META.bat (Meta Link), PLAY-PCVR-STEAMVR.bat (SteamVR) or PLAY-PCVR-VD.bat (Virtual Desktop) for PCVR."
}
