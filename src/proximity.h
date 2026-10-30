// Every interactive object sees the player hero nearest to it as "the hero".
//
// The game was written for one player: enemies, squads, bonuses (helmets),
// crates, bumpers (springs), push/pull objects, pressure stones, cable cars...
// all ask the trio for its leader (gameManager+0x14 -> CKGrpTrio vtable
// +0x64..+0x94, or trio+0x2C directly) and get player 1's hero. Player 2's hero
// was invisible to them: Romans would not attack him, helmets would not come to
// him, iron crates did not know Obelix hit them.
//
// Every hook and group is ticked by CKBundle vtable slot 17 (0x43DFE0): the
// bundle's group Life, then each active hook Life. That loop is re-implemented
// so that, while two players are in, the trio's leader is set to the player hero
// nearest the object for the length of that object's tick, then put back --
// unless the object itself changed it (a blue-circle switch).

#pragma once
#include <cstdint>

bool ProximityInstallHooks();

// Boss runs (level 2 sector 10, level 3 sector 6): the zone that wakes the boss
// disables the Tornado/Mole combos and walks the sidekick ahead along markers,
// zone by zone. In co-op those walks are dropped (each player runs their own
// hero) -- does trio event `ev` belong to such a run, flagged just now?
bool ProximityBossRunDropsWalk(int ev);
// A hero was put somewhere else: the detection service's last position for it
// becomes `at`, so the jump does not cross any zone on the way.
void ProximityForgetMove(void* heroNode, const float at[3]);
// Does event `ev`'s group command the trio (switch the leader, order the companion)?
bool ProximityGroupCommandsTrio(unsigned ev);
// Event `ev`'s group: up to `max` targets and the message each gets; the count.
int ProximityGroupTargets(unsigned ev, char** objs, uint16_t* msgs, int max);
// A Roman passed to hero `receiver` (LB): it ticks with that hero as the trio
// leader while it flies (state 4).
void ProximityPassTo(char* roman, int receiver);

// Would a Roman standing on the spawn marker at `p` be seen -- in a player's
// view within 40 units, no collision wall in between?
bool MarkerSeen(const float* p);

// Whether hero `hero` has had an enemy within the trio's fight radius in the
// last fight-ending delay (trio +0x404 s): the fight camera, per player.
bool ProximityHeroFighting(int hero);

// The hero a lever's (CKHkActivator) events switch the trio to, or -1.
int ProximityLeverSwitchTarget(void* lever);

// Diagnostics.
long ProximitySlices();
long ProximityP2Bonuses();   // bonuses sent flying to player 2's hero
long ProximityP2Contacts();  // collision contacts delivered with player 2's hero as leader
long ProximityP2Beacons();   // placed bonuses (beacons) sent flying to player 2's hero
long ProximitySwappedZones();   // zone events fired for the other hero (co-op second pass)
long ProximityP2Squads();       // Roman squad ticks run for the non-leading player
