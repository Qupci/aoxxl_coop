// Individual heroes (ini individual_heroes, on by default): in co-op each hero
// has its own health, shields and combo bar; the helmets and the magic potion
// stay shared.
//
// The game keeps one of each: health at game manager +0x40 -> +0x50 (points, 5
// a shield) and +0x51 (shields, 3 to 6); the combo bar at game manager +0xA4
// (gauge), +0xA8 (decay timer), +0xB0 (hits), +0xB4, +0xC4, +0xCC bits 9-10
// (level) and +0x40 -> +0x10 bit 0x4000 (full). The mod keeps a copy per hero
// and puts the right one in place whenever the game deals with one hero: its
// Life tick, an event sent to it (hits: 0x03xx, the combo bar: 0x4C0E), a
// bonus it picks up. Everything else -- the game manager's own tick, which
// starts the death and the game over when the health is 0, the HUD, the
// merchant (the shopper leads) -- sees the leader's, and the leader is always a
// hero still standing while one is.
//
// A hero at 0 health falls (its own situation 0x0E, the death animation) but
// the game goes on: the lead passes to the other hero, who can revive it by
// holding the grab button next to it for 10 seconds, or by going on into
// another sector. Both down, or one down on a ride (co-op suspended), is the
// game over of single player.

#pragma once
#include <cstdint>

bool IndivInstallHooks();

// Co-op on with individual heroes.
bool IndivActive();

// Puts hero `hero`'s stats in the game's place; returns the context to give to
// IndivLeave. Both do nothing while individual heroes are off.
int  IndivEnter(int hero);
void IndivLeave(int prev);

// Once per frame, before CoopFrame's own work (no hero tick running).
void IndivFrame();

// Hero `hero` just lost its last point of health (its stats are in place).
// True: it fell and the game goes on; false: it is the game over.
bool IndivHeroDown(int hero, const char* how);

bool IndivFallen(int hero);
// An event for a fallen hero that is not delivered (hits).
bool IndivDropEvent(int hero, int event);
// After a fallen hero's Life tick.
void IndivAfterFallenTick(int hero);
// After a hero's Life tick: the standing hero on what needs both heroes is put
// back there and then, before the thing's own tick can start it.
void IndivAfterHeroTick(int hero);

// A hero down while the other stands: that hero, else -1.
int IndivFallenHero();
// Things that need both heroes -- a lifter, a cable car, a blue circle, the
// spinning platform, a launcher, a zone that orders the companion (the boss
// gates, the slide, the boats), a Roman agent's talk -- stay shut while a hero is
// down: the standing hero who reaches one is put back on its last solid ground.
// True: it was (`what` for the log).
bool IndivBlockPartnerSpot(const char* what);
// The same for a zone the standing hero just crossed: back to where it was the
// frame before (`before`, the detection service's), a little further back.
bool IndivBlockPartnerSpotFrom(const char* what, const float before[3]);
// Hero `hero` stands where a launcher offers itself (its prompt): no safe spot there.
void IndivNoteLauncherSpot(int hero);
// Should this object (a hook) skip its tick now: it needs both heroes, one is
// down, and the standing hero is at it (on its ground; within a Roman agent's
// talk). That hero is put back too.
bool IndivHoldsBack(char* hook);

// The stats of `hero` (for the HUD): health points, shields, combo hits.
bool IndivStats(int hero, int* hp, int* shields, int* hits = nullptr, float* gauge = nullptr);
