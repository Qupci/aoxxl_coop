# Runs one autotest scenario end to end and collects the results.
#
#   .\run_test.ps1 exp1                 # deploy build\xxl_coop.asi, run, collect
#   .\run_test.ps1 p2basic -NoDeploy    # run whatever ASI is already installed
#
# The scenario is passed to the game through the XXL_AUTOTEST environment
# variable, which only this script sets -- a normal launch never runs a test.
# Results (log + screenshots) are copied to coop\testruns\<time>-<scenario>\.
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Scenario,
    [int]$TimeoutSec = 150,
    [int]$Level = -1,
    [switch]$NoDeploy,
    # Another game folder, relative to F:\XXL COOP MOD, e.g. "SPQR\Caesars Challenge" (the modded levels).
    [string]$GameDir = ""
)
$ErrorActionPreference = "Stop"
$coop = Split-Path -Parent $PSScriptRoot
$game = Join-Path (Split-Path -Parent $coop) "Game Files"
if ($GameDir) { $game = (Resolve-Path (Join-Path (Split-Path -Parent $coop) $GameDir)).Path }
$exe = Join-Path $game "axxl.exe"
if (-not (Test-Path $exe)) { $exe = Join-Path $game "Cesar's Challenge - Windowed.exe" }
$shots = Join-Path $game "shots"

Get-Process axxl, "Cesar's Challenge - Windowed" -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }

# The game keeps only three logs (xxl_coop.log, .1, .2) and every test run
# rotates them: save any log from a real play session (no "autotest:" line in
# it) to coop\userlogs\ first, so a test never pushes one out.
$userLogs = Join-Path $coop "userlogs"
foreach ($name in "xxl_coop.log", "xxl_coop.1.log", "xxl_coop.2.log") {
    $f = Join-Path $game $name
    if (-not (Test-Path $f)) { continue }
    if (Select-String -Path $f -Pattern "autotest:" -SimpleMatch -Quiet) { continue }
    New-Item -ItemType Directory -Force $userLogs | Out-Null
    $stamp = (Get-Item $f).LastWriteTime.ToString("yyyyMMdd-HHmmss")
    $dest = Join-Path $userLogs "$stamp-$name"
    if (-not (Test-Path $dest)) { Copy-Item $f $dest; "saved play log: $dest" }
}
if (-not $NoDeploy) {
    Copy-Item (Join-Path $coop "build\xxl_coop.asi") $game -Force
    # The co-op icons; a PNG already there (a designer's) is kept.
    $icons = Join-Path $game "coop_icons"
    New-Item -ItemType Directory -Force $icons | Out-Null
    Get-ChildItem (Join-Path $coop "icons") -Filter *.png | Where-Object { -not (Test-Path (Join-Path $icons $_.Name)) } |
        Copy-Item -Destination $icons
}
if (Test-Path $shots) { Remove-Item (Join-Path $shots "*") -Force -ErrorAction SilentlyContinue }

$env:XXL_AUTOTEST = $Scenario
if ($Level -ge 0) { $env:XXL_LEVEL = "$Level" }
try {
    $p = Start-Process -FilePath $exe -WorkingDirectory $game -PassThru
} finally {
    Remove-Item Env:XXL_AUTOTEST -ErrorAction SilentlyContinue
    Remove-Item Env:XXL_LEVEL -ErrorAction SilentlyContinue
}

if ($p.WaitForExit($TimeoutSec * 1000)) {
    $status = "exited (code $($p.ExitCode))"
} else {
    $p.Kill(); $p.WaitForExit(5000) | Out-Null
    $status = "TIMEOUT after $TimeoutSec s (killed)"
}

$tag = if ($Level -ge 0) { "$Scenario-L$Level" } else { $Scenario }
$dest = Join-Path $coop ("testruns\" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + $tag)
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $game "xxl_coop.log") $dest
if (Test-Path $shots) { Copy-Item (Join-Path $shots "*") $dest -ErrorAction SilentlyContinue }

"scenario: $Scenario"
"status:   $status"
"results:  $dest"
