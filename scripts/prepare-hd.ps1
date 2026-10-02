[CmdletBinding()]
param(
    [string]$Runtime,                 # the game folder (INSTALL.bat's; holds runtime\disc); default: install-location.txt
    [string]$DiscImage,               # DEVELOPMENT: a disc image instead of the installed one (then -Output is required)
    [string]$BuildDir,                # where rrhd.exe is (the game folder, the package's tools\, a build folder)
    [ValidateSet('Original','Menus','MenusAndMovies')][string]$HdMedia = 'MenusAndMovies',
    [string]$Upscaler,                # realesrgan-ncnn-vulkan.exe (or its folder, with models\), when not found by itself
    [string]$Cache,                   # intermediate files (default <Runtime>\runtime\.hd-work); kept for resume/inspection
    [string]$Output,                  # the pack (default <Runtime>\runtime\hd)
    [string[]]$Films,                 # only these films (EA_LOGO, INTRO, ...); default all eleven
    [int]$Gpu = -1,                   # Vulkan GPU index for the upscaler (-1: its default)
    [int]$Tile = 0,                   # upscaler tile size (0: automatic; 128 or 64 for a GPU with little memory)
    [switch]$KeepIntermediate,        # keep the network's full-size pictures (GBs) after they are packed
    [ValidateSet('png','jpg')][string]$Intermediate = 'png', # the network's output: png (lossless) or jpg (a quarter of
                                                              # the temporary disk space; one more lossy step)
    [int]$FilmChunk = 96,             # film pictures per pass through the network (streamed, see below)
    [double]$MinFreeGB = 0            # stop before the work when the cache's drive has less free space (0: the estimate)
)
# Road Rash: Jailbreak - OFFLINE HD media preparation (docs\HD-MEDIA.md). The same model as the Gran Turismo 2 port's
# prepare-hd.ps1: everything is made on this PC from the player's own disc, nothing is uploaded, nothing ships with the
# game, and the game never runs a network at play time.
#   1. rrhd extract: the front end's pictures (backgrounds, titles, panel films, loading screens, FEMISC.PSH sprites) as
#      the network's input; the four fonts and every HUD region as 4x CONTOUR atlases (xBR on palette indices - the
#      game keeps its live palettes); with MenusAndMovies every picture of the eleven .WVE films.
#   2. Real-ESRGAN (realesrgan-x4plus, ncnn / Vulkan): every picture and film picture enlarged 4x on the GPU.
#   3. rrhd compose / pack-movie: the originals' transparency on the pictures; the films bounded to 10 levels around a
#      bicubic enlargement of their own picture and held where the shot is still (temporal stability).
#   4. rrhd finish / verify: the index keyed by the source hash of every asset and the executable's SHA-1; every entry
#      checked against the disc before the pack is published into <Runtime>\runtime\hd (the old pack is kept as a backup).
# The game uses the pack when "HD textures and media" is on (F10 > Image quality; the VR menu > Graphics).
#
# The upscaler is NOT downloaded. It is found as (1) -Upscaler, (2) the official release archive
# realesrgan-ncnn-vulkan-20220424-windows.zip (Real-ESRGAN v0.2.5.0, SHA-256 pinned below) in the cache, the Gran Turismo 2
# port's cache or Downloads - extracted here after its hash is checked, or (3) an extracted copy whose executable and
# model files match the pinned hashes. Without one the script stops and says what to install.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot
$started = Get-Date
$timing = New-Object System.Collections.Generic.List[string]
function Stage([string]$name, [scriptblock]$body) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "== $name"
    & $body
    $line = '{0,-44} {1,8:N1} s' -f $name, $t.Elapsed.TotalSeconds
    $timing.Add($line)
    Write-Host "   $line"
}

