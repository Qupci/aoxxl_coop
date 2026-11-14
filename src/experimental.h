// Co-op difficulty (ini [coop_difficulty]) for a harder game with two players:
// more Romans at once in a squad, more Romans to defeat, a dearer merchant, a
// slower combo bar. Read once, at startup. The merchant's prices change as the
// level loads; the squads' numbers and the combo bar follow the game: on while
// both players play together, the game's own while the heroes are apart (one
// held by the level, down, or where the other can't walk to it) and, with
// difficulty_coop_only, in single player (difficulty_static: on all through
// co-op). A fight's Romans to defeat are set as it begins (behind a gate: as
// the gate takes its squads) and only go up (the heroes together again: those
// left doubled) -- down only with player 2 gone.
//
// A squad (CKGrpSquad, deserialize 0x52DE80) keeps one 0x1C record per pool of
// Romans (squad +0x144, count +0x14C; the editor's "Pools", one per phase):
//   +0x00 pool (CKGrpPoolSquad)  +0x04 enemy component  +0x08 (ref)
//   +0x10 Romans alive at once after a restart (U1; squad vf33 / FUN_0052D460)
//   +0x11 Romans alive now -- loaded as U2, the number placed when the squad
//         first wakes (vf33); each fallen one is replaced (+0x15) while the
//         pool lasts, so that stays the number at once
//   +0x14 queued  +0x15 fallen, to replace  +0x16 sent in so far
//   +0x18 Romans in the pool -- to defeat (int16, < 0 endless)
// The merchant (CKHkAsterixShop, deserialize 0x4C0C50): prices at +0xE4, 12
// uint32 (helmets), read by his menu for the price shown and paid.

#pragma once

bool ExperimentalInstallHooks();
// Once per frame: the combo bar's thresholds.
void ExperimentalFrame();
// Enemies a squad pool holds that are left as the level has them (turtles, the
// mobile tower, jetpack Romans) -- by the pool record's enemy component (+4).
bool EnemyKindKeptAsIs(char* cpnt);
// The squads' rules now: 0 the game's own (single player, coop_only), 1 the
// heroes apart (the game's numbers for the one left), 2 on.
int ExperimentalMode();
// Where extra Roman n of a squad (more Romans at once than its formation has
// spots) goes: spot n % spots shifted sideways by a row, facing as the spot.
bool ExperimentalExtraSpot(char* squad, int n, float* pos, float* dir);
