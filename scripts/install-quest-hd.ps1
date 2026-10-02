[CmdletBinding()]
param(
    [string]$Runtime,                  # the PC game folder whose runtime\hd holds the prepared pack
    [string]$Pack,                     # or the pack folder itself
    [string]$Adb,
    [string]$Serial,
    [string]$Package = 'com.rrjb.vr',
    [switch]$Remove                    # take the pack off the headset instead
)
# Copies the prepared HD media pack (scripts\prepare-hd.ps1, docs\HD-MEDIA.md) to the Quest:
#   /sdcard/Android/data/<package>/files/hd   (tools\rrgame\hd_media.cpp: <app external files>/hd)
# next to the disc image install-quest-player.ps1 put there. The pack goes to a fresh staging folder first and replaces hd only once its
# file count and bytes match; the APK, the disc copy and the saves are not touched. The game uses it when
# "HD textures and media" is on in the VR menu (Graphics). install-quest-player.ps1 calls this when a pack exists.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Pack) {
    if (!$Runtime) {
        $hint = Join-Path $repo 'install-location.txt'
        $Runtime = if (Test-Path -LiteralPath $hint) { (Get-Content -LiteralPath $hint -Raw).Trim() } else { Join-Path $env:LOCALAPPDATA 'RoadRashJailbreak' }
    }
    $Pack = Join-Path $Runtime 'runtime\hd'
}
if ($Package -notmatch '^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$') { throw 'Invalid package name.' }
. (Join-Path $PSScriptRoot 'platform-tools.ps1')
if (!$Adb) { $Adb = Get-Adb }
function Invoke-Adb([string[]]$Arguments) {
    # adb reports push/pull summaries on stderr; under PowerShell 5.1 with 'Stop' (or when the caller merges
    # stderr) those lines become terminating errors, so collect both streams as text and judge by the exit code.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $selector = @()
    if ($Serial) { $selector = @('-s', $Serial) }
    try { $output = @(& $Adb @selector @Arguments 2>&1 | ForEach-Object { "$_" }) } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($Arguments -join ' ')`r`n$($output -join "`r`n")" }
    return $output
}
if ($Serial -and $Serial -notmatch '^[A-Za-z0-9._:-]+$') { throw 'Invalid ADB serial.' }
$connected = @(Invoke-Adb @('devices') | Where-Object { $_ -match '^\S+\s+device$' } | ForEach-Object { ($_ -split '\s+')[0] })
if (!$Serial) {
    if ($connected.Count -ne 1) { throw 'Connect one Quest over USB and accept USB debugging inside the headset, or specify -Serial.' }
    $Serial = $connected[0]
}
if ($Serial -notin $connected) { throw 'The selected Quest is not connected and authorized.' }
$external = "/sdcard/Android/data/$Package/files"
if ($Remove) {
    Invoke-Adb @('shell', "rm -rf $external/hd $external/hd.new $external/hd.new.* $external/rrjb_hd_stage.* /sdcard/rrjb_hd_stage.*") | Out-Null
    Write-Host "HD media removed from the headset ($external/hd)."
    return
}
if (!(Test-Path -LiteralPath (Join-Path $Pack 'profile.txt')) -or !(Test-Path -LiteralPath (Join-Path $Pack 'index.txt'))) {
    throw "No prepared HD pack in $Pack. Run PREPARE-HD.bat first."
}
$files = @(Get-ChildItem -LiteralPath $Pack -Recurse -File)
$bytes = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host "Copying the HD pack ($($files.Count) files, $([math]::Round($bytes / 1MB)) MiB) to the headset..."
Invoke-Adb @('shell', 'mkdir', '-p', $external) | Out-Null
# adb push creating folders under /sdcard/Android/data/<pkg> fails intermittently on the Quest ("remote
# secure_mkdirs failed: Operation not permitted"). Push the pack to shared storage first (adb
# creates the folder itself; the pack folder is pushed as <target>), check it there, then move it next to the disc
# with one mv on the same volume.
$stageName = 'rrjb_hd_stage.' + [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
$sharedStage = "/sdcard/$stageName"
$stage = "$external/$stageName"
Invoke-Adb @('shell', "rm -rf $external/hd.new $external/hd.new.* $external/rrjb_hd_stage.* /sdcard/rrjb_hd_stage.*") | Out-Null
Invoke-Adb @('push', $Pack, $sharedStage) | Out-Null
Invoke-Adb @('shell', 'mv', $sharedStage, $stage) | Out-Null
$remoteCount = ((Invoke-Adb @('shell', "find $stage -type f | wc -l")) -join '').Trim()
$remoteBytes = ((Invoke-Adb @('shell', "find $stage -type f -exec stat -c %s {} + | awk '{s+=`$1} END {print s}'")) -join '').Trim()
if ($remoteCount -ne [string]$files.Count -or $remoteBytes -ne [string]$bytes) {
    throw "The copy on the headset differs ($remoteCount files / $remoteBytes bytes, expected $($files.Count) / $bytes); the previous pack is untouched."
}
$indexSha1 = (Get-FileHash -LiteralPath (Join-Path $Pack 'index.txt') -Algorithm SHA1).Hash.ToLowerInvariant()
if (((Invoke-Adb @('shell', 'sha1sum', "$stage/index.txt")) -join ' ') -notmatch "^$indexSha1\s") { throw 'The index on the headset differs.' }
Invoke-Adb @('shell', 'rm', '-rf', "$external/hd") | Out-Null
Invoke-Adb @('shell', 'mv', $stage, "$external/hd") | Out-Null
# Folders adb creates are drwxrws--- shell:ext_data_rw, which the app's own uid cannot enter ("has no profile.txt"
# on Quest 3); open them for reading.
Invoke-Adb @('shell', 'chmod', '-R', 'a+rX', "$external/hd") | Out-Null
Write-Host "HD media on the headset: $external/hd ($($files.Count) files, $bytes bytes). Switch 'HD textures and media' on in the VR menu (Graphics)."
