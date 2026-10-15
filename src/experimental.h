// Experimental rules (ini [experimental]) for a harder game with two players:
// more Romans at once in a squad, more Romans to defeat, dearer merchant. They
// change the level data as it loads -- single player too, and whatever co-op
// is doing -- so they are read once, at startup.
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