# ---- the disc, the tools, the folders
if (!$DiscImage) {
    if (!$Runtime) {
        $hint = Join-Path $repo 'install-location.txt'
        $Runtime = if (Test-Path -LiteralPath $hint) { (Get-Content -LiteralPath $hint -Raw).Trim() } else { Join-Path $env:LOCALAPPDATA 'RoadRashJailbreak' }
    }
    $discDir = Join-Path $Runtime 'runtime\disc'
    $images = @(Get-ChildItem -LiteralPath $discDir -Filter '*.bin' -File -ErrorAction SilentlyContinue)
    if ($images.Count -ne 1) { throw "Install the game first (INSTALL.bat / INSTALL-PC.bat): expected one disc image in $discDir." }
    $DiscImage = $images[0].FullName
    if (!$Cache) { $Cache = Join-Path $Runtime 'runtime\.hd-work' }
    if (!$Output) { $Output = Join-Path $Runtime 'runtime\hd' }
} elseif (!$Output -or !$Cache) { throw 'With -DiscImage, give -Cache and -Output as well.' }
$DiscImage = (Resolve-Path -LiteralPath $DiscImage).Path
if (!$BuildDir) {
    foreach ($candidate in @($Runtime, (Join-Path $repo 'tools'), (Join-Path $repo 'build_install'), (Join-Path $repo 'build'))) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'rrhd.exe'))) { $BuildDir = $candidate; break }
    }
}
if (!$BuildDir -or !(Test-Path -LiteralPath (Join-Path $BuildDir 'rrhd.exe'))) { throw 'rrhd.exe not found: install or build the updated game first (or pass -BuildDir).' }
$rrhd = (Resolve-Path -LiteralPath (Join-Path $BuildDir 'rrhd.exe')).Path
$Cache = [IO.Path]::GetFullPath($Cache)
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $Cache | Out-Null
function Run([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "HD preparation failed ($LASTEXITCODE): $(Split-Path -Leaf $exe) $($arguments -join ' '). The work so far is kept in $Cache." }
}
if ($HdMedia -eq 'Original') { Write-Host 'Original media selected: nothing to prepare. An existing HD pack is left as it is (switch it off in the settings).'; return }

# ---- the upscaler (never downloaded)
$zipSha256 = 'ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D' # realesrgan-ncnn-vulkan-20220424-windows.zip
$pinned = [ordered]@{ # the files of that archive this script runs or loads
    'realesrgan-ncnn-vulkan.exe'     = '07E49F7CBB4EDE01AE4DD4C399D3A7E5846E3D2085C3128EFF881E55CB7B1A0C'
    'vcomp140.dll'                   = '8F72EF2E483465444B2059FC6744D6CB22CD8D8A27F6FA56BEFD2A42DCD0F78B'
    'models\realesrgan-x4plus.param' = '35330ECECCEA33B6C397A72548E788D5D53BECEE4734C50B7FADA36E89F10A86'
    'models\realesrgan-x4plus.bin'   = '713EE713B0353AFAA27976F0563A64A5043BD70B9BD8936C2E26E25EBCDBCDDF'
}
function Test-UpscalerFolder([string]$folder, [bool]$requirePinned) {
    if (!(Test-Path -LiteralPath (Join-Path $folder 'realesrgan-ncnn-vulkan.exe'))) { return $false }
    foreach ($f in @('models\realesrgan-x4plus.param', 'models\realesrgan-x4plus.bin')) { if (!(Test-Path -LiteralPath (Join-Path $folder $f))) { return $false } }
    if (!$requirePinned) { return $true }
    foreach ($f in $pinned.Keys) {
        $p = Join-Path $folder $f
        if (!(Test-Path -LiteralPath $p) -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -ne $pinned[$f]) { return $false }
    }
    return $true
}
function Find-Upscaler {
    if ($Upscaler) {
        $u = (Resolve-Path -LiteralPath $Upscaler).Path
        if (Test-Path -LiteralPath $u -PathType Leaf) { $u = Split-Path $u }
        if (!(Test-UpscalerFolder $u $false)) { throw "-Upscaler: $u has no realesrgan-ncnn-vulkan.exe with models\realesrgan-x4plus.param/.bin." }
        return $u
    }
    $extracted = Join-Path $Cache 'realesrgan-20220424'
    if (Test-UpscalerFolder $extracted $true) { return $extracted }
    $zipNames = @('realesrgan-ncnn-vulkan-20220424-windows.zip', 'realesrgan-20220424.zip')
    $places = @($Cache, (Join-Path $env:LOCALAPPDATA 'GT2-VR\runtime\.hd-work'), (Join-Path $env:USERPROFILE 'Downloads'), $repo, 'C:\Tools', (Join-Path (Split-Path $repo) 'gt2-play\work\audit\hd-prototype'))
    foreach ($place in $places) {
        foreach ($name in $zipNames) {
            $zip = Join-Path $place $name
            if (!(Test-Path -LiteralPath $zip -PathType Leaf)) { continue }
            if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $zipSha256) { Write-Host "   $zip is not the pinned Real-ESRGAN 0.2.5.0 archive - ignored"; continue }
            $tmp = "$extracted.extract-$([guid]::NewGuid().ToString('N'))"
            Expand-Archive -LiteralPath $zip -DestinationPath $tmp
            if (!(Test-UpscalerFolder $tmp $true)) { Remove-Item -LiteralPath $tmp -Recurse -Force; throw "$zip passed its hash but its files did not - not used." }
            if (Test-Path -LiteralPath $extracted) { Remove-Item -LiteralPath $extracted -Recurse -Force }
            Move-Item -LiteralPath $tmp -Destination $extracted
            Write-Host "   Real-ESRGAN from the verified archive $zip"
            return $extracted
        }
    }
    # an already extracted official copy (the GT2 port's installer leaves one): accepted only when every file matches
    foreach ($root in @((Join-Path $env:LOCALAPPDATA 'GT2-VR\runtime\.hd-work'), 'C:\Tools', (Join-Path (Split-Path $repo) 'gt2-play\work'))) {
        if (!(Test-Path -LiteralPath $root)) { continue }
        foreach ($exe in @(Get-ChildItem -LiteralPath $root -Recurse -Depth 4 -Filter 'realesrgan-ncnn-vulkan.exe' -File -ErrorAction SilentlyContinue)) {
            if (Test-UpscalerFolder $exe.DirectoryName $true) { Write-Host "   Real-ESRGAN (hash-checked copy) in $($exe.DirectoryName)"; return $exe.DirectoryName }
        }
    }
    throw @"
