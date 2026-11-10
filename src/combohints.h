// Combo hints: how to do a combo, shown to one player in their own hint box,
// the buttons lit up as they are pressed in order.
//
//  * A combo bought from the merchant (or learned by the cheat) in this level:
//    to each player whose hero can do it, when its combo bar is full and a Roman
//    is near enough to use it on; it stays 8 s after that stops, and comes back
//    each time, until that player has done the combo once. Combos bought
//    together wait in line.
//  * The Twister for a crowd: a player whose bar fills in a fight with more than
//    100 Romans still to beat, the Twister learned -- once a level per player,
//    12 s (8 s more once two buttons are pressed in order).
//  * Twister Fusion (learned): both heroes spinning near each other -- X to fuse,
//    to both players, until the Twister ends or they fuse (until the first
//    fusion of the level).
//
// Combos and the merchant's item bits (game manager +0x40 -> +0x10 bit k; a
// purchase also sets game manager +0x400 + k, "bought this level"): 0 Club (A A
// X B), 1 Power Hammer (A B X), 2 Twister (A X B Y), 3 Twister Fusion (X while
// both spin), 4 Mole (A A Y B, Asterix), 5 Rage (RB: Idefix's rage, his bite
// state 0x49E4C0 with a full bar). A combo done spends the bar through
// FUN_00493810 (thiscall(game manager, k), ret 4) -- how the hints know.

#pragma once

bool ComboHintsInstallHooks();
// Once per frame (after the co-op frame).
void ComboHintsFrame();
// Autotests: combo `kind` for both players as if just bought (-1: the crowd's
// Twister hint is told again).
void ComboHintsQueue(int kind);
// Autotests: combo `kind`'s hint text (0 Club, 1 Power Hammer, 2 Twister, 3
// Twister Fusion, 4 Mole, 5 Rage; `crowd`: the crowd's Twister) for player
// `player`, its first `lit` buttons lit.
void ComboHintsText(wchar_t* out, int n, int player, int kind, bool crowd, int lit);
