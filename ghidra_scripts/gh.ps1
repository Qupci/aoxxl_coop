# Run the DumpFuncs Ghidra script against the already-analysed axxl.exe project.
#
# Usage:  .\gh.ps1 "decomp 0x414B70 | xrefs 0x58FFB0 | imports"
#
# Commands are separated by "|" and turned into the "SEP" token the Java script
# splits on. (PowerShell eats "--" and cmd.exe eats ";", so neither works here.)
param([Parameter(Mandatory = $true, Position = 0)][string]$Script)

$ghidra  = "F:\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat"
$proj    = Join-Path (Split-Path -Parent $PSScriptRoot) "ghidra"
$scripts = "F:\XXL COOP MOD\coop\ghidra_scripts"
$outFile = Join-Path $env:TEMP ("ghdump_" + [guid]::NewGuid().ToString("N") + ".txt")

$tokens = @()
foreach ($part in ($Script -split '\|')) {
    $t = $part.Trim()
    if ($t -eq "") { continue }
    if ($tokens.Count -gt 0) { $tokens += "SEP" }
    $tokens += ($t -split '\s+')
}

$scriptArgs = @($outFile) + $tokens

& $ghidra $proj axxl -process axxl.exe -noanalysis -readOnly `
    -scriptPath $scripts -postScript DumpFuncs.java @scriptArgs 2>&1 |
    Where-Object { $_ -match "ERROR|Exception" } | Select-Object -First 20

if (Test-Path $outFile) {
    Get-Content $outFile -Raw
    Remove-Item $outFile -Force
} else {
    Write-Output "(no output file produced)"
}
