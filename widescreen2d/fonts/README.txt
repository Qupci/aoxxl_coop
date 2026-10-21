Sharper text font for xxl_widescreen2d.asi
=========================================

arial24-windows0_LL.png: the first page of the game's text font ("arial24-windows",
from xxGLOC.KWN) at 4x (1024 x 1024), one per language pack LL: 00 French,
01 English, 02 German, 03 Spanish, 04 Italian. Deployed to
Game Files\xxl_widescreen2d\. The asi swaps the font's page for it at run time;
the glyphs' texture coordinates are shares of the page, so the layout is the
original's to the texel.

Each glyph is Arial Bold 24 px, placed as a Figma text node over the original
page (Figma file "d e s i g n", page "AoXXL co-op": "English table" by hand,
French / German / Spanish / Italian tables derived from it):
  - characters English has: the English node's offset from its cell;
  - the others: the common baseline, x picked (whole px, inside the cell) to
    leave the least of the old glyph's ink uncovered.
Straight quotes (' and ") as in the original; the degree sign is an Arial
ordinal o at 22 px, (R) Arial at 13 px, the ellipsis three dots at -8% spacing.
The icon cells are the original icons upscaled; text cells, the green cell
borders and their corner marks are cleared.

spec_LL.tsv   code, text, x, y, size, letter spacing % (the Figma node: top
              left, px on the 256 page; baseline = y + 0.9167 * size)
cells_LL.txt  code, the glyph's cell on the 256 page (x0 y0 x1 y1), 1 = text
tools\render.cs  the renderer (.NET Framework csc, System.Drawing):
  csc -r:System.Drawing.dll render.cs
  render.exe page spec_LL.tsv cells_LL.txt ORIGINAL_PAGE.png 4 arial24-windows0_LL.png
  render.exe check spec_LL.tsv ORIGINAL_PAGE.png 4 overlay.png   (share of old ink left bare)
  render.exe ink spec_LL.tsv                                      (outline bounds = Figma's)
The original pages: python ..\..\tools\export_game_icons.py "Game Files\LLGLOC.KWN" OUT
