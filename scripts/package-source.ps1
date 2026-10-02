param([string]$Output, [switch]$NoZip, [switch]$FileSystem)
# Exports the public source kit: the audited file list (source-files.ps1) copied into a new folder with a
# SHA-256 of every file in SOURCE-MANIFEST.json, the exported folder audited again, then a ZIP whose every
# entry is re-hashed against the folder and a .sha256 beside it.
#   default output: dist\RoadRashJailbreak-source-<version>  (+ .zip, + .zip.sha256)
# The repository is not a Git checkout: the manifest records a file-system SHA-256 snapshot and no commit.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(rrjb VERSION ([0-9]+\.[0-9]+\.[0-9]+) ') { throw 'Cannot determine the source version from CMakeLists.txt.' }
$sourceVersion = $Matches[1]
if (!$Output) { $Output = Join-Path $repo "dist/RoadRashJailbreak-source-$sourceVersion" }
$Output = [IO.Path]::GetFullPath($Output)
$archivePath = $Output + '.zip'
if (Test-Path -LiteralPath $Output) { throw 'The source output folder already exists. Choose a new folder.' }
if (!$NoZip -and (Test-Path -LiteralPath $archivePath)) { throw "The archive already exists: $archivePath" }
if ($Output.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase) -and $Output -notmatch '\\(dist|work)\\') {
    throw 'Inside the source tree the kit may only be written under dist\ or work\ (they are excluded from the file list).'
}
& (Join-Path $PSScriptRoot 'audit-source.ps1') -Repo $repo
Push-Location $repo
try {
    $files = @(& (Join-Path $PSScriptRoot 'source-files.ps1') -Repo $repo)
    if (!$files.Count) { throw 'Cannot list source files.' }
    New-Item -ItemType Directory -Force -Path $Output | Out-Null
    $manifest = @()
    foreach ($file in $files) {
        $destination = Join-Path $Output $file
        New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
        Copy-Item -LiteralPath $file -Destination $destination
        $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()) { throw "Source copy mismatch: $file" }
        $manifest += [ordered]@{ path = $file; bytes = (Get-Item -LiteralPath $destination).Length; sha256 = $hash }
    }
    [ordered]@{ name = 'Road Rash: Jailbreak PC & VR'; version = $sourceVersion; sourceCommit = $null; sourceDirty = $null
        sourceProvenance = 'filesystem-sha256'; created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'); files = $manifest } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Output 'SOURCE-MANIFEST.json') -Encoding UTF8
} finally { Pop-Location }
# The exported folder must pass the same audit on its own.
& (Join-Path $Output 'scripts/audit-source.ps1') -Repo $Output
Write-Host "Source folder ready: $Output ($($files.Count) files, filesystem SHA-256 snapshot)"
if ($NoZip) { return }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$all = @($files) + 'SOURCE-MANIFEST.json'
$prefix = [IO.Path]::GetFileName($Output) + '/'
$archive = [IO.Compression.ZipFile]::Open($archivePath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($relative in $all) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $Output $relative), ($prefix + $relative),
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
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
Write-Host "Verified ZIP: $archivePath ($hash)"
