# Draws the prototype co-op icons (PNG, 64 px high) into coop\icons.
# The mod loads them from Game Files\coop_icons at run time; a designer's PNG
# of the same name replaces one (any size, same aspect).
#   powershell -File make_icons.ps1 [-Out DIR]
param([string]$Out = (Join-Path $PSScriptRoot "..\icons"))

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$SZ = 64

function New-Canvas([int]$w = $SZ, [int]$h = $SZ) {
	$b = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$g = [System.Drawing.Graphics]::FromImage($b)
	$g.SmoothingMode = 'AntiAlias'
	$g.TextRenderingHint = 'AntiAliasGridFit'
	$g.PixelOffsetMode = 'HighQuality'
	return @($b, $g)
}
function Save($c, [string]$name) { $c[1].Dispose(); $c[0].Save((Join-Path $Out "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png); $c[0].Dispose() }
function C([int]$r, [int]$g, [int]$b, [int]$a = 255) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }
function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
	$p = New-Object System.Drawing.Drawing2D.GraphicsPath
	$p.AddArc($x, $y, 2 * $r, 2 * $r, 180, 90); $p.AddArc($x + $w - 2 * $r, $y, 2 * $r, 2 * $r, 270, 90)
	$p.AddArc($x + $w - 2 * $r, $y + $h - 2 * $r, 2 * $r, 2 * $r, 0, 90); $p.AddArc($x, $y + $h - 2 * $r, 2 * $r, 2 * $r, 90, 90)
	$p.CloseFigure(); return $p
}
function Grad($rect, $c1, $c2) { New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $c1, $c2, 90 }
function Text($g, [string]$t, [float]$size, $color, [float]$cx, [float]$cy, [string]$font = "Arial Black") {
	$f = New-Object System.Drawing.Font $font, $size, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
	$sf = New-Object System.Drawing.StringFormat; $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
	$p = New-Object System.Drawing.Drawing2D.GraphicsPath
	$p.AddString($t, $f.FontFamily, [int]$f.Style, $size, (New-Object System.Drawing.RectangleF ($cx - 100), ($cy - 100), 200, 200), $sf)
	$g.DrawPath((New-Object System.Drawing.Pen (C 0 0 0 200), 3), $p)
	$g.FillPath((New-Object System.Drawing.SolidBrush $color), $p)
}
# The game's own icon plate: a dark bevelled disc / square.
function Disc($g, $rim) {
	$g.FillEllipse((New-Object System.Drawing.SolidBrush (C 0 0 0 110)), 4, 6, 57, 57)
	$r = New-Object System.Drawing.RectangleF 3, 3, 57, 57
	$g.FillEllipse((Grad $r (C 92 96 92) (C 22 26 24)), $r)
	$g.FillEllipse((Grad (New-Object System.Drawing.RectangleF 7, 7, 49, 49) (C 30 34 32) (C 58 62 58)), 7, 7, 49, 49)
	$g.DrawEllipse((New-Object System.Drawing.Pen $rim, 2.5), 4, 4, 55, 55)
}

# Face buttons: Xbox colours on the game's dark plate.
$face = @{ a = @('A', (C 96 220 72)); b = @('B', (C 236 64 56)); x = @('X', (C 64 140 250)); y = @('Y', (C 250 200 40)) }
foreach ($k in $face.Keys) {
	$c = New-Canvas; Disc $c[1] $face[$k][1]
	Text $c[1] $face[$k][0] 34 $face[$k][1] 31.5 32.5
	Save $c "pad_$k"
}
# Bumpers and triggers.
foreach ($k in @('lb', 'rb', 'lt', 'rt')) {
	$c = New-Canvas; $g = $c[1]
	$trig = $k[1] -eq 't'
	$p = if ($trig) { RoundRect 12 3 40 57 14 } else { RoundRect 3 14 58 36 12 }
	$g.TranslateTransform(1, 2); $g.FillPath((New-Object System.Drawing.SolidBrush (C 0 0 0 110)), $p); $g.ResetTransform()
	$g.FillPath((Grad (New-Object System.Drawing.RectangleF 0, 0, 64, 64) (C 92 96 92) (C 24 28 26)), $p)
	$g.DrawPath((New-Object System.Drawing.Pen (C 200 210 200), 2.5), $p)
	Text $g $k.ToUpper() 25 (C 240 244 240) 32 32.5
	Save $c "pad_$k"
}
# Start / Back / stick clicks.
$c = New-Canvas; Disc $c[1] (C 200 210 200)
foreach ($y in 22, 30, 38) { $c[1].FillRectangle((New-Object System.Drawing.SolidBrush (C 240 244 240)), 20, $y, 24, 4) }
Save $c "pad_start"
$c = New-Canvas; Disc $c[1] (C 200 210 200)
$pen = New-Object System.Drawing.Pen (C 240 244 240), 3.5
$c[1].DrawRectangle($pen, 17, 19, 20, 16); $c[1].DrawRectangle($pen, 27, 28, 20, 16)
Save $c "pad_back"
foreach ($s in 'l', 'r') {
	$c = New-Canvas; Disc $c[1] (C 200 210 200)
	$c[1].DrawEllipse((New-Object System.Drawing.Pen (C 150 160 150), 2), 13, 13, 37, 37)
	Text $c[1] ($s.ToUpper() + "3") 20 (C 240 244 240) 31.5 32.5
	Save $c "pad_${s}3"
}

# Arrow heads for the sticks and the D-pad: up, down, left, right.
function Arrow($g, [string]$dir, [float]$cx, [float]$cy, [float]$len, $brush) {
	$pts = switch ($dir) {
		'up' { @(@(0, -1), @(-0.8, 0), @(0.8, 0)) } 'down' { @(@(0, 1), @(-0.8, 0), @(0.8, 0)) }
		'left' { @(@(-1, 0), @(0, -0.8), @(0, 0.8)) } 'right' { @(@(1, 0), @(0, -0.8), @(0, 0.8)) }
	}
	$arr = foreach ($p in $pts) { New-Object System.Drawing.PointF ($cx + $p[0] * $len), ($cy + $p[1] * $len) }
	$g.FillPolygon((New-Object System.Drawing.SolidBrush (C 0 0 0 200)), [System.Drawing.PointF[]]($arr | ForEach-Object { New-Object System.Drawing.PointF ($_.X + 1), ($_.Y + 1.5) }))
	$g.FillPolygon($brush, [System.Drawing.PointF[]]$arr)
}
$sets = [ordered]@{ up = @('up'); down = @('down'); left = @('left'); right = @('right'); updown = @('up', 'down'); leftright = @('left', 'right'); all = @('up', 'down', 'left', 'right') }
$lit = New-Object System.Drawing.SolidBrush (C 120 240 100)
$dim = New-Object System.Drawing.SolidBrush (C 110 118 110)
$at = @{ up = @(32, 9); down = @(32, 55); left = @(9, 32); right = @(55, 32) }
foreach ($stick in 'ls', 'rs') {
	foreach ($set in $sets.Keys) {
		$c = New-Canvas; $g = $c[1]
		$r = New-Object System.Drawing.RectangleF 14, 14, 36, 36
		$g.FillEllipse((New-Object System.Drawing.SolidBrush (C 0 0 0 110)), 15, 16, 36, 36)
		$g.FillEllipse((Grad $r (C 96 100 96) (C 26 30 28)), $r)
		$g.DrawEllipse((New-Object System.Drawing.Pen (C 200 210 200), 2.5), 15, 15, 34, 34)
		Text $g $stick.Substring(0, 1).ToUpper() 18 (C 240 244 240) 32 32.5
		foreach ($d in 'up', 'down', 'left', 'right') {
			$on = $sets[$set] -contains $d
			Arrow $g $d $at[$d][0] $at[$d][1] 8.5 $(if ($on) { $lit } else { $dim })
		}
		Save $c "${stick}_$set"
	}
}
foreach ($set in $sets.Keys) {
	$c = New-Canvas; $g = $c[1]
	$cross = New-Object System.Drawing.Drawing2D.GraphicsPath
	$cross.AddRectangle((New-Object System.Drawing.RectangleF 22, 4, 20, 56)); $cross.AddRectangle((New-Object System.Drawing.RectangleF 4, 22, 56, 20))
	$cross.FillMode = 'Winding'
	$g.TranslateTransform(1, 2); $g.FillPath((New-Object System.Drawing.SolidBrush (C 0 0 0 110)), $cross); $g.ResetTransform()
	$g.FillPath((Grad (New-Object System.Drawing.RectangleF 0, 0, 64, 64) (C 92 96 92) (C 26 30 28)), $cross)
	$outline = New-Object System.Drawing.Drawing2D.GraphicsPath
	$outline.AddPolygon([System.Drawing.PointF[]]@(
		(New-Object System.Drawing.PointF 22, 4), (New-Object System.Drawing.PointF 42, 4), (New-Object System.Drawing.PointF 42, 22),
		(New-Object System.Drawing.PointF 60, 22), (New-Object System.Drawing.PointF 60, 42), (New-Object System.Drawing.PointF 42, 42),
		(New-Object System.Drawing.PointF 42, 60), (New-Object System.Drawing.PointF 22, 60), (New-Object System.Drawing.PointF 22, 42),
		(New-Object System.Drawing.PointF 4, 42), (New-Object System.Drawing.PointF 4, 22), (New-Object System.Drawing.PointF 22, 22)))
	$g.DrawPath((New-Object System.Drawing.Pen (C 200 210 200), 2.5), $outline)
	$dat = @{ up = @(32, 15); down = @(32, 49); left = @(15, 32); right = @(49, 32) }
	foreach ($d in 'up', 'down', 'left', 'right') {
		$on = $sets[$set] -contains $d
		Arrow $g $d $dat[$d][0] $dat[$d][1] 7 $(if ($on) { $lit } else { $dim })
	}
	Save $c "dpad_$set"
}

# Keycaps: the mod writes the key's name on them (key = 1:1, key_wide = 2:1).
function Keycap([int]$w, [string]$name) {
	$c = New-Canvas $w $SZ; $g = $c[1]
	$g.FillPath((New-Object System.Drawing.SolidBrush (C 0 0 0 120)), (RoundRect 3 6 ($w - 5) 56 10))
	$g.FillPath((Grad (New-Object System.Drawing.RectangleF 0, 0, $w, 64) (C 150 156 150) (C 40 44 42)), (RoundRect 2 2 ($w - 5) 58 10))
	$g.FillPath((Grad (New-Object System.Drawing.RectangleF 0, 6, $w, 44) (C 46 50 48) (C 74 78 74)), (RoundRect 8 6 ($w - 17) 44 7))
	$g.DrawPath((New-Object System.Drawing.Pen (C 210 216 210), 2), (RoundRect 2 2 ($w - 5) 58 10))
	Save $c $name
}
Keycap 64 "key"
Keycap 128 "key_wide"

# Players and heroes.
foreach ($p in @(@('p1', (C 240 80 70)), @('p2', (C 70 130 250)))) {
	$c = New-Canvas; $g = $c[1]
	$g.FillPath((New-Object System.Drawing.SolidBrush (C 0 0 0 110)), (RoundRect 4 8 58 54 12))
	$g.FillPath((Grad (New-Object System.Drawing.RectangleF 0, 0, 64, 64) $p[1] (C 20 24 40)), (RoundRect 3 5 57 54 12))
	$g.DrawPath((New-Object System.Drawing.Pen (C 240 244 240), 2.5), (RoundRect 3 5 57 54 12))
	Text $g $p[0].ToUpper() 26 (C 255 255 255) 31.5 33
	Save $c $p[0]
}
# Asterix: the winged helmet.
$c = New-Canvas; $g = $c[1]; Disc $g (C 240 80 70)
$wing = New-Object System.Drawing.SolidBrush (C 245 248 245)
foreach ($s in -1, 1) {
	$pts = @(@(0, 0), @(13, -16), @(9, -6), @(17, -12), @(11, -1), @(16, -4), @(4, 6))
	$arr = foreach ($p in $pts) { New-Object System.Drawing.PointF (32 + $s * (11 + $p[0])), (28 + $p[1]) }
	$g.FillPolygon($wing, [System.Drawing.PointF[]]$arr)
	$g.DrawPolygon((New-Object System.Drawing.Pen (C 0 0 0 160), 1.5), [System.Drawing.PointF[]]$arr)
}
$g.FillPie((Grad (New-Object System.Drawing.RectangleF 14, 16, 36, 36) (C 255 230 120) (C 190 140 20)), 14, 18, 36, 36, 180, 180)
$g.FillRectangle((New-Object System.Drawing.SolidBrush (C 160 110 20)), 12, 35, 40, 6)
$g.DrawArc((New-Object System.Drawing.Pen (C 0 0 0 160), 1.5), 14, 18, 36, 36, 180, 180)
Save $c "asterix"
# Obelix: a menhir.
$c = New-Canvas; $g = $c[1]; Disc $g (C 80 150 240)
$men = New-Object System.Drawing.Drawing2D.GraphicsPath
$men.AddBezier(22, 52, 18, 30, 24, 12, 33, 10); $men.AddBezier(33, 10, 43, 12, 46, 32, 42, 52); $men.CloseFigure()
$g.FillPath((Grad (New-Object System.Drawing.RectangleF 18, 10, 30, 44) (C 210 214 220) (C 120 126 136)), $men)
$g.DrawPath((New-Object System.Drawing.Pen (C 0 0 0 170), 2), $men)
$g.DrawLine((New-Object System.Drawing.Pen (C 90 96 106), 2), 28, 26, 34, 30)
$g.FillRectangle((New-Object System.Drawing.SolidBrush (C 80 150 240)), 14, 50, 36, 3)
Save $c "obelix"

Write-Host "icons written to $Out"
