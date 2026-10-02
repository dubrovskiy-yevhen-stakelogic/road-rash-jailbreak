param([string]$Output)
# Synthetic cards only. Exercises binary ADB streams and atomic PC replacement without a headset.
$ErrorActionPreference = 'Stop'
if (!$Output) { $Output = Join-Path ([IO.Path]::GetTempPath()) ('rrjb-save-test-' + [guid]::NewGuid().ToString('N')) }
[IO.Directory]::CreateDirectory($Output) | Out-Null
. (Join-Path (Split-Path $PSScriptRoot) 'scripts/transfer-saves.ps1') -FunctionsOnly
$checks = 0
function Check([bool]$Ok, [string]$Name) {
    if (!$Ok) { throw "Save transfer test failed: $Name" }
    $script:checks++
}
function Refuses([scriptblock]$Action, [string]$Name) {
    $refused = $false
    try { & $Action } catch { $refused = $true }
    Check $refused $Name
}
$card = New-Object byte[] 131072
$card[0] = 77; $card[1] = 67; $card[128] = 0x51
for ($frame = 0; $frame -lt 16; ++$frame) {
    $xor = 0
    for ($i = 0; $i -lt 127; ++$i) { $xor = $xor -bxor $card[$frame * 128 + $i] }
    $card[$frame * 128 + 127] = $xor
}
for ($i = 8192; $i -lt $card.Length; ++$i) { $card[$i] = $i % 256 }
Test-MemoryCard $card
Check $true 'valid synthetic card'
Refuses { Test-MemoryCard (New-Object byte[] 10) } 'truncated card'
$bad = [byte[]]$card.Clone(); $bad[128] = $bad[128] -bxor 1
Refuses { Test-MemoryCard $bad } 'damaged directory'
$empty = New-Object byte[] 131072; $empty[0] = 77; $empty[1] = 67; $empty[127] = 77 -bxor 67
Refuses { Test-MemoryCard $empty } 'empty card'
$source = @'
using System;
public static class ByteEcho {
    public static int Main(string[] args) {
        if (args.Length != 0 && args[0] == "error") { Console.Error.WriteLine("expected failure"); return 7; }
        using (var input = Console.OpenStandardInput())
        using (var output = Console.OpenStandardOutput()) { input.CopyTo(output); }
        return 0;
    }
}
'@
$csharp = Join-Path $Output 'ByteEcho.cs'
[IO.File]::WriteAllText($csharp, $source)
$Adb = Join-Path $Output 'byte-echo.exe'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
& $compiler /nologo /target:exe "/out:$Adb" $csharp
if ($LASTEXITCODE) { throw 'Cannot compile binary stream test helper.' }
$bytes = Invoke-AdbBytes @() $card
Check ((Get-DataHash $bytes) -eq (Get-DataHash $card)) 'all byte values survive native stdin/stdout'
Refuses { Invoke-AdbBytes @('error') $card | Out-Null } 'ADB failure stops transfer'
Refuses { Invoke-AdbBytes @('invalid"argument') $card | Out-Null } 'invalid native argument'
# File replacement uses isolated synthetic files. The process guard is checked by the real transfer entry point.
function Assert-GameClosed { }
$path = Join-Path $Output 'saves/rrjb_card.mcr'
$backup = Join-Path $Output 'before.mcr'
Write-PcCard $path $card 'missing' $backup
Check ((Get-CardHash $path) -eq (Get-DataHash $card)) 'first PC import'
$next = [byte[]]$card.Clone(); $next[8192] = 99
Write-PcCard $path $next (Get-DataHash $card) $backup
Check ((Get-CardHash $backup) -eq (Get-DataHash $card)) 'previous PC card retained'
Check ((Get-CardHash $path) -eq (Get-DataHash $next)) 'replacement readback'
Refuses { Write-PcCard $path $card (Get-DataHash $card) (Join-Path $Output 'wrong.mcr') } 'changed PC destination'
Check ((Get-CardHash $path) -eq (Get-DataHash $next)) 'changed destination preserved'
$guard = [IO.File]::Open((Join-Path (Split-Path $path) '.transfer.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try { Refuses { Write-PcCard $path $card (Get-DataHash $next) $backup } 'concurrent PC transfer' }
finally { $guard.Dispose() }
Write-Host "$checks/$checks save transfer host checks passed"
