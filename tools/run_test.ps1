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
    [switch]$NoDeploy
)
$ErrorActionPreference = "Stop"
$coop = Split-Path -Parent $PSScriptRoot
$game = Join-Path (Split-Path -Parent $coop) "Game Files"
$shots = Join-Path $game "shots"

Get-Process axxl -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
if (-not $NoDeploy) { Copy-Item (Join-Path $coop "build\xxl_coop.asi") $game -Force }
if (Test-Path $shots) { Remove-Item (Join-Path $shots "*") -Force -ErrorAction SilentlyContinue }

$env:XXL_AUTOTEST = $Scenario
if ($Level -ge 0) { $env:XXL_LEVEL = "$Level" }
try {
    $p = Start-Process -FilePath (Join-Path $game "axxl.exe") -WorkingDirectory $game -PassThru
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
