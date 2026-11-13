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

// How a button's icon looks: plain (a prompt), or a combo hint's / a hold's
// button not pressed yet or pressed. A pad button waiting is its dimmed
// variant (pad_a_unpressed.png), pressed the plain one; a key waiting is the
// plain (glowing) keycap, pressed the dark one (kb\key_a_press.png).
enum IconLook { ICON_PLAIN, ICON_WAITING, ICON_PRESSED };

// The glyph of icon `name` (a PNG's name: "pad_a", "p1", "asterix"), 0 if none.
wchar_t IconGlyph(const char* name);
// The glyph of a keyboard key, 0 if none: coop_icons\kb's keycap of it
// (key_a.png, arrow_up.png; pressed: key_a_press.png), else one drawn with its
// name (key.png / key_wide.png; pressed: key_unpressed.png / key_wide_unpressed.png,
// else the plain one darkened).
wchar_t IconKeyGlyph(int vk, int look = ICON_PLAIN);
// The glyph for what player `player` (0 = player 1) presses for game action
// `action` (CKInput action: 4 jump, 5 action, 6 grab, 7 targeted attack, 8 launch
// Obelix, 10 Dogmatix, 12 pause, 15 start / display-hide the interface, 0-3
// up/down/left/right, 16-19 the interface's, 46-49 the camera's (down, up,
// left, right: "Camera up" is 47), 50 centre the camera, 20 confirm, 21 cancel),
// 0 if none.
wchar_t IconActionGlyph(int player, int action, int look = ICON_PLAIN);
// The merchant's icon of combo `kind` (0 Club, 1 Power Hammer, 2 Twister, 3
// Twister Fusion, 4 Mole, 5 Rage), read from a level file's textures
// (o_ico_massue_g01...), 0 if not found.
wchar_t IconComboGlyph(int kind);
// A sparkle and a glow for the combo hints' pressed buttons, `fade` 0 (bright)
// to 3 (nearly gone); spark.png / glow.png in coop_icons replace the drawn ones.
wchar_t IconSparkGlyph(int fade);
wchar_t IconGlowGlyph(int fade);
// The virtual key player 1's keyboard binding of `action` is on, 0 if none.
int IconsP1Vk(int action);
// A keyboard key's name ("Space", "K").
void IconsKeyName(int vk, wchar_t* out, int n);

// What the game's direction icons (U+E005-E008) in a string stand for: the
// hero's moves (the controls' Up..Right rows), the menus' (every other string
// with them: "Selection", "Page 1/3", "Setting"), the camera's (the controls'
// Camera rows).
enum IconDirs { DIRS_MOVE, DIRS_INTERFACE, DIRS_CAMERA };

// `s` with the game's action icons replaced by the buttons of player `who`
// (0, 1) or of both (-1: one icon when both press the same, else both, "X/F").
// Direction icons become the stick / D-pad / keys the `dirs` actions are on
// (ls_up, rs_leftright, dpad_all..., the bound direction of each), or the
// general is_* ones when they are on a mix. Returns `s` itself when nothing
// changes; otherwise a string kept for good.
const wchar_t* IconsRewrite(const wchar_t* s, int who, int dirs = DIRS_INTERFACE);
// `s` with player 1's icon of game action `action` after it ("Centre Camera"
// -> "Centre Camera [R3]"); `s` when there is none. Kept for good.
const wchar_t* IconsAppendAction(const wchar_t* s, int action);
