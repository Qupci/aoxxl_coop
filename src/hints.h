// Co-op hints in the HUD's action prompt box (where the game shows "Use",
// "Activate", "Pull"...).
//
// The in-game interface (CKHkInterfaceInGame, 2/132; tick = vtable 0x6285F0 slot
// 18) shows the prompt of game manager +0xCC bits 4-7 (0xE = none) with the text
// of string id [interface +0x1A4 + kind*4] (FUN_0042E590 on the text object
// +0x15C; strings from the localisation manager, FUN_00411780) and a button icon
// (+0x140). A hint borrows one prompt kind for the length of the interface's
// tick, with a private string id answered by the mod; the game's own prompts
// come first.

#pragma once
#include <windows.h>

bool HintsInstallHooks();
// Once per frame (after the co-op frame).
void HintsFrame();

// Shows `text` for `ms` milliseconds. `key` identifies the hint: showing a key
// again replaces its text and time. The newest hint is the one on screen.
void HintShow(int key, const wchar_t* text, DWORD ms);
void HintHide(int key);
bool HintShown(int key);

enum HintKey {
	HINT_JOIN = 1,      // level start: how player 2 joins / leaves
	HINT_PASS,          // spinning a Roman: LB passes it
	HINT_MODE,          // a shared mode: who does what
	HINT_MACHINE,       // a machine needs both heroes
	HINT_LIFTER,        // a lifter waits for both heroes
	HINT_REVIVE,        // a fallen hero: how to revive it, the revive under way
};

enum HintButton { HB_A, HB_X, HB_LB, HB_START, HB_BACK, HB_B, HB_COUNT };
// The name of `b` on player `player`'s (0 = player 1, 1 = player 2) controller:
// its icon (pad button, keycap), else the pad button's or the key's name.
const wchar_t* HintButtonName(int player, HintButton b);
// A keyboard key's name ("Space", "K"), or its keycap icon.
const wchar_t* HintKeyName(int vk);
// With the co-op icons: the hero's icon and name ("Asterix"), the hero and its
// player's icon ("Asterix (P1)" without), player's icon ("Player 2:" without).
const wchar_t* HintHeroName(int hero);
const wchar_t* HintHero(int hero, int player);
const wchar_t* HintPlayer(int player);
