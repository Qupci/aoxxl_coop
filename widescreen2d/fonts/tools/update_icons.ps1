# Puts a new export of the Figma frame "Action icons" into the font pages and the game.
#   update_icons.ps1 SHEET.png
# SHEET.png: the frame exported from Figma as PNG at 4x (1456 x 96): 13 plates of 24 px, one per
# 28 px column, in the frame's order. The pages of all five language packs are rebuilt
# (coop\widescreen2d\fonts\arial24-windows0/1_LL.png) and copied to Game Files\xxl_widescreen2d.
param([Parameter(Mandatory = $true)][string]$Sheet)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$fonts = Split-Path $PSScriptRoot
$game  = Join-Path (Split-Path (Split-Path (Split-Path $fonts))) 'Game Files\xxl_widescreen2d'

$img = [System.Drawing.Image]::FromFile((Resolve-Path $Sheet))
$size = "$($img.Width)x$($img.Height)"
$img.Dispose()
if ($size -ne '1456x96') { throw "the sheet is $size, not 1456x96: export the whole 'Action icons' frame at 4x" }

# The renderer, built when missing or older than its source.
$exe = Join-Path $env:TEMP 'xxl_font_render.exe'
$src = Join-Path $PSScriptRoot 'render.cs'
if (-not (Test-Path $exe) -or (Get-Item $exe).LastWriteTime -lt (Get-Item $src).LastWriteTime) {
	& "$env:SystemRoot\Microsoft.NET\Framework\v4.0.30319\csc.exe" -nologo -r:System.Drawing.dll "-out:$exe" $src
	if ($LASTEXITCODE) { throw 'render.cs did not build' }
}

$sheetPath = Join-Path $fonts 'icons\sheet_x4.png'
if ((Resolve-Path $Sheet).Path -ne (Resolve-Path $sheetPath).Path) { Copy-Item $Sheet $sheetPath -Force }
foreach ($l in '00', '01', '02', '03', '04') {
	$p0 = Join-Path $fonts "arial24-windows0_$l.png"
	$p1 = Join-Path $fonts "arial24-windows1_$l.png"
	& $exe icons $p0 $sheetPath (Join-Path $fonts "icons\list0_$l.txt") $p0
	if ($LASTEXITCODE) { throw "page 0 of pack $l failed" }
	& $exe icons blank $sheetPath (Join-Path $fonts "icons\list1_$l.txt") $p1
	if ($LASTEXITCODE) { throw "page 1 of pack $l failed" }
	Copy-Item $p0, $p1 $game -Force
	"pack $l done"
}
"pages copied to $game -- restart the game to see them"
