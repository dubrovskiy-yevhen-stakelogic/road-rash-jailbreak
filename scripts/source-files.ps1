[CmdletBinding()]
param([string]$Repo, [switch]$FileSystem)
if (!$Repo) { $Repo = Split-Path $PSScriptRoot }
# The public source file list, relative paths with '/' separators, sorted.
# The repository is a working folder, not a Git checkout, so the list is always taken from the file
# system (-FileSystem is accepted for command-line parity with the GT2 kit). It is the tree minus:
#   work\ (all game data, captures, goldens, installs under test), build*\, tr-tmp\, dist\, install\,
#   runtime\, saves\, Android build outputs, editor and assistant-tool folders, Python caches, *.user, local
#   settings, signing keys, install-location.txt, and the internal working notes listed in $internal below.
# audit-source.ps1 then rejects anything in the remaining list that looks like game data, a binary, or
# development-process text.
$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath $Repo).FullName.TrimEnd('\', '/')
$skipTop = '^(work|build[^/\\]*|tr-tmp|dist|install|runtime|saves|out|cmake-build-.*)$'
$skipDirectory = '^(\.git|\.vs|\.vscode|\.idea|\.claude|\.codex|\.agents|__pycache__|\.gradle|\.cxx|\.externalNativeBuild)$'
$skipAndroid = '^android/(build|app/build|app/\.cxx|\.gradle|\.kotlin|captures)$'
$skipFile = '(?i)^(AGENTS|CL[A]UDE|HANDOFF)\.md$|^local\.properties$|^credentials?\.(xml|json)$|^\.env($|\.)|\.user$|\.pyc$|^install-location\.txt$|\.(keystore|jks|p12|pfx|pem|key)$|^(Thumbs\.db|desktop\.ini|\.DS_Store)$'
# Internal working material that stays in the repository but is not part of the public source:
# the project notes and plan, the research logs (the shipped technical reference is docs\formats), the
# reverse-engineering fact database under db\, a stray empty file at the root, and the exploratory Python probes.
# Of tools\scout only the probes that tests\run_gates.ps1 runs, the modules they import, and the two independent
# decoders the texture and audio gates compare the C++ ones against (tex, audio) are shipped.
$scoutShipped = 'anim|anim_dmd3|anim_machine|anim_models|bike_step|camera|cell|chunk|collide|crash|docpaths|engine_note|exe|' +
    'frontend|ground|hud|impact|junction|pairs|pairs_models|psxgpu|raceloop|rmd3|road|roadq|roadrb|roadrt|savestate|shell|' +
    'sound|spawn|spine|spu_regs|tex|audio'
$internal = '^(docs/(PLAN\.md|research/|adr/)|db/|verify_physics\.cpp$|disc\.txt$)' +
    "|^tools/scout/(?!((crash_models|crash_runs|impact_models)/[a-z0-9_]+\.py|($scoutShipped)\.py)$)"
$files = New-Object 'System.Collections.Generic.List[string]'
$pending = New-Object 'System.Collections.Generic.Stack[string]'
$pending.Push($root)
while ($pending.Count) {
    $directory = $pending.Pop()
    foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
        $relative = $item.FullName.Substring($root.Length).TrimStart('\').Replace('\', '/')
        if ($item.PSIsContainer) {
            if ($directory -eq $root -and $item.Name -match $skipTop) { continue }
            if ($item.Name -match $skipDirectory -or $relative -match $skipAndroid) { continue }
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source links need explicit review: $($item.FullName)" }
            $pending.Push($item.FullName)
            continue
        }
        if ($item.Name -eq 'SOURCE-MANIFEST.json' -or $item.Name -match $skipFile -or $relative -match $internal) { continue }
        $files.Add($relative)
    }
}
$files | Sort-Object -Unique
