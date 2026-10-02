param(
    [string]$BuildDir = 'build_release',
    [string]$Output,
    [string]$Apk,
    [string]$OpenXRLoader,
    [string]$AndroidSdk,
    [string]$SourceManifest
)
# Builds the player package: prebuilt rrgame.exe / rrtool.exe, the installers, the docs and license
# texts, a release-manifest.json with the SHA-256 of every file, then a verified ZIP and its .sha256.
# NO game data: the player installs from their own disc image. The package is always fully usable for the
# desktop game; PCVR needs -OpenXRLoader (the official x64 Khronos openxr_loader.dll) and Quest needs
# -Apk (from scripts\build-quest.ps1) - both require the VR build. Without them the package still
# contains INSTALL-PCVR.bat / INSTALL.bat, which say so and stop.
#   default output: dist\RoadRashJailbreak-<version>  (+ .zip, + .zip.sha256)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
function Full([string]$p) { if ([IO.Path]::IsPathRooted($p)) { return [IO.Path]::GetFullPath($p) }; return [IO.Path]::GetFullPath((Join-Path $repo $p)) }
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(rrjb VERSION ([0-9]+\.[0-9]+\.[0-9]+) ') { throw 'Cannot determine the version from CMakeLists.txt.' }
$version = $Matches[1]
if (!$Output) { $Output = "dist/RoadRashJailbreak-$version" }
$Output = Full $Output
$BuildDir = Full $BuildDir
$archivePath = $Output + '.zip'
if ((Test-Path -LiteralPath $Output) -or (Test-Path -LiteralPath $archivePath)) { throw 'Output already exists. Choose a new directory.' }
& (Join-Path $PSScriptRoot 'audit-source.ps1') -Repo $repo
$sourceManifestHash = $null
if ($SourceManifest) {
    # The package must be built from exactly the exported source.
    $SourceManifest = Full $SourceManifest
    $snapshot = Get-Content -LiteralPath $SourceManifest -Raw | ConvertFrom-Json
    if ($snapshot.version -ne $version) { throw 'Unexpected source snapshot version.' }
    $sourceFiles = @(& (Join-Path $PSScriptRoot 'source-files.ps1') -Repo $repo)
    if (@(Compare-Object $sourceFiles @($snapshot.files.path)).Count) { throw 'Source snapshot inventory differs from the working source.' }
    foreach ($entry in $snapshot.files) {
        if ((Get-FileHash -LiteralPath (Join-Path $repo $entry.path) -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Source changed after export: $($entry.path)" }
    }
    $sourceManifestHash = (Get-FileHash -LiteralPath $SourceManifest -Algorithm SHA256).Hash.ToLowerInvariant()
}
foreach ($name in @('rrgame.exe', 'rrtool.exe', 'rrhd.exe')) { if (!(Test-Path -LiteralPath (Join-Path $BuildDir $name))) { throw "Missing Windows executable: $BuildDir\$name. Build it: cmd /c build.cmd <dir>" } }
$cache = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') { throw 'The Windows executables must come from a Release build.' }
if ($cache -notmatch ('(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=' + [regex]::Escape($repo.Replace('\', '/')) + '\r?$')) {
    throw 'The Windows build belongs to a different source folder.'
}
$apkName = "RoadRashJailbreak-VR-$version.apk"
if ($Apk) {
    $Apk = Full $Apk
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $apkZip = [IO.Compression.ZipFile]::OpenRead($Apk)
    try {
        if (!($apkZip.Entries | Where-Object FullName -EQ 'AndroidManifest.xml')) { throw 'The APK has no AndroidManifest.xml.' }
        $libs = @($apkZip.Entries | Where-Object FullName -Match '^lib/' | ForEach-Object FullName)
        if ($libs.Count -ne 2 -or 'lib/arm64-v8a/librrgame.so' -notin $libs -or 'lib/arm64-v8a/libopenxr_loader.so' -notin $libs) {
            throw 'The Quest APK must contain only the ARM64 game and OpenXR loader.'
        }
        if (@($apkZip.Entries | Where-Object FullName -Match '^assets/').Count) { throw 'Unexpected APK assets.' }
        $payload = @($apkZip.Entries | Where-Object { $_.FullName -match '(?i)\.(bin|cue|iso|mcr|geo|tim|str|stp|png)$' -and $_.FullName -notmatch '^res/' })
        if ($payload.Count) { throw ('Unexpected payload in the APK (game data?): ' + (($payload | ForEach-Object FullName) -join ', ')) }
    } finally { $apkZip.Dispose() }
    if (!$AndroidSdk) { throw 'Supply -AndroidSdk to verify the Quest release signature and metadata.' }
    if ($AndroidSdk) {
        $apksigner = @(Get-ChildItem -LiteralPath (Join-Path $AndroidSdk 'build-tools') -Recurse -Filter 'apksigner.bat' | Sort-Object FullName -Descending)
        if (!$apksigner.Count) { throw 'apksigner not found in the Android SDK.' }
        & $apksigner[0].FullName verify $Apk
        if ($LASTEXITCODE -ne 0) { throw 'APK signature is invalid.' }
        $aapt = Join-Path (Split-Path $apksigner[0].FullName) 'aapt.exe'
        $badging = @(& $aapt dump badging $Apk) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $badging -notmatch "package: name='com.rrjb.vr'" -or
            $badging -notmatch ("versionName='" + [regex]::Escape($version) + "'") -or
            $badging -notmatch "versionCode='11'" -or $badging -match 'application-debuggable') {
            throw 'Quest release metadata is wrong or the APK is debuggable.'
        }
    }
}
if ($OpenXRLoader -and !(Test-Path -LiteralPath $OpenXRLoader -PathType Leaf)) { throw 'Supply the official x64 Khronos openxr_loader.dll.' }

foreach ($dir in @('', 'tools', 'scripts', 'docs', 'LICENSES')) { New-Item -ItemType Directory -Force -Path (Join-Path $Output $dir) | Out-Null }
foreach ($name in @('rrgame.exe', 'rrtool.exe', 'rrhd.exe')) { Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $Output "tools/$name") }
if ($OpenXRLoader) { Copy-Item -LiteralPath $OpenXRLoader -Destination (Join-Path $Output 'tools/openxr_loader.dll') }
if ($Apk) { Copy-Item -LiteralPath $Apk -Destination (Join-Path $Output $apkName) }
foreach ($name in @('install-player.ps1', 'install.ps1', 'install-quest-player.ps1', 'platform-tools.ps1', 'transfer-saves.ps1',
                    'prepare-hd.ps1', 'prepare-hd-wizard.ps1', 'install-quest-hd.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $Output "scripts/$name")
}
foreach ($name in @('README.md', 'THIRD_PARTY.md', 'LICENSE', 'CHANGELOG.md', 'TRANSFER_QUEST_SAVES_TO_PC.bat', 'TRANSFER_PC_SAVES_TO_QUEST.bat', 'PREPARE-HD.bat')) {
    Copy-Item -LiteralPath (Join-Path $repo $name) -Destination (Join-Path $Output $name)
}
foreach ($name in @('PLAYER-INSTALL.md', 'PCVR.md', 'QUEST.md', 'SAVE-TRANSFER.md', 'VALIDATION.md', 'HD-MEDIA.md')) {
    Copy-Item -LiteralPath (Join-Path $repo "docs/$name") -Destination (Join-Path $Output "docs/$name")
}
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $Output 'LICENSES/RoadRashJailbreak-MIT.txt')
if ($OpenXRLoader -or $Apk) { Copy-Item -LiteralPath (Join-Path $repo 'third_party/openxr/LICENSE') -Destination (Join-Path $Output 'LICENSES/OpenXR-Apache-2.0.txt') }
$licences = @{
    'UltimateXR-MIT.txt' = 'third_party/vrhands/ULTIMATEXR_LICENSE.txt'
    'MiamiVR-MIT.txt' = 'third_party/vrhands/MIAMIVR_LICENSE.txt'
    'stb-LICENSE.txt' = 'third_party/stb/LICENSE.txt'
    'xBR-MIT.txt' = 'third_party/xbr/LICENSE.txt'
}
New-Item -ItemType Directory -Force -Path (Join-Path $Output 'docs/images') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'docs/images/gameplay.png') -Destination (Join-Path $Output 'docs/images/gameplay.png')
if ($OpenXRLoader) { $licences['JsonCpp-LICENSE.txt'] = 'third_party/openxr/JSONCPP-LICENSE.txt' }
foreach ($name in $licences.Keys) { Copy-Item -LiteralPath (Join-Path $repo $licences[$name]) -Destination (Join-Path $Output "LICENSES/$name") }
if ($Apk) {
    foreach ($name in @('NOTICE', 'NOTICE.toolchain')) {
        Copy-Item -LiteralPath (Join-Path $AndroidSdk "ndk/27.2.12479018/$name") -Destination (Join-Path $Output "LICENSES/Android-NDK-$name.txt")
    }
}
foreach ($target in @('Quest', 'PC', 'PCVR')) {
    $name = if ($target -eq 'Quest') { 'INSTALL.bat' } elseif ($target -eq 'PCVR') { 'INSTALL-PCVR.bat' } else { 'INSTALL-PC.bat' }
    $body = @'
@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install-player.ps1" -Target TARGET %*
if errorlevel 1 (
  echo Installation failed. Existing game files and saves were kept.
  pause
  exit /b 1
)
echo Installation completed.
pause
'@.Replace('TARGET', $target)
    [IO.File]::WriteAllText((Join-Path $Output $name), $body.Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.Encoding]::ASCII)
}
# Nothing but the files named above may be in the package.
$files = @(Get-ChildItem -LiteralPath $Output -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($Output.Length + 1).Replace('\', '/'); bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$unexpected = @($files.path | Where-Object { $_ -match '(?i)\.(bin|cue|iso|img|mcr|mcd|srm|png|jpe?g|geo|tim|str|stp|dat)$' -and $_ -ne 'docs/images/gameplay.png' })
if ($unexpected.Count) { throw ('Game data in the player package: ' + ($unexpected -join ', ')) }
[ordered]@{ name = 'Road Rash: Jailbreak PC & VR'; version = $version; buildType = 'Release'; questVersionCode = 11; sourceProvenance = 'filesystem-sha256'; sourceManifestSha256 = $sourceManifestHash
    pcvr = [bool]$OpenXRLoader; quest = [bool]$Apk; package = 'com.rrjb.vr'; files = $files } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Output 'release-manifest.json') -Encoding UTF8
& (Join-Path $Output 'scripts/install-player.ps1') -VerifyOnly
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$prefix = [IO.Path]::GetFileName($Output) + '/'
$all = @($files.path) + 'release-manifest.json'
$archive = [IO.Compression.ZipFile]::Open($archivePath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($relative in $all) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $Output $relative), ($prefix + $relative), [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $entries = @($zip.Entries | Where-Object { $_.Name })
    if ($entries.Count -ne $all.Count) { throw 'ZIP file count mismatch.' }
    foreach ($entry in $entries) {
        if (!$entry.FullName.StartsWith($prefix)) { throw 'Unexpected ZIP root.' }
        $relative = $entry.FullName.Substring($prefix.Length)
        if ($relative -notin $all) { throw "Unexpected ZIP file: $relative" }
        $stream = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') } finally { $stream.Dispose(); $sha.Dispose() }
        if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $Output $relative) -Algorithm SHA256).Hash) { throw "ZIP hash mismatch: $relative" }
    }
} finally { $zip.Dispose() }
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($archivePath + '.sha256'), $hash + '  ' + [IO.Path]::GetFileName($archivePath) + [Environment]::NewLine, [Text.Encoding]::ASCII)
Write-Host "Verified player package: $Output (desktop; PCVR $([bool]$OpenXRLoader); Quest $([bool]$Apk))"
Write-Host "Verified ZIP: $archivePath ($hash)"
