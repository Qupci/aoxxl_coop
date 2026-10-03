#pragma once
#include "kal.h"
#include "trio.h"

// The shared command block at gameManager+0x40 that CKHkAsterix::update and
// CKHkObelix::update read. A hero only consumes it when its own +0x1A is zero;
// see docs/RESEARCH.md section 6.
//
// Only the fields the update function is known to touch are named. The rest is
// still unmapped, which is why CoopDumpCommandBlock() exists.
struct HeroCommandBlock {
	uint8_t  unk00[0x1C];
	uint32_t controlBits;   // +0x1C: low nibble -> hero+0x18 bits 1..4,
	                        //        bits 4..5 -> hero+0x18 bit 0
	Vector3  moveDir;       // +0x20: desired movement direction
	uint8_t  unk2C[0x5C - 0x2C];
	int8_t   forceIgnore;   // +0x5C: != -1 is copied into hero+0x1A
	uint8_t  unk5D[0x8C - 0x5D];
	float    heading;       // +0x8C: -1.0f means "leave facing alone"
};

const size_t kCommandBlockSize = 0xC0;  // dumped conservatively; true size unknown

HeroCommandBlock* GetCommandBlock();

// Raw hook invocation counts. The watchdog reports these so we can tell
// "the hook is not installed" apart from "the game is not running that code".
struct HookCounters {
	volatile long inputUpdates = 0;
	volatile long asterixTicks = 0;
	volatile long obelixTicks  = 0;
	volatile long p2Swaps      = 0;   // times P2's action array was swapped in
	volatile long p2SwapSkips  = 0;   // times the swap was wanted but bailed out
	volatile long aiTicksSuppressed = 0;
};

// True when `heroIndex` is the hero player 2 is currently driving, so the
// engine's AI must be kept off it.
bool CoopHeroIsPlayerDriven(int heroIndex);

// Steer player 2's hero through the engine's AI order interface.
void P2DriveByOrder(Trio trio, Hero hero, int padIndex);
extern unsigned g_p2OrderOverrides;
void P2ResetSteering();
extern HookCounters g_hookCounters;

// Addresses of our installed hooks, so the watchdog can check the vtable slots
// still point at them.
extern uintptr_t g_patchedInputUpdate;
extern uintptr_t g_patchedAsterixTick;
extern uintptr_t g_patchedObelixTick;

// True only when the heroes actually exist and are being ticked, i.e. the
// player is really in control. A level whose data has been loaded without the
// game entering its play state looks alive from the outside -- the frame loop
// runs at full speed -- but no hero exists, so anything that measures input is
// measuring nothing.
bool CoopGameplayLive();

// Player 2 state, for the watchdog.
bool           CoopP2Joined();
int            CoopP2Hero();
int            CoopP2Pad();
const uint8_t* CoopP2ActionSnapshot();
int            CoopP2ActionCount();
void           CoopP2DebugAxes(float* out6);
unsigned       CoopP2MoveFlags();

void CoopInit();
void CoopShutdown();
void WatchdogStart();

// Page-guard sampler that identifies which code reads the CKInput action array.
void GuardWatchArm();
void GuardWatchReport();

// Called from the hooked hero Life ticks, once per hero per frame.
void CoopBeforeHeroTick(int heroIndex, Hero hero);
void CoopAfterHeroTick(int heroIndex, Hero hero);

// Called once per frame, from the first hero tick of the frame.
void CoopFrame();

bool CoopInstallHooks();