The neural upscaler is not installed, and this script does not download it. Install it once:
  1. Download realesrgan-ncnn-vulkan-20220424-windows.zip from the official Real-ESRGAN release v0.2.5.0:
     https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0
     (SHA-256 $zipSha256)
  2. Put the ZIP into $Cache (or into your Downloads folder) and run PREPARE-HD.bat again,
     or extract it anywhere and pass -Upscaler <folder>.
A Vulkan-capable GPU is required. The pictures take minutes, the films can take an hour or more.
"@
}
$upscalerDir = Find-Upscaler
$upscaler = Join-Path $upscalerDir 'realesrgan-ncnn-vulkan.exe'
function Upscale([string]$in, [string]$out) {
    $count = @(Get-ChildItem -LiteralPath $in -Filter '*.png' -File).Count
    $done = Join-Path $out 'complete.txt'
    if ((Test-Path -LiteralPath $done) -and @(Get-ChildItem -LiteralPath $out -Filter "*.$Intermediate" -File).Count -eq $count) { Write-Host "   reused: $out"; return }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $arguments = @('-i', $in, '-o', $out, '-n', 'realesrgan-x4plus', '-m', (Join-Path $upscalerDir 'models'), '-s', '4', '-f', $Intermediate, '-t', "$Tile", '-j', '2:2:2')
    if ($Gpu -ge 0) { $arguments += @('-g', "$Gpu") }
    # Below-normal priority: the PC stays usable while the GPU works.
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $upscaler
    $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($info)
    try { $p.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal } catch { }
    $errors = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    $made = @(Get-ChildItem -LiteralPath $out -Filter "*.$Intermediate" -File).Count
    if ($p.ExitCode -ne 0 -or $made -ne $count) {
        throw "Real-ESRGAN failed on $in (exit $($p.ExitCode), $made of $count pictures). A Vulkan GPU is required; try -Tile 128.`r`n$(($errors -split "`n" | Select-Object -Last 8) -join "`n")"
    }
    Set-Content -LiteralPath $done -Value "realesrgan-x4plus 4x, $count pictures" -Encoding ascii
}

