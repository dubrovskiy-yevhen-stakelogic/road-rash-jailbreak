param([string]$Repo, [switch]$FileSystem, [switch]$AllowLocalState)
# Refuses a source snapshot that carries anything but project source: disc images,
# the game's own files, BIOS, memory cards and savestates, RAM dumps, screenshots, executables and build
# outputs, signing keys, work\ contents, extracted tables and decompiled or disassembled game code.
# Path, type and size rules first; then every file is read: text only, and no decompiler output,
# disassembly listings, hex dumps or large numeric tables. The hash manifests under tests\ are allowed, and so are
# reviewed binary files by path and SHA-256: the VR hands and the README gameplay screenshot (THIRD_PARTY.md).
# Works on the repository and on an exported kit folder alike (-Repo <folder>).
$ErrorActionPreference = 'Stop'
if (!$Repo) { $Repo = Split-Path $PSScriptRoot }
$Repo = (Get-Item -LiteralPath $Repo).FullName
Push-Location $Repo
try {
    $files = @(& (Join-Path $PSScriptRoot 'source-files.ps1') -Repo $Repo -FileSystem:$FileSystem)
    if (!$files.Count) { throw 'No source files found for auditing.' }
    foreach ($required in @('LICENSE', 'README.md', 'THIRD_PARTY.md', 'CMakeLists.txt', 'build.cmd', 'scripts/audit-source.ps1', 'scripts/source-files.ps1')) {
        if ($files -notcontains $required) { throw "Missing $required in the source snapshot." }
    }
    if ((Get-Content -LiteralPath LICENSE -Raw) -notmatch 'MIT License') { throw 'LICENSE is not the project MIT license.' }
    # An exported kit (it has SOURCE-MANIFEST.json) must hold exactly the manifest's files, unchanged:
    # nothing the file list would skip (work\, build outputs, internal notes) may hide in it.
    $kitManifest = Join-Path $Repo 'SOURCE-MANIFEST.json'
    if (Test-Path -LiteralPath $kitManifest) {
        $listed = @{}
        foreach ($entry in (Get-Content -LiteralPath $kitManifest -Raw | ConvertFrom-Json).files) { $listed[$entry.path] = $entry.sha256 }
        $present = @(Get-ChildItem -LiteralPath $Repo -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($Repo.Length + 1).Replace('\', '/') } |
            Where-Object { $_ -ne 'SOURCE-MANIFEST.json' -and (!$AllowLocalState -or $_ -notmatch '^\.git(/|$)') })
        $extra = @($present | Where-Object { !$listed.ContainsKey($_) })
        if ($extra.Count) { throw ('Files outside SOURCE-MANIFEST.json in the kit: ' + ($extra -join ', ')) }
        foreach ($path in $listed.Keys) {
            $full = Join-Path $Repo $path
            if (!(Test-Path -LiteralPath $full -PathType Leaf)) { throw "Kit file missing: $path" }
            if ((Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash -ne $listed[$path]) { throw "Kit file changed after export: $path" }
        }
    }

    # Folders that only ever hold data, installs or build products.
    $forbidden = '(?i)^(work|runtime|install|saves|dist|tr-tmp|build[^/]*)/|^android/(\.gradle/|build/|app/(build|\.cxx)/|local\.properties$)|(^|/)(bin|obj|__pycache__|\.vs|\.claude|\.codex|\.agents)/'
    # Disc images, cards, savestates, RAM/VRAM dumps, media, executables, libraries, packages, keys, Ghidra databases.
    $forbidden += '|(?i)\.(bin|cue|iso|img|chd|ccd|sub|mdf|mds|nrg|ecm|pbp|cso|toc|m3u|mcr|mcd|srm|sav|gme|vmp|psv|mcs|state\d*|ss\d|st\d|oops|ram|vram|spuram|dmp|dump|mem|raw|raw2352|png|jpe?g|bmp|gif|tga|webp|wav|mp3|ogg|flac|avi|mp4|mkv|mov|str|xa|vab|vag|vh|vb|seq|exe|dll|lib|pdb|ilk|exp|obj|o|a|so|dylib|apk|aab|aar|dex|jar|class|pyc|zip|7z|rar|gz|xz|keystore|jks|p12|pfx|pem|key|gpr|rep|gzf|csv|tsv|tbl)$'
    # The game's own file types (every extension on the SLUS-01053 disc) and its well-known names.
    $forbidden += '|(?i)\.(geo|stp|tim|ph|tex|psx|wve|mro|pfn|dat|ca|bi|alb|vi|tcm|rls|map|loc|grf|vuk|qti|psh|gtp|fil|en|cnf|53)$'
    $forbidden += '|(?i)(^|/)(SLUS_\d{3}\.\d{2}|RASHCD[FGI]\b[^/]*|ROADGRF\d*\.TXT|SYSTEM\.CNF|SCPH\d+[^/]*|[^/]*bios[^/]*\.(bin|rom))$'
    # Internal working notes and local paths stay private, as in the GT2 kit.
    $forbidden += '|(?i)(^|/)(AGENTS|CL[A]UDE|HANDOFF)\.md$|(^|/)install-location\.txt$|(^|/)local\.properties$|credential\.xml$'
    # Reviewed binaries, allowed by exact path AND SHA-256: the VR hands embedded into rrgame and the
    # documentation-only gameplay screenshot. A changed, renamed or added image needs its own review.
    $allowedBinary = @{
        'third_party/vrhands/BigHandLeft.uxrh'   = '6BB32E6F79ED6DBA401BD36DD39D3178F8259096F889EF01C870D057134EB535'
        'third_party/vrhands/BigHandRight.uxrh'  = '48B14A77EFEA68027F5AE6723994CADC06DAC58708ECB7339CC16ECB1ADE2478'
        'third_party/vrhands/BigHandsAlbedo.png' = '44D2DF3FB67FA65AAA448F2A8847809D0A24E8400445D2720759FCA4232A3D9A'
        'docs/images/gameplay.png' = '73C2CA5671E9369A3B3EC89AA8CC8D476C935825A0B3F5B7CD28503C97C85978'
    }
    $allowed = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($path in $allowedBinary.Keys) {
        if ($files -notcontains $path) { continue }
        $hash = (Get-FileHash -LiteralPath (Join-Path $Repo $path) -Algorithm SHA256).Hash
        if ($hash -ne $allowedBinary[$path]) { throw "Allowed asset changed (SHA-256 $hash): $path" }
        [void]$allowed.Add($path)
    }
    $bad = @($files | Where-Object { $_ -match $forbidden -and !$allowed.Contains($_) })
    if ($bad.Count) { throw ('Non-source files in the snapshot: ' + ($bad -join ', ')) }

    $textType = '(?i)\.(cpp|h|hpp|c|inc|py|ps1|psm1|cmd|bat|sh|md|txt|json|yaml|yml|java|kt|kts|gradle|properties|xml|pro|cmake|glsl|vert|frag|comp|hlsl|in)$'
    $textName = '^(LICENSE|NOTICE|\.gitignore|\.gitattributes|CMakeLists\.txt)$'
    $decompiled = [regex]'\b(undefined[1248]?|FUN_[0-9a-fA-F]{8}|DAT_[0-9a-fA-F]{8}|LAB_[0-9a-fA-F]{8}|PTR_\w*_?[0-9a-fA-F]{8}|in_[a-z]{1,2}\d?_\w*|unaff_\w+|extraout_\w+)\b'
    $disassembly = [regex]'(?m)^\s*(0x)?8[0-9a-fA-F]{7}:?\s+[0-9a-fA-F]{8}\s+[a-z]'
    $hexdump = [regex]'(?m)^[^\r\n]*?([0-9a-fA-F]{2} ){15}[0-9a-fA-F]{2}'
    $table = [regex]'(?m)^[^\r\n]*?((0x[0-9A-Fa-f]+|-?\d+)[uUlL]*[ \t]*,[ \t]*){8,}'
    # Development-process text: the public source reads as a finished codebase, not a work log. None of these may
    # appear in a shipped text file - section tags of the internal notes, iteration names, process roles, dated
    # narrative, machine paths, pointers to the unshipped research notes, development build folders, non-English
    # text, unreleased version numbers. Upstream code under third_party\ is exempt (its own changelogs carry dates);
    # the project's own notes there (*.md) are not. The patterns are written so that they do not match their own
    # source text here.
    $research = 'ai|bike_step|camera|collision|crash_chain|edges|exe_overview|frame_trace|frontend_runtime|graphics_settings|' +
        'hazards|hud|input|interpreter|mdec|model_draw|multiplayer|oracle_from_vr_capture|physics|population_runtime|' +
        'race_loop|rider_anim|road_query|road_rebind|road_runtime|rrgame|savestate|sound|spine_seams|stream|vr_port_plan'
    $process = [ordered]@{
        'internal-notes tag'     = 'HANDOFF[\s.:(]*\d|HANDOFF\.md|CL[A]UDE\.md|AGENTS\.md|\bPLAN\.md'
        'iteration tag'          = '(?<![\w.=#x\-])1[a-c][a-z](?!\w)'
        'iteration name'         ='(?i)(?<!\b(sine|square|triangle|sawtooth|pulse|sound|standing|carrier) )\bwaves?\b'
        'process role'           = '(?i)\bcoordinator\b|\bthe user\b|\bmy mistake\b|\bI was wrong\b|(?<![.\\/])\b(?-i:[Ss]ub-?)?(?-i:[Aa]gents?)\b'
        'assistant name'         = '(?i)(?<![.\\/])\b(Cl[a]ude|Anthrop[i]c|ChatG[P]T|OpenA[I]|C[o]dex)\b'
        'assistant narrative'    = '(?i)\bas an A[I]\b|\bI (th[i]nk|need t[o]|should)\b|\blet.s (impl[e]ment|f[i]x)\b|\bwe need t[o]\b'
        'user narrative'         = '(?i)\buser (ask[e]d|w[a]nts|requ[e]sted|pref[e]rs|s[a]id)\b'
        'dated narrative'        = '\b20[2-3]\d-[01]\d-[0-3]\d\b'
        'machine path'           = '(?i)\bC:[\\/]+Dev\b'
        'unshipped research doc' = "(?i)docs[\\/]+research|\b($research)\.md\b"
        'scratch folder'         = '(?i)\bwork[\\/]+w\d+\b'
        'development build dir'  = '\bbuild_[a-z0-9_]+(?=[\\/])|\bbuild_(m\d|gfx|vr|quest|local|clean|rel\d|w\d)\w*\b'
        'non-English text'       = '[\u0400-\u04FF]'
        'unreleased version'     = '(?<![\w.])0\.1\.[1-9](?![\w.])'
    }
    # Legitimate uses, by path and the exact text matched: the build folders the scripts create by default, and two
    # values that are part of a file or switch format (the [vr] combat_defaults marker, the RRJB_LAYERS control).
    $processAllowed = @(
        @{ Path = '^tools/rrgame/vr_melee_settings\.h$'; Text = '^1c[r]$' },
        @{ Path = '^src/render/race_scene_pc\.cpp$'; Text = '^1b[x]$' },
        @{ Path = '^(scripts/build-quest\.ps1|android/app/build\.gradle)$'; Text = '^build_(quest|vr)$' },
        @{ Path = '^scripts/(install|prepare-hd|prepare-hd-wizard)\.ps1$'; Text = '^build_install$' },
        @{ Path = '^scripts/package-player\.ps1$'; Text = '^build_release$' },
        @{ Path = '^(play\.bat|tests/run_gates\.ps1|tools/scout/[a-z_]+\.py)$'; Text = '^build_m[0]$' }
    )
    $problems = New-Object 'System.Collections.Generic.List[string]'
    foreach ($path in $files) {
        if ($allowed.Contains($path)) { continue } # the hash-pinned binary assets (above)
        $item = Get-Item -LiteralPath $path -Force
        if ($item.Length -gt 2MB) { $problems.Add("unexpectedly large source file: $path"); continue }
        if ($path -notmatch $textType -and $item.Name -notmatch $textName) { $problems.Add("unexpected source file type: $path"); continue }
        $bytes = [IO.File]::ReadAllBytes($item.FullName)
        if ([Array]::IndexOf($bytes, [byte]0) -ge 0) { $problems.Add("binary data in a source text file: $path"); continue }
        $text = [Text.Encoding]::UTF8.GetString($bytes)
        $n = $decompiled.Matches($text).Count
        if ($n -gt 20) { $problems.Add("decompiler output markers ($n) in $path") }
        $n = $disassembly.Matches($text).Count
        if ($n -gt 40) { $problems.Add("disassembly listing ($n lines) in $path") }
        $n = $hexdump.Matches($text).Count
        if ($n -gt 40) { $problems.Add("hex dump ($n lines) in $path") }
        if ($path -notmatch '^third_party/') {
            $n = $table.Matches($text).Count
            if ($n -gt 48) { $problems.Add("large numeric table ($n rows) in $path") }
        }
        foreach ($rule in $process.Keys) {
            if ($path -match '^third_party/' -and $path -notmatch '\.md$') { break }
            foreach ($m in [regex]::Matches($text, $process[$rule])) {
                $ok = $false
                foreach ($a in $processAllowed) { if ($path -match $a.Path -and $m.Value -match $a.Text) { $ok = $true; break } }
                if ($ok) { continue }
                $line = $text.Substring(0, $m.Index).Split("`n").Count
                $problems.Add("development-process text ($rule) at ${path}:${line}: '$($m.Value)'")
            }
        }
    }
    if ($problems.Count) { throw ("Source audit failed:`r`n  " + ($problems -join "`r`n  ")) }
    Write-Host "Source audit passed: $($files.Count) files; reviewed binary assets match pinned hashes; no unrelated game payloads, dumps, binaries, keys, decompiled code or development-process text."
} finally { Pop-Location }
