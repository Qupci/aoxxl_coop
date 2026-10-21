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
Text cells, the green cell borders and their corner marks are cleared.

arial24-windows1_LL.png: the second page at 4x, the action icons that did not
fit the first (how many differs per pack; 02 and 03 are the same).

The action icons (U+E000-E003, E005-E00B, E00E, E00F; E004, E00C and E00D are
empty) are redrawn as vectors in the same Figma page, frame "Action icons": the
designer's left arrow (24 px plate, gradient fill and bevel stroke, white glyph
with green and yellow glows), the other glyphs traced from the originals.
icons\sheet_x4.png is that frame at 4x (a 4x copy screenshotted): one plate per
28 px column, in the order above. icons\listN_LL.txt: sheet column and the
icon's cell on page N (x0 y0 x1 y1); the plate goes at (x0 + 1, y0 + 2), where
the original's is.

spec_LL.tsv   code, text, x, y, size, letter spacing % (the Figma node: top
              left, px on the 256 page; baseline = y + 0.9167 * size)
cells_LL.txt  code, the glyph's cell on the 256 page (x0 y0 x1 y1), 1 = text
tools\render.cs  the renderer (.NET Framework csc, System.Drawing):
  csc -r:System.Drawing.dll render.cs
  render.exe page spec_LL.tsv cells_LL.txt ORIGINAL_PAGE.png 4 arial24-windows0_LL.png
  render.exe check spec_LL.tsv ORIGINAL_PAGE.png 4 overlay.png   (share of old ink left bare)
  render.exe ink spec_LL.tsv                                      (outline bounds = Figma's)
  render.exe icons arial24-windows0_LL.png icons\sheet_x4.png icons\list0_LL.txt arial24-windows0_LL.png
  render.exe icons blank icons\sheet_x4.png icons\list1_LL.txt arial24-windows1_LL.png
The original pages: python ..\..\tools\export_game_icons.py "Game Files\LLGLOC.KWN" OUT
