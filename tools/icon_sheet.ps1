# Lays every PNG of the given folders out on one sheet (on the game's menu blue),
# with its file name under it.  powershell -File icon_sheet.ps1 OUT.png DIR [DIR...]
param([string]$OutFile, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Dirs)
Add-Type -AssemblyName System.Drawing
$files = foreach ($d in $Dirs) { Get-ChildItem $d -Filter *.png | Sort-Object Name }
$cellW = 150; $cellH = 104; $cols = 8
$rows = [math]::Ceiling($files.Count / $cols)
$bmp = New-Object System.Drawing.Bitmap ($cellW * $cols), ($cellH * $rows)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(255, 62, 66, 120))
$g.InterpolationMode = 'HighQualityBicubic'
$font = New-Object System.Drawing.Font "Arial", 10
$i = 0
foreach ($f in $files) {
	$img = [System.Drawing.Image]::FromFile($f.FullName)
	$x = ($i % $cols) * $cellW; $y = [math]::Floor($i / $cols) * $cellH
	$h = 64; $w = [int]($img.Width * $h / $img.Height)
	if ($w -gt $cellW - 8) { $w = $cellW - 8; $h = [int]($img.Height * $w / $img.Width) }
	$g.DrawImage($img, [int]($x + ($cellW - $w) / 2), $y + 8, $w, $h)
	$g.DrawString($f.BaseName, $font, [System.Drawing.Brushes]::White, $x + 4, $y + 80)
	$img.Dispose(); $i++
}
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
