# Rebuilds the analysis base from scratch or refreshes it:
#   1. (optional) imports + auto-analyses Game Files\axxl.exe into coop\ghidra
#   2. NameClasses.java  - names every engine class vtable/method + known functions
#   3. ExportDecomp.java - writes pseudo-C for every function to coop\decomp
#
# Usage:  .\rebuild_decomp.ps1            # names + export (project must exist)
#         .\rebuild_decomp.ps1 -Import    # full rebuild, ~4 min import + export
param([switch]$Import, [int]$Threads = 4)

$ErrorActionPreference = "Stop"
$root    = Split-Path -Parent $PSScriptRoot           # coop\
$ghidra  = "F:\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat"
$proj    = Join-Path $root "ghidra"
$scripts = $PSScriptRoot
$exe     = Join-Path (Split-Path -Parent $root) "Game Files\axxl.exe"
$classes = Join-Path (Split-Path -Parent $root) "elbtools\pykedit\classes_ax1.txt"
$decomp  = Join-Path $root "decomp"
$env:GHIDRA_HEADLESS_MAXMEM = "4G"

if ($Import) {
    New-Item -ItemType Directory -Force $proj | Out-Null
    & $ghidra $proj axxl -import $exe -overwrite -log (Join-Path $proj "import.log") 2>&1 |
        Select-String "ERROR|REPORT" | Select-Object -Last 10
}

New-Item -ItemType Directory -Force $decomp | Out-Null
& $ghidra $proj axxl -process axxl.exe -noanalysis -scriptPath $scripts `
    -postScript NameClasses.java $classes (Join-Path $decomp "vtables.tsv") 2>&1 |
    Select-String "NameClasses|ERROR|Exception" | Select-Object -Last 20

& $ghidra $proj axxl -process axxl.exe -noanalysis -readOnly -scriptPath $scripts `
    -postScript ExportDecomp.java $decomp $Threads 2>&1 |
    Select-String "ExportDecomp|ERROR|Exception" | Select-Object -Last 20
