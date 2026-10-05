#pragma once
#include "kal.h"
#include "trio.h"

// Raw hook invocation counts. The watchdog reports these so we can tell
// "the hook is not installed" apart from "the game is not running that code".
struct HookCounters {
	volatile long inputUpdates = 0;   // CKInput::update, once per frame
	volatile long asterixTicks = 0;   // CKHkAsterixLife::Tick
	volatile long obelixTicks  = 0;   // CKHkObelixLife::Tick
	volatile long p2Routed     = 0;   // Life ticks that ran on player 2's input
	volatile long enforced     = 0;   // times a hero had to be put back into a player state
	volatile long trioEvents   = 0;   // CKGrpTrio::sendEvent calls seen
};
extern HookCounters g_hookCounters;

// Address of our CKInput::update hook, so the watchdog can check the slot.
extern uintptr_t g_patchedInputUpdate;

// True only when the heroes actually exist and are being ticked.
bool CoopGameplayLive();

// One log line per hero with every control-related field (f18, Life state, ...).
void CoopLogState(const char* tag);

// --- Player 2 ---------------------------------------------------------------
// Player 2 joins on any XInput pad other than player 1's by pressing Start, and
// takes the hero player 1 is not driving. From then on each player keeps their
// hero, whatever the game does with its internal "leader" (blue circles, scripted
// switches): the leader only decides what scripts, the HUD and a single camera
// follow. Input is routed per hero, never per leader.
bool CoopP2Joined();
int  CoopP2Hero();
int  CoopP1Hero();
void CoopNoteZoneHero(int heroIndex);
void CoopTorchCheat(bool give);   // fire for Asterix from the nearest brazier, or thrown away   // a trigger zone fired for this hero (sector attribution)
int  CoopNonLeaderPlayer();   // the player hero that is not the trio leader (-1 if none)
int  CoopP2Pad();
bool CoopSuspended();   // co-op paused for a scripted mode (ride, cinematic...)

// Ask player 2 to leave (same as holding Back).
void CoopRequestLeave(const char* why);

void CoopInit();
void CoopShutdown();
void WatchdogStart();

// Called once per frame, right after CKInput::update.
void CoopFrame();

// Called around every hero Life tick (life.cpp).
void CoopBeforeLifeTick(int heroIndex, Hero hero);
void CoopAfterLifeTick(int heroIndex, Hero hero);

// Called around CKGrpTrio::sendEvent. `before` returns a cookie for `after`.
int  CoopBeforeTrioEvent(int event, void* caller);
void CoopAfterTrioEvent(int event, int cookie);
// Also called before CKGrpTrio::sendEvent, with the event's parameter.
void CoopNoteTrioEvent(void* trio, int event, void* param);

bool CoopInstallHooks();
bool CoopHeroHeld(int hero);   // borrowed by the game for a scripted order
bool TraceInstallHooks();   // trace.cpp: event logging for interaction debugging
void TraceDumpCollisionTests(void* receiver);   // trace.cpp: collision tests involving `receiver`
float PotionGauge();            // trace.cpp: the magic potion gauge (0 = none)
void SetPotionGauge(float v);
void TracePotionFrame();        // trace.cpp: once per frame
void TraceDumpBindings();       // trace.cpp: which device byte feeds each mapped action

// Shared vtable patcher (hooks.cpp).
bool PatchVtableSlot(uintptr_t vtbl, size_t slot, uintptr_t expected,
                     void* replacement, void** outOriginal, const char* what);
