// Co-op hints and the action prompt, one box per player.
//
// The in-game interface (CKHkInterfaceInGame, 2/132; tick = vtable 0x6285F0 slot
// 18) shows the prompt of game manager +0xCC bits 4-7 (0xE = none) with the text
// of string id [interface +0x1A4 + kind*4] in its text object +0x15C (strings
// from the localisation manager, FUN_00411780) on its dark plate +0x140, at the
// bottom right. With player 2 in, the game's prompt box is not drawn: each
// player gets a box of the mod's -- player 1's at the bottom left (stacked split:
// the right of the top half), player 2's at the bottom right -- showing that
// player's hero's prompt ("Use", "Talk"...) or else that player's newest hint.
// The boxes are copies of the game's text object (CText2d, 0x6DC bytes) laid out
// with a private string id and drawn after the HUD's 2D layer (FUN_0042A470),
// with the game's plate. Without player 2, player 1's box stands at the game's
// place for its prompt too (the game's box left out) and its hints.

#pragma once
#include "icons.h"
#include <windows.h>

bool HintsInstallHooks();
// Once per frame (after the co-op frame).
void HintsFrame();

const int HINT_BOTH = -1;
// Without player 2, player 2's box is a second box at the bottom left (where
// player 1's is side by side), for hints shown to it by name only (not
// HINT_BOTH): second chance's revive countdown, apart from the revive's own
// hints in player 1's box.
const int HINT_LEFT_BOX = 1;
// Shows `text` on player `player`'s box (0 / 1, HINT_BOTH: each) for `ms`
// milliseconds. (`key`, `player`) identifies the hint: showing it again replaces
// its text and time. Each box shows its newest hint (by when it was first
// shown), the player's prompt before any.
void HintShow(int key, int player, const wchar_t* text, DWORD ms);
void HintHide(int key);               // every player's
void HintHide(int key, int player);
bool HintShown(int key);              // any player's, still timed
bool HintShown(int key, int player);
// The hint the box shows right now (in place or sliding in).
bool HintOnScreen(int key, int player);
// Where player `player`'s box (its plate) is drawn now, shares of the screen
// from the left and the bottom; false while it is out of sight.
bool HintBoxRect(int player, float* left, float* right, float* bottom, float* top);
// A burst of sparkles over the `n`-th button (from 0: a word other than ">") of
// the hint's last line, with the combo bar's level sound
// (`sound`).
void HintSpark(int key, int player, int n, bool sound);
// A hold's progress on hint (`key`, `player`), 0..1 (below 0: none): its box's
// plate fills from the left, white turning green (#00FF0E), fading out once full
// (the user's Figma design). Kept while the hint is shown again.
void HintProgress(int key, int player, float progress);
// A countdown's bar on hint (`key`, `player`): the plate filled from the left to
// `share` (the time gone of the whole: 0 at the start, 1 at its end) in red
// #FF0000 at 33%, pulsing between 33% and 66% in its last 10 s (`secondsLeft`).
void HintTimerBar(int key, int player, float share, float secondsLeft);

enum HintKey {
	HINT_JOIN = 1,      // level start: how player 2 joins / leaves
	HINT_PASS,          // spinning a Roman: LB passes it
	HINT_MODE,          // a shared mode: who does what
	HINT_MACHINE,       // a machine needs both heroes
	HINT_LIFTER,        // a lifter waits for both heroes
	HINT_REVIVE,        // a fallen hero: how to revive it, the revive under way
	HINT_COMBO,         // a combo to try (combohints.cpp)
	HINT_FUSE,          // both heroes spinning together: X fuses them
	HINT_SKIP,          // a cutscene: hold A / Enter to skip it (cutskip.cpp), also over a cutscene
	HINT_SWAP,          // co-op: the other player asks for a hero swap (coop.cpp HeroSwapFrame)
	HINT_SWITCH,        // single player: the hero switch button (coop.cpp ShowSwitchHint)
};

enum HintButton { HB_A, HB_X, HB_LB, HB_START, HB_BACK, HB_B, HB_Y, HB_RB, HB_COUNT };
// The name of `b` on player `player`'s (0 = player 1, 1 = player 2) controller:
// its icon (pad button, keycap; `look`: an IconLook, a combo's button waiting
// or pressed), else the pad button's or the key's name.
const wchar_t* HintButtonName(int player, HintButton b, int look = ICON_PLAIN);
// A string id the game's text objects can show (localisation lookups answer
// it with `text`): slots 0-129 for other modules (ctrlopts.cpp's rows).
uint32_t HintsModString(int slot, const wchar_t* text);
// A text element's (CText2d) laid-out width, as a share of the screen's.
float HintsTextWidth(char* text);
// Every text of the game fetched again next frame (their action icons changed).
void HintsRefreshTexts();
// A keyboard key's name ("Space", "K"), or its keycap icon.
const wchar_t* HintKeyName(int vk);
// " > " between a combo's buttons: the mod's chevron icon (the game's font has
// no '>').
const wchar_t* HintNext();
// With the co-op icons: the hero's icon alone (drawn half as big again as the
// text over a blank as wide), else its name ("Asterix"); the hero and its
// player's icon ("Asterix (P1)" without), player's icon ("Player 2:" without).
// (The hints name heroes only: each player's box is on its own side. The
// player tags stay for later use.)
const wchar_t* HintHeroName(int hero);
const wchar_t* HintHero(int hero, int player);
const wchar_t* HintPlayer(int player);
