[CmdletBinding()]
param(
    [string]$DiscImage,
    [string]$InstallDir,
    [string]$BuildDir = 'build_install',
    [switch]$SkipDependencies,
    [switch]$NoBuild,
    [switch]$SkipLaunchCheck,
    [switch]$RunGates,
    # Optional HD media (docs\HD-MEDIA.md, scripts\prepare-hd.ps1) after the installation: Original (none), Menus (pictures,
    # fonts, HUD) or MenusAndMovies (also the films). Not given: an interactive installation asks; a scripted one skips.
    [ValidateSet('', 'Original', 'Menus', 'MenusAndMovies')][string]$HdMedia = '',
    [string]$Upscaler
)
# Road Rash: Jailbreak PC & VR - source installation (INSTALL.bat) and the shared disc step of the
# player installers (install-player.ps1 calls this with -NoBuild -BuildDir <package>\tools).
#   1. installs missing CMake / Ninja / VS 2022 C++ Build Tools through WinGet (not with -SkipDependencies),
#   2. builds rrgame.exe and rrtool.exe with build.cmd (not with -NoBuild),
#   3. identifies the disc image by the SHA-1 of its executable and overlays (rrtool identify) and
#      rejects anything but the supported release,
#   4. copies the image into <InstallDir>\runtime\disc\ (the original is only read), checks the copy
#      byte for byte (SHA-256) and runs the parser self-checks against it,
#   5. publishes rrgame.exe, the PLAY launchers and saves\, keeping the previous files in backup-*\,
#   6. starts rrgame once, hidden and silent, WITHOUT a disc argument, to prove it finds the installed disc,
#   7. optionally prepares the HD media pack from the installed disc (prepare-hd.ps1; -HdMedia, or asked).
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot
if (!$InstallDir) { $InstallDir = Join-Path $repo 'install' }
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }

function Refresh-ToolPath {
    $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') + ';' + $env:Path
    if (Test-Path "$env:ProgramFiles\CMake\bin") { $env:Path = "$env:ProgramFiles\CMake\bin;" + $env:Path }
}
function Install-Dependency([string]$id, [string[]]$extra = @()) {
    if ($SkipDependencies) { throw "Missing dependency $id. Re-run without -SkipDependencies." }
    if (!(Get-Command winget.exe -ErrorAction SilentlyContinue)) { throw 'Install Microsoft App Installer (winget), then run INSTALL.bat again.' }
    & winget.exe install --exact --source winget --id $id --accept-source-agreements --accept-package-agreements --disable-interactivity @extra
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) { throw "Dependency installation failed: $id ($LASTEXITCODE)" }
    Refresh-ToolPath
}
function Find-Vs {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
}
function Invoke-Check([string]$name, [string]$expect, [string[]]$arguments, [string]$log) {
    $ErrorActionPreference = 'Continue' # a native tool's stderr line is not a PowerShell error
    $output = & $script:rrtool @arguments 2>&1 | Out-String
    Add-Content -LiteralPath $log -Value "== $name`r`n$output"
    if ($LASTEXITCODE -ne 0 -or $output -notmatch $expect) { throw "Installed disc check failed: $name. See $log" }
    Write-Host "  ok  $name"
}

