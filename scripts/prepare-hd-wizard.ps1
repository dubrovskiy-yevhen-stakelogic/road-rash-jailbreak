param([string]$Runtime, [string]$BuildDir)
# PREPARE-HD.bat: asks what to prepare and runs prepare-hd.ps1 on the installed game (docs\HD-MEDIA.md).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if (!$Runtime) {
    $location = Join-Path $root 'install-location.txt'
    if (Test-Path -LiteralPath $location) { $Runtime = (Get-Content -LiteralPath $location -Raw).Trim() }
    else { $Runtime = Join-Path $env:LOCALAPPDATA 'RoadRashJailbreak' }
}
if (!(Test-Path -LiteralPath (Join-Path $Runtime 'runtime\disc'))) { throw "Install the game with your disc first (INSTALL-PC.bat / INSTALL.bat): no runtime\disc in $Runtime." }
if (!$BuildDir) {
    foreach ($folder in @($Runtime, (Join-Path $root 'tools'), (Join-Path $root 'build_install'), (Join-Path $root 'build'))) {
        if (Test-Path -LiteralPath (Join-Path $folder 'rrhd.exe')) { $BuildDir = $folder; break }
    }
}
if (!$BuildDir) { throw 'rrhd.exe not found: run the updated installer first.' }
Write-Host "Road Rash: Jailbreak - HD media for the game in $Runtime"
Write-Host '1. HD pictures, fonts and HUD (menus, backgrounds, loading screens; minutes)'
Write-Host '2. HD pictures, fonts, HUD and the eleven films (EA logo, intro, cut-scenes; an hour or more, several GB of temporary files)'
Write-Host '3. Nothing (keep the original media)'
$choice = Read-Host 'Choose 1, 2 or 3 (Enter = 1)'
$mode = switch ($choice) { '' { 'Menus' }; '1' { 'Menus' }; '2' { 'MenusAndMovies' }; '3' { 'Original' }; default { throw 'Invalid choice.' } }
if ($mode -eq 'Original') { Write-Host 'No changes.'; return }
& (Join-Path $PSScriptRoot 'prepare-hd.ps1') -Runtime $Runtime -BuildDir $BuildDir -HdMedia $mode
Write-Host 'Quest: run scripts\install-quest-hd.ps1 (headset connected over USB) to copy the pack to the headset.'
