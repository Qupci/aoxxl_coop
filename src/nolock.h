// No-lock mode's Romans: while the level would have kept a player's hero
// waiting (no-lock mode leaves its scripted order unfollowed), Romans of a
// nearby active squad come for that hero now and then.

#pragma once

bool NoLockInstallHooks();
// Once a frame (coop's frame).
void NoLockRomansFrame();
// The Romans sent for a waiting hero start no fight (music, fight camera):
// hidden from the trio's fight-mode pass while `hide`, back after it.
void NoLockHideFromFight(bool hide);
// After squad `sq`'s tick: its Romans sent for a hero go after that hero.
void NoLockSteer(char* sq);
// Was squad `sq`'s fight ended with Romans of it still out for a hero (and
// those not beaten yet)?
bool NoLockEndedFight(char* sq);
// Has squad `sq` Romans out for a hero (not to be put away meanwhile)?
bool NoLockKeepsSquad(char* sq);