Refresh-ToolPath
if (!$NoBuild) {
    if ([IO.Path]::IsPathRooted($BuildDir)) { throw 'With a build, -BuildDir is a folder name inside the source tree (build.cmd builds there).' }
    if (!(Find-Vs)) {
        Install-Dependency 'Microsoft.VisualStudio.2022.BuildTools' @('--force', '--override', '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    }
    if (!(Find-Vs)) { throw 'C++ Build Tools are still unavailable. Restart Windows if the installer requested it.' }
    $vsNinja = Join-Path (Find-Vs) 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    $vsCmake = Join-Path (Find-Vs) 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (!(Get-Command cmake.exe -ErrorAction SilentlyContinue) -and !(Test-Path $vsCmake)) { Install-Dependency 'Kitware.CMake' }
    if (!(Get-Command ninja.exe -ErrorAction SilentlyContinue) -and !(Test-Path $vsNinja)) { Install-Dependency 'Ninja-build.Ninja' }
    & cmd.exe /c (Join-Path $repo 'build.cmd') $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'The source build failed; no installation was replaced.' }
    if ($RunGates) {
        # The project's full acceptance run needs local research data under work\ (RAM captures, goldens);
        # it is for developers. The disc checks below run for every installation.
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tests\run_gates.ps1') -Build $BuildDir -Quick
        if ($LASTEXITCODE -ne 0) { throw 'tests\run_gates.ps1 -Quick failed; no installation was replaced.' }
    }
}
$script:rrtool = Join-Path $build 'rrtool.exe'
$game = Join-Path $build 'rrgame.exe'
if (!(Test-Path $script:rrtool) -or !(Test-Path $game)) { throw "Build outputs rrgame.exe and rrtool.exe are required in $build." }

if (!$DiscImage) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select your Road Rash: Jailbreak (USA) disc image'
    $dialog.Filter = 'PS1 disc images or archives|*.bin;*.cue;*.zip;*.7z|All files|*.*'
    try { if ($dialog.ShowDialog() -ne 'OK') { throw 'No disc image selected.' }; $DiscImage = $dialog.FileName }
    finally { $dialog.Dispose() }
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$job = Join-Path $InstallDir ('.install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $job | Out-Null
# A rejected image leaves nothing behind; a later failure keeps its staging for diagnosis.
trap { if ((Test-Path -LiteralPath $job) -and !(Get-ChildItem -LiteralPath $job -Force)) { Remove-Item -LiteralPath $job }; break }

# Archive and cue sheet -> one raw MODE2/2352 image.
$source = (Resolve-Path -LiteralPath $DiscImage).Path
$extension = [IO.Path]::GetExtension($source).ToLowerInvariant()
if ($extension -eq '.zip' -or $extension -eq '.7z') {
    $unpacked = Join-Path $job 'unpacked'
    if ($extension -eq '.zip') { Expand-Archive -LiteralPath $source -DestinationPath $unpacked }
    else {
        $seven = "$env:ProgramFiles\7-Zip\7z.exe"
        if (!(Test-Path $seven)) { Install-Dependency '7zip.7zip' }
        & $seven x $source "-o$unpacked" -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw '7-Zip could not unpack the disc archive.' }
    }
    $images = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File | Where-Object { $_.Extension -eq '.bin' })
    if ($images.Count -ne 1) { throw "The archive must hold exactly one BIN image (found $($images.Count))." }
    $source = $images[0].FullName
} elseif ($extension -eq '.cue') {
    $cue = Get-Content -LiteralPath $source -Raw
    $files = [regex]::Matches($cue, '(?im)^\s*FILE\s+"([^"]+)"\s+BINARY\s*$')
    if ($files.Count -ne 1 -or $cue -notmatch '(?im)TRACK\s+0?1\s+MODE2/2352') { throw 'Use a single data-track MODE2/2352 BIN/CUE dump.' }
    $source = (Resolve-Path -LiteralPath (Join-Path (Split-Path $source) $files[0].Groups[1].Value)).Path
} elseif ($extension -eq '.iso') {
    throw 'A 2048-byte ISO lacks the XA audio/video sectors the game streams. Use a raw MODE2/2352 BIN/CUE dump.'
}

# Identify by bytes, never by file name.
$json = & $script:rrtool identify $source --json | Out-String
$code = $LASTEXITCODE
$info = $json | ConvertFrom-Json
if ($code -ne 0 -or !$info.supported) {
    throw "Unsupported disc image: $source`r`n$($info.reason)`r`nOnly Road Rash: Jailbreak (USA), SLUS-01053, executable SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1 is supported."
}
Write-Host "Disc identified: $($info.release) (volume $($info.volume), SLUS_010.53 SHA-1 $($info.exeSha1))"