# ---- free space: the work needs about 2.5 GB (pictures, png) / 1 GB (jpg), the films 0.5 GB more while streamed, the
# pack about 1.1 GB. Checked before the work and before every film chunk; the script stops rather than fill the drive.
function Assert-Space([double]$gb) {
    $root = [IO.Path]::GetPathRoot($Cache)
    $free = (New-Object IO.DriveInfo $root).AvailableFreeSpace / 1GB
    if ($free -lt $gb) { throw ("Only {0:N1} GB free on {1} - {2:N1} GB are needed. Free some space (or pass -Cache on another drive) and run again; finished steps are kept." -f $free, $root, $gb) }
}
$need = if ($MinFreeGB -gt 0) { $MinFreeGB } else { $(if ($Intermediate -eq 'png') { 2.5 } else { 1.0 }) + 1.2 + $(if ($HdMedia -eq 'MenusAndMovies') { 1.0 } else { 0 }) }
Assert-Space $need

# ---- the work, stage by stage (each resumable)
$work = Join-Path $Cache 'work'
Write-Host "Preparing HD media from $DiscImage into $Output. This takes a while; keep the PC on power."
$extractArgs = @('extract', $DiscImage, $work)
if ($HdMedia -eq 'MenusAndMovies') { $extractArgs += '--movies' }
Stage 'extract (originals, contour atlases)' { Run $rrhd $extractArgs }
$stage = Join-Path $work 'stage'
# Each group is done once: its stamp in the stage says so, and a stopped run resumes after the last finished group. The
# network's full-size PNGs are deleted once packed (-KeepIntermediate keeps them): they are several GB.
$picturesDone = Join-Path $stage 'pictures.complete'
$pictureCount = @(Get-Content -LiteralPath (Join-Path $work 'pictures\list.txt')).Count
if ((Test-Path -LiteralPath $picturesDone) -and (Get-Content -LiteralPath $picturesDone -Raw).Trim() -eq "pad8 $pictureCount") {
    Write-Host "== pictures: $pictureCount already prepared (stage\pictures.complete)"
} else {
    # rrhd frames each picture with 8 texels of its own edge (input-pad8) and crops them off again in compose
    Stage 'Real-ESRGAN: pictures' { Upscale (Join-Path $work 'pictures\input-pad8') (Join-Path $work 'pictures\upscaled-pad8') }
    Stage 'compose pictures' { Run $rrhd @('compose', $work, (Join-Path $work 'pictures\upscaled-pad8')) }
    Set-Content -LiteralPath $picturesDone -Value "pad8 $pictureCount" -Encoding ascii
    if (!$KeepIntermediate) { Remove-Item -LiteralPath (Join-Path $work 'pictures\upscaled-pad8') -Recurse -Force }
}
foreach ($old in @('input', 'upscaled')) { # a run before the frame: its folders are no longer used
    $p = Join-Path $work "pictures\$old"
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}
if ($HdMedia -eq 'MenusAndMovies') {
    foreach ($film in @(Get-ChildItem -LiteralPath (Join-Path $work 'movies') -Directory | Sort-Object Name)) {
        if ($Films -and ($Films | ForEach-Object { $_.ToUpperInvariant() }) -notcontains $film.Name) { continue }
        if (Test-Path -LiteralPath (Join-Path $stage "movies\$($film.Name).idx")) { Write-Host "== film $($film.Name): already packed"; continue }
        # Streamed: $FilmChunk pictures at a time through the network, bounded and packed (rrhd film-chunk), the
        # network's output deleted before the next chunk - the temporary space stays at one chunk plus the film's JPEGs.
        # A stopped run resumes at the chunk after the last one packed (movies\<film>\state.bin).
        Stage "film $($film.Name) (Real-ESRGAN + bound, streamed)" {
            $frames = @(Get-ChildItem -LiteralPath (Join-Path $film.FullName 'input') -Filter '*.png' -File).Count
            $state = Join-Path $film.FullName 'state.bin'
            $first = 0
            if (Test-Path -LiteralPath $state) { $first = [BitConverter]::ToInt32([IO.File]::ReadAllBytes($state), 0) }
            while ($first -lt $frames) {
                Assert-Space 1
                $cin = Join-Path $film.FullName 'chunk-in'
                $cout = Join-Path $film.FullName 'chunk-out'
                foreach ($d in @($cin, $cout)) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
                New-Item -ItemType Directory -Path $cin | Out-Null
                $last = [Math]::Min($frames, $first + $FilmChunk) - 1
                foreach ($i in $first..$last) {
                    $name = 'f{0:D5}.png' -f $i
                    $src = Join-Path $film.FullName "input\$name"
                    try { New-Item -ItemType HardLink -Path (Join-Path $cin $name) -Target $src -ErrorAction Stop | Out-Null }
                    catch { Copy-Item -LiteralPath $src -Destination (Join-Path $cin $name) }
                }
                Upscale $cin $cout
                Run $rrhd @('film-chunk', $work, $film.Name, $cout, "$first", "$FilmChunk")
                foreach ($d in @($cin, $cout)) { Remove-Item -LiteralPath $d -Recurse -Force }
                $first = $last + 1
            }
            Run $rrhd @('film-finish', $work, $film.Name)
        }
    }
}
Stage 'finish (index, profile)' { Run $rrhd @('finish', $work) }
Stage 'verify against the disc (every file decoded)' { Run $rrhd @('verify', $DiscImage, $stage, '--deep') }

