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
void CoopNoteStoryZone(int heroIndex);   // a zone bound to the other hero started a cutscene for this one
void CoopNoteBossRun(int heroIndex);     // this hero woke a boss that the heroes run from
void CoopTorchCheat(bool give);   // fire for Asterix from the nearest brazier, or thrown away   // a trigger zone fired for this hero (sector attribution)
int  CoopNonLeaderPlayer();   // the player hero that is not the trio leader (-1 if none)
int  CoopP2Pad();
int  CoopPromptPlayer();   // the player whose hero the HUD's action prompt is for (-1: unknown)
bool CoopSuspended();   // co-op paused for a scripted mode (ride, cinematic...)
bool CoopOneScreen();   // co-op running, but on one screen for now (Twister Fusion)
int  CoopShopper();      // the hero at the merchant while co-op goes on around it (-1: none)
// The hero at the merchant from its X until it is out of the shop's situation
// (-1: none): the merchant ticks as its and the game's camera is its.
int  CoopShopHero();
// Gives hero `idx` the trio's lead the game's way (switch event), as a player's
// hero (individual heroes: a leader that went down).
void CoopGiveLead(int idx, const char* why);
// Where a hero joining hero `to` is put (R3 + A, the sector catch-up, a revive
// at a sector change): beside `to` on its own solid ground, else just above it.
struct Vector3;
Vector3 CoopJoinSpot(int to);
// How long player `player` (0 = player 1) has held action `action` (seconds; 0 = not held).
float CoopHoldTime(int player, int action);
// Player 2 holds Back (not for a combo): their HUD shows.
bool CoopP2ShowsHud();

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
// A trio event of the automatic cable car's ride whose rider is not leading
// (cable_car_rider_leads off): run with the rider as the leader. End with what
// Begin returned.
bool CoopRideSliceBegin(int event, void* caller);
// Freedom mode: the rotary beam's catch (0x0C02) for a companion that jumped
// for it, its wait order lost to a lead switch. End with what Begin returned.
bool CoopBeamCatchBegin(int event, void* caller);
void CoopBeamCatchEnd(bool began);
void CoopRideSliceEnd(bool began);
// Also called before CKGrpTrio::sendEvent, with the event's parameter.
void CoopNoteTrioEvent(void* trio, int event, void* param);
// A trio event put `offer` in trio+0x4C (what X takes: a torch's 0xC06) while
// the trio's leader byte said `hero` (an object ticked as that hero's).
void CoopNoteOffer(uint32_t offer, int hero);
// A trio event to hold back for now (sent again later by the mod).
bool CoopDeferTrioEvent(void* trio, int event);

bool CoopInstallHooks();
bool CoopHeroHeld(int hero);   // borrowed by the game for a scripted order
// Individual heroes: hero `fallen` just went down. If the other was locked in a
// sequence waiting for it (a blue circle's "wait here", a cable car pulled by
// it), the sequence is undone so the other can come and revive it. Returns who
// led before that sequence (its event can hand the lead over), else -1.
int CoopUndoLockFor(int fallen);
// What R3 + B does for `hero` (autotests): unlocks it from a level lock.
void CoopUnlockHero(int hero);
// Freedom mode: `hero` has a scripted order the mode leaves unfollowed (it would be waiting).
bool CoopFreedomWaiting(int hero);
// An event that commands the trio was just flagged (before its group runs): the
// level's state is noted, should it lock a hero and need undoing.
void CoopNoteTrioEventFlagged(unsigned eventId);
uint8_t* CoopP2CameraValues();
const uint8_t* CoopP2Values();       // player 2's hero input this frame (nullptr: none)   // player 2's input as the game camera reads it (nullptr: none)
bool TraceInstallHooks();   // trace.cpp: event logging for interaction debugging
void TraceDumpCollisionTests(void* receiver);   // trace.cpp: collision tests involving `receiver`
float PotionGauge();            // trace.cpp: the magic potion gauge (0 = none)
void SetPotionGauge(float v);
void TracePotionFrame();        // trace.cpp: once per frame
void TraceDumpBindings();       // trace.cpp: which device byte feeds each mapped action

// Shared vtable patcher (hooks.cpp).
bool PatchVtableSlot(uintptr_t vtbl, size_t slot, uintptr_t expected,
                     void* replacement, void** outOriginal, const char* what);