# Copy into staging and verify the copy byte for byte before anything is replaced.
$staged = Join-Path $job 'disc'
New-Item -ItemType Directory -Path $staged | Out-Null
$name = [IO.Path]::GetFileName($source)
$copy = Join-Path $staged $name
Write-Host "Copying the disc image ($([math]::Round((Get-Item -LiteralPath $source).Length / 1MB)) MiB)..."
Copy-Item -LiteralPath $source -Destination $copy
$hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -ne $hash) { throw "Disc copy hash mismatch. Staging remains in $job" }
$log = Join-Path $staged 'install-check.log'
Invoke-Check 'identify (copy)' 'verdict  SUPPORTED' @('identify', $copy) $log
Invoke-Check 'models: every *.GEO parses' '112 \.GEO files, 0 failed' @('geoscan', $copy) $log
Invoke-Check 'models: every multi-part group assembles' '0 without a program' @('asmcheck', $copy) $log
Invoke-Check 'road network: routes close, distances hold' 'all invariants hold' @('roadcheck', $copy) $log
Invoke-Check 'scene cells: race 1-20 parses' '0 failed' @('cellcheck', $copy, 'DATA/RACE1_20.STP') $log
[ordered]@{
    format = 1; release = $info.release; volume = $info.volume; exeSha1 = $info.exeSha1
    overlays = @($info.parts | Select-Object -Skip 1 | ForEach-Object { [ordered]@{ name = $_.name; sha1 = $_.sha1 } })
    image = $name; imageBytes = (Get-Item -LiteralPath $copy).Length; imageSha256 = $hash.ToLowerInvariant()
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $staged 'disc-manifest.json') -Encoding UTF8

# Publish. The previous disc and executables are kept for rollback; saves are never touched.
$backup = Join-Path $InstallDir ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $backup | Out-Null
$discDir = Join-Path $InstallDir 'runtime\disc'
New-Item -ItemType Directory -Force -Path (Split-Path $discDir) | Out-Null
if (Test-Path -LiteralPath $discDir) { Move-Item -LiteralPath $discDir -Destination (Join-Path $backup 'disc') }
Move-Item -LiteralPath $staged -Destination $discDir
$binaries = @('rrgame.exe', 'rrtool.exe') + @(Get-ChildItem -LiteralPath $build -Filter 'rrhd.exe' -File -ErrorAction SilentlyContinue | ForEach-Object Name) + @(Get-ChildItem -LiteralPath $build -Filter '*.dll' -File -ErrorAction SilentlyContinue | ForEach-Object Name)
foreach ($file in $binaries) {
    $destination = Join-Path $InstallDir $file
    if (Test-Path -LiteralPath $destination) { Copy-Item -LiteralPath $destination -Destination (Join-Path $backup $file) }
    Copy-Item -LiteralPath (Join-Path $build $file) -Destination $destination -Force
}
$shaders = Join-Path $build 'shaders'
if (Test-Path -LiteralPath $shaders) { Copy-Item -LiteralPath $shaders -Destination $InstallDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'saves') | Out-Null
$old = Join-Path $InstallDir 'rrjb_card.mcr'
$card = Join-Path $InstallDir 'saves\rrjb_card.mcr'
if ((Test-Path -LiteralPath $old) -and !(Test-Path -LiteralPath $card)) {
    Copy-Item -LiteralPath $old -Destination $card
    Write-Host "Existing save copied to $card (the old file is kept)."
}

