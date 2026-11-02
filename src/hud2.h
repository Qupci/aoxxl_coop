// Individual heroes in split screen: each player's own health and combo bar.
//
// The in-game interface (CKHkInterfaceInGame, 2/132; tick vtable 0x6285F0 slot
// 18) keeps one HUD: the health shields (FUN_0051A140: state +0x24 bits 4-5,
// fields +0x1E4..+0x277, 30 shield models +0x2C..+0xA0 -- shield i/5, fill
// i%5 -- and sparkles +0xA4), the combo bar (FUN_00518190: bits 12-14, fields
// +0x2A0..+0x2EF; FUN_00519E70, its glow: bits 23-24, +0x2F0..+0x30F,
// +0x494..+0x49F; models +0xB4..+0xCC) and the helmet counter (FUN_0051C160:
// model +0xA8, plate +0x134, texts +0x150..+0x158). Player 1's hero is the
// game's HUD (top left). Player 2's hero gets a second copy of the health and
// combo bar parts: their state is swapped in, ticked with player 2's stats, and
// swapped out again; at render time it is swapped in for a second HUD pass,
// the models' places mirrored to the right edge (player 2's half). The shared
// helmet counter moves to the top middle meanwhile.

#pragma once

struct RwCamera;

bool Hud2InstallHooks();

// Around the game's interface tick (hints.cpp): player 1's "health changed"
// flag, then player 2's copy ticked.
void Hud2BeforeInterfaceTick(char* itf);
void Hud2AfterInterfaceTick(char* itf, void* time);

// Render (splitscreen.cpp): the game's HUD pass with the helmet counter in the
// middle; then, if Hud2BeginP2 says so, player 2's pass (the caller collects
// the HUD lists and draws them) and Hud2EndP2.
void Hud2BeginP1(RwCamera* hud);
void Hud2EndP1();
void Hud2ShopSides(RwCamera* hud);   // the shop's HUD in a stacked half: models out to its edges
bool Hud2BeginP2(RwCamera* hud, int part);   // part 0: health, 1: combo bar
void Hud2EndP2();

// Single player's second chance: a health row for each hero (Asterix's, Obelix's
// under it, each in its hero's colours) -- the game's HUD is the leading hero's,
// the other hero's row a copy as player 2's, drawn in the same second pass.
bool Hud2SoloRows();
// The hero whose stats the game's HUD shows (player 1's, a shopper's, single
// player's leader).
int Hud2HudHero();
