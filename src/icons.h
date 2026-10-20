// Button icons in the game's text.
//
// The game's action icons are characters of its font ("arial24-windows", an Rt2d
// bitmap font of CManager2d, read from xxGLOC.KWN by FUN_005FC970): U+E000 action,
// E001 Dogmatix, E002 jump, E003 pause, E005-E008 up/down/right/left, E009
// targeted attack, E00A grab, E00B launch Obelix, E00E confirm, E00F cancel. The
// strings embed them ("Use \xE000", a spy's "press \xE000 several times").
//
// The mod adds its own glyphs to that font, U+E100 on: the PNGs of
// Game Files\coop_icons (pad buttons, sticks, D-pad, P1/P2, the heroes) on one
// extra texture page, and keycaps drawn for the keys in use. With player 2 in,
// a string's action icons become the buttons each player presses.

#pragma once
#include <cstdint>

bool IconsInstallHooks();
// Once per frame: puts the glyphs into fonts loaded since.
void IconsFrame();

// The glyph of icon `name` (a PNG's name: "pad_a", "p1", "asterix"), 0 if none.
wchar_t IconGlyph(const char* name);
// The glyph of a keyboard key (a keycap with its name), 0 if none.
wchar_t IconKeyGlyph(int vk);
// The glyph for what player `player` (0 = player 1) presses for game action
// `action` (CKInput action: 4 jump, 5 action, 6 grab, 7 targeted attack, 8 launch
// Obelix, 10 Dogmatix, 12 pause, 15 start, 0-3 up/down/left/right, 20 confirm,
// 21 cancel), 0 if none.
wchar_t IconActionGlyph(int player, int action);
// The virtual key player 1's keyboard binding of `action` is on, 0 if none.
int IconsP1Vk(int action);
// A keyboard key's name ("Space", "K").
void IconsKeyName(int vk, wchar_t* out, int n);

// `s` with the game's action icons replaced by the buttons of player `who`
// (0, 1) or of both (-1: one icon when both press the same, else both, "X/F").
// Returns `s` itself when nothing changes; otherwise a string kept for good.
const wchar_t* IconsRewrite(const wchar_t* s, int who);