$launcher = @'
@echo off
setlocal
cd /d "%~dp0"
set "RRGAME_FOCUS=1"
"%~dp0rrgame.exe" %*
if errorlevel 1 pause
'@
[IO.File]::WriteAllText((Join-Path $InstallDir 'PLAY.bat'), $launcher, [Text.Encoding]::ASCII)
# PCVR: rrgame --vr (the VR build). Each runtime launcher selects its OpenXR runtime for the game process
# only; the Windows OpenXR default in the registry is never changed.
$vr = $launcher.Replace('rrgame.exe" %*', 'rrgame.exe" --vr %*')
[IO.File]::WriteAllText((Join-Path $InstallDir 'PLAY-PCVR.bat'), $vr, [Text.Encoding]::ASCII)
foreach ($runtime in @('META','STEAMVR','VD')) {
    $policy = if ($runtime -eq 'VD') { 'vdxr' } else { $runtime.ToLowerInvariant() }
    $selected = $vr.Replace('setlocal', "setlocal`r`nset `"XR_RUNTIME_JSON=`"`r`nset `"RRJB_XR_RUNTIME=$policy`"")
    [IO.File]::WriteAllText((Join-Path $InstallDir "PLAY-PCVR-$runtime.bat"), $selected, [Text.Encoding]::ASCII)
}
# Where the game went, for transfer-saves.ps1 and install-quest-player.ps1 (excluded from the source kit).
[IO.File]::WriteAllText((Join-Path $repo 'install-location.txt'), $InstallDir, [Text.UTF8Encoding]::new($false))

if (!$SkipLaunchCheck) {
    # rrgame from the install folder, no disc argument: it must report the disc it found. Hidden and
    # silent (RRJB_WINDOW=hidden), a fixed 60-frame scripted race, nothing written outside $job.
    $shot = Join-Path $job 'launch-check.png'
    $previous = $env:RRJB_WINDOW
    $env:RRJB_WINDOW = 'hidden'
    Push-Location $InstallDir
    $ErrorActionPreference = 'Continue'
    try { $output = & (Join-Path $InstallDir 'rrgame.exe') --race 1 20 --frames 60 --shot $shot 2>&1 | Out-String; $code = $LASTEXITCODE }
    finally { Pop-Location; $env:RRJB_WINDOW = $previous }
    Set-Content -LiteralPath (Join-Path $job 'launch-check.log') -Value $output -Encoding UTF8
    $expected = 'disc: ' + [regex]::Escape((Join-Path $discDir $name)) + ' \(from runtime\\disc\)'
    if ($code -ne 0 -or $output -notmatch $expected -or !(Test-Path -LiteralPath $shot)) {
        throw "rrgame did not start on the installed disc. See $job\launch-check.log. The previous files are in $backup."
    }
    Write-Host '  ok  rrgame starts from the install folder and finds runtime\disc without a disc argument'
    Remove-Item -LiteralPath $shot
    $ErrorActionPreference = 'Stop'
}
if (Test-Path -LiteralPath (Join-Path $job 'unpacked')) { Remove-Item -LiteralPath (Join-Path $job 'unpacked') -Recurse -Force }
# HD media (docs\HD-MEDIA.md): offered here, run by prepare-hd.ps1 on the installed disc; PREPARE-HD.bat does it later.
if (!$HdMedia) {
    $interactive = [Environment]::UserInteractive -and !([Environment]::GetCommandLineArgs() -match '(?i)^-NonInteractive$')
    $HdMedia = 'Original'
    if ($interactive -and (Test-Path -LiteralPath (Join-Path $InstallDir 'rrhd.exe'))) {
        Write-Host 'Optional HD media, made on this PC from your disc with a neural upscaler (Real-ESRGAN, needs a Vulkan GPU):'
        Write-Host '  1. none now (PREPARE-HD.bat can do it later)   2. pictures, fonts, HUD   3. also the films (an hour or more)'
        $answer = Read-Host 'Choose 1, 2 or 3 (Enter = 1)'
        if ($answer -eq '2') { $HdMedia = 'Menus' } elseif ($answer -eq '3') { $HdMedia = 'MenusAndMovies' }
    }
}
if ($HdMedia -ne 'Original') {
    $prepare = @{ Runtime = $InstallDir; BuildDir = $InstallDir; HdMedia = $HdMedia }
    if ($Upscaler) { $prepare['Upscaler'] = $Upscaler }
    try { & (Join-Path $PSScriptRoot 'prepare-hd.ps1') @prepare }
    catch { Write-Host "HD media were not prepared: $($_.Exception.Message)`r`nThe game is installed and plays with the original media; PREPARE-HD.bat can try again." -ForegroundColor Yellow }
}
Write-Host "Ready: $InstallDir"
Write-Host 'Use PLAY.bat for the desktop game. PLAY-PCVR-META.bat (Meta Link), PLAY-PCVR-STEAMVR.bat (SteamVR) and PLAY-PCVR-VD.bat (Virtual Desktop) start rrgame --vr.'
Write-Host "Saves: $InstallDir\saves. Previous files: $backup. Installation logs: $job."