# ---- publish: a copy of the stage, its manifest, then the swap (the previous pack kept as a backup)
Stage 'publish' {
    # Hard links of the stage's files where the volume allows (no second copy of a GB-sized pack; rrhd always writes a
    # new file and renames it, so a later run never changes a published file), a copy otherwise.
    $candidate = "$Output.candidate-$([guid]::NewGuid().ToString('N'))"
    foreach ($f in @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -notin @('.idx', '.complete', '.tmp', '.part') })) {
        $target = Join-Path $candidate $f.FullName.Substring($stage.Length + 1)
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        try { New-Item -ItemType HardLink -Path $target -Target $f.FullName -ErrorAction Stop | Out-Null }
        catch { Copy-Item -LiteralPath $f.FullName -Destination $target }
    }
    $files = @(Get-ChildItem -LiteralPath $candidate -Recurse -File | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($candidate.Length + 1).Replace('\', '/'); bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $profileSha = (Get-Content -LiteralPath (Join-Path $candidate 'profile.txt') -TotalCount 1).Trim()
    [ordered]@{ format = 1; exeSha1 = $profileSha; model = 'realesrgan-x4plus'; scale = 4; fonts = 'palette-contours4x-xbr'; hud = 'palette-contours4x-xbr';
                films = 'bounded-10-levels-held-3'; media = $HdMedia; files = $files } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $candidate 'manifest.json') -Encoding UTF8
    New-Item -ItemType Directory -Force -Path (Split-Path $Output) | Out-Null
    if (Test-Path -LiteralPath $Output) {
        $backup = "$Output.backup-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        Move-Item -LiteralPath $Output -Destination $backup
        Write-Host "   the previous pack is kept in $backup"
    }
    Move-Item -LiteralPath $candidate -Destination $Output
    $mb = [math]::Round(($files | ForEach-Object { $_.bytes } | Measure-Object -Sum).Sum / 1MB)
    Write-Host "   $($files.Count) files, $mb MiB in $Output"
}
$total = ((Get-Date) - $started).TotalSeconds
$timing.Add(('{0,-44} {1,8:N1} s' -f 'TOTAL', $total))
Set-Content -LiteralPath (Join-Path $Cache 'timing.txt') -Value $timing -Encoding UTF8
Write-Host ''
$timing | ForEach-Object { Write-Host "  $_" }
Write-Host "HD media ready in $Output. Switch 'HD textures and media' on in the game (F10 > Image quality, or the VR menu > Graphics)."
Write-Host 'The disc image and the saves were not changed. Intermediate files stay in the cache for inspection and resuming.'
Write-Host 'Quest: scripts\install-quest-hd.ps1 copies this pack to the headset.'
