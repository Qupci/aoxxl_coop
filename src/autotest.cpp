#include "autotest.h"
#include "coop.h"
#include "proximity.h"
#include "config.h"
#include "inputmgr.h"
#include "life.h"
#include "log.h"
#include "p2input.h"
#include "p2cam.h"
#include "pads.h"
#include "sound.h"
#include "gamecam.h"
#include "crash.h"
#include "individual.h"
#include "pathfind.h"
#include "detour.h"
#include "guard.h"
#include "hints.h"
#include "icons.h"
#include "combohints.h"

#include <cmath>
#include <cstdarg>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// The path slot step (FUN_0045C010, thiscall(slot, short*, Vector3*), ret 0xC):
// the pathfinding service's slots are 0..19. CPNTCHECK logs calls outside them.
const uint8_t kSlotStepPrologue[7] = { 0x6A, 0xFF, 0x68, 0x08, 0xCD, 0x61, 0x00 };
uint32_t(__fastcall* g_origSlotStep)(char* pf, void* edx, uint32_t slot, void* node, void* out) = nullptr;

uint32_t __fastcall SlotStepHook(char* pf, void* edx, uint32_t slot, void* node, void* out) {
	if ((slot & 0xFF) >= 20) {
		static int n = 0;
		if (n++ < 20) Log("autotest: path slot step with slot %u (service %p) from %p", slot & 0xFF, pf, _ReturnAddress());
	}
	return g_origSlotStep(pf, edx, slot, node, out);
}

enum StepKind { S_MARK, S_SHOT, S_FORCE, S_P1, S_VPAD, S_VPADR, S_TELEPORT, S_BONUS, S_PROBE, S_BUMPER,
                S_CRATE, S_ENEMY, S_CINE, S_DETECT, S_TRIOEV, S_PLACE, S_SCATTER, S_KEY, S_KEYMAP, S_SECTORS, S_GLIDE, S_LOADSECTOR, S_POTION, S_PLATES, S_DASHFLAG, S_SETF18, S_REGIONS, S_POKE8, S_EVGROUP, S_MUSICGROUPS, S_WATCHGROUPS, S_WATCHCLUE, S_TORCH, S_LEVERS, S_WATCHCAM, S_HEROFIELDS, S_WATCHRW, S_NEAROBJ, S_INPUTDUMP, S_WATCHBEACONS, S_P2LOOK, S_FINDGROUPS, S_WATCHGROUP2, S_P1COMBO, S_WATCHENEMIES, S_NODEDUMP, S_CAMDUMP, S_BOSSSCAN, S_SETPRESET, S_NEXTTO, S_HUDDIFF, S_GIVE, S_RELOAD, S_OBJEV, S_PFDUMP, S_REACH, S_UNLOCKHERO, S_CARPOS, S_CPNTCHECK, S_CAMSECTOR, S_DETOFF, S_FLAGEV, S_PULLLEVER, S_CINEDUMP, S_WATCHCINE, S_APARTSPOT, S_SQUADS, S_SQUADRESET, S_DOORS, S_STEERCHECK, S_CAMTARGET, S_BIND, S_AREAHIT, S_P1R, S_POOLHOLD, S_ENDFIGHT, S_POOLKINDS, S_GUARDSPOTS, S_HINTTEST, S_HINTSPARK, S_COMBOHINT, S_SPSWITCH, S_SPMOD, S_CAMFOV, S_END };

struct Step {
	int         t;        // ms after the scenario starts
	StepKind    kind;
	int         a;        // S_FORCE: hero index; S_VPAD: XInput slot
	int         b;        // S_FORCE: 1 = hold in player state, 0 = release
	uint16_t    buttons;  // S_P1 / S_VPAD
	float       x, y;     // S_P1 / S_VPAD left stick
	const char* name;
	float       z;        // PLACEAT / GLIDEAT: the point's z (x, y above)
};

#define MARK(t, n)                  { t, S_MARK,  0, 0, 0, 0, 0, n }
#define SHOT(t, n)                  { t, S_SHOT,  0, 0, 0, 0, 0, n }
#define FORCE(t, hero, on)          { t, S_FORCE, hero, on, 0, 0, 0, on ? "force player" : "release force" }
#define P1(t, btn, x, y, n)         { t, S_P1,    0, 0, btn, x, y, n }
#define VPAD(t, slot, btn, x, y, n) { t, S_VPAD,  slot, 0, btn, x, y, n }
#define END(t)                      { t, S_END,   0, 0, 0, 0, 0, "end" }
// A real key press (down=1) or release (down=0) through keybd_event, seen by the
// game's window procedure and by the mod's keyboard "pad" alike.
#define KEY(t, vk, down, n)         { t, S_KEY,   vk, down, 0, 0, 0, n }
// Player 2's keyboard layout: 1 = the ini's numpad layout, 2 = the same with
// jump on SPACE (one of the game's own keys).
// Logs the sector manager (yellow pages +0x90): sectors, flags, boxes, the
// current one, and which sector each hero is in.
#define SECTORS(t)                  { t, S_SECTORS, 0, 0, 0, 0, 0, "sectors" }
// Moves hero `hero` in a straight line to spot `spot` over `ms` milliseconds,
// frame by frame (walls ignored), so trigger zones and sector streaming see a
// continuous path, unlike a teleport.
#define GLIDE(t, hero, spot, ms)    { t, S_GLIDE, hero, spot, 0, ms, 0, "glide" }
// Streams sector `n` in, the way XXL Inspector's "go to sector" does: the level
// object (game manager +0x20, = yellow pages +0x90) loads +0x34 next.
#define LOADSECTOR(t, n)            { t, S_LOADSECTOR, n, 0, 0, 0, 0, "load sector" }
// Fills the magic potion gauge (game manager +0x40 -> +0x9C).
#define POTION(t)                   { t, S_POTION, 0, 0, 0, 0, 0, "potion" }
// Game manager +0x5C bit 19 (the potion dash flag) on (1) or off (0).
#define DASHFLAG(t, on)             { t, S_DASHFLAG, on, 0, 0, 0, 0, "dash flag" }
// Writes hero `hero`'s control byte (+0x18).
#define SETF18(t, hero, v)          { t, S_SETF18, hero, v, 0, 0, 0, "set f18" }
#define KEYMAP(t, layout)           { t, S_KEYMAP, layout, 0, 0, 0, 0, "keymap" }
// Puts hero `hero` on the nearest visible bonus (CKHkBasicBonus) and watches it.
#define TO_BONUS(t, hero)           { t, S_BONUS, hero, 0, 0, 0, 0, "to bonus" }
// Same for a placed bonus (a beacon helmet: what the pool spawned last frame at
// the beacons of the live sectors), at least 15 units from the other hero.
#define TO_BEACON(t, hero)          { t, S_BONUS, hero, 1, 0, 0, 0, "to beacon" }
// Puts hero `hero` 2.5 units behind the nearest spring (CKHkBumper), facing it.
#define NEAR_BUMPER(t, hero)        { t, S_BUMPER, hero, 0, 0, 0, 0, "near bumper" }
// Puts hero `hero` in front of the nearest intact crate, facing it: iron (iron=1,
// the kind only Obelix breaks) or wooden (iron=0). CRATE_REPORT logs how many
// pieces the chosen crate has left.
#define NEAR_CRATE(t, hero, iron)   { t, S_CRATE, hero, iron, 0, 0, 0, "near crate" }
#define CRATE_REPORT(t)             { t, S_CRATE, 0, -1, 0, 0, 0, "crate report" }
// Puts hero `hero` 1.3 units from the nearest visible Roman (CKHkBasicEnemy),
// facing it.
#define NEAR_ENEMY(t, hero)         { t, S_ENEMY, hero, 0, 0, 0, 0, "near enemy" }
// Cutscenes (CKSrvCinematic, yellow pages +0x80): CINE_LIST logs every scene's
// flags; CINE_START starts the first idle scene with letterbox bars (scene +0x30
// bit 0) through the service's own event 0x500 + index.
#define CINE_LIST(t)                { t, S_CINE, -1, 0, 0, 0, 0, "cine list" }
#define CINE_START(t)               { t, S_CINE, -2, 0, 0, 0, 0, "cine start" }
#define CINE_STOP(t)                { t, S_CINE, -3, 0, 0, 0, 0, "cine stop" }
// Ends every playing cutscene the game's own way (as Enter does on a skippable one).
#define CINE_SKIP(t)                { t, S_CINE, -4, 0, 0, 0, 0, "cine skip" }
#define CINE_ENTER(t)               { t, S_CINE, -5, 0, 0, 0, 0, "cine skip (as Enter)" }
// Turn the cutscene probe (extra state every log tick) on or off.
#define PROBE(t, on)                { t, S_PROBE, on, 0, 0, 0, 0, "probe" }
// Right stick of virtual pad `slot` (camera look).
#define VPADR(t, slot, x, y, n)     { t, S_VPADR, slot, 0, 0, x, y, n }
// Player 1's right stick: the game camera's input (bindings 46-49).
#define P1R(t, x, y, n)             { t, S_P1R, 0, 0, 0, x, y, n }
// Puts hero `hero` next to lifter `lifter` (offset dx/dz, 1.5 units up).
#define TELEPORT(t, hero, lifter, dx, dz) { t, S_TELEPORT, hero, lifter, 0, dx, dz, "teleport" }
// Hero `hero` put at / moved frame by frame to a point given by its coordinates.
#define PLACEAT(t, hero, X, Y, Z)       { t, S_PLACE, hero, -1, 0, X, Y, "place", Z }
#define GLIDEAT(t, hero, X, Y, Z, ms)   { t, S_GLIDE, hero, -1, ms, X, Y, "glide", Z }
#define GLIDEBY(t, hero, DX, DZ, ms)    { t, S_GLIDE, hero, -2, ms, DX, 0, "glide by the other hero", DZ }

// exp1: does a hero held in the player control state answer input? Player 1's
// synthetic input goes to the shared action array, so if the theory holds both
// heroes move together.
const Step kExp1[] = {
	MARK(0, "baseline"), SHOT(100, "exp1_00_start"),
	FORCE(500, HERO_OBELIX, 1),
	P1(1500, 0, 1, 0, "p1 right"),
	P1(3000, 0, 0, 1, "p1 up"),
	P1(4500, 0, -1, 0, "p1 left"),
	SHOT(5200, "exp1_01_left"),
	P1(6000, 0, 0, -1, "p1 down"),
	P1(7500, 0, 0, 0, "p1 neutral"),
	P1(8000, PAD_A, 0, 0, "p1 A"), P1(8300, 0, 0, 0, "p1 release"),
	SHOT(8450, "exp1_02_jump"),
	P1(9500, PAD_X, 0, 0, "p1 X"), P1(9650, 0, 0, 0, "p1 release"),
	FORCE(11000, HERO_OBELIX, 0),
	SHOT(13500, "exp1_03_end"),
	END(14000),
};

// p2basic: player 2 joins on virtual pad 1 and drives the other hero alone; then
// both players move in opposite directions at once; then player 2 leaves.
const Step kP2Basic[] = {
	MARK(0, "baseline"), SHOT(100, "p2_00_start"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	VPAD(1500, 1, 0, 1, 0, "p2 right"),
	VPAD(3000, 1, 0, 0, 1, "p2 up"),
	VPAD(4500, 1, 0, -1, 0, "p2 left"),
	SHOT(5200, "p2_01_left"),
	VPAD(6000, 1, 0, 0, -1, "p2 down"),
	VPAD(7500, 1, 0, 0, 0, "p2 neutral"),
	VPAD(8000, 1, PAD_A, 0, 0, "p2 A"), VPAD(8300, 1, 0, 0, 0, "p2 release"),
	SHOT(8450, "p2_02_jump"),
	VPAD(9500, 1, PAD_X, 0, 0, "p2 X"), VPAD(9650, 1, 0, 0, 0, "p2 release"),
	P1(10500, 0, 1, 0, "p1 right"), VPAD(10500, 1, 0, -1, 0, "p2 left"),
	SHOT(11500, "p2_03_apart"),
	P1(12000, 0, 0, 0, "p1 neutral"), VPAD(12000, 1, 0, 0, 0, "p2 neutral"),
	VPAD(12500, 1, PAD_BACK, 0, 0, "p2 hold back"),
	VPAD(14500, 1, 0, 0, 0, "p2 release"),
	// Rejoin, then leave again with a second Start press.
	VPAD(15500, 1, PAD_START, 0, 0, "p2 start (rejoin)"), VPAD(15700, 1, 0, 0, 0, "p2 release"),
	VPAD(16500, 1, 0, 0, 1, "p2 up"), VPAD(17500, 1, 0, 0, 0, "p2 neutral"),
	VPAD(18000, 1, PAD_START, 0, 0, "p2 start (leave)"), VPAD(18200, 1, 0, 0, 0, "p2 release"),
	SHOT(19500, "p2_04_end"),
	END(20000),
};

// kbd: player 2 on the keyboard (numpad layout) while the game itself reads the
// keyboard for player 1 (no gamepad attached -> the game's keyboard controls).
// Join with HOME, move, jump, both players moving at once (player 1 through the
// real arrow keys), then a player 2 key that is also the game's jump key (SPACE)
// must jump only Obelix; hold END to leave.
const Step kKbd[] = {
	MARK(0, "baseline"), KEYMAP(0, 1),
	KEY(500, 0x24, 1, "p2 HOME (join)"), KEY(700, 0x24, 0, "p2 release"),
	KEY(1500, 0x66, 1, "p2 NUM6 (right)"), KEY(3000, 0x66, 0, "p2 release"),
	KEY(3000, 0x68, 1, "p2 NUM8 (up)"), KEY(4500, 0x68, 0, "p2 release"),
	KEY(5000, 0x60, 1, "p2 NUM0 (jump)"), KEY(5300, 0x60, 0, "p2 release"),
	SHOT(5400, "kbd_01_jump"),
	KEY(6500, 0x27, 1, "p1 RIGHT"), KEY(6500, 0x64, 1, "p2 NUM4 (left)"),
	SHOT(7500, "kbd_02_apart"),
	KEY(8000, 0x27, 0, "p1 release"), KEY(8000, 0x64, 0, "p2 release"),
	KEYMAP(8500, 2),
	KEY(9000, 0x20, 1, "p2 SPACE (jump, also the game's)"), KEY(9300, 0x20, 0, "p2 release"),
	KEY(10500, 0x23, 1, "p2 hold END (leave)"), KEY(12500, 0x23, 0, "p2 release"),
	KEY(13000, 0x20, 1, "SPACE after leaving (player 1 jumps)"), KEY(13300, 0x20, 0, "release"),
	END(14500),
};

// p2mirror: what happened on the user's PC -- player 2 "joins" on the pad the
// game itself reads. Pad 1 mirrors player 1's input exactly, so the mod must work
// out it is player 1's pad and drop player 2. Then a Start press on pad 2 at the
// same moment player 1 jumps must be refused as a join (it looks like a pad whose
// Start the game also sees).
const Step kP2Mirror[] = {
	MARK(0, "baseline"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	P1(1500, 0, 1, 0, "mirror right"), VPAD(1500, 1, 0, 1, 0, "mirror right"),
	P1(1750, 0, 0, 1, "mirror up"), VPAD(1750, 1, 0, 0, 1, "mirror up"),
	P1(2000, 0, -1, 0, "mirror left"), VPAD(2000, 1, 0, -1, 0, "mirror left"),
	P1(2250, 0, 0, -1, "mirror down"), VPAD(2250, 1, 0, 0, -1, "mirror down"),
	P1(2500, 0, 1, 0, "mirror right"), VPAD(2500, 1, 0, 1, 0, "mirror right"),
	P1(2750, 0, 0, 1, "mirror up"), VPAD(2750, 1, 0, 0, 1, "mirror up"),
	P1(3000, 0, -1, 0, "mirror left"), VPAD(3000, 1, 0, -1, 0, "mirror left"),
	P1(3250, 0, 0, -1, "mirror down"), VPAD(3250, 1, 0, 0, -1, "mirror down"),
	P1(3500, 0, 1, 0, "mirror right"), VPAD(3500, 1, 0, 1, 0, "mirror right"),
	P1(3750, 0, 0, 1, "mirror up"), VPAD(3750, 1, 0, 0, 1, "mirror up"),
	P1(4000, 0, -1, 0, "mirror left"), VPAD(4000, 1, 0, -1, 0, "mirror left"),
	P1(4250, 0, 0, -1, "mirror down"), VPAD(4250, 1, 0, 0, -1, "mirror down"),
	P1(4500, 0, 1, 0, "mirror right"), VPAD(4500, 1, 0, 1, 0, "mirror right"),
	P1(4750, 0, 0, 1, "mirror up"), VPAD(4750, 1, 0, 0, 1, "mirror up"),
	P1(5000, 0, -1, 0, "mirror left"), VPAD(5000, 1, 0, -1, 0, "mirror left"),
	P1(5250, 0, 0, -1, "mirror down"), VPAD(5250, 1, 0, 0, -1, "mirror down"),
	P1(5500, 0, 0, 0, "mirror stop"),  VPAD(5500, 1, 0, 0, 0, "mirror stop"),
	VPAD(6000, 1, PAD_START, 0, 0, "pad1 start again"), VPAD(6200, 1, 0, 0, 0, "pad1 release"),
	P1(7000, PAD_A, 0, 0, "p1 A"), VPAD(7000, 2, PAD_START, 0, 0, "pad2 start with p1 A"),
	P1(7200, 0, 0, 0, "p1 release"), VPAD(7200, 2, 0, 0, 0, "pad2 release"),
	END(8000),
};

// lifter (level 2): player 2 joins, Asterix is put on the level's lifter, and
// player 1 tries the buttons that could activate it. Watches situation 13 (the
// group action that crashed at axxl.exe+0x9C228), co-op suspension, and that
// Obelix walks to his marker on AI and comes back to player 2 afterwards.
const Step kLifter[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	TELEPORT(1500, HERO_ASTERIX, 0, 0, 0),
	TELEPORT(1500, HERO_OBELIX, 0, 6, 0),
	TELEPORT(1500, HERO_IDEFIX, 0, -6, 0),
	SHOT(2500, "lifter_00_on"),
	P1(3000, PAD_B, 0, 0, "p1 B"), P1(3200, 0, 0, 0, "p1 release"),
	P1(4500, PAD_Y, 0, 0, "p1 Y"), P1(4700, 0, 0, 0, "p1 release"),
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(9000, "lifter_01"),
	SHOT(16000, "lifter_02"),
	SHOT(26000, "lifter_03"),
	VPAD(27000, 1, 0, 1, 0, "p2 right"), VPAD(28500, 1, 0, 0, 0, "p2 neutral"),
	SHOT(29000, "lifter_04_end"),
	END(29500),
};

// warplift58 (-Level 5): Asterix waits on sector 8's lifter (-375.5 0.8 -39.2;
// in co-op it waits for both heroes), player 2's Obelix at the sector's
// entrance joins him with R3 + A: on a lifter's ground he lands on the lifter
// with Asterix (not on solid ground near it), and the lifter goes.
const Step kWarpLift58[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, -376.82f, 17.2f, 20.05f), PLACEAT(3000, HERO_ASTERIX, -375.46f, 0.8f, -39.19f),
	SHOT(5000, "warplift58_0"),
	VPAD(6000, 1, PAD_RTHUMB, 0, 0, "p2 R3"), VPAD(6200, 1, PAD_RTHUMB | PAD_A, 0, 0, "p2 R3 + A"),
	VPAD(6400, 1, 0, 0, 0, "p2 release"),
	SHOT(7000, "warplift58_1"), SHOT(11000, "warplift58_2"), SHOT(16000, "warplift58_3"),
	END(18000),
};

// potion: the magic potion while player 2 is in -- what each hero gets -- then
// a second potion while the first runs (the user saw Obelix running on for good
// after that), and the end of it.
const Step kPotion[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	POTION(1500),
	P1(2500, 0, 1, 0, "p1 right"), VPAD(2500, 1, 0, -1, 0, "p2 left"),
	P1(4000, 0, 0, 0, "p1 neutral"), VPAD(4000, 1, 0, 0, 0, "p2 neutral"),
	SHOT(4200, "potion_01"),
	VPAD(5000, 1, PAD_A, 0, 0, "p2 A"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	VPAD(5500, 1, PAD_A, 0, 0, "p2 A"), VPAD(5700, 1, 0, 0, 0, "p2 release"),
	VPAD(6000, 1, PAD_A, 0, 0, "p2 A"), VPAD(6200, 1, 0, 0, 0, "p2 release"),
	POTION(8000),
	VPAD(9000, 1, 0, 1, 0, "p2 right"), VPAD(10500, 1, 0, 0, 0, "p2 neutral"),
	END(70000),
};

// pursuit: does a Roman come after a hero standing 6 units away? First player
// 2's Obelix (not leading), then Asterix (the leader) at another Roman.
const Step kPursuitO[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_ENEMY, HERO_OBELIX, 2, 0, 0, 0, "obelix 6 from a roman" },
	SHOT(4000, "pursuit_o"),
	END(9000),
};
const Step kPursuitA[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman" },
	SHOT(4000, "pursuit_a"),
	END(9000),
};

// flask: Asterix picks up a magic potion flask while player 2 is in (the user
// saw player 2's Obelix start running on his own then).
const Step kFlask[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_BONUS, HERO_ASTERIX, 3, 0, 0, 0, "asterix to a flask (beacon)" },
	{ 1600, S_BONUS, HERO_ASTERIX, 2, 0, 0, 0, "asterix to a flask (active)" },
	SHOT(3000, "flask_01"),
	SHOT(8000, "flask_02"),
	END(15000),
};

// release: the game tells player 2's Obelix to wait (0x4802); R3 + B takes him
// back, and he walks.
const Step kRelease[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_TRIOEV, 0x4802, 0, 0, 0, 0, "companion wait" },
	VPAD(2500, 1, PAD_RTHUMB, 0, 0, "p2 R3"), VPAD(2700, 1, PAD_RTHUMB | PAD_B, 0, 0, "p2 R3+B"),
	VPAD(2900, 1, 0, 0, 0, "p2 release"),
	VPAD(3500, 1, 0, 0, 1, "p2 up"), VPAD(5500, 1, 0, 0, 0, "p2 neutral"),
	END(6500),
};

// pursuitlead2: Obelix leads (as after the belly slide); a Roman 6 units from
// player 1's Asterix must come for him.
const Step kPursuitLead2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 2000, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman" },
	SHOT(5000, "pursuitlead2"),
	END(10000),
};

// dash: the potion's dash flag goes up while player 2 is in; only the leader may
// dash (situation 0x10), player 2's Obelix keeps walking.
const Step kDash[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	DASHFLAG(1500, 1),
	VPAD(1600, 1, 0, 0, 1, "p2 up"), VPAD(3500, 1, 0, 0, 0, "p2 neutral"),
	DASHFLAG(4000, 0),
	END(6000),
};

// lifterp2: player 2's Obelix (not leading) works the lifter; he must lead
// the group action from then on.
const Step kLifterP2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	TELEPORT(1500, HERO_OBELIX, 0, 0, 0),
	TELEPORT(1500, HERO_ASTERIX, 0, 6, 0),
	TELEPORT(1500, HERO_IDEFIX, 0, -6, 0),
	VPAD(3000, 1, PAD_B, 0, 0, "p2 B"), VPAD(3200, 1, 0, 0, 0, "p2 release"),
	VPAD(4500, 1, PAD_Y, 0, 0, "p2 Y"), VPAD(4700, 1, 0, 0, 0, "p2 release"),
	VPAD(6000, 1, PAD_X, 0, 0, "p2 X"), VPAD(6200, 1, 0, 0, 0, "p2 release"),
	SHOT(9000, "lifterp2_01"),
	SHOT(20000, "lifterp2_02"),
	END(26000),
};

// repair: Obelix left in the launcher's flight mode (as in the user's save);
// player 2's Start must put him back and join.
const Step kRepair[] = {
	MARK(0, "baseline"),
	SETF18(500, HERO_OBELIX, 0x05),
	VPAD(7000, 1, PAD_START, 0, 0, "p2 start"), VPAD(7200, 1, 0, 0, 0, "p2 release"),
	VPAD(8500, 1, 0, 0, 1, "p2 up"), VPAD(10000, 1, 0, 0, 0, "p2 neutral"),
	END(11000),
};

// joinlead2: the game leads with Obelix (as after the belly slides) when player
// 2 joins: player 2 still gets Obelix, and leaving keeps Obelix leading.
// bossbutton: the boss's button sequence as in the user's log -- Asterix (with
// an old wait order 3 left over) in situation 0x0F, then "Obelix leads" and
// "the companion waits": Asterix must go to the game, not stay player 1's.
const Step kBossButton[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_POKE8, HERO_ASTERIX, 0x564, 0, 3.0f, 0, "asterix order 3" },
	{ 1500, S_POKE8, HERO_ASTERIX, 0x3E9, 0, 15.0f, 0, "asterix situation 0F" },
	{ 1600, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 1600, S_TRIOEV, 0x4802, 0, 0, 0, 0, "companion waits" },
	P1(2500, 0, 1, 0, "p1 right"), P1(4000, 0, 0, 0, "p1 neutral"),
	END(5000),
};

// music: sector 8 streamed in, both heroes at its entrance, then one hero walks
// (glides) to the spot where the music starts for the leader.
const Step kMusicA[] = {
	MARK(0, "baseline"),
	LOADSECTOR(500, 8),
	{ 3500, S_PLACE, HERO_ASTERIX, 6, 0, 0, 0, "asterix at entrance" },
	{ 3500, S_PLACE, HERO_OBELIX, 6, 0, 0, 0, "obelix at entrance" },
	{ 3500, S_PLACE, HERO_IDEFIX, 6, 0, 0, 0, "idefix at entrance" },
	SECTORS(4500),
	{ 1000, S_WATCHGROUPS, 0, 0, 0, 0, 0, "watch groups" },
	GLIDE(5000, HERO_ASTERIX, 8, 4000),
	SHOT(9500, "musica"),
	{ 9600, S_MUSICGROUPS, 0, 0, 0, 0, 0, "music groups" },
	END(14000),
};
const Step kMusicO[] = {
	MARK(0, "baseline"),
	LOADSECTOR(500, 8),
	{ 3500, S_PLACE, HERO_ASTERIX, 6, 0, 0, 0, "asterix at entrance" },
	{ 3500, S_PLACE, HERO_OBELIX, 6, 0, 0, 0, "obelix at entrance" },
	{ 3500, S_PLACE, HERO_IDEFIX, 6, 0, 0, 0, "idefix at entrance" },
	VPAD(4000, 1, PAD_START, 0, 0, "p2 start"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	VPAD(4600, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(4800, 1, PAD_BACK | PAD_A, 0, 0, "p2 back+A (warp to Asterix)"),
	VPAD(5000, 1, 0, 0, 0, "p2 release"),
	GLIDE(6000, HERO_OBELIX, 8, 4000),
	SHOT(10500, "musico"),
	SHOT(14000, "musico2"),
	END(15000),
};

// savestate: Obelix in the player variant without player 2 (a save made during
// co-op): he must go back to being the AI companion.
const Step kSaveState[] = {
	MARK(0, "baseline"),
	SETF18(500, HERO_OBELIX, 0x00),
	P1(1500, 0, 1, 0, "p1 right"), P1(3000, 0, 0, 0, "p1 neutral"),
	END(4000),
};

const Step kMusicA2[] = {
	MARK(0, "baseline"),
	LOADSECTOR(500, 8),
	{ 3500, S_PLACE, HERO_ASTERIX, 6, 0, 0, 0, "asterix at entrance" },
	{ 3500, S_PLACE, HERO_IDEFIX, 6, 0, 0, 0, "idefix at entrance" },
	VPAD(4000, 1, PAD_START, 0, 0, "p2 start"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	VPAD(4600, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(4800, 1, PAD_BACK | PAD_A, 0, 0, "p2 back+A (warp to Asterix)"),
	VPAD(5000, 1, 0, 0, 0, "p2 release"),
	GLIDE(6000, HERO_ASTERIX, 8, 4000),
	SHOT(10500, "musica2"),
	END(15000),
};

const Step kSpyGroups[] = {
	MARK(0, "baseline"), SECTORS(200),
	{ 500, S_EVGROUP, 0x98, 0, 0, 0, 0, "group 098 (#152)" },
	{ 500, S_EVGROUP, 0x9B, 0, 0, 0, 0, "group 09B (box 16)" },
	{ 500, S_EVGROUP, 0xE1, 0, 0, 0, 0, "group 0E1 (0708, spy dialogue)" },
	{ 500, S_DETECT, 0, 0, 0, 0, 0, "detect" },
	END(1000),
};

// spyo / spya: walk (glide) to the spy first with player 2's Obelix, or with
// Asterix; the spy's (CKHkClueMan) state is logged every tick.
const Step kSpyO[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDE(1500, HERO_OBELIX, 7, 4000),
	SHOT(6000, "spyo_1"),
	SHOT(9000, "spyo_2"),
	END(10000),
};
const Step kSpyA[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDE(1500, HERO_ASTERIX, 7, 4000),
	SHOT(6000, "spya_1"),
	SHOT(25000, "spya_2"),
	END(40000),
};

// icons21: player 2's Obelix walks to the first spy (LV2-1, the cable car's
// "press [action] for Obelix to grab the rope") and clicks through the
// dialogue with X, a shot of each page: its action icons are the players'
// buttons.
#define PAGE(t, n) SHOT(t, "icons21_" #n), VPAD(t + 300, 1, PAD_X, 0, 0, "p2 X"), VPAD(t + 500, 1, 0, 0, 0, "p2 r")
const Step kIcons21[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDE(1500, HERO_OBELIX, 7, 4000),
	PAGE(6000, 1), PAGE(8500, 2), PAGE(11000, 3), PAGE(13500, 4), PAGE(16000, 5), PAGE(18500, 6),
	PAGE(21000, 7), PAGE(23500, 8), PAGE(26000, 9), PAGE(28500, 10), PAGE(31000, 11), PAGE(33500, 12),
	END(35000),
};
#undef PAGE

// spy2o: player 2's Obelix goes on alone to the lifter area's spy (Asterix
// stays at the start); the spy has to appear for him.
const Step kSpy2O[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	// Teleported, not glided: crossing the first spy's zone starts its dialogue.
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	{ 1500, S_PLACE, HERO_OBELIX, 9, 0, 0, 0, "obelix near the lifter area's spy" },
	SHOT(5000, "spy2o_1"),
	END(8000),
};

// spywalk: player 2's Obelix sets off the first spy's dialogue with Asterix
// still at the start; the trio's walk has to bring Asterix there (or the mod
// puts him there) and the dialogue has to end (trio event 0x5121).
const Step kSpyWalk[] = {
	MARK(0, "baseline"), SECTORS(200),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDE(1500, HERO_OBELIX, 7, 4000),
	SHOT(12000, "spywalk_1"),
	P1(20000, PAD_A, 0, 0, "p1 A"), P1(20200, 0, 0, 0, "p1 release"),
	P1(23000, PAD_A, 0, 0, "p1 A"), P1(23200, 0, 0, 0, "p1 release"),
	P1(26000, PAD_A, 0, 0, "p1 A"), P1(26200, 0, 0, 0, "p1 release"),
	P1(29000, PAD_A, 0, 0, "p1 A"), P1(29200, 0, 0, 0, "p1 release"),
	P1(32000, PAD_A, 0, 0, "p1 A"), P1(32200, 0, 0, 0, "p1 release"),
	SHOT(34000, "spywalk_2"),
	END(40000),
};

// sas: player 1's Asterix (leading) is put in the Sas box that streams in
// sector 2; player 2's Obelix, left at the start (sector 1 only), has to be
// brought over.
const Step kSas[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_ASTERIX, 10, 0, 0, 0, "asterix into the sas" },
	SECTORS(4000),
	SHOT(7000, "sas_1"),
	END(9000),
};

// sectorzone: player 2's Obelix crosses the rectangle that switches to
// sector 2 (a zone, not a Sas) while Asterix, leading, stays in sector 1.
const Step kSectorZone[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_ASTERIX, 13, 0, 0, 0, "asterix behind" },
	{ 1500, S_PLACE, HERO_OBELIX, 11, 0, 0, 0, "obelix before the rectangles" },
	GLIDE(2500, HERO_OBELIX, 12, 2000),
	SHOT(8000, "sectorzone_1"),
	END(10000),
};

// sasnl21 (the user's LV2-1 spots): Asterix, leading, stands in the Sas half
// that streams sector 1; player 2's Obelix walks through the half that streams
// sector 2, then over the rectangles (-810.6 531.2 -985 -> -989) that switch to
// it. Sector 2 should come with the Sas, and Asterix go to Obelix.
const Step kSasNl21[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 800, S_WATCHRW, HERO_ASTERIX, 0, 0, 8, 0, "requested sector writes" },
	PLACEAT(1500, HERO_ASTERIX, -802.514f, 526.927f, -970.293f),
	PLACEAT(1500, HERO_OBELIX, -803.5f, 527.5f, -972.0f),
	SECTORS(2500),
	GLIDEAT(3000, HERO_OBELIX, -811.770f, 528.636f, -970.530f, 1200),
	SECTORS(5500), SHOT(5500, "sasnl21_1"),
	GLIDEAT(6500, HERO_OBELIX, -810.617f, 531.231f, -985.0f, 1500),
	GLIDEAT(8200, HERO_OBELIX, -810.617f, 531.231f, -989.0f, 800),
	SECTORS(11000), SHOT(11000, "sasnl21_2"),
	SHOT(14000, "sasnl21_3"),
	END(15000),
};

// sasnl21w: sasnl21 walked with player 2's stick instead of glided (a glided
// hero is held in the air, and the game's physics put it back on its last
// ground -- in sector 1's half -- while the old sector was out).
const Step kSasNl21w[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -802.514f, 526.927f, -970.293f),
	PLACEAT(1500, HERO_OBELIX, -803.5f, 527.5f, -972.0f),
	SECTORS(2500),
	{ 3000, S_P2LOOK, 0, 0, 0, -811.77f, 528.6f, "p2 camera at sector 2's half", -970.53f },
	VPAD(3500, 1, 0, 0, 0.6f, "p2 forward"), VPAD(4600, 1, 0, 0, 0, "p2 stop"),
	SECTORS(6000), SHOT(6000, "sasnl21w_1"),
	{ 7000, S_P2LOOK, 0, 0, 0, -810.617f, 531.231f, "p2 camera at the rectangles", -990.0f },
	VPAD(7500, 1, 0, 0, 0.6f, "p2 forward"), VPAD(9500, 1, 0, 0, 0, "p2 stop"),
	SECTORS(11000), SHOT(11000, "sasnl21w_2"),
	SHOT(14000, "sasnl21w_3"),
	END(15000),
};

// warp25 (the user's LV2-5 spots): player 2's Obelix in the Sas half that
// streams sector 5, Asterix, leading, on the ground in sector 5; Obelix joins
// Asterix with R3 + A -- sector 5 has to stay.
const Step kWarp25[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 138.042f, 2.584f, -564.668f),
	PLACEAT(2500, HERO_OBELIX, 97.622f, 2.465f, -524.185f),
	SECTORS(4000), SHOT(4500, "warp25_1"),
	{ 5000, S_WATCHRW, HERO_ASTERIX, 0, 0, 8, 0, "requested sector writes" },
	VPAD(7000, 1, PAD_RTHUMB, 0, 0, "p2 R3"), VPAD(7100, 1, PAD_RTHUMB | PAD_A, 0, 0, "p2 R3 + A"),
	VPAD(7300, 1, 0, 0, 0, "p2 release"),
	SECTORS(8500), SHOT(8500, "warp25_2"),
	SHOT(12000, "warp25_3"),
	SECTORS(13000),
	END(14000),
};

// boot0 (XXL_LEVEL=-1): the game's start untouched -- level 0's intro, the logo,
// the title screen: every scene's flags as they change, screenshots.
const Step kBoot0[] = {
	MARK(0, "level 0"), { 0, S_WATCHCINE, 0, 0, 0, 0, 0, "watch scenes" },
	{ 300, S_CINEDUMP, -1, 0, 0, 0, 0, "the intro" },
	{ 17000, S_CINEDUMP, 1, 0, 0, 0, 0, "scene 1" }, { 17100, S_CINEDUMP, 3, 0, 0, 0, 0, "scene 3" },
	{ 17200, S_CINEDUMP, 4, 0, 0, 0, 0, "scene 4" },
	SHOT(5000, "boot0_05"), SHOT(12000, "boot0_12"), SHOT(20000, "boot0_20"), SHOT(27000, "boot0_27"),
	SHOT(35000, "boot0_35"), SHOT(45000, "boot0_45"), SHOT(55000, "boot0_55"), SHOT(65000, "boot0_65"),
	SHOT(80000, "boot0_80"), SHOT(95000, "boot0_95"), SHOT(110000, "boot0_110"), SHOT(125000, "boot0_125"),
	SHOT(140000, "boot0_140"), SHOT(155000, "boot0_155"), SHOT(170000, "boot0_170"), SHOT(185000, "boot0_185"),
	SHOT(200000, "boot0_200"), SHOT(220000, "boot0_220"),
	END(240000),
};

// boot1 / boot2 (XXL_LEVEL=-1): one skip at the game's start -- in the intro, 2 s
// in (about 12 s after launch), or in the film after it, 17 s in (about 27 s):
// either lands on the title, its logo shown ("Press Validate").
#define BOOT_SKIP(at, n) 	MARK(0, "level 0"), { 0, S_WATCHCINE, 0, 0, 0, 0, 0, "watch scenes" }, 	CINE_ENTER(at), 	SHOT(at + 1500, n "_a"), SHOT(at + 4000, n "_b"), SHOT(at + 7000, n "_c"), SHOT(at + 12000, n "_d"), 	END(at + 16000)
const Step kBoot1[] = { BOOT_SKIP(2000, "boot1") };
const Step kBoot2[] = { BOOT_SKIP(17000, "boot2") };

// cine1 (-Level 1): the cinematic scenes' graphs -- the one playing (the intro)
// and the first few.
const Step kCine1[] = {
	MARK(0, "baseline"),
	{ 100, S_CINEDUMP, -1, 0, 0, 0, 0, "playing scene" },
	{ 300, S_CINEDUMP, 0, 0, 0, 0, 0, "scene 0" }, { 400, S_CINEDUMP, 1, 0, 0, 0, 0, "scene 1" },
	{ 500, S_CINEDUMP, 2, 0, 0, 0, 0, "scene 2" }, { 600, S_CINEDUMP, 3, 0, 0, 0, 0, "scene 3" },
	END(1500),
};

// skip1 (-Level 1): the intro skipped as Enter does, 1.5 s in: what its later
// nodes would have done still happens (the level's music: event 0020 -> the
// stream). intro1: the intro played to its end, for comparison.
const Step kSkip1[] = {
	MARK(0, "baseline"),
	{ 1400, S_CINEDUMP, 1, 0, 0, 0, 0, "the intro" },
	CINE_ENTER(1500),
	SHOT(4000, "skip1_0"), SHOT(12000, "skip1_1"),
	END(15000),
};
const Step kIntro1[] = {
	MARK(0, "baseline"),
	SHOT(30000, "intro1_0"), SHOT(90000, "intro1_1"),
	END(150000),
};

// torch: list the torches, move the nearest to Asterix, give it, later drop it.
const Step kTorch[] = {
	MARK(0, "baseline"), { 500, S_TORCH, 0, 0, 0, 0, 0, "torches" },
	{ 1500, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	SHOT(2500, "torch_0"),
	{ 3000, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	SHOT(5000, "torch_1"),
	{ 7000, S_TORCH, 7, 0, 0, 0, 0, "cheat: throw fire away" },
	SHOT(9000, "torch_2"),
	END(11000),
};

// torchgrab: Asterix put next to the torch picks it up with the buttons.
const Step kTorchGrab[] = {
	MARK(0, "baseline"), { 500, S_TORCH, 0, 0, 0, 0, 0, "torches" },
	{ 900, S_TORCH, 4, 0, 0, 0, 0, "watch offer" },
	{ 1000, S_PLACE, HERO_ASTERIX, 14, 0, 0, 0, "asterix at the torch" },
	P1(2500, PAD_Y, 0, 0, "p1 Y"), P1(2700, 0, 0, 0, "p1 release"),
	SHOT(3500, "torchgrab_y"),
	P1(4500, PAD_B, 0, 0, "p1 B"), P1(4700, 0, 0, 0, "p1 release"),
	SHOT(5500, "torchgrab_b"),
	P1(6500, PAD_X, 0, 0, "p1 X"), P1(6700, 0, 0, 0, "p1 release"),
	SHOT(7500, "torchgrab_x"),
	END(9000),
};

// torchnl2 (level 2): Obelix leads (4801); player 1's Asterix (not leading) takes
// the torch (-744.2 513.8 -982.1) with X, lights it at the lit hearth beside it
// (-745.1 513.9 -978.6), lights the unlit hearth (from -785.7 526.8 -983.0), then
// LV2-3's keg (from -50.7 0.8 -522.0). torchl2: the same with Asterix leading.
const Step kTorchNL2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 600, S_TORCH, 0, 0, 0, 0, 0, "torches" },
	{ 900, S_TORCH, 4, 0, 0, 0, 0, "watch offer" },
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(1500, HERO_ASTERIX, -744.26f, 514.4f, -981.0f),
	SHOT(2700, "torchnl2_0"),
	P1(3000, PAD_X, 0, 0, "p1 X (take the torch)"), P1(3200, 0, 0, 0, "p1 release"),
	SHOT(4500, "torchnl2_1"),
	PLACEAT(5000, HERO_ASTERIX, -744.14f, 514.3f, -979.3f),
	P1(6000, PAD_X, 0, 0, "p1 X (lit hearth)"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(7000, "torchnl2_2"),
	PLACEAT(8000, HERO_ASTERIX, -785.748f, 527.1f, -982.975f),
	P1(9500, PAD_X, 0, 0, "p1 X (unlit hearth)"), P1(9700, 0, 0, 0, "p1 release"),
	SHOT(10500, "torchnl2_3"),
	LOADSECTOR(11000, 3),
	PLACEAT(12000, HERO_ASTERIX, -50.698f, 1.0f, -521.963f), PLACEAT(12000, HERO_OBELIX, -60.0f, 1.0f, -535.0f),
	P1(13500, PAD_X, 0, 0, "p1 X (keg)"), P1(13700, 0, 0, 0, "p1 release"),
	SHOT(14000, "torchnl2_4"),
	SHOT(15500, "torchnl2_5"),
	END(17000),
};

const Step kTorchL2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 600, S_TORCH, 0, 0, 0, 0, 0, "torches" },
	{ 900, S_TORCH, 4, 0, 0, 0, 0, "watch offer" },
	PLACEAT(1500, HERO_ASTERIX, -744.26f, 514.4f, -981.0f),
	SHOT(2700, "torchl2_0"),
	P1(3000, PAD_X, 0, 0, "p1 X (take the torch)"), P1(3200, 0, 0, 0, "p1 release"),
	SHOT(4500, "torchl2_1"),
	PLACEAT(5000, HERO_ASTERIX, -744.14f, 514.3f, -979.3f),
	P1(6000, PAD_X, 0, 0, "p1 X (lit hearth)"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(7000, "torchl2_2"),
	PLACEAT(8000, HERO_ASTERIX, -785.748f, 527.1f, -982.975f),
	P1(9500, PAD_X, 0, 0, "p1 X (unlit hearth)"), P1(9700, 0, 0, 0, "p1 release"),
	SHOT(10500, "torchl2_3"),
	LOADSECTOR(11000, 3),
	PLACEAT(12000, HERO_ASTERIX, -50.698f, 1.0f, -521.963f), PLACEAT(12000, HERO_OBELIX, -60.0f, 1.0f, -535.0f),
	P1(13500, PAD_X, 0, 0, "p1 X (keg)"), P1(13700, 0, 0, 0, "p1 release"),
	SHOT(14000, "torchl2_4"),
	SHOT(15500, "torchl2_5"),
	END(17000),
};

// lever: sector 5's lever behind the iron crates ("switch to Asterix") hit by
// player 2's Obelix while Asterix leads: safe sequence keeps it from working.
const Step kLever[] = {
	MARK(0, "baseline"),
	LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2500, S_PLACE, HERO_ASTERIX, 16, 0, 0, 0, "asterix at the hearth" },
	{ 2600, S_LEVERS, 0, 0, 0, 0, 0, "levers" },
	{ 3000, S_PLACE, HERO_OBELIX, 15, 0, 0, 0, "obelix at the lever" },
	VPAD(3800, 1, 0, -1, -1, "p2 turn back-left"), VPAD(3900, 1, 0, 0, 0, "p2 stop"),
	VPAD(4300, 1, PAD_X, 0, 0, "p2 X"), VPAD(4500, 1, 0, 0, 0, "p2 release"),
	VPAD(5300, 1, 0, -1, 0, "p2 turn left"), VPAD(5400, 1, 0, 0, 0, "p2 stop"),
	VPAD(5800, 1, PAD_X, 0, 0, "p2 X"), VPAD(6000, 1, 0, 0, 0, "p2 release"),
	VPAD(6800, 1, 0, 0, -1, "p2 turn back"), VPAD(6900, 1, 0, 0, 0, "p2 stop"),
	VPAD(7300, 1, PAD_X, 0, 0, "p2 X"), VPAD(7500, 1, 0, 0, 0, "p2 release"),
	VPAD(8300, 1, 0, -1, -1, "p2 back-left step"), VPAD(8600, 1, 0, 0, 0, "p2 stop"),
	VPAD(9000, 1, PAD_X, 0, 0, "p2 X"), VPAD(9200, 1, 0, 0, 0, "p2 release"),
	SHOT(10000, "lever_1"),
	END(12000),
};

// launchp2: player 2's Obelix (not leading) starts sector 2's launcher; co-op
// has to suspend as for the leader; Obelix keeps the lead he took for it (round
// 142), and the landing ground's own switch hands it to Asterix.
// launchp2 / launcha: sector 2's launcher started by player 2's Obelix (not
// leading) or by the leader Asterix; afterwards both walk, the game camera is
// logged. Co-op has to suspend for the flight either way, Asterix lead after.
const Step kLaunchP2[] = {
	MARK(0, "baseline"), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_ASTERIX, 10, 0, 0, 0, "asterix into the sas (sector 2)" },
	{ 4000, S_PLACE, HERO_ASTERIX, 18, 0, 0, 0, "other hero next to the launcher" },
	{ 4000, S_PLACE, HERO_OBELIX, 17, 0, 0, 0, "launcher hero on the launcher" },
	VPAD(5500, 1, PAD_X, 0, 0, "p2 X"), VPAD(5700, 1, 0, 0, 0, "p2 release"),
	SHOT(8000, "launch_1"),
	SHOT(31000, "launch_2"),
	P1(32000, 0, 0, 1, "p1 forward"), SHOT(34000, "launch_3"),
	P1(35000, 0, 1, 1, "p1 forward right"), SHOT(37000, "launch_4"),
	P1(38000, 0, 0, 0, "p1 stop"),
	VPAD(38500, 1, 0, 0, 1, "p2 forward"), SHOT(40500, "launch_5"), VPAD(41000, 1, 0, 0, 0, "p2 stop"),
	END(42000),
};
const Step kLaunchA[] = {
	MARK(0, "baseline"), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_ASTERIX, 10, 0, 0, 0, "asterix into the sas (sector 2)" },
	{ 4000, S_PLACE, HERO_OBELIX, 18, 0, 0, 0, "other hero next to the launcher" },
	{ 4000, S_PLACE, HERO_ASTERIX, 17, 0, 0, 0, "launcher hero on the launcher" },
	P1(5500, PAD_X, 0, 0, "p1 X"), P1(5700, 0, 0, 0, "p1 release"),
	SHOT(8000, "launch_1"),
	SHOT(31000, "launch_2"),
	P1(32000, 0, 0, 1, "p1 forward"), SHOT(34000, "launch_3"),
	P1(35000, 0, 1, 1, "p1 forward right"), SHOT(37000, "launch_4"),
	P1(38000, 0, 0, 0, "p1 stop"),
	VPAD(38500, 1, 0, 0, 1, "p2 forward"), SHOT(40500, "launch_5"), VPAD(41000, 1, 0, 0, 0, "p2 stop"),
	END(42000),
};



// lvl1 (run with -Level 1): the intro skipped, then player 2 tries to join
// before Obelix is found; state logged.
const Step kLvl1[] = {
	MARK(0, "baseline"), CINE_LIST(0), CINE_SKIP(200), CINE_LIST(600),
	SHOT(2500, "lvl1_1"), CINE_LIST(3000), CINE_LIST(6000), CINE_LIST(9000), CINE_LIST(12000),
	VPAD(13000, 1, PAD_START, 0, 0, "p2 start"), VPAD(13200, 1, 0, 0, 0, "p2 release"),
	SHOT(15000, "lvl1_2"),
	SECTORS(15500),
	END(17000),
};

// obelix1s (-Level 1): lvl1obelix with both cutscenes skipped as Enter does --
// the Obelix one 1 s in: its later events (music) must still happen.
// obelix1f: the Obelix cutscene played to its end, for comparison.
const Step kObelix1S[] = {
	MARK(0, "baseline"), CINE_ENTER(200),
	{ 7000, S_PLACE, HERO_ASTERIX, 19, 0, 0, 0, "asterix to sector 2's zone" },
	{ 9000, S_PLACE, HERO_ASTERIX, 20, 0, 0, 0, "asterix before the rectangle" },
	GLIDE(9500, HERO_ASTERIX, 21, 1500),
	{ 11900, S_CINEDUMP, 2, 0, 0, 0, 0, "the obelix cutscene" },
	CINE_ENTER(12000),
	SHOT(18000, "obelix1s_0"),
	END(24000),
};
const Step kObelix1F[] = {
	MARK(0, "baseline"), CINE_ENTER(200),
	{ 7000, S_PLACE, HERO_ASTERIX, 19, 0, 0, 0, "asterix to sector 2's zone" },
	{ 9000, S_PLACE, HERO_ASTERIX, 20, 0, 0, 0, "asterix before the rectangle" },
	GLIDE(9500, HERO_ASTERIX, 21, 1500),
	SHOT(40000, "obelix1f_0"),
	END(70000),
};

// lvl1obelix (-Level 1): Asterix finds Obelix (sector 2's rectangle starts the
// cutscene, then trio 0x4883); player 2 can join only after that.
const Step kLvl1Obelix[] = {
	MARK(0, "baseline"), CINE_SKIP(200),
	VPAD(6500, 1, PAD_START, 0, 0, "p2 start (too early)"), VPAD(6700, 1, 0, 0, 0, "p2 release"),
	{ 7000, S_PLACE, HERO_ASTERIX, 19, 0, 0, 0, "asterix to sector 2's zone" },
	{ 8800, S_DETECT, 0, 0, 0, 0, 0, "detect" },
	{ 9000, S_PLACE, HERO_ASTERIX, 20, 0, 0, 0, "asterix before the rectangle" },
	GLIDE(9500, HERO_ASTERIX, 21, 1500),
	SHOT(11500, "lvl1obelix_1"),
	CINE_SKIP(12000),
	SHOT(18000, "lvl1obelix_2"),
	VPAD(19000, 1, PAD_START, 0, 0, "p2 start"), VPAD(19200, 1, 0, 0, 0, "p2 release"),
	VPAD(20000, 1, 0, 0, 1, "p2 forward"), VPAD(22000, 1, 0, 0, 0, "p2 stop"),
	SHOT(22500, "lvl1obelix_3"),
	END(24000),
};

// potionmove (level 2): both heroes run and jump without, then with the potion.
const Step kPotionMove[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields" },
	P1(2000, 0, 0, 1, "p1 run"), VPAD(2000, 1, 0, 0, 1, "p2 run"),
	{ 3500, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields running" },
	P1(4000, 0, 0, 0, "p1 stop"), VPAD(4000, 1, 0, 0, 0, "p2 stop"),
	P1(4500, PAD_A, 0, 0, "p1 A"), P1(4700, 0, 0, 0, "p1 r"), P1(4900, PAD_A, 0, 0, "p1 A"), P1(5100, 0, 0, 0, "p1 r"),
	P1(5300, PAD_A, 0, 0, "p1 A"), P1(5500, 0, 0, 0, "p1 r"),
	VPAD(5500, 1, PAD_A, 0, 0, "p2 A"), VPAD(5700, 1, 0, 0, 0, "p2 r"), VPAD(5900, 1, PAD_A, 0, 0, "p2 A"), VPAD(6100, 1, 0, 0, 0, "p2 r"),
	VPAD(6300, 1, PAD_A, 0, 0, "p2 A"), VPAD(6500, 1, 0, 0, 0, "p2 r"),
	POTION(7000),
	{ 7500, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields potion" },
	P1(8000, 0, 0, -1, "p1 run back"), VPAD(8000, 1, 0, 0, -1, "p2 run back"),
	{ 9500, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields potion running" },
	P1(10000, 0, 0, 0, "p1 stop"), VPAD(10000, 1, 0, 0, 0, "p2 stop"),
	P1(10500, PAD_A, 0, 0, "p1 A"), P1(10700, 0, 0, 0, "p1 r"), P1(10900, PAD_A, 0, 0, "p1 A"), P1(11100, 0, 0, 0, "p1 r"),
	P1(11300, PAD_A, 0, 0, "p1 A"), P1(11500, 0, 0, 0, "p1 r"),
	VPAD(11500, 1, PAD_A, 0, 0, "p2 A"), VPAD(11700, 1, 0, 0, 0, "p2 r"), VPAD(11900, 1, PAD_A, 0, 0, "p2 A"), VPAD(12100, 1, 0, 0, 0, "p2 r"),
	VPAD(12300, 1, PAD_A, 0, 0, "p2 A"), VPAD(12500, 1, 0, 0, 0, "p2 r"),
	END(13000),
};

// hurto / hurtop: player 2's Obelix stands 2 units from a Roman for a while,
// without / with the magic potion (invincibility).
const Step kHurtO[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_ENEMY, HERO_OBELIX, 1, 0, 0, 0, "obelix next to a roman" },
	SHOT(6000, "hurt_1"),
	END(14000),
};
const Step kHurtOP[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	POTION(1000),
	{ 1500, S_ENEMY, HERO_OBELIX, 1, 0, 0, 0, "obelix next to a roman" },
	SHOT(6000, "hurt_1"),
	END(14000),
};

// plate3 (-Level 3): player 2's Obelix (not leading) steps on sector 1's
// pressure plate (194.9 0.1 18.8, "activatable by hero"), then off it.
const Step kPlate3[] = {
	MARK(0, "baseline"), { 0, S_PLATES, 0, 0, 0, 0, 0, "plates" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, 194.85f, 1.5f, 18.76f),
	{ 3500, S_PLATES, 0, 0, 0, 0, 0, "plates" },
	SHOT(4000, "plate3_1"),
	PLACEAT(5000, HERO_OBELIX, 194.85f, 1.5f, 26.0f),
	{ 7000, S_PLATES, 0, 0, 0, 0, 0, "plates" },
	END(7500),
};

// plate26 (level 2): sector 6's plate at 5.7 2.8 -389.2 (its sequence also
// sends the trio 0x2000) pressed by player 2's Obelix, not leading.
const Step kPlate26[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 5.7f, 3.5f, -400.0f), PLACEAT(2500, HERO_OBELIX, 5.7f, 3.5f, -389.2f),
	{ 4500, S_PLATES, 0, 0, 0, 0, 0, "plates" },
	SHOT(5000, "plate26_1"), SHOT(7000, "plate26_2"),
	END(7500),
};

// spy21o (level 2): the user's softlock -- Asterix sets the first spy's
// dialogue off, then waits by it while player 2's Obelix sets the second spy's
// off: Asterix (leading) has to get there.
const Step kSpy21O[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	PLACEAT(2000, HERO_ASTERIX, -817.82f, 508.0f, -1062.25f), PLACEAT(2000, HERO_OBELIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(3000, HERO_OBELIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(4500, "spy21o_1"), SHOT(6000, "spy21o_2"), SHOT(8000, "spy21o_3"), SHOT(11000, "spy21o_4"),
	END(40000),
};

// spy21a: the same, but player 1's Asterix (leading) sets the second spy off
// while Obelix waits by the first: how the game brings the companion in.
const Step kSpy21A[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	PLACEAT(2000, HERO_OBELIX, -817.82f, 508.0f, -1062.25f), PLACEAT(2000, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(3000, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(4500, "spy21a_1"), SHOT(6000, "spy21a_2"), SHOT(8000, "spy21a_3"), SHOT(11000, "spy21a_4"),
	END(40000),
};

// beach12o (-Level 1): sector 2's beach Romans, player 2's Obelix (not
// leading) next to them, Asterix away; then Obelix leads (0x4801).
// boss210 (-Level 2): sector 10's boss gate (rectangles #34 at 1672.3 and #4 at
// 1687.1, both bound to Asterix) crossed by player 2's Obelix, Asterix 50 units back.
const Step kBoss210[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1640.0f, -742.5f, -1210.1f), PLACEAT(3000, HERO_OBELIX, 1665.34f, -742.8f, -1210.13f),
	SECTORS(3500), SHOT(4500, "boss_1"),
	GLIDEAT(5000, HERO_OBELIX, 1695.0f, -742.8f, -1210.5f, 3000),
	SHOT(8500, "boss_2"), SHOT(11000, "boss_3"), SHOT(14000, "boss_4"),
	GLIDEAT(17000, HERO_OBELIX, 1697.0f, -742.8f, -1210.5f, 4000),
	SHOT(21500, "boss_5"), SHOT(24000, "boss_6"), SHOT(28000, "boss_7"), SHOT(33000, "boss_8"), SHOT(38000, "boss_9"),
	END(40000),
};

// boss210s (-Level 2): player 2's Obelix crosses the boss gate's #34 (its
// cutscene, skipped as Enter does 1.5 s in), Asterix 25 units back; then Obelix
// crosses #4 (the boss): Asterix must be brought to him, as without the skip.
// boss210f: the same with the cutscene played to its end.
const Step kBoss210s[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1640.0f, -742.5f, -1210.1f), PLACEAT(3000, HERO_OBELIX, 1665.34f, -742.8f, -1210.13f),
	GLIDEAT(4000, HERO_OBELIX, 1678.0f, -742.8f, -1210.5f, 1500),
	{ 7400, S_CINEDUMP, -1, 0, 0, 0, 0, "the boss cutscene" },
	CINE_ENTER(7500),
	SHOT(9000, "boss210s_0"),
	GLIDEAT(14000, HERO_OBELIX, 1695.0f, -742.8f, -1210.5f, 2000),
	SHOT(19000, "boss210s_1"), SHOT(25000, "boss210s_2"),
	END(28000),
};
const Step kBoss210f[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1640.0f, -742.5f, -1210.1f), PLACEAT(3000, HERO_OBELIX, 1665.34f, -742.8f, -1210.13f),
	GLIDEAT(4000, HERO_OBELIX, 1678.0f, -742.8f, -1210.5f, 1500),
	SHOT(9000, "boss210f_0"),
	GLIDEAT(40000, HERO_OBELIX, 1695.0f, -742.8f, -1210.5f, 2000),
	SHOT(45000, "boss210f_1"), SHOT(51000, "boss210f_2"),
	END(54000),
};

// boss210u / boss210v (-Level 2): the user's (userlogs xxl_coop.lv2-10_asterix_no_tp):
// player 2's Obelix crosses #34 beside Asterix (u: its cutscene skipped as Enter
// does; v: played out), then from the user's spots Obelix crosses #4 with Asterix
// 9 units behind (1682.8 -743.8 -1207.1): Asterix must be brought into the run.
#define BOSS210_USER(skip) 	MARK(0, "baseline"), LOADSECTOR(200, 10), 	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), 	PLACEAT(3000, HERO_ASTERIX, 1665.24f, -743.4f, -1212.82f), PLACEAT(3000, HERO_OBELIX, 1670.51f, -743.4f, -1212.8f), 	GLIDEAT(4000, HERO_OBELIX, 1678.0f, -743.4f, -1212.8f, 1500), 	skip, 	PLACEAT(25000, HERO_ASTERIX, 1682.77f, -743.4f, -1207.05f), PLACEAT(25000, HERO_OBELIX, 1682.87f, -743.4f, -1212.85f), 	GLIDEAT(26000, HERO_OBELIX, 1695.0f, -747.4f, -1210.5f, 2000), 	SHOT(30000, "boss210u_1"), SHOT(36000, "boss210u_2"), 	END(38000)
const Step kBoss210u[] = { BOSS210_USER(CINE_ENTER(7500)) };
const Step kBoss210v[] = { BOSS210_USER(MARK(7500, "no skip")) };

// boss210t (-Level 2, XXL_NOLOCK=1): the user's -- Asterix crosses the boss gate
// (#34 loads it, its cutscene; #4 sets it moving), both heroes wait out of its
// way, then Asterix takes the automatic cable car, whose end jumps him onto the
// boss's button: the boss's "Obelix leads, Asterix waits" must lock him there.
const Step kBoss210t[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1666.036f, -743.845f, -1212.924f), PLACEAT(3000, HERO_OBELIX, 1665.036f, -743.845f, -1213.924f),
	GLIDEAT(4000, HERO_ASTERIX, 1678.0f, -740.0f, -1210.9f, 1500),
	CINE_SKIP(7000),   // (Enter, as the user skips it)
	GLIDEAT(10000, HERO_ASTERIX, 1692.0f, -740.0f, -1210.9f, 1500),
	PLACEAT(13000, HERO_ASTERIX, 1715.187f, -747.845f, -1139.669f), PLACEAT(13000, HERO_OBELIX, 1714.187f, -747.845f, -1138.669f),
	SHOT(20000, "boss210t_0"),
	PLACEAT(30000, HERO_ASTERIX, 1717.434f, -738.903f, -1141.398f),   // the cable car
	SHOT(38000, "boss210t_1"), SHOT(42000, "boss210t_2"), SHOT(48000, "boss210t_3"),
	END(55000),
};

// boss36 (-Level 3): sector 6's boss. Obelix crosses the rectangle that loads it
// (-208.3 11.0 -199.2 -> -226), then drops onto #46 (-232.0 9.8 -173.9) that
// wakes it; Asterix stays back by the loading rectangle.
const Step kBoss36[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -205.0f, 11.5f, -202.0f), PLACEAT(3000, HERO_OBELIX, -208.28f, 11.5f, -199.22f),
	SECTORS(3500),
	GLIDEAT(4000, HERO_OBELIX, -226.0f, 11.5f, -199.22f, 1500),
	SHOT(6000, "boss36_1"),
	PLACEAT(8000, HERO_OBELIX, -232.0f, 16.0f, -173.9f),
	SHOT(9500, "boss36_2"),
	{ 11000, S_NEAROBJ, HERO_OBELIX, 177, 0, 12.0f, 0, "obelix away from the machine" },
	{ 12500, S_NEAROBJ, HERO_OBELIX, 177, 0, 30.0f, 0, "obelix further away" },
	{ 14000, S_NEAROBJ, HERO_OBELIX, 177, 0, 50.0f, 0, "obelix further away" },
	SHOT(15000, "boss36_4"), SHOT(17000, "boss36_5"), SHOT(20000, "boss36_6"),
	END(22000),
};

// boss36s (-Level 3): the user's run -- the boss loaded (-208.3 -> -226), both
// heroes drop onto #46 (-232.0 11.0 -173.9) and go on towards x -248, the boss
// rolls after them: both players' cameras shake.
const Step kBoss36s[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -208.28f, 11.5f, -200.22f), PLACEAT(3000, HERO_OBELIX, -208.28f, 11.5f, -199.22f),
	GLIDEAT(4000, HERO_OBELIX, -226.0f, 11.5f, -199.22f, 1500),
	PLACEAT(7000, HERO_ASTERIX, -232.5f, 16.0f, -174.4f), PLACEAT(7000, HERO_OBELIX, -232.0f, 16.0f, -173.9f),
	SHOT(8500, "boss36s_0"),
	GLIDEAT(9000, HERO_ASTERIX, -248.0f, 11.0f, -174.4f, 3000), GLIDEAT(9000, HERO_OBELIX, -248.0f, 11.0f, -173.4f, 3000),
	SHOT(10500, "boss36s_1"), SHOT(10600, "boss36s_2"), SHOT(12000, "boss36s_3"), SHOT(12100, "boss36s_4"),
	END(16000),
};

// sky2: player 2 turns their camera half around (right stick), then both
// cameras hold still facing opposite ways; XXL_SKYLOG logs the sky's scroll.
const Step kSky2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	SHOT(2500, "sky_1"),
	VPADR(3000, 1, 1.0f, 0.0f, "p2 camera right"), VPADR(4600, 1, 0.0f, 0.0f, "p2 camera still"),
	SHOT(5000, "sky_2"), SHOT(5300, "sky_3"), SHOT(8000, "sky_4"),
	END(10000),
};

// shadow33 (-Level 3): sector 3's room (x 290.1, z -331.7); player 2's Obelix
// (not leading) 8 units from Asterix; XXL_SHADOWFIX=0 for the game's own shadows.
const Step kShadow33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 290.14f, 40.0f, -331.68f), PLACEAT(3000, HERO_OBELIX, 298.14f, 40.0f, -331.68f),
	SECTORS(3500), SHOT(6000, "shadow_1"),
	PLACEAT(6500, HERO_OBELIX, 290.14f, 40.0f, -323.68f), SHOT(9000, "shadow_2"),
	END(9500),
};

// shadow3s (-Level 3): at the level's start with sector 1 unloaded (the shadows
// show clearly there), player 2's Obelix 6 units from Asterix, then to his side.
const Step kShadow3S[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(2000, 4),
	PLACEAT(4000, HERO_OBELIX, -5.7f, 0.0f, 36.0f), SHOT(6500, "shadow3s_1"),
	PLACEAT(7000, HERO_OBELIX, -11.7f, 0.0f, 42.0f), SHOT(9500, "shadow3s_2"),
	END(10000),
};

// shadow29 (-Level 2): sector 9's white floor (1516.3 -640.5 -882.7), player 2's
// Obelix 5 units from Asterix, then the other side; XXL_SHADOWFIX 0/1/2.
const Step kShadow29[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 9),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1516.26f, -640.0f, -882.72f), PLACEAT(3000, HERO_OBELIX, 1521.26f, -640.0f, -882.72f),
	SECTORS(3500), SHOT(6500, "shadow29_1"),
	PLACEAT(7000, HERO_OBELIX, 1516.26f, -640.0f, -877.72f), SHOT(10000, "shadow29_2"),
	END(10500),
};

// shadow29d (-Level 2, XXL_SHADOWLOG=1): the user's LV2-9 spot near the boxes
// (1536.1 -638.3 -883.8): Asterix at 1512.3 -641.1 -847.4, player 2's Obelix 10,
// 40, 50, 55 and 65 units away towards -902.4, then Obelix given the lead.
const Step kShadow29D[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 9),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1512.319f, -641.06f, -847.377f), PLACEAT(3000, HERO_OBELIX, 1512.319f, -641.0f, -857.377f),
	SECTORS(3500), SHOT(6000, "d10"),
	PLACEAT(6500, HERO_OBELIX, 1512.319f, -639.0f, -887.377f), SHOT(9000, "d40"),
	PLACEAT(9500, HERO_OBELIX, 1512.319f, -638.5f, -897.377f), SHOT(12000, "d50"),
	PLACEAT(12500, HERO_OBELIX, 1512.319f, -637.86f, -902.377f), SHOT(15000, "d55"),
	PLACEAT(15500, HERO_ASTERIX, 1512.319f, -641.06f, -837.377f), SHOT(18000, "d65"),
	{ 18500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" }, SHOT(21000, "d65o"),
	PLACEAT(21500, HERO_ASTERIX, 1512.319f, -641.06f, -847.377f), SHOT(24000, "d55o"),
	END(24500),
};

// crates29 (-Level 2): the boxes at 1536.1 -638.3 -883.8 seen by player 2's Obelix
// (1512.3 -637.9 -902.4, camera turned to them) with Asterix 10, 55 and 65 units
// away, then 65 with Obelix leading; live crate objects near the boxes logged.
#define CRATES29(t, n) { t, S_P2LOOK, 0, 0, 0, 1536.139f, -638.294f, "p2 camera at the boxes", -883.842f }, \
	{ t + 2500, S_CRATE, 0, -2, 0, 1536.139f, -638.294f, n, -883.842f }, SHOT(t + 2500, n)
const Step kCrates29[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 9),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 1512.319f, -637.86f, -902.377f), PLACEAT(3000, HERO_ASTERIX, 1512.319f, -640.0f, -892.377f),
	SECTORS(3500), CRATES29(4000, "c10"),
	PLACEAT(7000, HERO_ASTERIX, 1512.319f, -641.06f, -847.377f), CRATES29(7500, "c55"),
	PLACEAT(10500, HERO_ASTERIX, 1512.319f, -641.06f, -837.377f), CRATES29(11000, "c65"),
	PLACEAT(14000, HERO_ASTERIX, 1512.319f, -640.0f, -892.377f), CRATES29(14500, "c10b"),
	PLACEAT(17500, HERO_ASTERIX, 1512.319f, -641.06f, -837.377f),
	{ 17600, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" }, CRATES29(18000, "c65o"),
	END(21000),
};

// crates44 (-Level 4): sector 4, a wall between the heroes: Asterix at -709.3 6.0
// -232.7, player 2's Obelix beside him, then at -712.1 12.0 -142.4 (90 units),
// then Obelix leading; live crates within 30 units of each spot logged.
#define CRATES44(t, n) { t, S_CRATE, 0, -2, 0, -709.263f, 5.95f, n " (asterix's spot)", -232.708f }, \
	{ t, S_CRATE, 0, -2, 0, -712.051f, 11.99f, n " (obelix's spot)", -142.356f }, SHOT(t, n)
const Step kCrates44[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -709.263f, 5.95f, -232.708f), PLACEAT(3000, HERO_OBELIX, -706.263f, 5.95f, -232.708f),
	SECTORS(3500), CRATES44(6000, "w0"),
	PLACEAT(6500, HERO_OBELIX, -712.051f, 11.99f, -142.356f), SECTORS(7000), CRATES44(9500, "w90"), CRATES44(12500, "w90b"),
	{ 13000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" }, CRATES44(16000, "w90o"),
	PLACEAT(16500, HERO_OBELIX, -706.263f, 5.95f, -232.708f), CRATES44(19500, "w0o"),
	END(20000),
};

// anim2: player 1 taps forward with player 2 in (split screen); XXL_ANIMLOG logs
// Asterix's animation blends per walk, XXL_ANIMFIX=0 the game's own.
const Step kAnim2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	P1(3000, 0, 0.0f, 1.0f, "p1 forward"), P1(3300, 0, 0.0f, 0.0f, "p1 release"),
	P1(5000, 0, 0.0f, 1.0f, "p1 forward"), P1(6500, 0, 0.0f, 0.0f, "p1 release"),
	END(8000),
};

// lbsp (-Level 4): single player, Romans by the level's start; player 1 presses
// LB every 2 s (Obelix grabs, spins, whistles, passes). Writes to Obelix's +0x884
// watched; XXL_LOGTRIO=1 for the trio events.
#define LB_TAP(t) P1(t, PAD_LB, 0, 0, "p1 LB"), P1(t + 150, 0, 0, 0, "p1 release")
const Step kLbSp[] = {
	MARK(0, "baseline"),
	PLACEAT(1000, HERO_ASTERIX, -299.16f, 1.2f, 89.27f), PLACEAT(1000, HERO_OBELIX, -297.0f, 1.2f, 89.27f),
	{ 1500, S_WATCHRW, HERO_OBELIX, 0x884, 0, 0, 0, "who accesses obelix +884" },
	SHOT(3500, "lbsp_1"),
	LB_TAP(4000), LB_TAP(6000), SHOT(7000, "lbsp_2"), LB_TAP(8000), SHOT(9000, "lbsp_3"), LB_TAP(10000),
	SHOT(11000, "lbsp_4"), LB_TAP(12000), LB_TAP(14000), SHOT(15000, "lbsp_5"), LB_TAP(16000), LB_TAP(18000),
	SHOT(19000, "lbsp_6"),
	END(20000),
};

// rbp2 (-Level 4): player 2's Obelix (not leading) presses RB by the level's
// Romans, Asterix (leading, with Idefix) 25 units away: Idefix should go and
// bite (AI order 6).
#define RB2_TAP(t) VPAD(t, 1, PAD_RB, 0, 0, "p2 RB"), VPAD(t + 150, 1, 0, 0, 0, "p2 release")
const Step kRbP2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1000, HERO_ASTERIX, -290.0f, 1.2f, 115.0f), PLACEAT(1000, HERO_OBELIX, -299.16f, 1.2f, 89.27f),
	RB2_TAP(4000), RB2_TAP(6000), SHOT(6500, "rbp2_1"), RB2_TAP(8000), RB2_TAP(10000), SHOT(10500, "rbp2_2"), RB2_TAP(12000),
	RB2_TAP(14000), RB2_TAP(15000), SHOT(15500, "rbp2_3"), RB2_TAP(17000), SHOT(17500, "rbp2_4"),
	END(19000),
};

// grab22 (-Level 2): sector 2's lone Roman (-317.5 16.5 -595.6; keep out of z
// -591.1..-583.8): player 1's Asterix hits it once, grabs it (B held), spins,
// lets go. The log has the target and its state (+0x21: 3 held, 4 flying).
const Step kGrab22[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	PLACEAT(3000, HERO_ASTERIX, -317.46f, 17.0f, -598.5f), PLACEAT(3000, HERO_OBELIX, -322.0f, 17.0f, -600.0f),
	SECTORS(3500),
	{ 4500, S_NEAROBJ, HERO_ASTERIX, 95, 0, 1.6f, 0, "asterix by the roman (an archer)" },
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5150, 0, 0, 0, "p1 release"),
	SHOT(5600, "grab22_1"),
	P1(6000, PAD_B, 0, 0, "p1 B"), SHOT(6800, "grab22_2"), P1(7000, PAD_B, 1.0f, 0, "p1 B + right"), SHOT(8000, "grab22_3"),
	P1(9000, PAD_B, 0, 0, "p1 B"), P1(9500, 0, 0, 0, "p1 release"), SHOT(10000, "grab22_4"),
	END(12000),
};

// pass22 (-Level 2): the lone archer of sector 2; player 2's Obelix 8 units
// away; player 1's Asterix hits it, grabs and spins it, presses LB: it should
// fly to Obelix.
const Step kPass22[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -317.46f, 17.0f, -598.5f), PLACEAT(3000, HERO_OBELIX, -326.0f, 17.0f, -598.5f),
	SECTORS(3500),
	{ 4500, S_NEAROBJ, HERO_ASTERIX, 95, 0, 1.6f, 0, "asterix by the archer" },
	{ 4600, S_WATCHRW, HERO_ASTERIX, 0x21, 0, 4, 0, "archer state writes" },
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5150, 0, 0, 0, "p1 release"),
	P1(6000, PAD_B, 0, 0, "p1 B"), SHOT(6900, "pass22_1"),
	P1(7200, PAD_B | PAD_LB, 0, 0, "p1 B + LB"), P1(7350, PAD_B, 0, 0, "p1 B"),
	SHOT(7500, "pass22_2"), SHOT(7800, "pass22_3"), SHOT(8200, "pass22_4"),
	P1(8500, 0, 0, 0, "p1 release"),
	SHOT(9500, "pass22_5"),
	END(11000),
};

// lb2: player 2's Obelix presses LB with no Roman spinning: the other hero's "no".
const Step kLb2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(3000, 1, PAD_LB, 0, 0, "p2 LB"), VPAD(3150, 1, 0, 0, 0, "p2 release"),
	P1(4000, PAD_LB, 0, 0, "p1 LB"), P1(4150, 0, 0, 0, "p1 release"),
	END(6000),
};

// whistle22 (-Level 2): single player at the lone archer of sector 2: Asterix
// stuns it, LB sends Obelix to grab and spin it; no LB for 5 s (the whistle),
// then LB. XXL_LOGSOUNDS=2 logs every sound start.
const Step kWhistle22[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -317.46f, 17.0f, -598.5f), PLACEAT(3000, HERO_OBELIX, -324.0f, 17.0f, -598.5f),
	VPAD(3300, 1, PAD_BACK, 0, 0, "p2 holds back (leaves)"), VPAD(5000, 1, 0, 0, 0, "p2 release"),
	SECTORS(3500),
	{ 4500, S_NEAROBJ, HERO_ASTERIX, 95, 0, 1.6f, 0, "asterix by the archer" },
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5150, 0, 0, 0, "p1 release"),
	P1(6000, 0, 0, -1.0f, "p1 back"), P1(6400, 0, 0, 0, "p1 stop"),
	LB_TAP(6800), SHOT(8000, "whistle_1"), SHOT(10000, "whistle_2"), SHOT(12000, "whistle_3"), LB_TAP(13500), SHOT(14000, "whistle_4"),
	END(16000),
};

// lbwait (-Level 4): single player by the level's Romans; one LB, then 6 s of
// waiting (Obelix grabs, spins, whistles), then LB for the pass.
const Step kLbWait[] = {
	MARK(0, "baseline"),
	PLACEAT(1000, HERO_ASTERIX, -299.16f, 1.2f, 89.27f), PLACEAT(1000, HERO_OBELIX, -297.0f, 1.2f, 89.27f),
	LB_TAP(4000), SHOT(6000, "lbwait_1"), SHOT(8000, "lbwait_2"), LB_TAP(10000), SHOT(10500, "lbwait_3"),
	LB_TAP(13000), SHOT(16000, "lbwait_4"), LB_TAP(19000),
	END(22000),
};

// preset0: the controller preset forced to 0 in play (the shuffled layout two
// sessions ended up on); the mod should put the game's choice back.
const Step kPreset0[] = {
	MARK(0, "baseline"), { 2000, S_SETPRESET, 0, 0, 0, 0, 0, "preset 0" }, END(5000),
};

// preset1 (with XXL_PRESETNOPAD=1): the keyboard preset 1 set as if the game had
// seen no pad (the slow start of xxl_coop.pad_strange_newer.log); the mod should
// set 3 within a second.
const Step kPreset1[] = {
	MARK(0, "baseline"), { 2000, S_SETPRESET, 1, 0, 0, 0, 0, "preset 1" }, END(5000),
};

// presetopts: the saved options (FUN_00503B00) run in play, which leave preset 1
// set from 0x503B41 -- as at a slow boot where force_level started the level
// before the title's choice (xxl_coop.pad_preset_issue.log); the mod should set 3.
const Step kPresetOpts[] = {
	MARK(0, "baseline"), { 2000, S_SETPRESET, -1, 0, 0, 0, 0, "saved options" }, END(5000),
};

// fog21: level 2's starting camp (snow, CFogBoxNodeFx): 6 s single player, then
// player 2 joins (split screen); XXL_FOGLOG logs a snowflake, XXL_FOGFIX=0 the
// game's own walks.
const Step kFog21[] = {
	MARK(0, "baseline"),
	PLACEAT(500, HERO_ASTERIX, -815.0f, 509.0f, -1068.0f), PLACEAT(500, HERO_OBELIX, -817.0f, 509.0f, -1069.0f),
	SHOT(3000, "fog_sp"),
	VPAD(6000, 1, PAD_START, 0, 0, "p2 start"), VPAD(6200, 1, 0, 0, 0, "p2 release"),
	SHOT(9000, "fog_split"),
	END(12000),
};

const Step kBeach12O[] = {
	MARK(0, "baseline"), CINE_SKIP(300), CINE_SKIP(1500), CINE_SKIP(3000), CINE_SKIP(5000), CINE_SKIP(7000),
	P1(400, PAD_A, 0, 0, "p1 A"), P1(550, 0, 0, 0, "p1 release"), P1(1200, PAD_A, 0, 0, "p1 A"), P1(1350, 0, 0, 0, "p1 release"), P1(2200, PAD_A, 0, 0, "p1 A"), P1(2350, 0, 0, 0, "p1 release"), P1(3400, PAD_A, 0, 0, "p1 A"), P1(3550, 0, 0, 0, "p1 release"), P1(4600, PAD_A, 0, 0, "p1 A"), P1(4750, 0, 0, 0, "p1 release"), P1(6000, PAD_A, 0, 0, "p1 A"), P1(6150, 0, 0, 0, "p1 release"), 
	PLACEAT(8000, HERO_ASTERIX, -2.216f, -1.8f, 58.619f),
	PLACEAT(11000, HERO_ASTERIX, 46.0f, 6.0f, 105.84f), GLIDEAT(11500, HERO_ASTERIX, 58.0f, 6.0f, 105.84f, 1500),
	CINE_SKIP(14000), CINE_SKIP(15500), CINE_SKIP(17000), CINE_SKIP(18500), CINE_SKIP(20000), CINE_SKIP(21500), CINE_SKIP(23000), CINE_SKIP(24500), 
	VPAD(27000, 1, PAD_START, 0, 0, "p2 start"), VPAD(27200, 1, 0, 0, 0, "p2 release"),
	VPAD(27500, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(27600, 1, PAD_BACK | PAD_B, 0, 0, "p2 back+B (release)"), VPAD(27800, 1, 0, 0, 0, "p2 release"),
	{ 27900, S_WATCHENEMIES, 0, 0, 0, -20.0f, -9.0f, "watch romans", 120.0f },
	PLACEAT(28000, HERO_OBELIX, 18.73f, -3.3f, 115.24f),
	GLIDEAT(28500, HERO_OBELIX, 6.09f, -5.6f, 115.86f, 1200), GLIDEAT(29800, HERO_OBELIX, -3.6f, -9.3f, 118.4f, 1000),
	SHOT(32000, "beach12o_1"), SHOT(36000, "beach12o_2"),
	{ 38000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	SHOT(41000, "beach12o_3"), SHOT(45000, "beach12o_4"),
	END(46000),
};

// mgun3a / mgun3n / mgun3o (-Level 3): sector 6's machinegun (-172.1 6.1
// -196.7). a: the leader Asterix presses X at it; n: Asterix, Obelix leading
// (trio 0x4801), presses X; o: player 2's Obelix (not leading) presses X.
#define MGUN_SETUP \
	MARK(0, "baseline"), LOADSECTOR(200, 6), \
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), \
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f), \
	SECTORS(3500)
const Step kMgun3A[] = {
	MGUN_SETUP,
	PLACEAT(4500, HERO_ASTERIX, -172.13f, 6.6f, -196.69f), PLACEAT(4500, HERO_OBELIX, -176.0f, 6.6f, -199.0f),
	SHOT(6000, "mgun_1"),
	P1(6500, PAD_X, 0, 0, "p1 X"), P1(6700, 0, 0, 0, "p1 release"),
	SHOT(8000, "mgun_2"),
	END(10000),
};
const Step kMgun3N[] = {
	MGUN_SETUP,
	{ 4000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(4500, HERO_ASTERIX, -172.13f, 6.6f, -196.69f), PLACEAT(4500, HERO_OBELIX, -176.0f, 6.6f, -199.0f),
	SHOT(6000, "mgun_1"),
	P1(6500, PAD_X, 0, 0, "p1 X"), P1(6700, 0, 0, 0, "p1 release"),
	SHOT(8000, "mgun_2"),
	END(10000),
};
const Step kMgun3O[] = {
	MGUN_SETUP,
	PLACEAT(4500, HERO_OBELIX, -172.13f, 6.6f, -196.69f), PLACEAT(4500, HERO_ASTERIX, -176.0f, 6.6f, -199.0f),
	SHOT(6000, "mgun_1"),
	VPAD(6500, 1, PAD_X, 0, 0, "p2 X"), VPAD(6700, 1, 0, 0, 0, "p2 release"),
	SHOT(8000, "mgun_2"),
	END(10000),
};

// mgun33o / mgun33a (-Level 3): sector 3's machine gun (the user's spot, gun seat
// 289.0 31.9 -326.1); player 2's Obelix (o) or player 1's Asterix (a) presses X
// beside it, the other hero a few steps away.
#define MGUN33_SETUP 	MARK(0, "baseline"), LOADSECTOR(200, 3), 	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), 	PLACEAT(3000, HERO_ASTERIX, 284.4f, 31.92f, -325.9f), PLACEAT(3000, HERO_OBELIX, 289.3f, 31.92f, -325.6f), 	SECTORS(3500)
// iconsgun (-Level 3): the LV3-3 gun's "Use" prompt with only player 2's Obelix
// by it (his button), both heroes (both buttons), only player 1's Asterix (his).
const Step kIconsGun[] = {
	MGUN33_SETUP,
	PLACEAT(4000, HERO_ASTERIX, 262.0f, 31.92f, -325.9f),
	SHOT(5500, "iconsgun_p2"),
	PLACEAT(6000, HERO_ASTERIX, 284.4f, 31.92f, -325.9f),
	SHOT(7500, "iconsgun_both"),
	PLACEAT(8000, HERO_OBELIX, 262.0f, 31.92f, -325.6f),
	SHOT(9500, "iconsgun_p1"),
	END(10000),
};
const Step kMgun33O[] = {
	MGUN33_SETUP,
	SHOT(5000, "mgun33_1"),
	VPAD(6000, 1, PAD_X, 0, 0, "p2 X"), VPAD(6200, 1, 0, 0, 0, "p2 release"),
	SHOT(8000, "mgun33_2"),
	VPAD(9000, 1, 0, -1.0f, 0, "p2 stick left"), VPAD(10500, 1, 0, 0, 0, "p2 release"),
	P1(11000, PAD_X, 0, 0, "p1 X"), P1(11500, 0, 0, 0, "p1 release"),
	SHOT(11200, "mgun33_3"),
	END(13000),
};
// mgun33j (-Level 3): Asterix takes the LV3-3 gun alone (split screen stays,
// player 1 turns and fires it), Obelix joins at the handles with X (one screen,
// player 2 turns it), Asterix leaves with A (split screen back).
const Step kMgun33J[] = {
	MGUN33_SETUP,
	PLACEAT(4000, HERO_ASTERIX, 289.3f, 31.92f, -325.6f), PLACEAT(4000, HERO_OBELIX, 280.0f, 31.92f, -326.0f),
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(8000, "mgun33j_solo"),
	P1(8500, 0, -1.0f, 0, "p1 turns"), P1(9500, PAD_X, 0, 0, "p1 fires"), P1(9900, 0, 0, 0, "p1 release"),
	PLACEAT(10000, HERO_OBELIX, 293.9f, 31.92f, -326.5f),
	VPAD(11000, 1, PAD_X, 0, 0, "p2 X"), VPAD(11200, 1, 0, 0, 0, "p2 release"),
	SHOT(13500, "mgun33j_joined"),
	VPAD(14000, 1, 0, -1.0f, 0, "p2 turns"), VPAD(15000, 1, 0, 0, 0, "p2 release"),
	P1(16000, PAD_A, 0, 0, "p1 A (leave)"), P1(16200, 0, 0, 0, "p1 release"),
	SHOT(18500, "mgun33j_after"),
	END(19500),
};
// mgun33l (-Level 3): mgun33j with Obelix leading (4801 first): Asterix takes the
// gun as the hero not leading -- Obelix keeps the lead throughout (the cable cars'
// equality); the gun turns and fires for player 1, Obelix joins and turns it,
// Asterix leaves with A.
const Step kMgun33L[] = {
	MGUN33_SETUP,
	PLACEAT(4000, HERO_ASTERIX, 289.3f, 31.92f, -325.6f), PLACEAT(4000, HERO_OBELIX, 280.0f, 31.92f, -326.0f),
	{ 4500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(8000, "mgun33l_solo"),
	P1(8500, 0, -1.0f, 0, "p1 turns"), P1(9500, PAD_X, 0, 0, "p1 fires"), P1(9900, 0, 0, 0, "p1 release"),
	SHOT(9800, "mgun33l_fire"),
	PLACEAT(10000, HERO_OBELIX, 293.9f, 31.92f, -326.5f),
	VPAD(11000, 1, PAD_X, 0, 0, "p2 X"), VPAD(11200, 1, 0, 0, 0, "p2 release"),
	SHOT(13500, "mgun33l_joined"),
	VPAD(14000, 1, 0, -1.0f, 0, "p2 turns"), VPAD(15000, 1, 0, 0, 0, "p2 release"),
	P1(16000, PAD_A, 0, 0, "p1 A (leave)"), P1(16200, 0, 0, 0, "p1 release"),
	SHOT(18500, "mgun33l_after"),
	END(19500),
};
// mgun33d (-Level 3, XXL_DOWNAT=1:9000 XXL_UPAT=20000): Obelix leads, Asterix
// takes the LV3-3 gun (not leading), Obelix is put by the rope cable car's start
// (307.6 10.6 -209.2: across the river from the gun) and goes down there; Asterix
// stays on the gun -- nothing moves him -- and leaves it: brought to Obelix (out of
// reach, he led), and after the revive back where he stood by the gun.
const Step kMgun33D[] = {
	MGUN33_SETUP,
	PLACEAT(4000, HERO_ASTERIX, 289.3f, 31.92f, -325.6f), PLACEAT(4000, HERO_OBELIX, 280.0f, 31.92f, -326.0f),
	{ 4500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6200, 0, 0, 0, "p1 release"),
	PLACEAT(7000, HERO_OBELIX, 307.582f, 10.6f, -209.154f),
	SHOT(11000, "mgun33d_ongun"),
	P1(13000, PAD_A, 0, 0, "p1 A (leave)"), P1(13200, 0, 0, 0, "p1 release"),
	SHOT(18000, "mgun33d_brought"),
	SHOT(23000, "mgun33d_back"),
	END(24000),
};
const Step kMgun33A[] = {
	MGUN33_SETUP,
	PLACEAT(4000, HERO_ASTERIX, 289.3f, 31.92f, -325.6f), PLACEAT(4000, HERO_OBELIX, 284.4f, 31.92f, -325.9f),
	SHOT(5000, "mgun33_1"),
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(8000, "mgun33_2"),
	VPAD(9000, 1, 0, -1.0f, 0, "p2 stick left"), VPAD(10500, 1, 0, 0, 0, "p2 release"),
	P1(11000, PAD_X, 0, 0, "p1 X"), P1(11500, 0, 0, 0, "p1 release"),
	SHOT(11200, "mgun33_3"),
	END(13000),
};

// boat49o / boat49a (-Level 4): sector 9's boat (the user's spot -165.4 6.0
// -652.0); player 2's Obelix (o) or player 1's Asterix (a) presses X there, the
// other hero beside; then player 2 steers left, player 1 presses A.
#define BOAT49_SETUP 	MARK(0, "baseline"), LOADSECTOR(200, 9), 	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), 	SECTORS(3500)
const Step kBoat49O[] = {
	BOAT49_SETUP,
	PLACEAT(3000, HERO_OBELIX, -165.4f, 6.0f, -652.0f), PLACEAT(3000, HERO_ASTERIX, -162.0f, 6.0f, -650.0f),
	SHOT(5000, "boat49_1"),
	VPAD(6000, 1, PAD_X, 0, 0, "p2 X"), VPAD(6200, 1, 0, 0, 0, "p2 release"),
	SHOT(8000, "boat49_2"),
	VPAD(9000, 1, 0, -1.0f, 0.5f, "p2 stick left"), VPAD(11000, 1, 0, 0, 0, "p2 release"),
	P1(11500, PAD_A, 0, 0, "p1 A"), P1(11700, 0, 0, 0, "p1 release"),
	SHOT(11800, "boat49_3"),
	END(14000),
};
const Step kBoat49A[] = {
	BOAT49_SETUP,
	PLACEAT(3000, HERO_ASTERIX, -165.4f, 6.0f, -652.0f), PLACEAT(3000, HERO_OBELIX, -162.0f, 6.0f, -650.0f),
	SHOT(5000, "boat49_1"),
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(8000, "boat49_2"),
	VPAD(9000, 1, 0, -1.0f, 0.5f, "p2 stick left"), VPAD(11000, 1, 0, 0, 0, "p2 release"),
	P1(11500, PAD_A, 0, 0, "p1 A"), P1(11700, 0, 0, 0, "p1 release"),
	SHOT(11800, "boat49_3"),
	END(14000),
};
// iconsboat49 (-Level 4, XXL_ICONP2KB=1 XXL_ICONLOG=1): the LV4-9 boat's "Use"
// prompt with player 2's Obelix on its deck (the user's spot -165.2 5.4 -651.7,
// dropped from 7.5; placed at 5.4 he sticks in the hull): Asterix 12 units off on
// the jetty (player 2's key), 7 off (the same); then Asterix alone on the deck
// (player 1's button), Obelix walking back on beside him (both players'). The
// deck is small: 1.6 units past the spot is water.
const Step kIconsBoat49[] = {
	BOAT49_SETUP,
	PLACEAT(3000, HERO_OBELIX, -165.213f, 7.5f, -651.691f), PLACEAT(3000, HERO_ASTERIX, -164.66f, 6.6f, -640.0f),
	SHOT(5500, "iconsboat_far"),
	PLACEAT(6000, HERO_ASTERIX, -164.66f, 6.6f, -645.0f),
	SHOT(7500, "iconsboat_near"),
	PLACEAT(8000, HERO_OBELIX, -164.66f, 6.6f, -640.0f),
	PLACEAT(8100, HERO_ASTERIX, -165.213f, 7.5f, -651.691f),
	SHOT(9500, "iconsboat_p1"),
	PLACEAT(10000, HERO_OBELIX, -165.3f, 6.6f, -646.0f),
	GLIDEAT(10200, HERO_OBELIX, -165.3f, 6.3f, -650.6f, 1500),
	SHOT(13000, "iconsboat_both"),
	END(13500),
};

// hintjoin: the level-start hint (player 2 joins with Start), on screen.
const Step kHintJoin[] = {
	MARK(0, "baseline"), SHOT(1500, "hint_1"), SHOT(4000, "hint_2"), END(5000),
};

// spy3n (-Level 3): sector 2's Roman agent (176.4 6.2 -218.4); Obelix leads
// (trio 0x4801) far away, player 1's Asterix talks to the agent with X, then
// presses A now and then to go through the dialogue.
// spy3p (-Level 3): the same agent; player 1's Asterix starts the talk, player 2
// clicks through the dialogue with X.
const Step kSpy3P[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 176.43f, 6.7f, -218.37f), PLACEAT(3000, HERO_OBELIX, 179.0f, 6.7f, -216.0f),
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5200, 0, 0, 0, "p1 release"),
	SHOT(8000, "spy3p_1"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X"), VPAD(10200, 1, 0, 0, 0, "p2 r"), VPAD(13000, 1, PAD_X, 0, 0, "p2 X"), VPAD(13200, 1, 0, 0, 0, "p2 r"),
	SHOT(14500, "spy3p_2"),
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X"), VPAD(16200, 1, 0, 0, 0, "p2 r"), VPAD(19000, 1, PAD_X, 0, 0, "p2 X"), VPAD(19200, 1, 0, 0, 0, "p2 r"),
	VPAD(22000, 1, PAD_X, 0, 0, "p2 X"), VPAD(22200, 1, 0, 0, 0, "p2 r"), VPAD(25000, 1, PAD_X, 0, 0, "p2 X"), VPAD(25200, 1, 0, 0, 0, "p2 r"),
	SHOT(27000, "spy3p_3"),
	END(30000),
};

// hudvanish (-Level 3): player 2's Obelix lands hits by the sector 2 agent (his
// combo bar shows), then player 1's Asterix talks to the agent: the dialogue's
// screen has no HUD (before the fix player 2's bar stayed on it).
const Step kHudVanish[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 176.43f, 6.7f, -218.37f), PLACEAT(3000, HERO_OBELIX, 175.57f, 11.0f, -239.11f),
	{ 4000, S_GIVE, HERO_OBELIX, 3, 0, 8.0f, 0, "obelix hits" },
	SHOT(4600, "hudvanish_1"),
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5200, 0, 0, 0, "p1 release"),
	SHOT(6500, "hudvanish_2"), SHOT(9000, "hudvanish_3"),
	END(10000),
};

// hudlayout (-Level 4): every HUD piece up at once in split screen -- LV4-1's
// fight gate counter (Asterix in its zone by the start), both combo bars, both
// health rows (Back held on both pads) and the helmet counter (gm +0x5C bit 6,
// "helmets changed"). XXL_SPLITMODE=horizontal for the stacked split.
const Step kHudLayout[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(2000, HERO_ASTERIX, -289.92f, 0.5f, 117.8f, 1500),
	{ 4000, S_GIVE, HERO_OBELIX, 3, 0, 8.0f, 0, "hits" }, { 4000, S_GIVE, HERO_ASTERIX, 3, 0, 8.0f, 0, "hits" },
	VPAD(4200, 1, PAD_BACK, 0, 0, "p2 holds back"), P1(4200, PAD_BACK, 0, 0, "p1 holds back"),
	{ 4300, S_POKE8, -2, 0x5C, 0, 64.0f, 0, "helmets changed (gm +0x5C bit 6)" },
	SHOT(5500, "hudlayout_1"),
	{ 5600, S_POKE8, -2, 0x5C, 0, 64.0f, 0, "helmets changed (gm +0x5C bit 6)" },
	SHOT(7000, "hudlayout_2"),
	VPAD(7500, 1, 0, 0, 0, "p2 release"), P1(7500, 0, 0, 0, "p1 release"),
	SHOT(9000, "hudlayout_3"),
	END(9500),
};

// hudreq (-Level 4): the rest of the shared pieces with hudlayout's -- the
// spy's helmet requirement (gm +0x5C bit 11; FUN_0051CD80) and the x3 / x10
// sign (Asterix's stats +0x84 the multiplier, +0x98 its time: 8 s).
const Step kHudReq[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(2000, HERO_ASTERIX, -289.92f, 0.5f, 117.8f, 1500),
	{ 3800, S_POKE8, -2, 0x5D, 0, 8.0f, 0, "helmet requirement (gm +0x5C bit 11)" },
	{ 3800, S_POKE8, -10, 0x84, 0, 3.0f, 0, "asterix x3" }, { 3800, S_POKE8, -10, 0x9B, 0, 65.0f, 0, "for 8 s" },
	{ 4000, S_GIVE, HERO_OBELIX, 3, 0, 8.0f, 0, "hits" }, { 4000, S_GIVE, HERO_ASTERIX, 3, 0, 8.0f, 0, "hits" },
	VPAD(4200, 1, PAD_BACK, 0, 0, "p2 holds back"), P1(4200, PAD_BACK, 0, 0, "p1 holds back"),
	{ 4300, S_POKE8, -2, 0x5C, 0, 64.0f, 0, "helmets changed (gm +0x5C bit 6)" },
	SHOT(5500, "hudreq_1"),
	SHOT(7000, "hudreq_2"),
	END(7500),
};
// hudx3 (-Level 4): the x3 sign alone (its place is then the requirement's).
const Step kHudX3[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 3800, S_POKE8, -10, 0x84, 0, 3.0f, 0, "asterix x3" }, { 3800, S_POKE8, -10, 0x9B, 0, 65.0f, 0, "for 8 s" },
	{ 4300, S_POKE8, -2, 0x5C, 0, 64.0f, 0, "helmets changed (gm +0x5C bit 6)" },
	SHOT(4200, "hudx3_1"), SHOT(5500, "hudx3_2"),
	END(6000),
};

const Step kSpy3N[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(3000, HERO_ASTERIX, 176.43f, 6.7f, -218.37f),
	{ 2000, S_WATCHCLUE, 0, 0, 0, 0, 0, "spies" },
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5200, 0, 0, 0, "p1 release"),
	SHOT(7000, "spy3n_1"),
	P1(10000, PAD_A, 0, 0, "p1 A"), P1(10200, 0, 0, 0, "p1 r"), P1(14000, PAD_A, 0, 0, "p1 A"), P1(14200, 0, 0, 0, "p1 r"),
	P1(18000, PAD_A, 0, 0, "p1 A"), P1(18200, 0, 0, 0, "p1 r"), P1(22000, PAD_A, 0, 0, "p1 A"), P1(22200, 0, 0, 0, "p1 r"),
	P1(26000, PAD_X, 0, 0, "p1 X"), P1(26200, 0, 0, 0, "p1 r"), P1(30000, PAD_X, 0, 0, "p1 X"), P1(30200, 0, 0, 0, "p1 r"),
	SHOT(32000, "spy3n_2"),
	P1(34000, PAD_START, 0, 0, "p1 start"), P1(34200, 0, 0, 0, "p1 r"),
	SHOT(36000, "spy3n_3"),
	END(40000),
};

// catapultp (level 2): player 2's Obelix (not leading) stands on sector 2's
// launcher: the "Activate" prompt (CKGrpMeca owner) is logged every tick;
// catapulta: the same with the leader Asterix, for comparison.
const Step kCatapultP[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_ASTERIX, 10, 0, 0, 0, "asterix into the sas (sector 2)" },
	{ 4000, S_PLACE, HERO_ASTERIX, 18, 0, 0, 0, "asterix next to the launcher" },
	GLIDE(4000, HERO_OBELIX, 17, 1500),
	SHOT(7000, "catapult_1"),
	END(8000),
};
const Step kCatapultA[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_ASTERIX, 10, 0, 0, 0, "asterix into the sas (sector 2)" },
	{ 4000, S_PLACE, HERO_OBELIX, 18, 0, 0, 0, "obelix next to the launcher" },
	GLIDE(4000, HERO_ASTERIX, 17, 1500),
	{ 6000, S_WATCHRW, HERO_ASTERIX, 0x3F1, 0, 0, 0, "who reads the launcher bit" },
	SHOT(7000, "catapult_1"),
	END(8000),
};

// shop (level 2): the leader Asterix at sector 1's merchant (-764.3 508.2
// -1013.0) presses X; probe on.
const Step kShop[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant (CKHkAsterixShop)" },
	SHOT(3500, "shop_0"),
	PROBE(3800, 1),
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4200, 0, 0, 0, "p1 release"),
	SHOT(6000, "shop_1"),
	P1(7000, 0, 0, -1, "p1 down"), P1(7200, 0, 0, 0, "p1 release"),
	SHOT(8000, "shop_2"),
	P1(9000, PAD_B, 0, 0, "p1 B"), P1(9200, 0, 0, 0, "p1 release"),
	SHOT(10500, "shop_3"),
	END(11000),
};

// shopox (level 2): player 2's Obelix opens the merchant with X and presses X
// through the merchant's words; the menu must stay up, B leaves; afterwards
// each player's camera follows its own hero again.
const Step kShopOX[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	VPAD(6500, 1, PAD_X, 0, 0, "p2 X"), VPAD(6700, 1, 0, 0, 0, "p2 release"),
	VPAD(8000, 1, PAD_X, 0, 0, "p2 X"), VPAD(8200, 1, 0, 0, 0, "p2 release"),
	VPAD(9500, 1, PAD_X, 0, 0, "p2 X"), VPAD(9700, 1, 0, 0, 0, "p2 release"),
	SHOT(11000, "shopox_1"),
	VPAD(12000, 1, PAD_B, 0, 0, "p2 B (back)"), VPAD(12200, 1, 0, 0, 0, "p2 release"),
	VPAD(14000, 1, PAD_X, 0, 0, "p2 X"), VPAD(14200, 1, 0, 0, 0, "p2 release"),
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X"), VPAD(16200, 1, 0, 0, 0, "p2 release"),
	SHOT(22000, "shopox_2"),
	SHOT(31000, "shopox_3"),
	END(32000),
};

// shoppause (level 2): player 2's Obelix at the merchant's menu, then player 1
// pauses: the pause menu shows player 1's buttons, the shop again player 2's.
const Step kShopPause[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	VPAD(7500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(7700, 1, 0, 0, 0, "p2 release"),
	SHOT(9500, "shoppause_1"),
	{ 10000, S_POKE8, -3, 2, 0, 0, 0, "the pause menu (game manager state 2)" },
	SHOT(12000, "shoppause_2"),
	{ 13000, S_POKE8, -3, 1, 0, 0, 0, "back to the game (state 1)" },
	SHOT(15000, "shoppause_3"),
	END(15500),
};

// shopsplit (level 2): player 2's Obelix shops in his half while player 1's
// Asterix runs and presses A / X (which must not reach the shop); player 2
// browses with the stick, then leaves with B.
const Step kShopSplit[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	VPAD(7500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(7700, 1, 0, 0, 0, "p2 release"),
	SHOT(9500, "shopsplit_1"),
	P1(10000, 0, 0, 1.0f, "p1 runs"), P1(10500, PAD_A, 0, 1.0f, "p1 jumps"), P1(10700, 0, 0, 1.0f, "p1 runs"),
	P1(11500, PAD_X, 0, 1.0f, "p1 attacks"), P1(11700, 0, 0, 1.0f, "p1 runs"), SHOT(12000, "shopsplit_2"), P1(12500, 0, 0, 0, "p1 stops"),
	VPAD(13000, 1, 0, 1.0f, 0, "p2 right"), VPAD(13300, 1, 0, 0, 0, "p2 release"),
	SHOT(14000, "shopsplit_3"),
	VPAD(15000, 1, PAD_B, 0, 0, "p2 B (back)"), VPAD(15200, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(18200, 1, 0, 0, 0, "p2 release"),
	SHOT(24000, "shopsplit_4"),
	END(25000),
};

// shopsplita (level 2): player 1's Asterix shops in his half while player 2's
// Obelix runs and presses X; player 1 leaves with B.
const Step kShopSplitA[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4200, 0, 0, 0, "p1 release"),
	VPAD(5000, 1, 0, 0, 1.0f, "p2 runs"), VPAD(6000, 1, PAD_X, 0, 1.0f, "p2 attacks"), VPAD(6200, 1, 0, 0, 1.0f, "p2 runs"),
	P1(7500, PAD_X, 0, 0, "p1 X (words)"), P1(7700, 0, 0, 0, "p1 release"),
	SHOT(9500, "shopsplita_1"), VPAD(9800, 1, 0, 0, 0, "p2 stops"),
	P1(11000, PAD_B, 0, 0, "p1 B (back)"), P1(11200, 0, 0, 0, "p1 release"),
	P1(14000, PAD_X, 0, 0, "p1 X (words)"), P1(14200, 0, 0, 0, "p1 release"),
	SHOT(21000, "shopsplita_2"),
	END(22000),
};

// hit2o (level 2): the health down to one point, player 2's Obelix (not
// leading) next to a Roman, standing still: the Roman's hit kills him -- the
// death must be Obelix's.
const Step kHit2O[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_POKE8, -1, 0x50, 0, 1, 0, "health 1" },
	{ 1600, S_ENEMY, HERO_OBELIX, 0, 0, 0, 0, "obelix at a roman" },
	SHOT(6000, "hit2o_1"), SHOT(9000, "hit2o_2"), SHOT(14000, "hit2o_3"),
	END(20000),
};

// down2a (level 2, individual heroes): player 1's Asterix (leading) at one
// point of health beside a Roman goes down; the lead passes to Obelix, the game
// goes on; player 2's Obelix comes over and holds B for 10 s: Asterix is back.
const Step kDown2A[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_POKE8, -10, 0x50, 0, 1, 0, "asterix health 1" },
	{ 1600, S_ENEMY, HERO_ASTERIX, 0, 0, 0, 0, "asterix at a roman" },
	SHOT(5000, "down2a_1"), SHOT(7000, "down2a_2"),
	{ 8000, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 0, 0, "obelix to asterix" },
	VPAD(8500, 1, PAD_B, 0, 0, "p2 holds B"),
	SHOT(13000, "down2a_3"),
	VPAD(19500, 1, 0, 0, 0, "p2 release"),
	SHOT(20500, "down2a_4"),
	VPAD(21000, 1, 0, 0, 1, "p2 up"), P1(21000, 0, 0, 1, "p1 up"),
	SHOT(22500, "down2a_5"),
	END(23000),
};

// pit26b (-Level 2, individual heroes): Obelix falls into the pit (down),
// then Asterix too: both down is the game over, the respawn gives both their
// health back.
const Step kPit26B[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 88.0f, 2.2f, -390.4f), PLACEAT(3000, HERO_OBELIX, 92.5f, 2.2f, -390.4f),
	GLIDEAT(4500, HERO_OBELIX, 97.0f, 2.2f, -390.4f, 400),
	SHOT(7000, "pit26b_1"),
	GLIDEAT(8000, HERO_ASTERIX, 97.0f, 2.2f, -390.4f, 600),
	SHOT(10000, "pit26b_2"), SHOT(18000, "pit26b_3"),
	VPAD(19000, 1, 0, 0, 1, "p2 up"), P1(19000, 0, 0, 1, "p1 up"),
	SHOT(20500, "pit26b_4"),
	END(21000),
};

// pit26c: as pit26b, Asterix first (the lead goes to Obelix, who then falls
// too; the respawn gives the lead back to Asterix).
const Step kPit26C[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 88.0f, 2.2f, -390.4f), PLACEAT(3000, HERO_ASTERIX, 92.5f, 2.2f, -390.4f),
	GLIDEAT(4500, HERO_ASTERIX, 97.0f, 2.2f, -390.4f, 400),
	SHOT(7000, "pit26c_1"),
	GLIDEAT(8000, HERO_OBELIX, 97.0f, 2.2f, -390.4f, 600),
	SHOT(10000, "pit26c_2"), SHOT(18000, "pit26c_3"),
	VPAD(19000, 1, 0, 0, 1, "p2 up"), P1(19000, 0, 0, 1, "p1 up"),
	SHOT(20500, "pit26c_4"),
	END(21000),
};

// rejoin2 (level 2, individual heroes): player 2's Obelix at 3 points leaves
// and comes back: still 3 points.
const Step kRejoin2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_POKE8, -11, 0x50, 0, 3, 0, "obelix health 3" },
	VPAD(2500, 1, PAD_START, 0, 0, "p2 start (leave)"), VPAD(2700, 1, 0, 0, 0, "p2 release"),
	VPAD(5000, 1, PAD_START, 0, 0, "p2 start (rejoin)"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	SHOT(7000, "rejoin2_1"),
	END(8000),
};

// combo2 (level 2, individual heroes): player 2's Obelix hits Romans; only
// his combo bar fills.
const Step kCombo2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_ENEMY, HERO_OBELIX, 0, 0, 0, 0, "obelix at a roman" },
	VPAD(2000, 1, PAD_X, 0, 0, "p2 X"), VPAD(2150, 1, 0, 0, 0, "p2 release"),
	VPAD(2500, 1, PAD_X, 0, 0, "p2 X"), VPAD(2650, 1, 0, 0, 0, "p2 release"),
	VPAD(3000, 1, PAD_X, 0, 0, "p2 X"), VPAD(3150, 1, 0, 0, 0, "p2 release"),
	VPAD(3500, 1, PAD_X, 0, 0, "p2 X"), VPAD(3650, 1, 0, 0, 0, "p2 release"),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4150, 1, 0, 0, 0, "p2 release"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"), VPAD(4650, 1, 0, 0, 0, "p2 release"),
	{ 5000, S_ENEMY, HERO_OBELIX, 0, 0, 0, 0, "obelix at a roman" },
	VPAD(5500, 1, PAD_X, 0, 0, "p2 X"), VPAD(5650, 1, 0, 0, 0, "p2 release"),
	VPAD(6000, 1, PAD_X, 0, 0, "p2 X"), VPAD(6150, 1, 0, 0, 0, "p2 release"),
	VPAD(6500, 1, PAD_X, 0, 0, "p2 X"), VPAD(6650, 1, 0, 0, 0, "p2 release"),
	SHOT(7000, "combo2_1"),
	END(12000),
};

// huddiff (level 2): which HUD bytes change while the HUD shows -- a hit on
// Asterix, hits on Romans (combo bar), Back.
const Step kHudDiff[] = {
	MARK(0, "baseline"),
	{ 500, S_HUDDIFF, 0, 0, 0, 0, 0, "hud diff" },
	{ 1500, S_ENEMY, HERO_ASTERIX, 0, 0, 0, 0, "asterix at a roman" },
	P1(2000, PAD_X, 0, 0, "x"), P1(2150, 0, 0, 0, "release"),
	P1(2500, PAD_X, 0, 0, "x"), P1(2650, 0, 0, 0, "release"),
	P1(3000, PAD_X, 0, 0, "x"), P1(3150, 0, 0, 0, "release"),
	SHOT(3300, "huddiff_1"),
	P1(6000, PAD_BACK, 0, 0, "back"), P1(6300, 0, 0, 0, "release"),
	SHOT(6600, "huddiff_2"),
	END(10000),
};

// hud2 (level 2): the HUD in split screen -- Obelix hits a Roman (the combo bar
// shows), player 1's Back shows the whole HUD.
const Step kHud2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_POKE8, -10, 0x50, 0, 7, 0, "asterix health 7" },
	{ 1500, S_POKE8, -11, 0x50, 0, 12, 0, "obelix health 12" },
	{ 1600, S_ENEMY, HERO_OBELIX, 0, 0, 0, 0, "obelix at a roman" },
	VPAD(2000, 1, PAD_X, 0, 0, "p2 X"), VPAD(2150, 1, 0, 0, 0, "p2 release"),
	VPAD(2500, 1, PAD_X, 0, 0, "p2 X"), VPAD(2650, 1, 0, 0, 0, "p2 release"),
	VPAD(3000, 1, PAD_X, 0, 0, "p2 X"), VPAD(3150, 1, 0, 0, 0, "p2 release"),
	SHOT(3300, "hud2_1"),
	P1(4000, PAD_BACK, 0, 0, "p1 back"), P1(4300, 0, 0, 0, "p1 release"),
	SHOT(4600, "hud2_2"),
	VPAD(6000, 1, PAD_BACK, 0, 0, "p2 back"),
	SHOT(6600, "hud2_3"),
	VPAD(9000, 1, 0, 0, 0, "p2 release"),
	SHOT(9300, "hud2_4"),
	SHOT(14000, "hud2_5"),
	END(14500),
};

// bump2 (level 2): player 2's Obelix walks into player 1's Asterix standing
// 4 units in front of him (the user: heroes walking into each other jump).
const Step kBump2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2000, S_NEXTTO, HERO_ASTERIX, HERO_OBELIX, 0, 4.0f, 0, "asterix in front of obelix" },
	{ 2300, S_HEROFIELDS, HERO_ASTERIX, 1, 0, 0, 0, "dump asterix" },
	VPAD(5000, 1, 0, 0, 1.0f, "p2 forward"),
	SHOT(6500, "bump2_1"),
	VPAD(8000, 1, 0, 0, 0, "p2 stop"),
	END(9000),
};

// bump2i: bump2 with Idefix sent 20 units away first (is the hop over the dog?).
const Step kBump2I[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2000, S_NEXTTO, HERO_ASTERIX, HERO_OBELIX, 0, 4.0f, 0, "asterix in front of obelix" },
	{ 2200, S_NEXTTO, HERO_IDEFIX, HERO_OBELIX, 0, -20.0f, 0, "idefix away" },
	{ 2300, S_WATCHRW, HERO_OBELIX, 0, 0, 7, 0, "obelix vertical speed" },
	{ 4800, S_NEXTTO, HERO_IDEFIX, HERO_OBELIX, 0, -20.0f, 0, "idefix away" },
	VPAD(5000, 1, 0, 0, 1.0f, "p2 forward"),
	SHOT(6500, "bump2i_1"),
	VPAD(8000, 1, 0, 0, 0, "p2 stop"),
	END(9000),
};

// bump2l / bump2w: bump2i with Obelix leading (0x4801); w: and Asterix told to
// wait (0x4802, the game's AI companion).
const Step kBump2L[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 2000, S_NEXTTO, HERO_ASTERIX, HERO_OBELIX, 0, 4.0f, 0, "asterix in front of obelix" },
	{ 2200, S_NEXTTO, HERO_IDEFIX, HERO_OBELIX, 0, -20.0f, 0, "idefix away" },
	{ 4800, S_NEXTTO, HERO_IDEFIX, HERO_OBELIX, 0, -20.0f, 0, "idefix away" },
	VPAD(5000, 1, 0, 0, 1.0f, "p2 forward"),
	VPAD(8000, 1, 0, 0, 0, "p2 stop"),
	END(9000),
};
const Step kBump2W[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 1700, S_TRIOEV, 0x4802, 0, 0, 0, 0, "companion wait" },
	{ 2000, S_NEXTTO, HERO_ASTERIX, HERO_OBELIX, 0, 4.0f, 0, "asterix in front of obelix" },
	{ 2200, S_NEXTTO, HERO_IDEFIX, HERO_OBELIX, 0, -20.0f, 0, "idefix away" },
	{ 4800, S_NEXTTO, HERO_IDEFIX, HERO_OBELIX, 0, -20.0f, 0, "idefix away" },
	VPAD(5000, 1, 0, 0, 1.0f, "p2 forward"),
	VPAD(8000, 1, 0, 0, 0, "p2 stop"),
	END(9000),
};

// shield2o (level 2, individual heroes): player 2's Obelix picks up the shield
// bonus at -807.3 512.0 -1044.9 (the user's): his fourth shield, not Asterix's.
const Step kShield2O[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_OBELIX, -807.3f, 512.5f, -1044.9f, 1500),
	SHOT(4500, "shield2o_1"),
	SHOT(6000, "shield2o_2"),
	VPAD(7000, 1, PAD_BACK, 0, 0, "p2 back"),
	SHOT(8000, "shield2o_3"),
	VPAD(8500, 1, 0, 0, 0, "p2 release"),
	END(9000),
};

// revshield (-Level 2, individual heroes): Asterix (player 1) falls into the
// LV2-6 pit (laid on the planks), Obelix revives him (no Romans about), then he
// gets a shield as the bonus gives it: player 1's HUD shows his own health.
const Step kRevShield[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 88.0f, 2.2f, -390.4f), PLACEAT(3000, HERO_ASTERIX, 92.5f, 2.2f, -390.4f),
	GLIDEAT(4500, HERO_ASTERIX, 97.0f, 2.2f, -390.4f, 400),
	PLACEAT(7000, HERO_OBELIX, 90.5f, 2.2f, -390.4f),
	VPAD(7500, 1, PAD_B, 0, 0, "p2 holds B"),
	SHOT(12000, "revshield_0"),
	VPAD(17000, 1, 0, 0, 0, "p2 release"),
	SHOT(17500, "revshield_1"),
	{ 19000, S_GIVE, HERO_ASTERIX, 1, 0, 0, 0, "asterix a shield" },
	SHOT(20000, "revshield_2"),
	SHOT(22000, "revshield_3"),
	{ 23000, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	SHOT(24500, "revshield_4"),
	END(25000),
};

// combofull2 (level 2, individual heroes): player 2's combo bar filled ("Combo"
// on it, the right way round in player 2's mirrored HUD).
const Step kComboFull2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2000, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix combo bar full" },
	SHOT(2600, "combofull2_1"), SHOT(3200, "combofull2_2"), SHOT(4000, "combofull2_3"),
	{ 5000, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix combo bar full" },
	SHOT(5800, "combofull2_4"),
	END(6500),
};

// combomix (level 2, individual heroes): player 2's Obelix fills his combo bar
// hit by hit ("Combo"), then player 1's Asterix -- his bar must fill on its own
// (not a full red bar without "Combo"); the pause menu hides both HUDs.
#define HITS(t, hero, n) { t, S_GIVE, hero, 3, 0, (float)(n), 0, "hits" }
const Step kComboMix[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	HITS(2000, HERO_OBELIX, 8), HITS(2300, HERO_OBELIX, 8), HITS(2600, HERO_OBELIX, 8), HITS(2900, HERO_OBELIX, 8),
	HITS(3200, HERO_OBELIX, 8),
	SHOT(4500, "combomix_1"),
	HITS(5000, HERO_ASTERIX, 6), HITS(5300, HERO_ASTERIX, 6), HITS(5600, HERO_ASTERIX, 6),
	SHOT(6500, "combomix_2"),
	HITS(6900, HERO_ASTERIX, 6), HITS(7200, HERO_ASTERIX, 6),
	SHOT(8000, "combomix_3"),
	{ 9500, S_POKE8, -2, 0x5F, 0, 4.0f, 0, "the pause's HUD hiding (game manager +0x5C bit 26)" },
	SHOT(11000, "combomix_4"),
	{ 12000, S_POKE8, -2, 0x5F, 0, 4.0f, 0, "the pause's HUD back" },
	{ 13600, S_POKE8, -3, 2, 0, 0, 0, "the pause menu (game manager state 2)" },
	SHOT(15500, "combomix_6"),
	{ 16000, S_POKE8, -3, 1, 0, 0, 0, "back to the game (state 1)" },
	SHOT(17500, "combomix_7"),
	SHOT(13500, "combomix_5"),
	END(18500),
};

// pausedown (level 2, XXL_DOWNAT=0:3000): Asterix down 8 units from Obelix; the
// pause menu open and shut (user: the fallen hero walked over to the other as
// the sidekick, then fell again there -- into a gap if one was on the way).
const Step kPauseDown[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -854.9f, 508.5f, -1072.2f), PLACEAT(1500, HERO_OBELIX, -846.9f, 508.5f, -1072.2f),
	SHOT(5500, "pausedown_0"),
	{ 6000, S_POKE8, -3, 2, 0, 0, 0, "the pause menu (game manager state 2)" },
	{ 6000, S_POKE8, -2, 0x5E, 0, (float)0x10, 0, "its bars (game manager +0x5C bit 20) on" },
	{ 8000, S_POKE8, -3, 1, 0, 0, 0, "back to the game (state 1; the game clears the bars itself)" },
	SHOT(9000, "pausedown_1"), SHOT(12000, "pausedown_2"),
	END(12500),
};

// cork53 (-Level 5, sector 3; nolock on): LV5-3's corkscrew (circle -259.71 10.49
// 353.69, turnstile -255.87 9.54 358.30) -- Asterix onto its circle (event 0110:
// Obelix leads, Asterix waits; nolock_except_corkscrews locks him), player 2's Obelix
// turns the turnstile, the launch (0x0C09 -> 4805, AI order 2) jumps Asterix up:
// he must be his player's again once landed (nothing in LV5-3 frees him after).
const Step kCork53[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -226.0f, 10.2f, 323.6f), PLACEAT(2500, HERO_OBELIX, -252.0f, 10.0f, 358.0f),
	GLIDEAT(2800, HERO_ASTERIX, -210.0f, 10.4f, 326.2f, 1200),   // through zone 0368 (arms the circle)
	GLIDEAT(4200, HERO_ASTERIX, -248.33f, 10.2f, 350.65f, 1200),
	{ 5300, S_OBJEV, 44, 0x999, 0, 0, 0, "corkscrew armed (its fight skipped: 0x999)" },
	GLIDEAT(5500, HERO_ASTERIX, -259.71f, 10.9f, 353.69f, 800),
	{ 6500, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile" },
	{ 7000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	SHOT(20000, "cork53_1"), SHOT(25000, "cork53_2"),
	P1(26000, 0, 1.0f, 0, "p1 walks"), P1(27500, 0, 0, 0, "p1 stops"),
	SHOT(28000, "cork53_3"),
	END(29000),
};

// cork53b (-Level 5): cork53 with the landing box #3 switched off after the circle
// (in the user's run it never went off): the launch's jump over, Asterix is his
// player's again (not leading).
const Step kCork53B[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -226.0f, 10.2f, 323.6f), PLACEAT(2500, HERO_OBELIX, -252.0f, 10.0f, 358.0f),
	GLIDEAT(2800, HERO_ASTERIX, -210.0f, 10.4f, 326.2f, 1200),   // through zone 0368 (arms the circle)
	GLIDEAT(4200, HERO_ASTERIX, -248.33f, 10.2f, 350.65f, 1200),
	{ 5300, S_OBJEV, 44, 0x999, 0, 0, 0, "corkscrew armed (its fight skipped: 0x999)" },
	GLIDEAT(5500, HERO_ASTERIX, -259.71f, 10.9f, 353.69f, 800),
	{ 6600, S_DETOFF, 0x2C, 3, 0, 0, 0, "box #3 (event 0118) off: the user's run, where it never went off" },
	{ 6500, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile" },
	{ 7000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 7900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 8950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 9850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 10900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 11950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 12850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 13900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 14950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 15850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16000, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16150, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16300, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16450, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16600, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16750, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 16900, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17050, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17200, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17350, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17500, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17650, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17800, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 17950, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18100, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18250, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18400, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18550, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18700, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	{ 18850, S_OBJEV, 45, 0x996, 0, 0, 0, "the turnstile spins (0x996: its hit group; its fight skipped)" },
	SHOT(20000, "cork53b_1"), SHOT(25000, "cork53b_2"),
	P1(26000, 0, 1.0f, 0, "p1 walks"), P1(27500, 0, 0, 0, "p1 stops"),
	SHOT(28000, "cork53b_3"),
	END(29000),
};

// leave2 (level 2): the level's switch to Obelix (4801), then player 2 leaves
// (Start): the lead stays Obelix's -- player 1 drives him, as in single player.
const Step kLeave2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 3000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (the level)" },
	SHOT(4500, "leave2_1"),
	VPAD(5000, 1, PAD_START, 0, 0, "p2 start (leave)"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	P1(6500, 0, 1.0f, 0, "p1 walks"), P1(8000, 0, 0, 0, "p1 stops"),
	SHOT(8500, "leave2_2"),
	END(9500),
};

// launch53 (-Level 5, sector 3): the user's LV5-3 launcher (-341.28 14.08 151.93) --
// Obelix leads (4801), player 1's Asterix uses it (X): he leads for the flight (the
// launcher asks for the leader) and keeps the lead after landing, as in single
// player where only the leader uses one.
const Step kLaunch53[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -341.64f, 12.2f, 152.53f), PLACEAT(3000, HERO_OBELIX, -340.69f, 12.2f, 151.56f),
	{ 3500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	P1(5000, PAD_X, 0, 0, "p1 X (launcher)"), P1(5200, 0, 0, 0, "p1 release"),
	SHOT(8000, "launch53_flight"),
	SHOT(19000, "launch53_landed"),
	END(20000),
};

// pushkeg5 (-Level 5, sector 3): Asterix lights the powder keg (the level
// hands the lead to Obelix), player 2's Obelix pushes the big block (the level
// then makes it slide on its own: 09A7), and Asterix walks past the block --
// Obelix must get his controls back when the slide ends.
const Step kPushKeg5[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -346.19f, 19.705f, 370.093f), PLACEAT(3000, HERO_OBELIX, -346.5f, 19.7f, 365.0f),
	{ 3800, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4000, S_TORCH, 6, 0, 0, 0, 0, "asterix gets a lit torch" },
	PLACEAT(5000, HERO_ASTERIX, -346.72f, 19.81f, 369.79f),
	P1(5500, PAD_X, 0, 0, "p1 X (keg)"), P1(5700, 0, 0, 0, "p1 release"),
	P1(7000, PAD_B, 0, 0, "p1 B (keg)"), P1(7200, 0, 0, 0, "p1 release"),
	PLACEAT(10500, HERO_OBELIX, -344.226f, 19.634f, 335.444f),
	VPAD(12000, 1, 0, -0.65f, 0.76f, "p2 turns to the block"), VPAD(12150, 1, 0, 0, 0, "p2 release"),
	VPAD(12500, 1, PAD_X, 0, 0, "p2 holds X (grabs the block)"),
	VPAD(14000, 1, PAD_X, -0.65f, 0.76f, "p2 pushes (X held)"),
	PLACEAT(14500, HERO_ASTERIX, -343.0f, 19.63f, 341.0f),
	SHOT(15000, "pushkeg5_1"),
	GLIDEAT(15000, HERO_ASTERIX, -344.0f, 19.63f, 338.2f, 1000),
	GLIDEAT(17000, HERO_ASTERIX, -350.0f, 19.70f, 328.0f, 2000),
	SHOT(20000, "pushkeg5_2"), SHOT(28000, "pushkeg5_3"),
	VPAD(30000, 1, 0, 0, 0, "p2 lets go"),
	VPAD(31000, 1, 0, 0, -1.0f, "p2 walks back"), VPAD(33000, 1, 0, 0, 0, "p2 stops"),
	SHOT(33500, "pushkeg5_4"),
	END(35000),
};

// molefov (level 2, individual heroes): player 1's Asterix does the mole
// combo -- the field of view widens and must come back for both halves after
// it; then player 2's combo cheat (Back + D-pad left) brings up "Combo" on
// player 2's bar.
const Step kMoleFov[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat" },
	SHOT(2500, "molefov_1"),
	{ 3000, S_P1COMBO, 5, 0, 0, 0, 0, "mole?" },
	SHOT(4000, "molefov_2"),
	{ 9000, S_P1COMBO, 3, 0, 0, 0, 0, "combo 3" },
	SHOT(12000, "molefov_3"),
	SHOT(20000, "molefov_4"),
	VPAD(21000, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(21100, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 combo cheat"), VPAD(21300, 1, 0, 0, 0, "p2 release"),
	SHOT(22500, "molefov_5"), SHOT(24000, "molefov_6"),
	END(25000),
};

// shields6 (level 2, individual heroes): both heroes with six shields -- both
// HUDs fit their halves.
const Step kShields6[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_GIVE, HERO_ASTERIX, 1, 0, 0, 0, "asterix a shield" },
	{ 1600, S_GIVE, HERO_ASTERIX, 1, 0, 0, 0, "asterix a shield" },
	{ 1700, S_GIVE, HERO_ASTERIX, 1, 0, 0, 0, "asterix a shield" },
	{ 1800, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	{ 1900, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	{ 2000, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	SHOT(3500, "shields6_1"),
	{ 5000, S_POKE8, -10, 0x50, 0, 22, 0, "asterix health 22" },
	{ 5000, S_POKE8, -11, 0x50, 0, 17, 0, "obelix health 17" },
	VPAD(5200, 1, PAD_BACK, 0, 0, "p2 back"), P1(5200, PAD_BACK, 0, 0, "p1 back"), P1(5500, 0, 0, 0, "p1 release"),
	SHOT(6500, "shields6_2"),
	VPAD(7000, 1, 0, 0, 0, "p2 release"),
	END(7500),
};

// shieldfull (level 2, individual heroes): Obelix (player 2) with six shields
// walks over the LV2-1 shield: not his (stays for Asterix); Asterix then takes it.
const Step kShieldFull[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	{ 1100, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	{ 1200, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	GLIDEAT(1500, HERO_OBELIX, -807.3f, 512.5f, -1044.9f, 1500),
	SHOT(4500, "shieldfull_1"),
	GLIDEAT(6000, HERO_OBELIX, -820.0f, 512.5f, -1060.0f, 800),
	GLIDEAT(6000, HERO_ASTERIX, -807.3f, 512.5f, -1044.9f, 1500),
	SHOT(9000, "shieldfull_2"),
	END(9500),
};

// req410 (-Level 4): the slide's helmet requirement in LV4-10 (the user's
// detector -775.8 300.7 -1366 -> -1374, the spy's words clicked through with X):
// its "0/1500" under its own plate at the right.
const Step kReq410[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -775.754f, 300.724f, -1366.0f), PLACEAT(3000, HERO_OBELIX, -773.0f, 300.724f, -1366.0f),
	GLIDEAT(4000, HERO_ASTERIX, -775.754f, 300.724f, -1374.0f, 800),
	VPAD(7000, 1, PAD_X, 0, 0, "x"), VPAD(7200, 1, 0, 0, 0, "release"),
	VPAD(8500, 1, PAD_X, 0, 0, "x"), VPAD(8700, 1, 0, 0, 0, "release"),
	VPAD(10000, 1, PAD_X, 0, 0, "x"), VPAD(10200, 1, 0, 0, 0, "release"),
	VPAD(11500, 1, PAD_X, 0, 0, "x"), VPAD(11700, 1, 0, 0, 0, "release"),
	VPAD(13000, 1, PAD_X, 0, 0, "x"), VPAD(13200, 1, 0, 0, 0, "release"),
	VPAD(14500, 1, PAD_X, 0, 0, "x"), VPAD(14700, 1, 0, 0, 0, "release"),
	SHOT(16000, "req410_1"), SHOT(18000, "req410_2"), SHOT(20000, "req410_3"),
	END(20500),
};

// hudorder (level 2, individual heroes): Asterix at 7, Obelix at 12; player 1's
// HUD button then player 2's Back while it shows; later the other way round.
// Each HUD must show its own hero's health.
const Step kHudOrder[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_POKE8, -10, 0x50, 0, 7, 0, "asterix health 7" },
	{ 1500, S_POKE8, -11, 0x50, 0, 12, 0, "obelix health 12" },
	{ 10000, S_GIVE, HERO_ASTERIX, 0, 0, 1, 0, "asterix +1 (player 1's HUD)" },
	VPAD(10500, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(10800, 1, 0, 0, 0, "p2 release"),
	SHOT(11500, "hudorder_1"),
	VPAD(20000, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(20300, 1, 0, 0, 0, "p2 release"),
	{ 20500, S_GIVE, HERO_ASTERIX, 0, 0, 1, 0, "asterix +1 (player 1's HUD)" },
	SHOT(21500, "hudorder_2"),
	END(22000),
};


// shopsave (-Level 2, sector 10): player 1's Asterix at the merchant
// (1647.88 -745.09 -1208.53), player 2's Obelix opens the save point's menu
// (1658.87 -743.84 -1216.27): the shop closes first; player 1 leaves the save
// menu (B, left, A), then walks -- and can shop again.
const Step kShopSave[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1647.88f, -744.5f, -1208.53f), PLACEAT(3000, HERO_OBELIX, 1658.87f, -743.3f, -1216.27f),
	PLACEAT(3400, HERO_ASTERIX, 1647.88f, -744.5f, -1208.53f), PLACEAT(3400, HERO_OBELIX, 1658.87f, -743.3f, -1216.27f),
	SECTORS(3600),
	{ 4000, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(4500, PAD_X, 0, 0, "p1 X"), P1(4700, 0, 0, 0, "p1 release"),
	P1(8000, PAD_X, 0, 0, "p1 X (words)"), P1(8200, 0, 0, 0, "p1 release"),
	SHOT(9500, "shopsave_1"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X (save point)"), VPAD(10200, 1, 0, 0, 0, "p2 release"),
	SHOT(12000, "shopsave_2"),
	KEY(13000, VK_ESCAPE, 1, "esc"), KEY(13150, VK_ESCAPE, 0, "esc up"),
	SHOT(14000, "shopsave_3"),
	KEY(14500, VK_LEFT, 1, "left"), KEY(14650, VK_LEFT, 0, "left up"),
	KEY(15500, VK_RETURN, 1, "enter"), KEY(15650, VK_RETURN, 0, "enter up"),
	SHOT(17500, "shopsave_4"),
	P1(18000, 0, 0, 1.0f, "p1 walks"), P1(19000, 0, 0, 0, "p1 stops"),
	SHOT(19500, "shopsave_5"),
	{ 20000, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant again" },
	P1(20500, PAD_X, 0, 0, "p1 X"), P1(20700, 0, 0, 0, "p1 release"),
	SHOT(23000, "shopsave_6"),
	P1(23500, PAD_B, 0, 0, "p1 B (back)"), P1(23700, 0, 0, 0, "p1 release"),
	SHOT(25500, "shopsave_7"),
	END(26500),
};

// shopzone (-Level 2, sector 10): player 2's Obelix at the merchant, player 1's
// Asterix walks to the save point -- the zone there (event 05A0) sends "Asterix
// leads": held back until the visit is over (the shop stays Obelix's, in his
// half; xxl_coop.bro_test.log). Player 2 then leaves the shop (B, B).
const Step kShopZone[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 1647.88f, -744.5f, -1208.53f), PLACEAT(3000, HERO_ASTERIX, 1652.0f, -744.5f, -1205.0f),
	PLACEAT(3400, HERO_OBELIX, 1647.88f, -744.5f, -1208.53f), PLACEAT(3400, HERO_ASTERIX, 1652.0f, -744.5f, -1205.0f),
	SECTORS(3600),
	{ 4000, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"), VPAD(4700, 1, 0, 0, 0, "p2 release"),
	VPAD(8000, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(8200, 1, 0, 0, 0, "p2 release"),
	SHOT(9000, "shopzone_1"),
	GLIDEAT(9500, HERO_ASTERIX, 1661.5f, -743.8f, -1217.3f, 2500),
	SHOT(13000, "shopzone_2"),
	VPAD(14000, 1, PAD_B, 0, 0, "p2 B (back)"), VPAD(14200, 1, 0, 0, 0, "p2 release"),
	VPAD(15500, 1, PAD_B, 0, 0, "p2 B (leave)"), VPAD(15700, 1, 0, 0, 0, "p2 release"),
	SHOT(19000, "shopzone_3"),
	VPAD(19500, 1, 0, 0, -1.0f, "p2 walks"), VPAD(21000, 1, 0, 0, 0, "p2 stops"),
	SHOT(22000, "shopzone_4"),
	END(22500),
};

// shopbuyo (level 2, individual heroes; userlogs xxl_coop.merchant_issues.log):
// Asterix leads, player 2's Obelix (not leading) buys three items at the
// merchant, both heroes at 5 health: what he buys is his (it went to the
// leader). Meanwhile player 1's Asterix walks with the stick up: along his own
// camera's forward (he steered by the shop's view), and again after the visit.
const Step kShopBuyO[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_POKE8, -10, 0x50, 0, 5.0f, 0, "asterix health 5" },
	{ 1000, S_POKE8, -11, 0x50, 0, 5.0f, 0, "obelix health 5" },
	{ 1000, S_POKE8, -1, 0x6A, 0, 2.0f, 0, "helmets (+0x68) 131072 more" },
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	VPAD(7500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(7700, 1, 0, 0, 0, "p2 release"),
	SHOT(9500, "shopbuyo_1"),
	VPAD(10000, 1, PAD_A, 0, 0, "p2 A (buy)"), VPAD(10200, 1, 0, 0, 0, "p2 release"),
	VPAD(12000, 1, 0, 1.0f, 0, "p2 right"), VPAD(12300, 1, 0, 0, 0, "p2 release"),
	VPAD(13000, 1, PAD_A, 0, 0, "p2 A (buy)"), VPAD(13200, 1, 0, 0, 0, "p2 release"),
	VPAD(15000, 1, 0, 1.0f, 0, "p2 right"), VPAD(15300, 1, 0, 0, 0, "p2 release"),
	VPAD(16000, 1, PAD_A, 0, 0, "p2 A (buy)"), VPAD(16200, 1, 0, 0, 0, "p2 release"),
	SHOT(17500, "shopbuyo_2"),
	{ 18000, S_STEERCHECK, HERO_ASTERIX, 0, 0, 0, 0, "asterix's place" },
	P1(18000, 0, 0, 1.0f, "p1 forward"), P1(19500, 0, 0, 0, "p1 stop"),
	{ 19600, S_STEERCHECK, HERO_ASTERIX, 1, 0, 0, 0, "asterix's walk (obelix shopping)" },
	SHOT(19700, "shopbuyo_3"),
	VPAD(20000, 1, PAD_B, 0, 0, "p2 B (back)"), VPAD(20200, 1, 0, 0, 0, "p2 release"),
	VPAD(23000, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(23200, 1, 0, 0, 0, "p2 release"),
	SHOT(26000, "shopbuyo_4"),
	{ 26500, S_STEERCHECK, HERO_ASTERIX, 0, 0, 0, 0, "asterix's place" },
	P1(26500, 0, 0, 1.0f, "p1 forward"), P1(28000, 0, 0, 0, "p1 stop"),
	{ 28100, S_STEERCHECK, HERO_ASTERIX, 1, 0, 0, 0, "asterix's walk (after the visit)" },
	END(29000),
};

// shopsavea (-Level 2, sector 10; userlogs xxl_coop.LV2_10_merchant_save.log):
// the save point's zone (05A0, "Asterix leads", one shot) set off first, then
// Obelix leads; player 1's Asterix (not leading) at the merchant, player 2's
// Obelix opens the save point's menu: the shop closes. The menu left (cancel,
// left, confirm), each camera follows its own hero (Obelix's half was on
// Asterix) and both heroes walk along their own views.
const Step kShopSaveA[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1652.0f, -744.5f, -1205.0f), PLACEAT(3000, HERO_OBELIX, 1658.87f, -743.3f, -1216.27f),
	PLACEAT(3400, HERO_ASTERIX, 1652.0f, -744.5f, -1205.0f), PLACEAT(3400, HERO_OBELIX, 1658.87f, -743.3f, -1216.27f),
	SECTORS(3600),
	GLIDEAT(3800, HERO_ASTERIX, 1661.5f, -743.8f, -1217.3f, 2500),
	{ 7000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	GLIDEAT(7500, HERO_ASTERIX, 1647.88f, -744.5f, -1208.53f, 2000),
	{ 10000, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(10500, PAD_X, 0, 0, "p1 X"), P1(10700, 0, 0, 0, "p1 release"),
	P1(14000, PAD_X, 0, 0, "p1 X (words)"), P1(14200, 0, 0, 0, "p1 release"),
	SHOT(15500, "shopsavea_1"),
	{ 15600, S_CAMTARGET, 0, 0, 0, 0, 0, "cameras (asterix shopping)" },
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X (save point)"), VPAD(16200, 1, 0, 0, 0, "p2 release"),
	SHOT(18000, "shopsavea_2"),
	{ 19000, S_BIND, 21, 0, 0, 0, 0, "cancel" },
	SHOT(20000, "shopsavea_3"),
	{ 20500, S_BIND, 18, 34, 54, 6.0f, 0, "left (the D-pad's bindings; 54 the message box's)" },
	{ 21500, S_BIND, 20, 44, 56, 3.0f, 0, "confirm (the A button's bindings; 56 the message box's)" },
	SHOT(23500, "shopsavea_4"),
	{ 24000, S_CAMTARGET, 0, 0, 0, 0, 0, "cameras (after the save menu)" },
	{ 24000, S_STEERCHECK, HERO_ASTERIX, 0, 0, 0, 0, "asterix's place" },
	{ 24000, S_STEERCHECK, HERO_OBELIX, 0, 0, 0, 0, "obelix's place" },
	P1(24000, 0, 0, 1.0f, "p1 forward"), VPAD(24000, 1, 0, 0, 1.0f, "p2 forward"),
	P1(25200, 0, 0, 0, "p1 stop"), VPAD(25200, 1, 0, 0, 0, "p2 stop"),
	{ 25300, S_STEERCHECK, HERO_ASTERIX, 1, 0, 0, 0, "asterix's walk" },
	{ 25300, S_STEERCHECK, HERO_OBELIX, 1, 0, 0, 0, "obelix's walk" },
	SHOT(25500, "shopsavea_5"),
	END(26500),
};

// shopsaveo (-Level 2, sector 10; userlogs xxl_coop.LV2_10_merchant_save2.log):
// Asterix leads, player 2's Obelix at the merchant (player 1's Asterix walks
// meanwhile: along his own view), player 1's Asterix opens the save point's
// menu: the shop closes. After the menu (cancel, left, confirm) each camera
// follows its own hero (player 1's half was on Obelix).
const Step kShopSaveO[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1652.0f, -744.5f, -1205.0f), PLACEAT(3000, HERO_OBELIX, 1647.88f, -744.5f, -1208.53f),
	PLACEAT(3400, HERO_ASTERIX, 1652.0f, -744.5f, -1205.0f), PLACEAT(3400, HERO_OBELIX, 1647.88f, -744.5f, -1208.53f),
	SECTORS(3600),
	GLIDEAT(3800, HERO_ASTERIX, 1661.5f, -743.8f, -1217.3f, 2500),
	{ 6800, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(7000, 1, PAD_X, 0, 0, "p2 X"), VPAD(7200, 1, 0, 0, 0, "p2 release"),
	VPAD(10500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(10700, 1, 0, 0, 0, "p2 release"),
	SHOT(12000, "shopsaveo_1"),
	{ 12100, S_CAMTARGET, 0, 0, 0, 0, 0, "cameras (obelix shopping)" },
	{ 12500, S_STEERCHECK, HERO_ASTERIX, 0, 0, 0, 0, "asterix's place" },
	P1(12500, 0, 0, 1.0f, "p1 forward"), P1(13500, 0, 0, 0, "p1 stop"),
	{ 13600, S_STEERCHECK, HERO_ASTERIX, 1, 0, 0, 0, "asterix's walk (obelix shopping)" },
	GLIDEAT(13700, HERO_ASTERIX, 1658.87f, -743.3f, -1216.27f, 1000),
	SHOT(15400, "shopsaveo_2a"),
	P1(15500, 0, -1.0f, 0, "p1 left (turns to the druid)"), P1(15700, 0, 0, 0, "p1 neutral"),
	P1(16000, PAD_X, 0, 0, "p1 X (save point)"), P1(16200, 0, 0, 0, "p1 release"),
	SHOT(17500, "shopsaveo_2"),
	{ 18500, S_BIND, 21, 0, 0, 0, 0, "cancel" },
	SHOT(19500, "shopsaveo_3"),
	{ 20000, S_BIND, 18, 34, 54, 6.0f, 0, "left (the D-pad's bindings; 54 the message box's)" },
	{ 21000, S_BIND, 20, 44, 56, 3.0f, 0, "confirm (the A button's bindings; 56 the message box's)" },
	SHOT(23000, "shopsaveo_4"),
	{ 23500, S_CAMTARGET, 0, 0, 0, 0, 0, "cameras (after the save menu)" },
	{ 23500, S_STEERCHECK, HERO_ASTERIX, 0, 0, 0, 0, "asterix's place" },
	{ 23500, S_STEERCHECK, HERO_OBELIX, 0, 0, 0, 0, "obelix's place" },
	P1(23500, 0, 0, 1.0f, "p1 forward"), VPAD(23500, 1, 0, 0, 1.0f, "p2 forward"),
	P1(24700, 0, 0, 0, "p1 stop"), VPAD(24700, 1, 0, 0, 0, "p2 stop"),
	{ 24800, S_STEERCHECK, HERO_ASTERIX, 1, 0, 0, 0, "asterix's walk" },
	{ 24800, S_STEERCHECK, HERO_OBELIX, 1, 0, 0, 0, "obelix's walk" },
	SHOT(25000, "shopsaveo_5"),
	END(26000),
};

// combodrain (level 2, individual heroes): player 2's Obelix (not leading)
// fills his combo bar (cheat: Dpad left) and lets it run out -- it must leave
// the "full" state ("indiv: ... ran out"), then fill again from hits.
const Step kComboDrain[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(1800, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(2000, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 combo cheat"), VPAD(2200, 1, 0, 0, 0, "p2 release"),
	SHOT(3000, "combodrain_1"),
	SHOT(20000, "combodrain_2"),
	VPAD(39800, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(40000, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 combo cheat"), VPAD(40200, 1, 0, 0, 0, "p2 release"),
	SHOT(41000, "combodrain_3"),
	END(41500),
};

// shopboss (-Level 2, sector 10): player 1's Asterix at the merchant, player 2's
// Obelix walks east into the boss's zone (cutscene; xxl_coop.cinematic+shop.log):
// the shop closes, Asterix is brought to Obelix, and after the cutscene both
// cameras follow their heroes (not the shop's).
const Step kShopBoss[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 1647.88f, -744.5f, -1208.53f), PLACEAT(3000, HERO_OBELIX, 1663.5f, -743.3f, -1211.1f),
	PLACEAT(3400, HERO_ASTERIX, 1647.88f, -744.5f, -1208.53f), PLACEAT(3400, HERO_OBELIX, 1663.5f, -743.3f, -1211.1f),
	SECTORS(3600),
	{ 4000, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(4500, PAD_X, 0, 0, "p1 X"), P1(4700, 0, 0, 0, "p1 release"),
	P1(8000, PAD_X, 0, 0, "p1 X (words)"), P1(8200, 0, 0, 0, "p1 release"),
	SHOT(9500, "shopboss_1"),
	GLIDEAT(10000, HERO_OBELIX, 1685.0f, -743.8f, -1211.0f, 3000),
	SHOT(14000, "shopboss_2"),
	SHOT(24000, "shopboss_3"),
	P1(25000, 0, 0, 1.0f, "p1 walks"), VPAD(25000, 1, 0, 0, -1.0f, "p2 walks"),
	P1(26500, 0, 0, 0, "p1 stops"), VPAD(26500, 1, 0, 0, 0, "p2 stops"),
	SHOT(27500, "shopboss_4"),
	END(28000),
};

// shopbossrun (-Level 2, sector 10): the boss's cutscene first (Asterix into
// its zone, both by the merchant); then player 2's Obelix opens the shop and
// Asterix wakes the boss (its run zone, event 00A0; xxl_coop.shop+boss_tp.log):
// the shop closes and Obelix is brought along.
const Step kShopBossRun[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 1650.0f, -744.5f, -1208.5f), PLACEAT(3000, HERO_ASTERIX, 1663.5f, -743.3f, -1211.1f),
	PLACEAT(3400, HERO_OBELIX, 1650.0f, -744.5f, -1208.5f), PLACEAT(3400, HERO_ASTERIX, 1663.5f, -743.3f, -1211.1f),
	SECTORS(3600),
	GLIDEAT(4000, HERO_ASTERIX, 1672.45f, -743.82f, -1210.53f, 1000),
	SHOT(7000, "shopbossrun_1"),
	P1(16000, PAD_START, 0, 0, "p1 start (cutscene)"), P1(16200, 0, 0, 0, "p1 release"),
	{ 19000, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(19500, 1, PAD_X, 0, 0, "p2 X"), VPAD(19700, 1, 0, 0, 0, "p2 release"),
	VPAD(23000, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(23200, 1, 0, 0, 0, "p2 release"),
	SHOT(24500, "shopbossrun_2"),
	GLIDEAT(25000, HERO_ASTERIX, 1687.2f, -742.1f, -1210.7f, 1500),
	SHOT(28000, "shopbossrun_3"),
	VPAD(29000, 1, 0, 0, -1.0f, "p2 walks"), VPAD(30500, 1, 0, 0, 0, "p2 stops"),
	SHOT(31500, "shopbossrun_4"),
	END(32000),
};

// shopsas (level 2): player 1's Asterix at LV2-1's merchant, player 2's Obelix
// goes through the Sas into sector 2 -- the shop closes, the catch-up brings
// Asterix over; then Obelix starts the launcher with Asterix by it.
const Step kShopSas[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4200, 0, 0, 0, "p1 release"),
	P1(7500, PAD_X, 0, 0, "p1 X (words)"), P1(7700, 0, 0, 0, "p1 release"),
	SHOT(9000, "shopsas_1"),
	{ 9500, S_PLACE, HERO_OBELIX, 10, 0, 0, 0, "obelix into the sas (sector 2)" },
	{ 11000, S_PLACE, HERO_OBELIX, 11, 0, 0, 0, "obelix before the rectangles" },
	GLIDEAT(11500, HERO_OBELIX, -819.0f, 532.0f, -987.0f, 1200),
	SHOT(14000, "shopsas_2"), SHOT(15800, "shopsas_2b"),
	{ 16000, S_PLACE, HERO_ASTERIX, 18, 0, 0, 0, "asterix next to the launcher" },
	{ 16000, S_PLACE, HERO_OBELIX, 17, 0, 0, 0, "obelix on the launcher" },
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17700, 1, 0, 0, 0, "p2 release"),
	SHOT(20000, "shopsas_3"),
	SHOT(40000, "shopsas_4"),
	END(40500),
};
// shopsas0: shopsas without the shop (the cameras after the Sas and catch-up).
const Step kShopSas0[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
			SHOT(9000, "shopsas0_1"),
	{ 9500, S_PLACE, HERO_OBELIX, 10, 0, 0, 0, "obelix into the sas (sector 2)" },
	{ 11000, S_PLACE, HERO_OBELIX, 11, 0, 0, 0, "obelix before the rectangles" },
	GLIDEAT(11500, HERO_OBELIX, -819.0f, 532.0f, -987.0f, 1200),
	SHOT(14000, "shopsas0_2"), SHOT(15800, "shopsas0_2b"),
	{ 16000, S_PLACE, HERO_ASTERIX, 18, 0, 0, 0, "asterix next to the launcher" },
	{ 16000, S_PLACE, HERO_OBELIX, 17, 0, 0, 0, "obelix on the launcher" },
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17700, 1, 0, 0, 0, "p2 release"),
	SHOT(20000, "shopsas0_3"),
	SHOT(40000, "shopsas0_4"),
	END(40500),
};

// shoplift (level 2): player 1's Asterix at LV2-1's merchant (into the item
// menu), player 2's Obelix put on the lifter beside it -- the shop stays.
const Step kShopLift[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4200, 0, 0, 0, "p1 release"),
	P1(7500, PAD_X, 0, 0, "p1 X (words)"), P1(7700, 0, 0, 0, "p1 release"),
	P1(9000, PAD_X, 0, 0, "p1 X (words)"), P1(9200, 0, 0, 0, "p1 release"),
	TELEPORT(10500, HERO_OBELIX, 0, 0, 0),
	SHOT(12000, "shoplift_1"),
	SHOT(25000, "shoplift_2"),
	END(25500),
};

// shoplaunch4 (-Level 4): LV4-5's merchant (the user's spot -860.4 41.9 -608.1)
// opened by player 1's Asterix, into the item menu; player 2's Obelix presses X on
// the launcher (-887.8 11.0 -634.7). After the landing player 1's stick must
// move Asterix.
const Step kShopLaunch4[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -860.383f, 41.897f, -608.083f), PLACEAT(3000, HERO_OBELIX, -887.753f, 10.971f, -634.670f),
	{ 4500, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5200, 0, 0, 0, "p1 release"),
	P1(8500, PAD_X, 0, 0, "p1 X (words)"), P1(8700, 0, 0, 0, "p1 release"),
	P1(10000, PAD_X, 0, 0, "p1 X (words)"), P1(10200, 0, 0, 0, "p1 release"),
	P1(11500, PAD_X, 0, 0, "p1 X (words)"), P1(11700, 0, 0, 0, "p1 release"),
	SHOT(12500, "shoplaunch4_1"),
	PLACEAT(13000, HERO_OBELIX, -887.753f, 10.971f, -634.670f),
	VPAD(13500, 1, PAD_X, 0, 0, "p2 X"), VPAD(13700, 1, 0, 0, 0, "p2 release"),
	SHOT(14000, "shoplaunch4_2"),
	SHOT(36000, "shoplaunch4_3"),
	P1(37000, 0, 0, 1.0f, "p1 stick up"), P1(39000, 0, 0, 0, "p1 release"),
	P1(39500, PAD_A, 0, 0, "p1 A"), P1(39700, 0, 0, 0, "p1 release"),
	SHOT(40000, "shoplaunch4_4"),
	END(41000),
};

// shoplaunch4b: Obelix leads (4801); player 1's Asterix (not leading) opens the
// merchant, into the item menu; player 2's Obelix (the leader) takes the
// launcher. After the landing both sticks move their heroes.
const Step kShopLaunch4b[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -860.383f, 41.897f, -608.083f), PLACEAT(3000, HERO_OBELIX, -887.753f, 10.971f, -634.670f),
	{ 3500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 4500, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5200, 0, 0, 0, "p1 release"),
	P1(8500, PAD_X, 0, 0, "p1 X (words)"), P1(8700, 0, 0, 0, "p1 release"),
	P1(10000, PAD_X, 0, 0, "p1 X (words)"), P1(10200, 0, 0, 0, "p1 release"),
	P1(11500, PAD_X, 0, 0, "p1 X (words)"), P1(11700, 0, 0, 0, "p1 release"),
	SHOT(12500, "shoplaunch4b_1"),
	PLACEAT(13000, HERO_OBELIX, -887.753f, 10.971f, -634.670f),
	VPAD(13500, 1, PAD_X, 0, 0, "p2 X"), VPAD(13700, 1, 0, 0, 0, "p2 release"),
	SHOT(14000, "shoplaunch4b_2"),
	SHOT(36000, "shoplaunch4b_3"),
	P1(37000, 0, 0, 1.0f, "p1 stick up"), VPAD(37000, 1, 0, 0, 1.0f, "p2 stick up"),
	P1(39000, 0, 0, 0, "p1 release"), VPAD(39000, 1, 0, 0, 0, "p2 release"),
	SHOT(40000, "shoplaunch4b_4"),
	END(41000),
};

// shoplaunch4c: Asterix leads; player 2's Obelix (not leading) opens the
// merchant, into the item menu; player 1's Asterix (the leader) takes the
// launcher.
const Step kShopLaunch4c[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, -860.383f, 41.897f, -608.083f), PLACEAT(3000, HERO_ASTERIX, -887.753f, 10.971f, -634.670f),
	{ 4500, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(5000, 1, PAD_X, 0, 0, "p2 X"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	VPAD(8500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(8700, 1, 0, 0, 0, "p2 release"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(10200, 1, 0, 0, 0, "p2 release"),
	VPAD(11500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(11700, 1, 0, 0, 0, "p2 release"),
	SHOT(12500, "shoplaunch4c_1"),
	PLACEAT(13000, HERO_ASTERIX, -887.753f, 10.971f, -634.670f),
	P1(13500, PAD_X, 0, 0, "p1 X"), P1(13700, 0, 0, 0, "p1 release"),
	SHOT(14000, "shoplaunch4c_2"),
	SHOT(36000, "shoplaunch4c_3"),
	P1(37000, 0, 0, 1.0f, "p1 stick up"), VPAD(37000, 1, 0, 0, 1.0f, "p2 stick up"),
	P1(39000, 0, 0, 0, "p1 release"), VPAD(39000, 1, 0, 0, 0, "p2 release"),
	SHOT(40000, "shoplaunch4c_4"),
	END(41000),
};

// shoplaunch4d: Obelix leads (4801) and opens the merchant (player 2), into the
// item menu; player 1's Asterix (not leading) takes the launcher.
const Step kShopLaunch4d[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, -860.383f, 41.897f, -608.083f), PLACEAT(3000, HERO_ASTERIX, -887.753f, 10.971f, -634.670f),
	{ 3500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 4500, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(5000, 1, PAD_X, 0, 0, "p2 X"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	VPAD(8500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(8700, 1, 0, 0, 0, "p2 release"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(10200, 1, 0, 0, 0, "p2 release"),
	VPAD(11500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(11700, 1, 0, 0, 0, "p2 release"),
	SHOT(12500, "shoplaunch4d_1"),
	PLACEAT(13000, HERO_ASTERIX, -887.753f, 10.971f, -634.670f),
	P1(13500, PAD_X, 0, 0, "p1 X"), P1(13700, 0, 0, 0, "p1 release"),
	SHOT(14000, "shoplaunch4d_2"),
	SHOT(36000, "shoplaunch4d_3"),
	P1(37000, 0, 0, 1.0f, "p1 stick up"), VPAD(37000, 1, 0, 0, 1.0f, "p2 stick up"),
	P1(39000, 0, 0, 0, "p1 release"), VPAD(39000, 1, 0, 0, 0, "p2 release"),
	SHOT(40000, "shoplaunch4d_4"),
	END(41000),
};

// shopjoin (level 2): single player at the merchant, player 2 presses Start --
// refused while the shop is open; after leaving it, Start joins.
const Step kShopJoin[] = {
	MARK(0, "baseline"),
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4200, 0, 0, 0, "p1 release"),
	P1(7500, PAD_X, 0, 0, "p1 X (words)"), P1(7700, 0, 0, 0, "p1 release"),
	VPAD(9000, 1, PAD_START, 0, 0, "p2 start (shop open)"), VPAD(9200, 1, 0, 0, 0, "p2 release"),
	SHOT(10000, "shopjoin_1"),
	P1(11000, PAD_B, 0, 0, "p1 B (back)"), P1(11200, 0, 0, 0, "p1 release"),
	P1(12500, PAD_B, 0, 0, "p1 B (leave)"), P1(12700, 0, 0, 0, "p1 release"),
	SHOT(16000, "shopjoin_2"),
	VPAD(17000, 1, PAD_START, 0, 0, "p2 start"), VPAD(17200, 1, 0, 0, 0, "p2 release"),
	SHOT(18500, "shopjoin_3"),
	END(19000),
};

// shopspy (level 2): player 2's Obelix at the merchant (item menu), player 1's
// Asterix sets off the second spy: the shop closes first; then player 1's A.
const Step kShopSpy[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	VPAD(7500, 1, PAD_X, 0, 0, "p2 X (words)"), VPAD(7700, 1, 0, 0, 0, "p2 release"),
	SHOT(12000, "shopspy_1"),
	PLACEAT(14000, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(14500, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(17000, "shopspy_2"),
	P1(19000, PAD_A, 0, 0, "p1 A"), P1(19200, 0, 0, 0, "p1 release"),
	SHOT(21000, "shopspy_3"),
	END(24000),
};

// shopspyb (level 2): the mirror of shopspy -- Obelix leads (4801), player 1's
// Asterix (not leading) at the merchant (words), player 2's Obelix sets off the
// second spy: the shop closes first, the agent's dialogue plays.
const Step kShopSpyB[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	{ 1200, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4200, 0, 0, 0, "p1 release"),
	P1(7500, PAD_X, 0, 0, "p1 X (words)"), P1(7700, 0, 0, 0, "p1 release"),
	SHOT(12000, "shopspyb_1"),
	PLACEAT(14000, HERO_OBELIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(14500, HERO_OBELIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(17000, "shopspyb_2"),
	VPAD(19000, 1, PAD_A, 0, 0, "p2 A"), VPAD(19200, 1, 0, 0, 0, "p2 release"),
	SHOT(21000, "shopspyb_3"),
	SHOT(30000, "shopspyb_4"),
	END(31000),
};

// shopspyw (level 2): shopspy with the merchant still talking (state 2, before
// his menu): after the agent's dialogue no shop screen (bars) may be left.
const Step kShopSpyW[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	PLACEAT(8500, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(9000, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(12000, "shopspyw_1"), SHOT(22000, "shopspyw_2"), SHOT(26000, "shopspyw_3"),
	END(28000),
};

// spyonly (level 2): shopspy without the merchant (what the agent's dialogue
// shows by itself).
const Step kSpyOnly[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	PLACEAT(10000, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(10500, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(13000, "spyonly_1"), SHOT(14500, "spyonly_2"), SHOT(17000, "spyonly_3"),
	END(19000),
};

// bump1 (level 2): single player -- Asterix runs into the AI companion Obelix.
const Step kBump1[] = {
	MARK(0, "baseline"),
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 1500, S_TRIOEV, 0x4802, 0, 0, 0, 0, "companion wait" },
	{ 2300, S_HEROFIELDS, HERO_ASTERIX, 1, 0, 0, 0, "dump asterix" },
	PLACEAT(2000, HERO_ASTERIX, -851.6f, 508.0f, -1072.2f),
	P1(2500, 0, 0, 1.0f, "p1 forward"),
	SHOT(3500, "bump1_1"),
	P1(5000, 0, 0, 0, "p1 stop"),
	END(6000),
};

// pit26o (-Level 2): sector 6's big death pit; player 2's Obelix (not leading)
// jumps in (the user's spot 92.5 2.2 -390.4 towards x 96), Asterix stays on the
// ground. Whose death does the game show?
const Step kPit26O[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 88.0f, 2.2f, -390.4f), PLACEAT(3000, HERO_OBELIX, 92.5f, 2.2f, -390.4f),
	SHOT(4000, "pit26o_0"),
	GLIDEAT(4500, HERO_OBELIX, 97.0f, 2.2f, -390.4f, 400),
	SHOT(5500, "pit26o_1"), SHOT(7000, "pit26o_2"), SHOT(9000, "pit26o_3"), SHOT(12000, "pit26o_4"),
	SHOT(16000, "pit26o_5"),
	END(18000),
};
const Step kPit26A[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 88.0f, 2.2f, -390.4f), PLACEAT(3000, HERO_ASTERIX, 92.5f, 2.2f, -390.4f),
	SHOT(4000, "pit26a_0"),
	GLIDEAT(4500, HERO_ASTERIX, 97.0f, 2.2f, -390.4f, 400),
	SHOT(5500, "pit26a_1"), SHOT(7000, "pit26a_2"), SHOT(9000, "pit26a_3"), SHOT(12000, "pit26a_4"),
	SHOT(16000, "pit26a_5"),
	END(18000),
};

// pf26 (-Level 2): the pathfinding graph at LV2-6's big pit (user's spots):
// Obelix on the near side (115.5 2.6 -389.6), Asterix carried over to the far
// side (91.6 2.0 -389.2) -- no way over for Obelix; then Asterix hits the
// lever (86.2 2.4 -396.5) and the bridge comes out -- a way over.
#define PFDUMP(t, hero, r) { t, S_PFDUMP, hero, 0, 0, r, 0, "pf dump" }
#define REACH(t)           { t, S_REACH, 0, 0, 0, 0, 0, "reach" }
// hero `hero` put where hero `from` can't walk to nor come from (`minD`..`maxD` units off)
#define APARTSPOT(t, hero, from, minD, maxD) { t, S_APARTSPOT, hero, from, 0, minD, maxD, "apart spot" }
// the squads within s.x units of hero `hero` in the log: box, active, pools
#define SQUADS(t, hero, r) { t, S_SQUADS, hero, 0, 0, r, 0, "squads" }
// every active squad put away the game's way (vf34, as when left out of view): its Romans back to their pools
#define SQUADRESET(t)      { t, S_SQUADRESET, 0, 0, 0, 0, 0, "squad reset" }
// the swing and slide doors within s.x units of hero `hero` in the log: place, how far open, steps
#define DOORS(t, hero, r)  { t, S_DOORS, hero, 0, 0, r, 0, "doors" }
// the fights behind gates won at once: every squad a gate counts put away, nothing left to send, its end sent
#define ENDFIGHT(t)        { t, S_ENDFIGHT, 0, 0, 0, 0, 0, "end fight" }
// ... the Romans within 15 units of hero `hero` left alive: the rest beaten (taken off their squads the game's
// way), nothing left to send -- the fights' ends left to the game (and the mod)
#define ENDFIGHTKEEP(t, hero) { t, S_ENDFIGHT, hero, 1, 0, 0, 0, "end fight, keep romans" }
// ... every Roman of them beaten (taken off the game's way, counted), the end (0D16) given: a fight won
#define ENDFIGHTWON(t)     { t, S_ENDFIGHT, 0, 2, 0, 0, 0, "fight won" }
// squad #i's fight won wherever it is (its members taken off as beaten, its pools at their counts, no 0D16)
#define SQUADWON(t, i)     { t, S_ENDFIGHT, i, 3, 0, 0, 0, "squad won" }
// the lever (CKHkActivator 2,52) nearest (X, Y, Z) worked as a punch on it does
#define PULLLEVERAT(t, X, Y, Z) { t, S_PULLLEVER, -1, 0, 0, X, Y, "pull lever", Z }
const Step kPf26[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 115.53f, 3.0f, -389.57f), PLACEAT(3000, HERO_ASTERIX, 91.62f, 2.5f, -389.18f),
	SHOT(4000, "pf26_0"), PFDUMP(4200, HERO_ASTERIX, 16.0f), REACH(4400),
	PLACEAT(5000, HERO_ASTERIX, 86.232f, 2.8f, -396.479f),
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6150, 0, 0, 0, "p1 release"),
	SHOT(7000, "pf26_1"), SHOT(10000, "pf26_2"),
	PLACEAT(10500, HERO_ASTERIX, 91.62f, 2.5f, -389.18f), PLACEAT(10500, HERO_OBELIX, 115.53f, 3.0f, -389.57f),
	PFDUMP(11000, HERO_ASTERIX, 16.0f), PFDUMP(11000, HERO_OBELIX, 6.0f), REACH(11200),
	// the user's gap the sidekick jumps (-49.6 45.2 -182.5 / -37.0 45.1 -182.6): a way
	PLACEAT(11500, HERO_ASTERIX, -49.562f, 45.6f, -182.527f), PLACEAT(11500, HERO_OBELIX, -36.996f, 45.4f, -182.608f),
	REACH(11530), PFDUMP(11560, HERO_ASTERIX, 10.0f),
	END(12500),
};

// reach26 (-Level 2, XXL_DOWNAT=0:5000): LV2-6, Asterix (leading) falls on the
// pit's far side, Obelix on the near side can't walk to him: Obelix is brought
// beside him. reach26o: Obelix leads when Asterix
// falls -- brought all the same (since round 195, user; nobody moved before).
// reach26b: the bridge is out first -- nobody moves.
const Step kReach26[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 115.53f, 3.0f, -389.57f), PLACEAT(3000, HERO_ASTERIX, 91.62f, 2.5f, -389.18f),
	SHOT(4500, "reach26_0"), SHOT(7000, "reach26_1"), SHOT(10000, "reach26_2"), REACH(10200),
	END(11000),
};
const Step kReach26O[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 115.53f, 3.0f, -389.57f), PLACEAT(3000, HERO_ASTERIX, 91.62f, 2.5f, -389.18f),
	{ 4000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	SHOT(4500, "reach26o_0"), SHOT(10000, "reach26o_1"), REACH(10200),
	END(11000),
};
const Step kReach26B[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, 115.53f, 3.0f, -389.57f), PLACEAT(2500, HERO_ASTERIX, 86.232f, 2.8f, -396.479f),
	P1(3000, PAD_X, 0, 0, "p1 X (lever)"), P1(3150, 0, 0, 0, "p1 release"),
	PLACEAT(4500, HERO_ASTERIX, 91.62f, 2.5f, -389.18f), PLACEAT(4500, HERO_OBELIX, 115.53f, 3.0f, -389.57f),
	SHOT(5000, "reach26b_0"), SHOT(10000, "reach26b_1"), REACH(10200),
	END(11000),
};

// reach26c (-Level 2, XXL_DOWNAT=0:6800 XXL_REVIVELIMIT=auto XXL_REVIVEGRACE=8):
// Asterix (leading) walks LV2-6's far side from the pit's edge round the
// corner north (placed spot by spot: his path's solid ground) and falls there;
// Obelix across the pit can't walk to him: he is brought to the nearest spot he
// can walk to him from (the pit's edge), not beside him (user). The limit is that
// walk plus 8 s; nobody revives him: the game over, the death sequence on
// Asterix, Obelix on his feet through it.
// reach26f: the same with a fixed limit (XXL_REVIVELIMIT=14 XXL_REVIVEGRACE=10):
// only spots within the walk the time left leaves before the 10 s (~2 s, 11
// units).
#define REACH26C_PATH \
	PLACEAT(3000, HERO_OBELIX, 115.53f, 3.0f, -389.57f), PLACEAT(3000, HERO_ASTERIX, 90.5f, 3.5f, -389.3f), \
	PLACEAT(3700, HERO_ASTERIX, 86.0f, 3.5f, -389.3f), PLACEAT(4400, HERO_ASTERIX, 84.7f, 3.5f, -385.0f), \
	PLACEAT(5100, HERO_ASTERIX, 84.7f, 3.5f, -381.0f), PLACEAT(5800, HERO_ASTERIX, 84.7f, 3.5f, -378.5f)
const Step kReach26C[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	REACH26C_PATH, SHOT(6500, "reach26c_0"),
	SHOT(9000, "reach26c_1"), SHOT(10400, "reach26c_2"), SHOT(14000, "reach26c_3"), SHOT(19500, "reach26c_4"),
	SHOT(21500, "reach26c_5"), SHOT(22500, "reach26c_6"), SHOT(23500, "reach26c_7"), SHOT(25000, "reach26c_8"),
	SHOT(27000, "reach26c_9"), SHOT(31000, "reach26c_10"),
	END(33000),
};
const Step kReach26F[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	REACH26C_PATH, SHOT(10400, "reach26f_0"), SHOT(12000, "reach26f_1"),
	END(13000),
};

// revive41 (-Level 4, XXL_DOWNAT=0:4000 XXL_REVIVELIMIT=auto XXL_REVIVEGRACE=30):
// LV4-1's open field, the heroes able to walk to each other (user): Obelix ~30
// units off, Asterix falls: the limit is Obelix's walk to him plus 30 s.
// Obelix comes to him and holds B: revived in time.
const Step kRevive41[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -288.15f, 0.8f, 117.27f), PLACEAT(2600, HERO_OBELIX, -296.0f, 0.5f, 92.0f),
	PFDUMP(3300, HERO_ASTERIX, 30.0f), REACH(3500),
	SHOT(5000, "revive41_0"), SHOT(7000, "revive41_1"), SHOT(9500, "revive41_2"),
	{ 10000, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 1.5f, 0, "obelix by asterix" },
	VPAD(10500, 1, PAD_B, 0, 0, "p2 holds B"), SHOT(12500, "revive41_3"), VPAD(16500, 1, 0, 0, 0, "p2 release"),
	SHOT(17000, "revive41_4"),
	END(18000),
};

// catrevive31 (-Level 3, XXL_DOWNAT=0:3500 XXL_DOWNAT2=0:14000
// XXL_REVIVELIMIT=auto): Asterix down by LV3-1's Roman catapult (the user's
// spot, 106.0 1.0 11.5: 2 units off, inside its 12-unit passive range), Obelix
// holds B: the usual 5 s. Then Asterix down again 17 units off, under its fire
// (a stone every 3.5 s): revived in 0.4 x 3.5 = 1.4 s (user: LV6-4's catapult
// can't be reached, its stones keep a revive from ending). Asterix's player's box
// keeps the time left through the holds. The courtyard's fight won first (its
// Romans punched Obelix off the grab -- user: beat them in autotests).
const Step kCatRevive31[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, 106.008f, 0.951f, 11.521f), PLACEAT(1500, HERO_OBELIX, 104.0f, 1.0f, 13.0f),
	ENDFIGHTWON(2500),
	SHOT(4500, "catrevive31_0"),
	{ 5000, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 1.5f, 0, "obelix by asterix" },
	VPAD(6000, 1, PAD_B, 0, 0, "p2 holds B"), SHOT(8000, "catrevive31_1"), VPAD(11500, 1, 0, 0, 0, "p2 release"),
	SHOT(12000, "catrevive31_2"), ENDFIGHTWON(12100),
	PLACEAT(12500, HERO_ASTERIX, 91.14f, -1.4f, 14.38f), PLACEAT(12500, HERO_OBELIX, 93.0f, -1.4f, 16.0f),
	SHOT(15000, "catrevive31_3"),
	{ 15500, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 1.5f, 0, "obelix by asterix" },
	VPAD(16000, 1, PAD_B, 0, 0, "p2 holds B"), SHOT(16700, "catrevive31_4"), VPAD(18500, 1, 0, 0, 0, "p2 release"),
	SHOT(19000, "catrevive31_5"),
	END(20000),
};

// liftdown21 (-Level 2, XXL_DOWNAT=0:4000): the user's LV2-1 softlock
// (xxl_coop.lv2-1_lifter_revival_tp.log): Asterix down by the hearth at the
// lifter's top, Obelix at its bottom steps onto the lifter (-746.7 505.7 -992.1):
// put back on his last solid ground below, where only the lifter (shut while a
// hero is down) leads up -- he has to be brought up to Asterix, revives him by
// B and stays up there. (Obelix's spot below is 20 units off the Roman agent at
// -745.5 508.0 -1010.2: nearer, his dialogue starts and the fall is a game over.)
const Step kLiftDown21[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -745.06f, 515.2f, -979.92f), PLACEAT(2500, HERO_OBELIX, -766.3f, 508.5f, -1001.0f),
	REACH(3200), PLACEAT(4600, HERO_OBELIX, -746.709f, 506.5f, -992.057f),
	SHOT(5000, "liftdown21_0"), SHOT(6500, "liftdown21_1"), SHOT(8500, "liftdown21_2"), REACH(8700),
	VPAD(9500, 1, PAD_B, 0, 0, "p2 holds B"), VPAD(15000, 1, 0, 0, 0, "p2 release"),
	SHOT(15500, "liftdown21_3"), REACH(15700),
	END(16500),
};

// liftdown21r (-Level 2, XXL_P2HERO=asterix XXL_DOWNAT=1:4000): liftdown21 the
// other way round (user): Obelix (not leading) down by the hearth on top, player
// 2's Asterix (leading) by him, then gone down below and onto the lifter: put
// back below, brought up to Obelix, revives him by B, stays.
const Step kLiftDown21R[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, -745.06f, 515.2f, -979.92f), PLACEAT(2500, HERO_ASTERIX, -747.0f, 515.2f, -978.0f),
	PLACEAT(6800, HERO_ASTERIX, -766.3f, 508.5f, -1001.0f), PLACEAT(7600, HERO_ASTERIX, -746.709f, 506.5f, -992.057f),
	SHOT(8000, "liftdown21r_0"), SHOT(10500, "liftdown21r_1"), REACH(10700),
	VPAD(11000, 1, PAD_B, 0, 0, "p2 holds B"), VPAD(16500, 1, 0, 0, 0, "p2 release"),
	SHOT(17000, "liftdown21r_2"), REACH(17200),
	END(18000),
};

// liftdie21a (-Level 2, XXL_DOWNAT=0:4500): Asterix down alone on LV2-1's
// lifter (at its foot, the user's -746.7 505.7 -992.1), Obelix comes onto it:
// not put back (user: the fallen hero waits on the lifter); the lifter held while
// he is on it, revived by B, then it goes up with both. Squad 29 beaten first
// (user: the lifter goes up once it is).
// liftdie21b (-Level 2, XXL_DOWNAT=0:3500): both on it as it goes up, Asterix
// down on the way: Obelix stays on it, the lifter held, revived, on up.
const Step kLiftDie21A[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), SQUADWON(1500, 29),
	PLACEAT(2500, HERO_ASTERIX, -746.709f, 506.5f, -992.057f), PLACEAT(2500, HERO_OBELIX, -766.3f, 508.5f, -1001.0f),
	SHOT(5000, "liftdie21a_0"), PLACEAT(7000, HERO_OBELIX, -746.2f, 506.5f, -990.0f),
	SHOT(7500, "liftdie21a_1"), REACH(7600),
	VPAD(8000, 1, PAD_B, 0, 0, "p2 holds B"), SHOT(10000, "liftdie21a_2"), VPAD(13500, 1, 0, 0, 0, "p2 release"),
	SHOT(14000, "liftdie21a_3"), SHOT(17000, "liftdie21a_4"),
	END(18000),
};
const Step kLiftDie21B[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), SQUADWON(1500, 29),
	PLACEAT(2500, HERO_ASTERIX, -746.709f, 506.5f, -992.057f), PLACEAT(2500, HERO_OBELIX, -746.2f, 506.5f, -990.0f),
	SHOT(3400, "liftdie21b_0"), SHOT(4500, "liftdie21b_1"), REACH(4600),
	VPAD(5500, 1, PAD_B, 0, 0, "p2 holds B"), SHOT(7500, "liftdie21b_2"), VPAD(11000, 1, 0, 0, 0, "p2 release"),
	SHOT(11500, "liftdie21b_3"), SHOT(14000, "liftdie21b_4"),
	END(15000),
};

// catrate31 (-Level 3, XXL_P2HERO=asterix XXL_CATTRACE=1): player 2's Asterix
// (leading) from LV3-1's catapult walks out into its reach and stands there,
// healed, while it fires at him: the trace has each fire (the charge back to 0)
// and each hit -- the stones' flight and their spacing.
#define HEALA3(t) { t, S_GIVE, HERO_ASTERIX, 0, 0, 15.0f, 0, "heal asterix" }
const Step kCatRate31[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, 106.008f, 0.951f, 11.521f), PFDUMP(1800, HERO_ASTERIX, 40.0f),
	VPAD(2500, 1, 0, 0, -1, "p2 walks toward his camera"), VPAD(5000, 1, 0, 0, 0, "p2 stops"),
	SHOT(5200, "catrate31_0"), HEALA3(6000), HEALA3(9000), HEALA3(12000), HEALA3(15000), HEALA3(18000),
	HEALA3(21000), HEALA3(24000), SHOT(14000, "catrate31_1"), SHOT(24500, "catrate31_2"),
	END(26000),
};

// reach26s (-Level 2, XXL_P2HERO=asterix XXL_DOWNAT=1:4000): both on LV2-6's
// far side, Obelix (not leading) down; player 2's Asterix (leading) can walk to
// him, then cuts himself off (put across the pit: a drop jumped down, user) --
// he is brought back where he can walk to Obelix, revives him by B and stays.
// (reach26o / unreach26: out of reach from the start -- brought too, then sent
// back after the revive.)
const Step kReach26S[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 90.5f, 3.5f, -389.3f), PLACEAT(2500, HERO_OBELIX, 84.7f, 3.5f, -385.0f),
	SHOT(5000, "reach26s_0"), PLACEAT(7000, HERO_ASTERIX, 115.53f, 3.0f, -389.57f), REACH(7300),
	SHOT(9200, "reach26s_1"), { 9500, S_NEXTTO, HERO_ASTERIX, HERO_OBELIX, 0, 1.5f, 0, "asterix by obelix" },
	VPAD(10000, 1, PAD_B, 0, 0, "p2 holds B"), VPAD(15500, 1, 0, 0, 0, "p2 release"),
	SHOT(16000, "reach26s_2"), REACH(16300),
	END(17000),
};

// zonejump26 (-Level 2, XXL_DOWNAT=1:3500): the user's LV2-6 softlock
// (xxl_coop.lv2-6_stuck.log): Obelix down at (21.8 2.1 -386.3), Asterix off the
// ledge at y 13 (43.0 13.0 -391.8) and down through zone 0CA8 (it needs both
// heroes): put back from the frame before -- in the air -- a step further back,
// he was left in the ledge's rock (48.1 8.2 -392.2) for good.
const Step kZoneJump26[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, 21.77f, 2.6f, -386.33f), PLACEAT(2500, HERO_ASTERIX, 60.6f, 10.9f, -390.4f),
	PLACEAT(3000, HERO_ASTERIX, 49.08f, 13.4f, -391.08f), SHOT(5500, "zonejump26_0"),
	GLIDEAT(6000, HERO_ASTERIX, 43.0f, 13.3f, -391.8f, 600), GLIDEAT(6700, HERO_ASTERIX, 36.0f, 4.0f, -390.0f, 700),
	SHOT(7700, "zonejump26_1"), SHOT(9000, "zonejump26_2"), SHOT(11000, "zonejump26_3"),
	END(12000),
};

// unreach26 (-Level 2, XXL_P2HERO=asterix XXL_DOWNAT=1:4000): the user's
// xxl_coop.lv2-6_unreachable.log -- Obelix (not leading) down below the belly
// slide's ramp (27.4 1.9 -391.6), player 2's Asterix (leading) up on it (54.9
// 10.0 -390.0), cut off before the fall: brought to Obelix, revives him by B,
// back up where he was.
const Step kUnreach26[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, 27.43f, 2.5f, -391.57f), PLACEAT(2500, HERO_ASTERIX, 54.9f, 10.4f, -390.0f),
	SQUADWON(2800, 13), SQUADWON(2800, 14), SQUADWON(2800, 15),   // (the fight round Obelix -- its Romans hit Asterix off the grab)
	REACH(3500), SHOT(5000, "unreach26_0"), SHOT(8000, "unreach26_1"),
	{ 8500, S_NEXTTO, HERO_ASTERIX, HERO_OBELIX, 0, 1.5f, 0, "asterix by obelix" },
	VPAD(9000, 1, PAD_B, 0, 0, "p2 holds B"), VPAD(14500, 1, 0, 0, 0, "p2 release"),
	SHOT(15500, "unreach26_2"),
	END(16500),
};

// zonejump26j (-Level 2, XXL_P2HERO=asterix XXL_DOWNAT=1:3500): zonejump26 the
// user's way -- player 2's Asterix jumps from (48.8 13.2 -389.4) ahead onto the
// belly slide's slope and slides down through zone 0CA8.
const Step kZoneJump26J[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, 21.77f, 2.6f, -386.33f), PLACEAT(2500, HERO_ASTERIX, 60.6f, 10.9f, -390.4f),
	PLACEAT(3000, HERO_ASTERIX, 48.836f, 13.6f, -389.419f), SHOT(5500, "zonejump26j_0"),
	VPAD(6000, 1, 0, 0, 1, "p2 ahead"), VPAD(6100, 1, PAD_A, 0, 1, "p2 ahead + jump"), VPAD(6400, 1, 0, 0, 1, "p2 ahead"),
	VPAD(7200, 1, 0, 0, 0, "p2 stops"),
	SHOT(7500, "zonejump26j_1"), SHOT(9000, "zonejump26j_2"),
	VPAD(9500, 1, 0, 0, -1, "p2 back"), VPAD(10500, 1, 0, 1, 0, "p2 right"), VPAD(11500, 1, 0, 0, 0, "p2 stops"),
	SHOT(12000, "zonejump26j_3"),
	END(12500),
};

// reach23 (-Level 2, XXL_DOWNAT=0:8000): LV2-3, Asterix lights the hearth and
// the door shuts behind him; he falls (leading): Obelix is brought in.
const Step kReach23[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -44.03f, 0.8f, -541.286f), PLACEAT(3000, HERO_OBELIX, -60.0f, 1.0f, -535.0f),
	{ 4500, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4700, S_TORCH, 6, 0, 0, 0, 0, "asterix gets a lit torch" },
	P1(5800, PAD_X, 0, 0, "p1 X"), P1(5950, 0, 0, 0, "p1 release"),
	SHOT(7500, "reach23_0"), SHOT(11000, "reach23_1"), SHOT(13500, "reach23_2"), REACH(13700),
	END(14500),
};

// reach26w (-Level 2, XXL_DOWNAT=0:5000): the user's LV2-6 run -- Obelix the
// waiting AI companion (trio 4802) from before the pit (no safe spot of his
// since the level's start), Asterix falls on the far side: the lock's undo must
// not put Obelix back at the level's start (-854 508 -1073); brought to Asterix,
// he revives him by holding B and goes back to his side of the pit.
const Step kReach26W[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2500, S_TRIOEV, 0x4802, 0, 0, 0, 0, "companion waits" },
	PLACEAT(3000, HERO_OBELIX, 117.82f, 3.0f, -389.01f), PLACEAT(3000, HERO_ASTERIX, 91.4f, 2.8f, -392.7f),
	SHOT(7000, "reach26w_0"),
	VPAD(9500, 1, PAD_B, 0, 0, "p2 hold B"), VPAD(16000, 1, 0, 0, 0, "p2 release"),
	SHOT(17000, "reach26w_1"), REACH(17200),
	END(18000),
};

// reach26p (-Level 2): the user's LV2-6 fall -- Asterix (leading) walks off the
// far edge into the pit and is laid on his last solid ground at its edge;
// Obelix, brought beside him, must land on ground, not over the pit
// (lv2-6_obelix_tps_to_death.log: he fell too).
const Step kReach26P[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 117.82f, 3.0f, -389.01f), PLACEAT(3000, HERO_ASTERIX, 93.2f, 2.6f, -391.1f),
	GLIDEAT(4500, HERO_ASTERIX, 97.0f, 2.2f, -391.1f, 300),
	SHOT(9000, "reach26p_0"), SHOT(11000, "reach26p_1"), REACH(11200),
	END(12000),
};

// warppit26 (-Level 2): Asterix stands up to LV2-6's pit edge (93.2 2.6 -391.1),
// then is over the pit falling, and player 2's Obelix (across the pit) joins
// him with R3 + A: Obelix has to land on the edge, not fall in after him.
const Step kWarpPit26[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, 117.82f, 3.0f, -389.01f),
	PLACEAT(2500, HERO_ASTERIX, 88.2f, 3.5f, -391.1f), PLACEAT(3300, HERO_ASTERIX, 90.7f, 3.5f, -391.1f),
	PLACEAT(4100, HERO_ASTERIX, 93.2f, 3.0f, -391.1f),
	SHOT(5000, "warppit26_0"),
	VPAD(5400, 1, PAD_RTHUMB, 0, 0, "p2 R3"),
	PLACEAT(5500, HERO_ASTERIX, 97.0f, 7.0f, -391.1f),
	VPAD(5600, 1, PAD_RTHUMB | PAD_A, 0, 0, "p2 R3 + A"),
	VPAD(5800, 1, 0, 0, 0, "p2 release"),
	SHOT(6200, "warppit26_1"), SHOT(9000, "warppit26_2"),
	END(11000),
};

// reach23g (-Level 2, XXL_DOWNAT=0:8000): reach23, then player 2's Obelix
// revives Asterix by holding B by him (the user's way: the revive suspends co-op
// for a frame) -- Obelix must go back out of the room.
const Step kReach23G[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -44.03f, 0.8f, -541.286f), PLACEAT(3000, HERO_OBELIX, -60.0f, 1.0f, -535.0f),
	{ 4500, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4700, S_TORCH, 6, 0, 0, 0, 0, "asterix gets a lit torch" },
	P1(5800, PAD_X, 0, 0, "p1 X"), P1(5950, 0, 0, 0, "p1 release"),
	SHOT(11800, "reach23g_0"),
	VPAD(12000, 1, PAD_B, 0, 0, "p2 hold B"), VPAD(18500, 1, 0, 0, 0, "p2 release"),
	SHOT(19500, "reach23g_1"), REACH(19700),
	END(20500),
};

// relight23 (-Level 2, XXL_DOWNAT=0:8000): reach23g, then Asterix takes fire
// at the hearth again (the user's log: Obelix would not move after that) and
// player 2 walks Obelix about. relight23n: no fall -- Asterix lights the
// hearth twice, player 2 walks.
const Step kRelight23[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -44.03f, 0.8f, -541.286f), PLACEAT(3000, HERO_OBELIX, -60.0f, 1.0f, -535.0f),
	{ 4500, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4700, S_TORCH, 6, 0, 0, 0, 0, "asterix gets a lit torch" },
	P1(5800, PAD_X, 0, 0, "p1 X"), P1(5950, 0, 0, 0, "p1 release"),
	VPAD(12000, 1, PAD_B, 0, 0, "p2 hold B"), VPAD(18500, 1, 0, 0, 0, "p2 release"),
	VPAD(19500, 1, 0, 0, 1, "p2 up"), VPAD(20500, 1, 0, 0, 0, "p2 neutral"),
	{ 20600, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 20800, S_TORCH, 6, 0, 0, 0, 0, "asterix gets a lit torch again" },
	PLACEAT(21000, HERO_ASTERIX, -44.03f, 0.8f, -541.286f),
	P1(21500, PAD_X, 0, 0, "p1 X (hearth again)"), P1(21650, 0, 0, 0, "p1 release"),
	SHOT(23500, "relight23_0"),
	VPAD(24000, 1, 0, 0, 1, "p2 up"), VPAD(26000, 1, 0, 1, 0, "p2 right"), VPAD(28000, 1, 0, 0, 0, "p2 neutral"),
	SHOT(28200, "relight23_1"),
	END(29000),
};
const Step kRelight23N[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -44.03f, 0.8f, -541.286f), PLACEAT(3000, HERO_OBELIX, -60.0f, 1.0f, -535.0f),
	{ 4500, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4700, S_TORCH, 6, 0, 0, 0, 0, "asterix gets a lit torch" },
	P1(5800, PAD_X, 0, 0, "p1 X"), P1(5950, 0, 0, 0, "p1 release"),
	VPAD(12000, 1, 0, 0, 1, "p2 up"), VPAD(13000, 1, 0, 0, 0, "p2 neutral"),
	PLACEAT(14000, HERO_ASTERIX, -44.03f, 0.8f, -541.286f),
	P1(14500, PAD_X, 0, 0, "p1 X (hearth again)"), P1(14650, 0, 0, 0, "p1 release"),
	SHOT(16500, "relight23n_0"),
	VPAD(17000, 1, 0, 0, 1, "p2 up"), VPAD(19000, 1, 0, 1, 0, "p2 right"), VPAD(21000, 1, 0, 0, 0, "p2 neutral"),
	SHOT(21200, "relight23n_1"),
	END(22000),
};

// pf23 (-Level 2): LV2-3's hearth room -- the graph around its door, then
// Asterix lights the hearth (-44.0 0.3 -541.3) with a lit torch: event #423
// shuts the door (CKHkSwingDoor) behind him.
const Step kPf23[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -44.03f, 0.8f, -541.286f), PLACEAT(3000, HERO_OBELIX, -60.0f, 1.0f, -535.0f),
	SHOT(3800, "pf23_0"), PFDUMP(4000, HERO_ASTERIX, 20.0f), REACH(4200),
	{ 4500, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4700, S_TORCH, 6, 0, 0, 0, 0, "asterix gets a lit torch" },
	P1(5800, PAD_X, 0, 0, "p1 X"), P1(5950, 0, 0, 0, "p1 release"),
	SHOT(7000, "pf23_1"), SHOT(10000, "pf23_2"),
	PFDUMP(11000, HERO_ASTERIX, 20.0f), REACH(11200),
	END(12000),
};

// shopa (level 2): Obelix leads (4801), player 1's Asterix (not leading) opens
// the merchant with X, reads the words, backs out, the goodbye; then walks. The
// lead stays Obelix's throughout; the shop's camera is in Asterix's half.
const Step kShopA[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	GLIDEAT(1500, HERO_ASTERIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 172, 0, 1.6f, 0, "asterix at the merchant" },
	SHOT(3500, "shopa_0"),
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4200, 0, 0, 0, "p1 release"),
	SHOT(6000, "shopa_1"),
	P1(7500, PAD_X, 0, 0, "p1 X (words)"), P1(7700, 0, 0, 0, "p1 release"),
	SHOT(9500, "shopa_2"),
	P1(11000, PAD_B, 0, 0, "p1 B (back)"), P1(11200, 0, 0, 0, "p1 release"),
	P1(14000, PAD_X, 0, 0, "p1 X (words)"), P1(14200, 0, 0, 0, "p1 release"),
	SHOT(17000, "shopa_3"),
	SHOT(21000, "shopa_4"),
	P1(22000, 0, 0, 1.0f, "p1 forward"), P1(23500, 0, 0, 0, "p1 stop"),
	SHOT(24000, "shopa_5"),
	END(25000),
};

// shopo (level 2): player 2's Obelix (not leading) opens the merchant with X.
const Step kShopO[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_OBELIX, -764.3f, 509.0f, -1011.0f, 1500),
	{ 3200, S_NEAROBJ, HERO_OBELIX, 172, 0, 1.6f, 0, "obelix at the merchant" },
	SHOT(3500, "shopo_0"),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4200, 1, 0, 0, 0, "p2 release"),
	SHOT(6000, "shopo_1"),
	VPAD(8000, 1, PAD_A, 0, 0, "p2 A"), VPAD(8200, 1, 0, 0, 0, "p2 release"),
	SHOT(9500, "shopo_2"),
	VPAD(10000, 1, 0, 1, 0, "p2 right"), VPAD(10300, 1, 0, 0, 0, "p2 release"),
	SHOT(11500, "shopo_3"),
	VPAD(12000, 1, 0, 1, 0, "p2 right"), VPAD(12300, 1, 0, 0, 0, "p2 release"),
	SHOT(13000, "shopo_4"),
	VPAD(14000, 1, PAD_X, 0, 0, "p2 X (back)"), VPAD(14200, 1, 0, 0, 0, "p2 release"),
	SHOT(15500, "shopo_5"),
	SHOT(17500, "shopo_6"),
	SHOT(21000, "shopo_7"),
	VPAD(22000, 1, 0, 0, 1, "p2 forward"), VPAD(23500, 1, 0, 0, 0, "p2 stop"),
	SHOT(24000, "shopo_8"),
	END(25000),
};

// combop2 / combop1: the combo cheat (Back + D-pad left on player 2's pad),
// then Twister (A X B Y) by player 2's Obelix / player 1's Asterix, then Club
// (A A X B) next to a Roman.
#define TAP_P2(t, btn, n) VPAD(t, 1, btn, 0, 0, n), VPAD(t + 90, 1, 0, 0, 0, "release")
#define TAP_P1(t, btn, n) P1(t, btn, 0, 0, n), P1(t + 90, 0, 0, 0, "release")
const Step kComboP2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(1500, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(1600, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 back+left (combo cheat)"),
	VPAD(1800, 1, 0, 0, 0, "p2 release"),
	TAP_P2(3000, PAD_A, "p2 A"), TAP_P2(3650, PAD_X, "p2 X"), TAP_P2(3760, PAD_B, "p2 B"), TAP_P2(3870, PAD_Y, "p2 Y"),
	SHOT(4000, "combo_twister"),
	{ 8000, S_ENEMY, HERO_OBELIX, 0, 0, 0, 0, "obelix next to a roman" },
	VPAD(8500, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(8600, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 back+left (bar again)"),
	VPAD(8800, 1, 0, 0, 0, "p2 release"),
	TAP_P2(9500, PAD_A, "p2 A"), TAP_P2(9620, PAD_A, "p2 A"), TAP_P2(10350, PAD_X, "p2 X"), TAP_P2(10460, PAD_B, "p2 B"),
	SHOT(10500, "combo_club"),
	END(14000),
};
// twistroman (level 2): player 2's Obelix in the Twister runs into Romans (the
// user: he stumbles on them, Asterix does not).
const Step kTwistRoman[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_ENEMY, HERO_OBELIX, 2, 0, 0, 0, "obelix 6 from a roman" },
	VPAD(2000, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(2100, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 back+left (combo cheat)"),
	VPAD(2300, 1, 0, 0, 0, "p2 release"),
	TAP_P2(3000, PAD_A, "p2 A"), TAP_P2(3650, PAD_X, "p2 X"), TAP_P2(3760, PAD_B, "p2 B"), TAP_P2(3870, PAD_Y, "p2 Y"),
	VPAD(4200, 1, 0, 0, 1.0f, "p2 forward"),
	SHOT(5000, "twistroman_1"), SHOT(6000, "twistroman_2"),
	VPAD(8000, 1, 0, 0, 0, "p2 stop"),
	END(9000),
};
// twistromana: the same with player 1's Asterix (P1 combo 4 = Twister).
const Step kTwistRomanA[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman" },
	VPAD(2000, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(2100, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 back+left (combo cheat)"),
	VPAD(2300, 1, 0, 0, 0, "p2 release"),
	{ 2500, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix combo bar full" },
	{ 3000, S_P1COMBO, 4, 0, 0, 0, 0, "twister" },
	P1(4200, 0, 0, 1.0f, "p1 forward"),
	SHOT(5000, "twistromana_1"), SHOT(6000, "twistromana_2"),
	P1(8000, 0, 0, 0, "p1 stop"),
	END(9000),
};

// twistend / twistenda (with XXL_SHAKELOG=1): player 2's Obelix / player 1's
// Asterix does the Twister to its end; its camera shake (asked for 24 s) must
// stop with it, on that player's camera only.
const Step kTwistEnd[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(2000, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(2100, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 back+left (combo cheat)"),
	VPAD(2300, 1, 0, 0, 0, "p2 release"),
	TAP_P2(3000, PAD_A, "p2 A"), TAP_P2(3650, PAD_X, "p2 X"), TAP_P2(3760, PAD_B, "p2 B"), TAP_P2(3870, PAD_Y, "p2 Y"),
	SHOT(6000, "twistend_1"),
	END(30000),
};
const Step kTwistEndA[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(2000, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(2100, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 back+left (combo cheat)"),
	VPAD(2300, 1, 0, 0, 0, "p2 release"),
	{ 2500, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix combo bar full" },
	{ 3000, S_P1COMBO, 4, 0, 0, 0, 0, "twister" },
	SHOT(6000, "twistenda_1"),
	END(30000),
};

const Step kComboP1[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(1500, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(1600, 1, PAD_BACK | PAD_DPAD_LEFT, 0, 0, "p2 back+left (combo cheat)"),
	VPAD(1800, 1, 0, 0, 0, "p2 release"),
	TAP_P1(3000, PAD_A, "p1 A"), TAP_P1(3180, PAD_X, "p1 X"), TAP_P1(3360, PAD_B, "p1 B"), TAP_P1(3540, PAD_Y, "p1 Y"),
	SHOT(4000, "combo_twister"),
	END(8000),
};

// helmets3 (-Level 3): player 2's Obelix looks at the placed helmets around
// (29.5 0.1 44.2) from 60 units away while player 1's Asterix, south of them,
// runs about (turning the game camera); beacon bonuses near them are counted.
const Step kHelmets3[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, 89.92f, 0.6f, 32.69f), PLACEAT(1500, HERO_ASTERIX, 33.03f, -2.5f, -68.74f),
	{ 2500, S_P2LOOK, 0, 0, 0, 29.48f, 0.12f, "p2 camera at the helmets", 44.15f },
	{ 2500, S_WATCHBEACONS, 0, 0, 0, 29.48f, 0.12f, "watch the helmets", 44.15f },
	SHOT(4000, "helmets_0"),
	P1(4500, 0, 1, 0, "p1 right"), SHOT(6000, "helmets_1"),
	P1(6500, 0, 0, -1, "p1 down"), SHOT(8000, "helmets_2"),
	P1(8500, 0, -1, 0, "p1 left"), SHOT(10000, "helmets_3"),
	P1(10500, 0, 0, 1, "p1 up"), SHOT(12000, "helmets_4"),
	P1(12500, 0, 0, 0, "p1 stop"),
	END(13500),
};

// helmets12 (-Level 1): the user's LV1-2 spot. Obelix found (both cutscenes
// skipped as Enter does), player 1's Asterix (leading) at 2.6 -7.4 117.2,
// player 2's Obelix at 24.4 -2.4 122.1; both turn their cameras. The placed
// helmets at -45.8 -14.4 140.0 (53 units from Asterix) came and went on player
// 1's half. XXL_KLOG=-45.8,140.0 follows their kluster.
const Step kHelmets12[] = {
	MARK(0, "baseline"), CINE_ENTER(200), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	{ 7000, S_PLACE, HERO_ASTERIX, 19, 0, 0, 0, "asterix to sector 2's zone" },
	{ 9000, S_PLACE, HERO_ASTERIX, 20, 0, 0, 0, "asterix before the rectangle" },
	GLIDE(9500, HERO_ASTERIX, 21, 1500),
	CINE_ENTER(12000),
	VPAD(17000, 1, PAD_START, 0, 0, "p2 start"), VPAD(17200, 1, 0, 0, 0, "p2 release"),
	PLACEAT(19000, HERO_ASTERIX, 2.614f, -7.371f, 117.215f), PLACEAT(19000, HERO_OBELIX, 24.378f, -2.403f, 122.115f),
	{ 19000, S_WATCHBEACONS, 0, 0, 0, -45.809f, -14.426f, "watch the helmets", 139.981f },
	SHOT(21000, "helmets12_0"),
	P1R(21500, 0.35f, 0.0f, "p1 camera right"),
	SHOT(22500, "helmets12_p1a"), SHOT(23500, "helmets12_p1b"), SHOT(24500, "helmets12_p1c"), SHOT(25500, "helmets12_p1d"),
	SHOT(26500, "helmets12_p1e"), SHOT(27500, "helmets12_p1f"), SHOT(28500, "helmets12_p1g"), SHOT(29500, "helmets12_p1h"),
	P1R(30000, 0.0f, 0.0f, "p1 camera still"),
	VPADR(30500, 1, 0.35f, 0.0f, "p2 camera right"),
	SHOT(31500, "helmets12_p2a"), SHOT(32500, "helmets12_p2b"), SHOT(33500, "helmets12_p2c"), SHOT(34500, "helmets12_p2d"),
	SHOT(35500, "helmets12_p2e"), SHOT(36500, "helmets12_p2f"), SHOT(37500, "helmets12_p2g"), SHOT(38500, "helmets12_p2h"),
	VPADR(39000, 1, 0.0f, 0.0f, "p2 camera still"),
	END(40000),
};

// helmets12d (-Level 1): helmets12's spot with the helmet pool's (handler 6)
// free bonuses held, as if in flight, so the beacons take every bonus left;
// player 1's Asterix then beats the nearest Roman: its drops must take back
// the bonuses of the beacons placed last.
const Step kHelmets12D[] = {
	MARK(0, "baseline"), CINE_ENTER(200),
	{ 7000, S_PLACE, HERO_ASTERIX, 19, 0, 0, 0, "asterix to sector 2's zone" },
	{ 9000, S_PLACE, HERO_ASTERIX, 20, 0, 0, 0, "asterix before the rectangle" },
	GLIDE(9500, HERO_ASTERIX, 21, 1500),
	CINE_ENTER(12000),
	VPAD(17000, 1, PAD_START, 0, 0, "p2 start"), VPAD(17200, 1, 0, 0, 0, "p2 release"),
	PLACEAT(19000, HERO_ASTERIX, 2.614f, -7.371f, 117.215f), PLACEAT(19000, HERO_OBELIX, 24.378f, -2.403f, 122.115f),
	{ 19000, S_WATCHBEACONS, 0, 0, 0, -45.809f, -14.426f, "watch the helmets", 139.981f },
	{ 21000, S_POOLHOLD, 6, 1, 0, 0, 0, "hold the free helmets" },
	{ 21500, S_POOLHOLD, 6, 1, 0, 0, 0, "hold the free helmets" },
	NEAR_ENEMY(22000, HERO_ASTERIX), SHOT(22300, "helmets12d_0"),
	P1(22500, PAD_X, 0, 0, "p1 X"), P1(22650, 0, 0, 0, "p1 r"), P1(23100, PAD_X, 0, 0, "p1 X"), P1(23250, 0, 0, 0, "p1 r"),
	P1(23700, PAD_X, 0, 0, "p1 X"), P1(23850, 0, 0, 0, "p1 r"), { 24000, S_POOLHOLD, 6, 2, 0, 0, 0, "helmet pool" },
	SHOT(24100, "helmets12d_1"),
	P1(24300, PAD_X, 0, 0, "p1 X"), P1(24450, 0, 0, 0, "p1 r"), P1(24900, PAD_X, 0, 0, "p1 X"), P1(25050, 0, 0, 0, "p1 r"),
	P1(25500, PAD_X, 0, 0, "p1 X"), P1(25650, 0, 0, 0, "p1 r"), { 25800, S_POOLHOLD, 6, 2, 0, 0, 0, "helmet pool" },
	SHOT(25900, "helmets12d_2"),
	NEAR_ENEMY(26500, HERO_ASTERIX),
	P1(26800, PAD_X, 0, 0, "p1 X"), P1(26950, 0, 0, 0, "p1 r"), P1(27400, PAD_X, 0, 0, "p1 X"), P1(27550, 0, 0, 0, "p1 r"),
	P1(28000, PAD_X, 0, 0, "p1 X"), P1(28150, 0, 0, 0, "p1 r"), { 28500, S_POOLHOLD, 6, 2, 0, 0, 0, "helmet pool" },
	SHOT(28600, "helmets12d_3"), SHOT(30000, "helmets12d_4"),
	{ 31000, S_POOLHOLD, 6, 0, 0, 0, 0, "give the helmets back" },
	SHOT(32000, "helmets12d_5"),
	END(33000),
};

// music5a / music5o (-Level 5): the fight at the start of level 5 (340.98
// 11.13 246.87). a: the leader Asterix walks into it; o: player 2's Obelix
// (not leading), Asterix stays back. Music events and streams are logged.
const Step kMusic5A[] = {
	MARK(0, "baseline"), { 200, S_FINDGROUPS, 0x624C2C, 0x501, 0, 1, 0, "event blocs started" },
	{ 300, S_WATCHGROUP2, 0x43, 0x64, 0, 0, 0, "music groups" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_ASTERIX, 340.98f, 11.6f, 246.87f, 4000),
	SHOT(7000, "music_1"),
	END(16000),
};
// music5l (-Level 5): Obelix leads (trio 0x4801), then player 2's Obelix walks in.
const Step kMusic5L[] = {
	MARK(0, "baseline"), { 200, S_FINDGROUPS, 0x624C2C, 0x501, 0, 1, 0, "event blocs started" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	GLIDEAT(1500, HERO_OBELIX, 340.98f, 11.6f, 246.87f, 4000),
	SHOT(7000, "music_1"),
	END(16000),
};
// camp2 (level 2): player 2's Obelix watches the Roman camp's marker 26
// (-790.6 508.6 -1078.8) from 14 units; player 1's Asterix at the start
// walks away from it. Squad resets and Romans near the marker are logged.
const Step kCamp2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, -778.0f, 509.0f, -1072.0f),
	{ 2500, S_P2LOOK, 0, 0, 0, -790.6f, 508.6f, "p2 camera at the marker", -1078.8f },
	SHOT(3000, "camp_0"),
	P1(3500, 0, -1, 0, "p1 left"), SHOT(6000, "camp_1"), P1(7500, 0, 0, 0, "p1 stop"),
	{ 8000, S_P2LOOK, 0, 0, 0, -790.6f, 508.6f, "p2 camera at the marker", -1078.8f },
	SHOT(9000, "camp_2"), SHOT(12000, "camp_3"),
	END(13000),
};
const Step kGroup38[] = {
	MARK(0, "baseline"), { 500, S_EVGROUP, 0x38, 0, 0, 0, 0, "group 038" }, { 500, S_EVGROUP, 0x76, 0, 0, 0, 0, "group 076" },
	CINE_LIST(600),
	END(1000),
};
const Step kMusic5O[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(1500, HERO_OBELIX, 340.98f, 11.6f, 246.87f, 4000),
	SHOT(7000, "music_1"),
	END(16000),
};

// twistsp: single player -- player 1's Asterix does the Twister (combo 4),
// the AI companion is watched; then X (fusion?) with them together.
const Step kTwistSP[] = {
	MARK(0, "baseline"), { 500, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat" },
	{ 600, S_WATCHRW, HERO_OBELIX, 0xE5, 0, 1, 0, "obelix body writes" },
	{ 3500, S_P1COMBO, 4, 0, 0, 0, 0, "twister" },
	SHOT(4500, "twistsp_1"),
	P1(6000, PAD_X, 0, 0, "p1 X"), P1(6100, 0, 0, 0, "p1 release"),
	SHOT(7000, "twistsp_2"),
	SHOT(7000, "twistsp_3"),
	END(14000),
};
// twistco: the same with player 2 joined (player 1 does it).
const Step kTwistCo[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat" },
	{ 1500, S_P1COMBO, 4, 0, 0, 0, 0, "twister" },
	SHOT(2500, "twistco_1"),
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4100, 0, 0, 0, "p1 release"),
	SHOT(5000, "twistco_2"),
	SHOT(7000, "twistco_3"),
	END(14000),
};

// fight4o / fight4a (-Level 4): the Romans at the start of level 4 (around
// -299.2 0.6 89.3) fought for 50 s by player 2's Obelix (Asterix stays at the
// start) / by the leader Asterix (Obelix stays). Potion on (invincible, one
// hit). Live Romans and squads near the spot are logged.
#define TAP2(t)  VPAD(t, 1, PAD_X, 0, 0, "p2 X"), VPAD(t + 120, 1, 0, 0, 0, "r")
#define TAP1(t)  P1(t, PAD_X, 0, 0, "p1 X"), P1(t + 120, 0, 0, 0, "r")
#define FACE2(t) { t, S_ENEMY, HERO_OBELIX, 3, 0, 0, 0, "obelix faces the nearest roman" }
#define TAPS2(t) FACE2(t - 50), TAP2(t), TAP2(t + 500), FACE2(t + 950), TAP2(t + 1000), TAP2(t + 1500), TAP2(t + 2000), \
                 FACE2(t + 2450), TAP2(t + 2500), TAP2(t + 3000), TAP2(t + 3500), TAP2(t + 4000), TAP2(t + 4500)
#define FACE1(t) { t, S_ENEMY, HERO_ASTERIX, 3, 0, 0, 0, "asterix faces the nearest roman" }
#define TAPS1(t) FACE1(t - 50), TAP1(t), TAP1(t + 500), FACE1(t + 950), TAP1(t + 1000), TAP1(t + 1500), TAP1(t + 2000), \
                 FACE1(t + 2450), TAP1(t + 2500), TAP1(t + 3000), TAP1(t + 3500), TAP1(t + 4000), TAP1(t + 4500)
const Step kFight4O[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 800, S_WATCHENEMIES, 0, 0, 0, -299.16f, 0.57f, "watch romans", 89.27f },
	PLACEAT(1000, HERO_OBELIX, -299.16f, 1.2f, 89.27f), POTION(1200),
	TAPS2(3000), TAPS2(8000), TAPS2(13000), TAPS2(18000), TAPS2(23000), POTION(27000),
	TAPS2(28000), TAPS2(33000), TAPS2(38000), TAPS2(43000), TAPS2(48000),
	SHOT(53000, "fight4_end"),
	END(54000),
};
// camfight (-Level 4): player 2's Obelix next to sector 1's Romans while
// Asterix stays at the start: player 2's camera pulls back for the fight,
// player 1's does not.
const Step kCamFight[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	SHOT(900, "camfight_0"),
	PLACEAT(1000, HERO_OBELIX, -299.16f, 1.2f, 89.27f),
	SHOT(2000, "camfight_1"), TAPS2(3000), SHOT(6000, "camfight_2"),
	PLACEAT(8000, HERO_OBELIX, -260.0f, 1.2f, 89.27f), SHOT(16000, "camfight_3"),
	END(16500),
};
// fight36 (-Level 3): sector 6 -- player 1's Asterix at -188.0 2.6 -229.9
// (camera looking towards -z), player 2's Obelix fights at -184.1 2.6 -230.0;
// new Romans and where they appear are logged (the user saw them pop up on
// the marker at -195.2 2.6 -219.3 in player 2's view).
const Step kFight36[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -187.99f, 3.2f, -229.88f), PLACEAT(4000, HERO_OBELIX, -184.14f, 3.2f, -230.0f),
	P1(4200, 0, 0, -1, "p1 face -z"), P1(4400, 0, 0, 0, "p1 stop"),
	{ 4500, S_WATCHENEMIES, 0, 0, 0, -186.0f, 2.6f, "watch romans", -228.0f },
	POTION(4600),
	TAPS2(5000), TAPS2(10000), TAPS2(15000), TAPS2(20000), TAPS2(25000), POTION(29000),
	TAPS2(30000), TAPS2(35000), TAPS2(40000),
	SHOT(45000, "fight36_end"),
	END(46000),
};
// fight36v (-Level 3): the same fight at the user's spot (Asterix -179.3 2.6
// -232.8, Obelix -185.5 2.6 -228.2), both fighting. With every spawn marker of
// the squad in a player's view its Romans wait for one out of view (log "wait
// for one out of view"); XXL_ROMANSINVIEW=1: they come at once on the farthest,
// in plain view (the user saw a batch pop up there in player 1's half).
const Step kFight36V[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	{ 4500, S_WATCHENEMIES, 0, 0, 0, -185.14f, 2.64f, "watch romans", -225.10f },
	POTION(4600),
	TAPS1(5000), TAPS2(5000), SHOT(9000, "fight36v_1"), TAPS1(10000), TAPS2(10000), SHOT(14000, "fight36v_2"),
	TAPS1(15000), TAPS2(15000), SHOT(19000, "fight36v_3"), TAPS1(20000), TAPS2(20000), SHOT(24000, "fight36v_4"),
	TAPS1(25000), TAPS2(25000), POTION(29000), SHOT(29500, "fight36v_5"), TAPS1(30000), TAPS2(30000),
	SHOT(34000, "fight36v_6"), TAPS1(35000), TAPS2(35000), SHOT(39000, "fight36v_7"), TAPS1(40000), TAPS2(40000),
	SHOT(44000, "fight36v_8"), TAPS1(45000), TAPS2(45000), SHOT(49000, "fight36v_9"), TAPS1(50000), TAPS2(50000),
	SHOT(55000, "fight36v_end"),
	END(56000),
};
// join36 (-Level 3; the user ran it with XXL_NOLOCK=1 XXL_NOLOCKROMANS=1): Asterix
// teleported into LV3-6's fight in single player (Obelix left at the level's
// start, sector 0), its gate closes (100); player 2 joins 1.5 s later and
// Asterix fights. The heroes are apart from the join -- the fight stays at the
// level's 100 (it was doubled by the join before the path look-up said
// "apart"). Writes to the fight squad's "sent" counts are watched (S_WATCHRW x
// = 9; XXL_GATELOG=1 logs the gate's count pool by pool at every change).
const Step kJoin36[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f),
	PLACEAT(4000, HERO_ASTERIX, -184.8f, 3.2f, -230.2f),
	{ 4500, S_WATCHENEMIES, 0, 0, 0, -185.14f, 2.64f, "watch romans", -225.10f },
	{ 4600, S_WATCHRW, HERO_ASTERIX, 0, 0, 9, 0, "fight squad 'sent' writes" },
	POTION(4700),
	VPAD(5500, 1, PAD_START, 0, 0, "p2 start"), VPAD(5700, 1, 0, 0, 0, "p2 release"),
	SHOT(6500, "join36_1"),
	TAPS1(7000), TAPS1(12000), SHOT(16500, "join36_2"), SQUADS(16800, HERO_ASTERIX, 5.0f),
	TAPS1(17000), TAPS1(22000), SQUADS(26800, HERO_ASTERIX, 5.0f), SHOT(27000, "join36_end"),
	END(27500),
};
// nolockmark36 (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1): LV3-6's fight with
// both heroes in it; the level's "wait here" for Obelix (4802, sent directly) is
// not followed in no-lock mode, so Romans come for him -- at the sending squad's
// own spawn markers only (XXL_NOLOCKROMANSANY=1: any squad's markers near him).
const Step kNoLockMark36[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600),
	{ 6000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	SHOT(9000, "nolockmark36_1"), TAPS1(10000), SHOT(15000, "nolockmark36_2"), TAPS1(16000), SHOT(21000, "nolockmark36_3"),
	TAPS1(22000), SHOT(27000, "nolockmark36_4"), TAPS1(28000), SHOT(33000, "nolockmark36_5"), TAPS1(34000),
	SHOT(39000, "nolockmark36_end"),
	END(40000),
};
// nolock34 (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0: the
// potion for Asterix only, Obelix's Romans can hurt him): lock34g with no-lock
// Romans -- Obelix left in the library behind its doors while Asterix fights in
// the courtyard (the lock's sphere sends Obelix to a marker, 4821): what comes
// for Obelix; then Asterix's part of the fight won (ENDFIGHTKEEP: the Romans by
// Obelix left) and Asterix works the lever by the library's doors (115.7 -1.1
// -321.6, the user's), the heroes together again.
const Step kNoLock34[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 124.8f, 0.0f, -324.8f), PLACEAT(3000, HERO_OBELIX, 126.8f, 0.0f, -325.76f), POTION(3200),
	{ 3500, S_LEVERS, 0, 0, 0, 0, 0, "levers" },
	GLIDEAT(4000, HERO_ASTERIX, 120.5f, -0.8f, -327.5f, 800), GLIDEAT(5500, HERO_ASTERIX, 114.0f, -0.8f, -326.0f, 800),
	GLIDEAT(7000, HERO_ASTERIX, 108.0f, -0.8f, -319.0f, 800), GLIDEAT(8000, HERO_ASTERIX, 103.32f, -0.8f, -313.57f, 800),
	SQUADS(8900, HERO_ASTERIX, 60.0f), { 9000, S_FLAGEV, 0x578, 0, 0, 0, 0, "lock34 sphere" },
	SQUADS(10500, HERO_ASTERIX, 60.0f), DOORS(10600, HERO_OBELIX, 20.0f), REACH(10700), SHOT(10800, "nolock34_1"),
	{ 11000, S_WATCHENEMIES, 0, 0, 0, 126.8f, -1.2f, "watch romans by obelix", -325.8f },
	TAPS1(12000), TAPS1(17000), SHOT(21500, "nolock34_2"), TAPS1(22000),
	ENDFIGHTKEEP(27000, HERO_OBELIX), SQUADS(28000, HERO_ASTERIX, 60.0f), DOORS(28100, HERO_OBELIX, 20.0f), REACH(28200),
	SHOT(29000, "nolock34_3"), SHOT(36000, "nolock34_4"),
	PLACEAT(42000, HERO_ASTERIX, 113.6f, -0.6f, -320.6f), PULLLEVERAT(43000, 115.734f, -1.082f, -321.566f),
	DOORS(45000, HERO_OBELIX, 20.0f), REACH(45100), SHOT(45500, "nolock34_5"),
	GLIDEAT(47000, HERO_ASTERIX, 121.0f, -0.8f, -327.0f, 1500), REACH(49000), DOORS(49100, HERO_OBELIX, 20.0f),
	SHOT(50000, "nolock34_6"), SHOT(58000, "nolock34_7"), SQUADS(58100, HERO_OBELIX, 60.0f),
	END(59000),
};
// nolock34r (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0):
// nolock34 with the courtyard fight won at 20 s (ENDFIGHTWON: its Romans beaten, its end)
// and the squad put away (asleep, as out of view) while Obelix still waits in the
// library -- the courtyard squad (woken) sends its Romans
// back for him ("revenge": its box holds the library).
const Step kNoLock34R[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 124.8f, 0.0f, -324.8f), PLACEAT(3000, HERO_OBELIX, 126.8f, 0.0f, -325.76f), POTION(3200),
	GLIDEAT(4000, HERO_ASTERIX, 120.5f, -0.8f, -327.5f, 800), GLIDEAT(5500, HERO_ASTERIX, 114.0f, -0.8f, -326.0f, 800),
	GLIDEAT(7000, HERO_ASTERIX, 108.0f, -0.8f, -319.0f, 800), GLIDEAT(8000, HERO_ASTERIX, 103.32f, -0.8f, -313.57f, 800),
	{ 9000, S_FLAGEV, 0x578, 0, 0, 0, 0, "lock34 sphere" }, SHOT(10800, "nolock34r_1"),
	{ 11000, S_WATCHENEMIES, 0, 0, 0, 126.8f, -1.2f, "watch romans by obelix", -325.8f },
	TAPS1(12000), ENDFIGHTWON(20000), SQUADRESET(20500), SHOT(21500, "nolock34r_2"), SHOT(27000, "nolock34r_3"), SHOT(33000, "nolock34r_4"),
	SHOT(40000, "nolock34r_5"), END(46000),
};
// nolock36w (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_NOLOCKROMANSANY=1
// XXL_OBELIXPOTION=0 XXL_NOLOCKPAUSE=4000): the user's LV3-6 -- the fight below the
// walls won (ENDFIGHTWON), Asterix up on the ledge (-159.6 11.8 -209.0), Obelix
// told to wait on the ground (-177.7 2.6 -216.4): which squad sends Romans for him,
// where, and whether its box (x, y, z) keeps them (the user saw them come in
// behind a wall and die over and over).
const Step kNoLock36W[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), SQUADS(6500, HERO_OBELIX, 5.0f), ENDFIGHTWON(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -177.7f, 3.1f, -216.4f),
	SQUADS(8500, HERO_OBELIX, 5.0f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -185.0f, 2.6f, "watch romans", -218.0f },
	SHOT(12000, "nolock36w_1"), SHOT(14000, "nolock36w_2"), SHOT(17000, "nolock36w_3"), SHOT(22000, "nolock36w_4"),
	SHOT(28000, "nolock36w_5"), SQUADS(28500, HERO_OBELIX, 5.0f), SHOT(36000, "nolock36w_6"),
	END(37000),
};
// nolock36x: nolock36w with Obelix waiting by the fight's west marker (-195.2 2.6
// -219.3, where the user's Romans came in).
const Step kNoLock36X[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), ENDFIGHTWON(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -186.5f, 3.1f, -217.5f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -192.0f, 2.6f, "watch romans", -219.0f },
	SHOT(11500, "nolock36x_1"), SHOT(12500, "nolock36x_2"), SHOT(14000, "nolock36x_3"), SHOT(17000, "nolock36x_4"),
	SHOT(22000, "nolock36x_5"), SQUADS(22500, HERO_OBELIX, 5.0f), SHOT(30000, "nolock36x_6"),
	END(31000),
};
// nolock36e: nolock36x, then Obelix walks east out of the sending squad's box
// (x -200..-163; the user's Obelix went to -116): its Romans wait at the box's
// edge (held back there they walked on the spot), and come on as he comes back.
const Step kNoLock36E[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), ENDFIGHTWON(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -186.5f, 3.1f, -217.5f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -175.0f, 2.6f, "watch romans", -228.0f },
	SHOT(14000, "nolock36e_1"),
	GLIDEAT(16000, HERO_OBELIX, -179.5f, 3.0f, -233.0f, 1500), GLIDEAT(17600, HERO_OBELIX, -170.0f, 3.4f, -235.0f, 1500),
	GLIDEAT(19200, HERO_OBELIX, -152.0f, 3.4f, -235.6f, 2500), SHOT(23000, "nolock36e_2"), SHOT(27000, "nolock36e_3"),
	GLIDEAT(28000, HERO_OBELIX, -170.0f, 3.4f, -235.0f, 2500), SHOT(32000, "nolock36e_4"),
	END(33000),
};
// nolock36f (nolock36w's env, XXL_NOLOCKFIRST=2000): nolock36w with the fight
// below the walls ended, not won (ENDFIGHT) -- no revenge to come: the ledge
// squad (#29, never fought, run for Asterix on the ledge) sends its spear
// Romans; Obelix walks round as the user's did: they face him as they follow
// (they walked backwards, facing as their squad's formation round Asterix).
const Step kNoLock36F[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), ENDFIGHT(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -177.7f, 3.1f, -216.4f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -185.0f, 2.6f, "watch romans", -218.0f },
	SHOT(13500, "nolock36f_1"),   // Obelix behind them as they stand round him, then round as the user's did
	GLIDEAT(14000, HERO_OBELIX, -183.8f, 3.0f, -227.7f, 2000), SHOT(14700, "nolock36f_2"), SHOT(15400, "nolock36f_3"),
	GLIDEAT(16500, HERO_OBELIX, -187.0f, 3.0f, -225.3f, 1500), SHOT(17200, "nolock36f_4"),
	GLIDEAT(18500, HERO_OBELIX, -185.7f, 3.0f, -231.6f, 1500), SHOT(19200, "nolock36f_5"),
	GLIDEAT(20500, HERO_OBELIX, -184.7f, 3.0f, -236.8f, 1500), SHOT(21200, "nolock36f_6"),
	GLIDEAT(22500, HERO_OBELIX, -186.9f, 3.0f, -218.2f, 3000), SHOT(23500, "nolock36f_7"), SHOT(24500, "nolock36f_8"),
	SQUADS(26000, HERO_OBELIX, 5.0f), END(26500),
};
// nolock36s (nolock36w's env, XXL_NOLOCKPAUSE=3000 XXL_NOLOCKFIRST=8000
// XXL_NOLOCKGROUP=3 XXL_NOLOCKTRACE=1): nolock36w with Obelix waiting by squad
// #4's marker (-177.5 2.6 -227.1), then walking east as the user's did (userlogs
// 20261031-xxl_coop.lv3-6_roman_stuck_running.log) out of #4's box (x max -132)
// down to (-123.5 -4.0 -230.8), Asterix far off (the corkscrew launched him: #4 run
// for Obelix alone), then back into the box (x -140). The user's Roman was held
// back at the box's edge (-132.0 -233.6) for 9 s, running on the spot; here they
// stand at the edge. With XXL_NOLOCKNOCLAMP=1 (their spots past the edge) all 3
// are held there, running on the spot: they stand after 1 s, come on again.
const Step kNoLock36S[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), ENDFIGHTWON(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -177.7f, 3.1f, -216.4f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -150.0f, 2.6f, "watch romans", -230.0f },
	GLIDEAT(11000, HERO_OBELIX, -176.9f, 3.0f, -234.0f, 2000), SHOT(18500, "nolock36s_1"),
	GLIDEAT(19000, HERO_OBELIX, -169.1f, 3.0f, -236.3f, 2000), GLIDEAT(21100, HERO_OBELIX, -155.5f, 3.0f, -235.0f, 2000),
	PLACEAT(22000, HERO_ASTERIX, -159.67f, 1.3f, -133.62f),   // the corkscrew took the user's Asterix out of #4's box
	GLIDEAT(23200, HERO_OBELIX, -141.3f, 0.0f, -234.4f, 2000), GLIDEAT(25300, HERO_OBELIX, -127.4f, -1.5f, -233.9f, 2000),
	SHOT(31000, "nolock36s_2"), GLIDEAT(33000, HERO_OBELIX, -125.1f, -1.5f, -233.3f, 1000),
	GLIDEAT(34200, HERO_OBELIX, -124.2f, -3.0f, -231.7f, 600), SHOT(36000, "nolock36s_3"),
	GLIDEAT(37000, HERO_OBELIX, -123.5f, -3.5f, -230.8f, 1500), SHOT(40000, "nolock36s_4"), SHOT(44000, "nolock36s_5"),
	PFDUMP(45000, HERO_OBELIX, 14.0f), GLIDEAT(46000, HERO_OBELIX, -140.0f, 0.0f, -234.2f, 2000), SHOT(49000, "nolock36s_6"),
	SQUADS(50000, HERO_OBELIX, 5.0f), END(52000),
};
// nolock36v (nolock36s's env, XXL_NOLOCKGROUP=2): the user's walk of userlogs
// 20261031-160145-xxl_coop.lv3-6_roman_stuck_running2.log -- Obelix waiting by
// #4's marker (-179.9 2.6 -229.4) as 2 Romans come, round them, then east along
// the pier out of #4's box to (-125.0 -4.1 -226.4) and (-124.7 -4.0 -231.3),
// Asterix far off. The user's first Roman (spot clamped to x -132.5, 4.5 north of
// it on the pier's wall cells) was held back at (-132.0 -1.9 -233.2), gave up
// after 1 s and still ran on the spot to the level's end. Then back into the box.
const Step kNoLock36V[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), ENDFIGHTWON(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -177.7f, 3.1f, -216.4f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -150.0f, 2.6f, "watch romans", -230.0f },
	GLIDEAT(11000, HERO_OBELIX, -179.9f, 2.6f, -229.4f, 2000), SHOT(16000, "nolock36v_1"),   // the Romans come at ~16.5 s
	GLIDEAT(16500, HERO_OBELIX, -183.0f, 2.6f, -227.4f, 900), GLIDEAT(17500, HERO_OBELIX, -184.2f, 2.6f, -228.9f, 1900),
	GLIDEAT(19500, HERO_OBELIX, -179.4f, 2.6f, -232.2f, 1900), GLIDEAT(21500, HERO_OBELIX, -169.5f, 2.6f, -236.5f, 1900),
	GLIDEAT(23500, HERO_OBELIX, -155.6f, 2.6f, -235.9f, 1900),
	PLACEAT(24000, HERO_ASTERIX, -159.67f, 1.3f, -133.62f),   // the corkscrew took the user's Asterix out of #4's box
	GLIDEAT(25500, HERO_OBELIX, -142.0f, -0.7f, -234.3f, 1900), GLIDEAT(27500, HERO_OBELIX, -129.7f, -2.9f, -230.7f, 1900),
	GLIDEAT(29500, HERO_OBELIX, -125.0f, -4.1f, -226.4f, 1900), SHOT(33000, "nolock36v_2"),
	GLIDEAT(33500, HERO_OBELIX, -124.7f, -4.0f, -231.3f, 1900), SHOT(36000, "nolock36v_3"),
	SHOT(40000, "nolock36v_4"), SHOT(45000, "nolock36v_5"),
	GLIDEAT(50000, HERO_OBELIX, -140.0f, 0.0f, -234.2f, 2000), SHOT(53000, "nolock36v_6"),
	SQUADS(54000, HERO_OBELIX, 5.0f), END(56000),
};
// nolock36y (nolock36s's env, XXL_NOLOCKGROUP=4): the user's walk of userlogs
// 20261101-012914-xxl_coop.lv3-6_roman_stuck_running_again.log, timed from the
// group's coming (93.2 s there, ~16.5 s here) -- Obelix by #4's marker, then east
// along the pier's row (z -233.7) out of #4's box to (-122.4 -1.8 -233.7), standing
// there, Asterix far off; player 2's camera turned round to the Romans (the
// user's idea). The user's second Roman was held back at (-132.0 -1.9 -233.6),
// 0.8 units off its spot, gave up 1 s after and ran on the spot to the level's
// end (14 s) -- here too (the squad's attacker slid along the crates onto #4's
// box edge) before #4's box grew over Obelix's reach. Then back into the box.
// (XXL_NOLOCKCORK=0: the user's ini locks Obelix at a corkscrew here.)
const Step kNoLock36Y[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), ENDFIGHTWON(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -177.7f, 3.1f, -216.4f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -150.0f, 2.6f, "watch romans", -230.0f },
	GLIDEAT(11000, HERO_OBELIX, -178.8f, 2.6f, -228.2f, 2000), SHOT(16000, "nolock36y_1"),   // the Romans come at ~16.5 s
	GLIDEAT(17000, HERO_OBELIX, -176.9f, 2.6f, -230.3f, 3800), GLIDEAT(20900, HERO_OBELIX, -176.3f, 2.8f, -233.5f, 1300),
	GLIDEAT(22300, HERO_OBELIX, -172.7f, 2.6f, -235.2f, 500), GLIDEAT(22900, HERO_OBELIX, -159.4f, 2.6f, -235.1f, 1900),
	GLIDEAT(24900, HERO_OBELIX, -145.3f, 0.0f, -235.0f, 1900),
	PLACEAT(26000, HERO_ASTERIX, -159.67f, 1.3f, -133.62f),   // the corkscrew took the user's Asterix out of #4's box
	GLIDEAT(26900, HERO_OBELIX, -131.5f, -1.9f, -233.7f, 1900), GLIDEAT(28900, HERO_OBELIX, -122.4f, -1.8f, -233.7f, 1900),
	VPADR(31000, 1, 1.0f, 0.0f, "p2 camera round to the Romans"), VPADR(32600, 1, 0.0f, 0.0f, "p2 camera still"),
	SHOT(33000, "nolock36y_2"), SHOT(35000, "nolock36y_3"), SHOT(38000, "nolock36y_4"), SHOT(44000, "nolock36y_5"),
	GLIDEAT(46000, HERO_OBELIX, -140.0f, 0.0f, -234.2f, 2000), SHOT(49000, "nolock36y_6"),
	SQUADS(50000, HERO_OBELIX, 5.0f), END(52000),
};
// nolock36u (nolock36s's env): nolock36s's Romans by Obelix at #4's marker, then
// Asterix far off and Obelix put at the ledge spot (-159.6 12.3 -209.0, in #4's
// box) -- he drops into the water below (-159.5 -5.5 -208.9) -- for 10 s, then
// back on the ground (-165.0 3.0 -222.0): Romans that can't get to their spots
// stand on their own (0x22, 18-26 units off them; no running on the spot).
const Step kNoLock36U[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600), ENDFIGHTWON(7000),
	PLACEAT(8000, HERO_ASTERIX, -159.6f, 12.3f, -209.0f), PLACEAT(8000, HERO_OBELIX, -177.7f, 3.1f, -216.4f),
	{ 9000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits here (the level's 4802)" },
	{ 9200, S_WATCHENEMIES, 0, 0, 0, -165.0f, 2.6f, "watch romans", -220.0f },
	GLIDEAT(11000, HERO_OBELIX, -176.9f, 3.0f, -234.0f, 2000), PLACEAT(20000, HERO_ASTERIX, -159.67f, 1.3f, -133.62f),
	SHOT(24000, "nolock36u_1"), PLACEAT(25000, HERO_OBELIX, -159.6f, 12.3f, -209.0f), SHOT(27000, "nolock36u_2"),
	SHOT(30000, "nolock36u_3"), SHOT(34000, "nolock36u_4"), PLACEAT(36000, HERO_OBELIX, -165.0f, 3.0f, -222.0f),
	SHOT(38000, "nolock36u_5"), SHOT(42000, "nolock36u_6"), END(44000),
};
// nolock34m (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0
// XXL_NOLOCKPAUSE=3000): nolock34r's wait in the library with squad #19's fight
// (by the level's start, ~380 units off) won at 3.5 s: its Romans are brought over
// to Obelix (its box moved round the library); beaten at 20 s (its own box back,
// the next group from it again); the courtyard fight (#5) won at 25 s; #19's
// beaten again at 29 s (the next group from the other won squad, the courtyard,
// whose box holds the library); those beaten at 36 s (the next from #19); Asterix
// pulls the lever by the doors at 45 s: the wait is over, #19's box back, its
// Romans still out sent off.
const Step kNoLock34M[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 124.8f, 0.0f, -324.8f), PLACEAT(3000, HERO_OBELIX, 126.8f, 0.0f, -325.76f), POTION(3200),
	SQUADWON(3500, 19),
	GLIDEAT(4000, HERO_ASTERIX, 120.5f, -0.8f, -327.5f, 800), GLIDEAT(5500, HERO_ASTERIX, 114.0f, -0.8f, -326.0f, 800),
	GLIDEAT(7000, HERO_ASTERIX, 108.0f, -0.8f, -319.0f, 800), GLIDEAT(8000, HERO_ASTERIX, 103.32f, -0.8f, -313.57f, 800),
	{ 9000, S_FLAGEV, 0x578, 0, 0, 0, 0, "lock34 sphere" },
	{ 11000, S_WATCHENEMIES, 0, 0, 0, 126.8f, -1.2f, "watch romans by obelix", -325.8f },
	SHOT(13000, "nolock34m_1"), SHOT(17000, "nolock34m_2"), SQUADWON(20000, 19), SHOT(24500, "nolock34m_3"),
	ENDFIGHTWON(25000), SQUADWON(29000, 19), SHOT(33500, "nolock34m_4"), SQUADWON(36000, 5), SHOT(41000, "nolock34m_5"),
	PLACEAT(44000, HERO_ASTERIX, 113.6f, -0.6f, -320.6f), PULLLEVERAT(45000, 115.734f, -1.082f, -321.566f),
	DOORS(47000, HERO_OBELIX, 20.0f), SHOT(47500, "nolock34m_6"), SHOT(51000, "nolock34m_7"),
	END(52000),
};
const Step kFight4A[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 800, S_WATCHENEMIES, 0, 0, 0, -299.16f, 0.57f, "watch romans", 89.27f },
	PLACEAT(1000, HERO_ASTERIX, -299.16f, 1.2f, 89.27f), POTION(1200),
	TAPS1(3000), TAPS1(8000), TAPS1(13000), TAPS1(18000), TAPS1(23000), POTION(27000),
	TAPS1(28000), TAPS1(33000), TAPS1(38000), TAPS1(43000), TAPS1(48000),
	SHOT(53000, "fight4_end"),
	END(54000),
};
// fight64 (-Level 6): Asterix fights LV6-4's start squad (squad 8, around
// 350.96 42.05 -292.06; pools 40 + 10 leaders + 950), potion on.
const Step kFight64[] = {
	MARK(0, "baseline"),
	TAP1(1500), TAP1(2500), TAP1(3500), TAP1(4500), TAP1(5500), TAP1(6500), TAP1(7500), TAP1(8500), TAP1(9500), SHOT(10500, "fight64_0"),
	{ 11000, S_WATCHENEMIES, 0, 0, 0, 350.96f, 42.05f, "watch romans", -292.06f },
	PLACEAT(11200, HERO_ASTERIX, 350.96f, 42.8f, -292.06f), POTION(11400),
	TAPS1(13000), TAPS1(18000), TAPS1(23000), TAPS1(28000), TAPS1(33000), POTION(37000),
	TAPS1(38000), TAPS1(43000), TAPS1(48000), TAPS1(53000), TAPS1(58000), POTION(62000),
	TAPS1(63000), TAPS1(68000), TAPS1(73000), TAPS1(78000), TAPS1(83000),
	SHOT(88000, "fight64_end"),
	END(89000),
};
// fight56 (-Level 5): Asterix alone fights LV5-6's squad 4 (pools 18, 40, 24 +
// 8 leaders; the fight lock at -116.16 9.84 33.90), potion on, for 4 minutes.
#define FIGHT20S(t) POTION(t), TAPS1(t + 500), TAPS1(t + 5500), TAPS1(t + 10500), TAPS1(t + 15500)
const Step kFight56[] = {
	MARK(0, "baseline"),
	LOADSECTOR(200, 6),
	{ 2500, S_WATCHENEMIES, 0, 0, 0, -116.16f, 9.84f, "watch romans", 33.90f },
	PLACEAT(2600, HERO_ASTERIX, -116.16f, 10.3f, 33.90f),
	FIGHT20S(3000), FIGHT20S(23000), FIGHT20S(43000), FIGHT20S(63000), FIGHT20S(83000), FIGHT20S(103000),
	SHOT(123000, "fight56_2m"),
	FIGHT20S(123500), FIGHT20S(143500), FIGHT20S(163500), FIGHT20S(183500), FIGHT20S(203500), FIGHT20S(223500),
	SHOT(244000, "fight56_end"),
	END(245000),
};

// gatedone (-Level 5): LV5-6's fight lock (squad 4) cleared by Asterix's
// Twister, both heroes in it -- the gate counter's end: its sparkle and its
// way out (XXL_GATESHOTS: shots then; the run ends 3 s after it has gone).
#define TWIST1(t) { t, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix combo bar full" }, { t + 300, S_P1COMBO, 4, 0, 0, 0, 0, "twister" }, \
	P1(t + 1200, 0, 0, 1.0f, "p1 forward"), P1(t + 3500, 0, 0, -1.0f, "p1 back"), P1(t + 5800, 0, 0, 0, "p1 stop")
const Step kGateDone[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(600, 6),
	{ 1000, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat" },
	{ 2500, S_WATCHENEMIES, 0, 0, 0, -116.16f, 9.84f, "watch romans", 33.90f },
	PLACEAT(2600, HERO_ASTERIX, -116.16f, 10.3f, 33.90f), PLACEAT(2600, HERO_OBELIX, -114.16f, 10.3f, 33.90f),
	SHOT(4000, "gatedone_start"),
	TWIST1(4000), TWIST1(10500), TWIST1(17000), TWIST1(23500), TWIST1(30000), TWIST1(36500), TWIST1(43000),
	TWIST1(49500), TWIST1(56000), TWIST1(62500), TWIST1(69000), TWIST1(75500), TWIST1(82000), TWIST1(88500),
	TWIST1(95000), TWIST1(101500), TWIST1(108000), TWIST1(114500), TWIST1(121000), TWIST1(127500), TWIST1(134000),
	TWIST1(140500), TWIST1(147000), TWIST1(153500), TWIST1(160000), TWIST1(166500), TWIST1(173000), TWIST1(179500),
	TWIST1(186000), TWIST1(192500), TWIST1(199000), TWIST1(205500), TWIST1(212000), TWIST1(218500), TWIST1(225000),
	TWIST1(231500), TWIST1(238000), TWIST1(244500), TWIST1(251000), TWIST1(257500), TWIST1(264000), TWIST1(270500),
	TWIST1(277000), TWIST1(283500), TWIST1(290000), TWIST1(296500),
	END(305000),
};
// marker56 (-Level 5): LV5-6's fight at the user's spot (Asterix -119.4 9.6
// 35.9, Obelix -121.8 9.6 37.1), both cameras on marker 28 (-128.5 9.9 54.7,
// behind the rock plateau); Asterix fights with the potion (no Twister, so
// Romans keep being asked for). Squad 4's markers count as seen only if no
// collision wall stands between a camera and them -- the Romans come from behind
// the rock, as in single player (before: all three "in view", nobody came).
const Step kMarker56[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(600, 6),
	{ 2500, S_WATCHENEMIES, 0, 0, 0, -120.0f, 9.6f, "watch romans", 36.5f },
	PLACEAT(2600, HERO_ASTERIX, -119.355f, 10.1f, 35.882f), PLACEAT(2600, HERO_OBELIX, -121.844f, 10.1f, 37.134f),
	POTION(3000),
	{ 3500, S_P2LOOK, 0, 0, 0, -128.537f, 9.891f, "p2 camera at marker 28", 54.691f },
	SHOT(4500, "marker56_start"),
	TAPS1(5000), { 9800, S_P2LOOK, 0, 0, 0, -128.537f, 9.891f, "p2 camera at marker 28", 54.691f }, SHOT(10000, "marker56_1"),
	TAPS1(10500), SHOT(15000, "marker56_2"), TAPS1(15500), { 19800, S_P2LOOK, 0, 0, 0, -128.537f, 9.891f, "p2 camera at marker 28", 54.691f },
	SHOT(20000, "marker56_3"), TAPS1(20500), SHOT(25000, "marker56_4"), POTION(25200), TAPS1(25500),
	SHOT(30000, "marker56_5"), TAPS1(30500), SHOT(35000, "marker56_end"),
	END(36000),
};
// gatedonew (-Level 5): the same fight, the squad's records logged; at 35 s both
// cameras turn away from the arena's three spawn markers (around -128.5 9.9 54.7).
// With every marker in view the Romans wait, and the Twister beats the rest of
// the squad's Romans without them (vf41): the squad's end comes from the mod
// (log "the squad's end (0D16)") and the gate goes. S_WATCHRW x = 9 shows who
// writes the squad's "sent" counts.
const Step kGateDoneW[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(600, 6),
	{ 1000, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat" },
	{ 2500, S_WATCHENEMIES, 0, 0, 0, -116.16f, 9.84f, "watch romans", 33.90f },
	PLACEAT(2600, HERO_ASTERIX, -116.16f, 10.3f, 33.90f), PLACEAT(2600, HERO_OBELIX, -114.16f, 10.3f, 33.90f),
	SHOT(4000, "gatedonew_start"),
	TWIST1(4000), TWIST1(10500), SQUADS(15000, HERO_ASTERIX, 20), TWIST1(17000), SQUADS(22000, HERO_ASTERIX, 20),
	TWIST1(23500), SQUADS(28000, HERO_ASTERIX, 20), SHOT(34000, "gatedonew_before"),
	{ 35000, S_P2LOOK, 0, 0, 0, -110.0f, 10.0f, "p2 camera away from the markers", 0.0f },
	P1R(35000, 1.0f, 0, "p1 camera turns"), P1R(36500, 0, 0, "p1 camera stops"),
	SHOT(37000, "gatedonew_away"), SQUADS(38000, HERO_ASTERIX, 20), SQUADS(45000, HERO_ASTERIX, 20),
	TWIST1(46000), SQUADS(55000, HERO_ASTERIX, 20), TWIST1(56000), SQUADS(65000, HERO_ASTERIX, 20),
	END(70000),
};

// sq36a / sq36b (-Level 3): LV3-6's Romans in the open by the bars. a: Asterix
// in the open (-187.2 2.6 -233.2), then player 2's Obelix behind the bars
// (-183.5 5.5 -200.1); b: the other way round. Nobody moves; who the Romans go
// for is logged (Romans within 7 units of each hero); at 16 s the hero in the
// open walks off for 1.5 s.
#define SQ36(first, second, walk, stop) 	MARK(0, "baseline"), 	LOADSECTOR(200, 6), 	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), 	{ 1500, S_WATCHENEMIES, 0, 0, 0, -187.225f, 2.583f, "watch romans", -233.172f }, 	PLACEAT(2000, first, -187.225f, 3.0f, -233.172f), 	PLACEAT(3000, second, -183.501f, 6.0f, -200.060f), 	SHOT(8000, "sq36_1"), SHOT(15000, "sq36_2"), walk, stop, SHOT(25000, "sq36_3"), 	END(26000)
const Step kSq36a[] = { SQ36(HERO_ASTERIX, HERO_OBELIX, P1(16000, 0, 0, -1.0f, "p1 walks"), P1(17500, 0, 0, 0, "p1 stops")) };
// sq36c: both heroes in the open, then player 2's Obelix walks off: half the Romans go with him.
const Step kSq36c[] = {
	MARK(0, "baseline"),
	LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_WATCHENEMIES, 0, 0, 0, -187.225f, 2.583f, "watch romans", -233.172f },
	PLACEAT(2000, HERO_ASTERIX, -187.225f, 3.0f, -233.172f),
	PLACEAT(2100, HERO_OBELIX, -178.0f, 3.0f, -222.0f),
	SHOT(8000, "sq36_1"), SHOT(15000, "sq36_2"),
	VPAD(16000, 1, 0, 0, -1.0f, "p2 walks"), VPAD(18000, 1, 0, 0, 0, "p2 stops"),
	SHOT(20000, "sq36_3"), SHOT(25000, "sq36_4"),
	END(26000),
};
// guard36 (-Level 3, XXL_DOWNAT=1:20000 XXL_OBELIXPOTION=0): sq36c with the
// potion for Asterix only -- player 2's Obelix walks off with half the Romans,
// goes down at 20 s, then Asterix goes to the fight zone's far corner (walked
// out of it the squad puts most of its Romans away): the Romans by Obelix stay
// round him (guards) instead of going over to Asterix (the squads run for the
// leader).
const Step kGuard36[] = {
	MARK(0, "baseline"),
	LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_WATCHENEMIES, 0, 0, 0, -187.225f, 2.583f, "watch romans", -233.172f },
	PLACEAT(2000, HERO_ASTERIX, -187.225f, 3.0f, -233.172f),
	PLACEAT(2100, HERO_OBELIX, -178.0f, 3.0f, -222.0f), POTION(2500),
	SHOT(8000, "guard36_1"), SHOT(15000, "guard36_2"),
	VPAD(16000, 1, 0, 0, -1.0f, "p2 walks"), VPAD(18000, 1, 0, 0, 0, "p2 stops"),
	SHOT(19500, "guard36_3"),
	GLIDEAT(22000, HERO_ASTERIX, -192.0f, 2.6f, -211.0f, 1500),   // the fight zone's far corner, 17 units from Obelix
	SHOT(26000, "guard36_4"), SHOT(32000, "guard36_5"),
	END(33000),
};
const Step kSq36b[] = { SQ36(HERO_OBELIX, HERO_ASTERIX, VPAD(16000, 1, 0, 0, -1.0f, "p2 walks"), VPAD(17500, 1, 0, 0, 0, "p2 stops")) };

// guard41 (-Level 4, XXL_DOWNAT=1:13000 XXL_OBELIXPOTION=0): LV4-1's fight lock
// (an open field), both heroes taken out where its Romans come in at 8 s; Obelix
// down there at 13 s among the Romans. Asterix (no potion, healed every 2.5 s)
// stays 12 units off till 20 s (the guards at their spots), then faces the Roman
// nearest Obelix and hits it every 0.5 s, then 4-hit combos (they fight him, a
// shield blocks, a guard breaking off, the last guard), and goes 14 units off at
// 32 s (back to their spots).
#define HEALA(t) { t, S_GIVE, HERO_ASTERIX, 0, 0, 10.0f, 0, "heal asterix" }
#define HIT(t) P1(t, PAD_X, 0, 0, "p1 X"), P1(t + 150, 0, 0, 0, "p1 release")
#define FACEHIT(t) { t, S_NEAROBJ, HERO_ASTERIX, 93, HERO_OBELIX + 1, 1.3f, 0, "face the roman nearest obelix" }, HIT(t + 50)
#define FH5(t) FACEHIT(t), FACEHIT(t + 500), FACEHIT(t + 1000), FACEHIT(t + 1500), FACEHIT(t + 2000), HEALA(t + 2200)
// a 4-hit combo at the Roman nearest Obelix (a quick one breaks a shield of 2 or 3 hits)
#define FC4(t) FACEHIT(t), HIT(t + 300), HIT(t + 550), HIT(t + 800), HEALA(t + 1100)
const Step kGuard41[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_WATCHENEMIES, 0, 0, 0, -288.15f, 0.5f, "watch romans", 117.27f },
	PLACEAT(2500, HERO_ASTERIX, -288.15f, 0.8f, 124.0f), PLACEAT(2600, HERO_OBELIX, -286.15f, 0.8f, 118.0f),
	GLIDEAT(3000, HERO_ASTERIX, -288.15f, 0.5f, 117.27f, 800),
	PLACEAT(8000, HERO_ASTERIX, -296.0f, 0.5f, 92.0f), PLACEAT(8100, HERO_OBELIX, -294.0f, 0.5f, 90.0f),   // out where the Romans come in
	HEALA(4500), HEALA(7000), HEALA(9500), HEALA(12000), PLACEAT(13300, HERO_ASTERIX, -306.0f, 0.5f, 92.0f),
	HEALA(14500), HEALA(17000), SHOT(11500, "guard41_0"), SHOT(17500, "guard41_1"),
	FH5(20000), SHOT(22400, "guard41_2"), FH5(22500), FC4(25000), FC4(26500), FC4(28000), FC4(29500), FC4(31000),
	PLACEAT(32300, HERO_ASTERIX, -308.0f, 0.5f, 98.0f), HEALA(33000), HEALA(35500), SHOT(37500, "guard41_3"),
	HEALA(38000), END(40000),
};
// guard41m (-Level 4, XXL_DOWNAT=1:13000 XXL_OBELIXPOTION=0): guard41's start
// (Obelix down at 13 s among LV4-1's lock fight Romans, Asterix 12 units off),
// then Asterix comes 3 units from Obelix (the guards go for him), walks round
// him 3.5 units off, stands 5.5 units off (they stay within 5 of Obelix), and
// goes 14 units off (back to their spots). XXL_GUARDRING=0.8 (round 170's ring:
// spots in Obelix's body) for guards that never get to their spots.
const Step kGuard41M[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_WATCHENEMIES, 0, 0, 0, -288.15f, 0.5f, "watch romans", 117.27f },
	PLACEAT(2500, HERO_ASTERIX, -288.15f, 0.8f, 124.0f), PLACEAT(2600, HERO_OBELIX, -286.15f, 0.8f, 118.0f),
	GLIDEAT(3000, HERO_ASTERIX, -288.15f, 0.5f, 117.27f, 800),
	PLACEAT(8000, HERO_ASTERIX, -296.0f, 0.5f, 92.0f), PLACEAT(8100, HERO_OBELIX, -294.0f, 0.5f, 90.0f),
	HEALA(4500), HEALA(7000), HEALA(9500), HEALA(12000), PLACEAT(13300, HERO_ASTERIX, -306.0f, 0.5f, 92.0f),
	HEALA(14500), HEALA(17000), SHOT(17500, "guard41m_0"),
	GLIDEBY(18000, HERO_ASTERIX, -3.0f, 0.0f, 1500), HEALA(19500), SHOT(21000, "guard41m_1"),
	GLIDEBY(21500, HERO_ASTERIX, 0.0f, 3.5f, 1200), HEALA(22000), GLIDEBY(23000, HERO_ASTERIX, 3.5f, 0.0f, 1200),
	GLIDEBY(24500, HERO_ASTERIX, 0.0f, -3.5f, 1200), HEALA(24600), GLIDEBY(26000, HERO_ASTERIX, -3.5f, 0.0f, 1200),
	HEALA(27000), SHOT(27500, "guard41m_2"), GLIDEBY(28000, HERO_ASTERIX, -5.5f, 0.0f, 800), HEALA(29500),
	SHOT(31000, "guard41m_3"), HEALA(32000), GLIDEBY(32500, HERO_ASTERIX, -14.0f, 0.0f, 1500), HEALA(34500),
	HEALA(37000), SHOT(37500, "guard41m_4"), END(39000),
};

// guard41a (-Level 4, XXL_DOWNAT=0:13000 XXL_OBELIXPOTION=0 XXL_GUARDTRACE=1): the
// user's LV4-1 the other way round -- Asterix down among the lock fight's Romans
// at 13 s, Obelix (no potion, healed every 2.5 s) 12 units off, then 3 off (the
// guards go for him), round Asterix 3.5 off, 7.5 off for 5 s (past the range: a
// guard after him on its own keeps its spot), 14 off (back to their spots; one
// pressed on Asterix's body walking on the spot stops within about 1 s).
#define HEALO(t) { t, S_GIVE, HERO_OBELIX, 0, 0, 10.0f, 0, "heal obelix" }
const Step kGuard41A[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_WATCHENEMIES, 0, 0, 0, -288.15f, 0.5f, "watch romans", 117.27f },
	PLACEAT(2500, HERO_ASTERIX, -288.15f, 0.8f, 124.0f), PLACEAT(2600, HERO_OBELIX, -286.15f, 0.8f, 118.0f),
	GLIDEAT(3000, HERO_ASTERIX, -288.15f, 0.5f, 117.27f, 800),
	PLACEAT(8000, HERO_ASTERIX, -296.0f, 0.5f, 92.0f), PLACEAT(8100, HERO_OBELIX, -294.0f, 0.5f, 90.0f),
	HEALO(4500), HEALO(7000), HEALO(9500), HEALO(12000), PLACEAT(13300, HERO_OBELIX, -306.0f, 0.5f, 92.0f),
	HEALO(14500), HEALO(17000), SHOT(17500, "guard41a_0"),
	GLIDEBY(18000, HERO_OBELIX, -3.0f, 0.0f, 1500), HEALO(19500), SHOT(21000, "guard41a_1"),
	GLIDEBY(21500, HERO_OBELIX, 0.0f, 3.5f, 1200), HEALO(22000), GLIDEBY(23000, HERO_OBELIX, 3.5f, 0.0f, 1200),
	GLIDEBY(24500, HERO_OBELIX, 0.0f, -3.5f, 1200), HEALO(24600), GLIDEBY(26000, HERO_OBELIX, -3.5f, 0.0f, 1200),
	HEALO(27000), SHOT(27500, "guard41a_2"), GLIDEBY(28000, HERO_OBELIX, -7.5f, 0.0f, 800), HEALO(29500),
	HEALO(32000), SHOT(32500, "guard41a_3"), GLIDEBY(33000, HERO_OBELIX, -14.0f, 0.0f, 1500), HEALO(34500),
	HEALO(37000), SHOT(37500, "guard41a_4"), HEALO(39500), SHOT(41500, "guard41a_5"), END(42000),
};

// pool36 (-Level 3): the user's "strange" LV3-6 run -- sector 6 loaded with
// Obelix left at the level's start (squad #19's box, its Romans out for him),
// Asterix taken into the fight below the walls (squad #4, sharing a pool with
// #19): its Romans come (they did not till Obelix came over).
const Step kPool36[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	SQUADS(2500, HERO_OBELIX, 5.0f),
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(4000, HERO_ASTERIX, -185.5f, 3.2f, -228.2f),
	{ 4200, S_WATCHENEMIES, 0, 0, 0, -185.5f, 3.2f, "watch romans", -228.2f },
	HEALA(6000), SQUADS(7000, HERO_ASTERIX, 5.0f), SHOT(8000, "pool36_1"), HEALA(9000), SQUADS(12000, HERO_ASTERIX, 5.0f),
	HEALA(12000), SQUADS(12100, HERO_OBELIX, 5.0f), SHOT(14000, "pool36_2"), HEALA(15000), SQUADS(18000, HERO_ASTERIX, 5.0f),
	SHOT(18500, "pool36_3"), END(19000),
};

// poolkinds (any level): every squad's pools -- the enemy kind (component class,
// flags, shield points), a Roman of each pool (its bones, its model's textures) and
// whether it may guard a downed hero (guard.cpp).
#define POOLKINDS(t) { t, S_POOLKINDS, 0, 0, 0, 0, 0, "pool kinds" }
const Step kPoolKinds[] = { MARK(0, "baseline"), POOLKINDS(2500), END(4000) };

// spots26 (-Level 2): the guards' spots round each hero at LV2-6's big pit --
// Obelix on its near edge, Asterix on the far side -- spots over the pit (the
// graph can't reach them) left out.
#define GUARDSPOTS(t, hero) { t, S_GUARDSPOTS, hero, 0, 0, 0, 0, "guard spots" }
const Step kSpots26[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, 115.53f, 3.0f, -389.57f), PLACEAT(3000, HERO_ASTERIX, 91.62f, 2.5f, -389.18f),
	SHOT(4000, "spots26_0"), GUARDSPOTS(4200, HERO_OBELIX), GUARDSPOTS(4300, HERO_ASTERIX),
	PLACEAT(5000, HERO_OBELIX, 120.0f, 3.0f, -389.57f), GUARDSPOTS(6000, HERO_OBELIX),
	END(6500),
};

// over56 (-Level 5, XXL_DOWNAT=1:6000 XXL_DOWNAT2=0:9000): both heroes go down
// at LV5-6's squad 4 fight lock: the game over and the respawn after it.
const Step kOver56[] = {
	MARK(0, "baseline"),
	LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2600, HERO_ASTERIX, -116.16f, 10.3f, 33.90f), PLACEAT(2700, HERO_OBELIX, -113.0f, 10.3f, 36.0f),
	SHOT(8000, "over56_1"), SHOT(12000, "over56_2"), SHOT(20000, "over56_3"), SHOT(30000, "over56_4"),
	SHOT(40000, "over56_5"),
	END(42000),
};

// reload2 (level 2): player 2 in, split screen with both game cameras, then
// the same level loaded again (the Inspector's "go to level 2"): no crash.
const Step kReload2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(1500, 1, 0, 0, 1.0f, "p2 walks"), VPAD(3000, 1, 0, 0, 0, "p2 stops"),
	{ 4000, S_RELOAD, 0, 0, 0, 0, 0, "reload" },
	SHOT(12000, "reload2_1"),
	VPAD(13000, 1, 0, 1.0f, 0, "p2 walks"), VPAD(15000, 1, 0, 0, 0, "p2 stops"),
	SHOT(16000, "reload2_2"),
	END(17000),
};

// lock4 (-Level 4): Asterix walks into LV4-1's fight-lock sphere (-288.1 0.1
// 117.3, r 5): the gate shows its squads' Romans to defeat (vanilla 94 + 6).
const Step kLock4[] = {
	MARK(0, "baseline"),
	PLACEAT(1000, HERO_ASTERIX, -288.15f, 0.8f, 124.0f),
	GLIDEAT(1500, HERO_ASTERIX, -288.15f, 0.5f, 117.27f, 800),
	SHOT(3000, "lock4_1"), SHOT(4500, "lock4_2"), SHOT(7000, "lock4_3"),
	END(8000),
};

// rules4d / rules4t / rules4p (-Level 4): LV4-1's fight lock (lock4: 94 + 6
// hiding Romans) with player 2 in, Asterix fights it with the potion.
// rules4d (XXL_DOWNAT=1:1000 XXL_UPAT=20000): Obelix is down as the fight
// starts (the squads woken at the level's start are put away at 2 s and wake
// again) -- the level's numbers (100) -- and revived at 20 s: doubled for the
// rest (the gate jumps), the at-once numbers and the combo bar the rules' again.
// Obelix lies away from the fight and is put back there after the revive: a
// player's view on the fight turns down the reinforcement points behind the
// other's camera (the fight stalls).
// rules4t: both heroes there from the start (200). rules4p: Obelix put where
// Asterix (in the fight lock) can't walk to (the graph), the squads put away at
// 4.5 s (they wake again: 100), Obelix brought beside him at 21 s (doubled).
// Options: rules4d with XXL_DEFEATSTAYS=1 stays at 100 after the revive, with
// XXL_STATIC=1 is 200 from the start; rules4p with XXL_APARTPATH=0 is 200 from
// the start, with XXL_FACTORS1=1 the path is never looked up.
#define RULES4A(other, mid) 	PLACEAT(2500, HERO_ASTERIX, -288.15f, 0.8f, 124.0f), other, 	GLIDEAT(3000, HERO_ASTERIX, -288.15f, 0.5f, 117.27f, 800), POTION(3900), mid, 	SHOT(5000, "rules4_1"), TAPS1(5500), TAPS1(10500), TAPS1(15500), SHOT(20200, "rules4_2")
#define RULES4B 	TAPS1(21000), TAPS1(26000), POTION(30500), TAPS1(31000), SHOT(36000, "rules4_3"), END(37000)
const Step kRules4D[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(900, HERO_OBELIX, -261.66f, 0.8f, 52.83f), SQUADRESET(2000),
	RULES4A(MARK(2600, "obelix down since 1 s"), SQUADS(4500, HERO_ASTERIX, 10.0f)),
	PLACEAT(20600, HERO_OBELIX, -261.66f, 0.8f, 52.83f), RULES4B,
};
// rules4n: rules4d without the squads put away -- LV4-1's lock squad woke at
// the level's start with the heroes together, its fight begins at the lock with
// Obelix down (100).
const Step kRules4N[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(900, HERO_OBELIX, -261.66f, 0.8f, 52.83f),
	RULES4A(MARK(2600, "obelix down since 1 s"), SQUADS(4500, HERO_ASTERIX, 10.0f)),
	PLACEAT(20600, HERO_OBELIX, -261.66f, 0.8f, 52.83f), RULES4B,
};
const Step kRules4T[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	RULES4A(PLACEAT(2600, HERO_OBELIX, -286.15f, 0.8f, 118.0f), MARK(4000, "fight")), RULES4B,
};
const Step kRules4P[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1000, HERO_ASTERIX, -288.15f, 0.8f, 117.27f), APARTSPOT(1500, HERO_OBELIX, HERO_ASTERIX, 8.0f, 80.0f),
	REACH(2400), SQUADS(3000, HERO_ASTERIX, 10.0f), REACH(3500), POTION(3900), SQUADRESET(4500),
	SHOT(5500, "rules4_1"), TAPS1(6000), TAPS1(11000), TAPS1(16000), SHOT(20700, "rules4_2"),
	PLACEAT(21000, HERO_OBELIX, -286.15f, 0.8f, 118.0f), REACH(21500),
	TAPS1(22000), TAPS1(27000), POTION(31500), TAPS1(32000), SHOT(37000, "rules4_3"), END(38000),
};

// rules4l (-Level 4): rules4t, then player 2 leaves at 15 s (coop_only: the
// fight goes back to the level's numbers, the combo bar the game's) and comes
// in again at 21 s (doubled again).
const Step kRules4L[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -288.15f, 0.8f, 124.0f), PLACEAT(2600, HERO_OBELIX, -286.15f, 0.8f, 118.0f),
	GLIDEAT(3000, HERO_ASTERIX, -288.15f, 0.5f, 117.27f, 800), POTION(3900),
	SHOT(5000, "rules4l_1"), TAPS1(5500), TAPS1(10500),
	VPAD(15200, 1, PAD_START, 0, 0, "p2 start (leave)"), VPAD(15400, 1, 0, 0, 0, "p2 release"),
	SHOT(15800, "rules4l_2"), TAPS1(16000),
	VPAD(21200, 1, PAD_START, 0, 0, "p2 start (join)"), VPAD(21400, 1, 0, 0, 0, "p2 release"),
	SHOT(22000, "rules4l_3"), TAPS1(22500), TAPS1(27500), SHOT(32500, "rules4l_4"), END(33500),
};

// lock34 (-Level 3): the user's LV3-4 spots -- Obelix behind the doors at
// 126.8 -1.2 -325.8 while Asterix walks into the fight at 103.3 -1.3 -313.6:
// the doors, the squads, the graph's verdict as it goes.
const Step kLock34[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 124.8f, 0.0f, -324.8f), PLACEAT(3000, HERO_OBELIX, 126.8f, 0.0f, -325.76f),
	DOORS(3500, HERO_ASTERIX, 80.0f), SQUADS(3600, HERO_ASTERIX, 60.0f), REACH(3700), SHOT(4000, "lock34_1"),
	GLIDEAT(5000, HERO_ASTERIX, 103.32f, -0.8f, -313.57f, 2000),
	DOORS(7500, HERO_ASTERIX, 80.0f), SQUADS(7600, HERO_ASTERIX, 60.0f), REACH(7700), SHOT(8000, "lock34_2"),
	DOORS(11000, HERO_ASTERIX, 80.0f), SQUADS(11100, HERO_ASTERIX, 60.0f), REACH(11200), SHOT(12000, "lock34_3"),
	END(13000),
};
// lock34f (-Level 3): lock34 with the graph around the library's doors (117.8
// -1.2 -328.4), Asterix walking out through them and fighting in the courtyard.
const Step kLock34F[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 124.8f, 0.0f, -324.8f), PLACEAT(3000, HERO_OBELIX, 126.8f, 0.0f, -325.76f),
	PFDUMP(3500, HERO_OBELIX, 12.0f), REACH(3700),
	GLIDEAT(4000, HERO_ASTERIX, 120.5f, -0.8f, -327.5f, 800), REACH(5000), SQUADS(5100, HERO_ASTERIX, 60.0f),
	GLIDEAT(5500, HERO_ASTERIX, 114.0f, -0.8f, -326.0f, 800), REACH(6500), SQUADS(6600, HERO_ASTERIX, 60.0f), SHOT(6700, "lock34f_1"),
	GLIDEAT(7000, HERO_ASTERIX, 108.0f, -0.8f, -319.0f, 800), REACH(8000),
	GLIDEAT(8500, HERO_ASTERIX, 103.32f, -0.8f, -313.57f, 800), REACH(9500), SQUADS(9600, HERO_ASTERIX, 60.0f),
	DOORS(9700, HERO_ASTERIX, 40.0f), SHOT(9800, "lock34f_2"), POTION(10000),
	TAPS1(10500), SQUADS(13000, HERO_ASTERIX, 60.0f), DOORS(13100, HERO_ASTERIX, 40.0f), REACH(13200),
	TAPS1(15500), SQUADS(18000, HERO_ASTERIX, 60.0f), SHOT(18100, "lock34f_3"),
	TAPS1(20500), TAPS1(25500), SQUADS(28000, HERO_ASTERIX, 60.0f), DOORS(28100, HERO_ASTERIX, 40.0f), REACH(28200),
	SHOT(28500, "lock34f_4"), END(29000),
};
// lock34g (-Level 3): LV3-4's fight lock with Obelix left in the library --
// Asterix walks out to the fight (lock34f's way) and the lock's sphere (99.9
// -4.0 -315.3, r 25; switched on by a cross rectangle at x 117.4) is set off
// as he is in it: its event list #175 (0x578: fight lock with icon, squad 0D17
// "add all pools to the lock", sidekick to a marker, a cinematic).
const Step kLock34G[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 124.8f, 0.0f, -324.8f), PLACEAT(3000, HERO_OBELIX, 126.8f, 0.0f, -325.76f), POTION(3200),
	GLIDEAT(4000, HERO_ASTERIX, 120.5f, -0.8f, -327.5f, 800), GLIDEAT(5500, HERO_ASTERIX, 114.0f, -0.8f, -326.0f, 800),
	GLIDEAT(7000, HERO_ASTERIX, 108.0f, -0.8f, -319.0f, 800), GLIDEAT(8000, HERO_ASTERIX, 103.32f, -0.8f, -313.57f, 800),
	SQUADS(8900, HERO_ASTERIX, 60.0f), { 9000, S_FLAGEV, 0x578, 0, 0, 0, 0, "lock34 sphere" },
	SQUADS(10500, HERO_ASTERIX, 60.0f), DOORS(10600, HERO_OBELIX, 20.0f), REACH(10700), SHOT(10800, "lock34g_1"),
	TAPS1(12000), SQUADS(15000, HERO_ASTERIX, 60.0f), REACH(15100), SHOT(15200, "lock34g_2"),
	TAPS1(17000), PLACEAT(21000, HERO_OBELIX, 101.6f, -0.8f, -316.2f), REACH(21500), TAPS1(22000),
	SQUADS(27000, HERO_ASTERIX, 60.0f), REACH(27200), SHOT(27500, "lock34g_3"), TAPS1(28000), SHOT(33000, "lock34g_4"),
	END(33500),
};
// lock34t (-Level 3): lock34g with Obelix beside Asterix as the sphere goes off
// (the heroes together: the fight doubled).
const Step kLock34T[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 124.8f, 0.0f, -324.8f), PLACEAT(3000, HERO_OBELIX, 126.8f, 0.0f, -325.76f), POTION(3200),
	GLIDEAT(4000, HERO_ASTERIX, 120.5f, -0.8f, -327.5f, 800), GLIDEAT(5500, HERO_ASTERIX, 114.0f, -0.8f, -326.0f, 800),
	GLIDEAT(7000, HERO_ASTERIX, 108.0f, -0.8f, -319.0f, 800), GLIDEAT(8000, HERO_ASTERIX, 103.32f, -0.8f, -313.57f, 800),
	PLACEAT(8000, HERO_OBELIX, 101.6f, -0.8f, -316.2f), REACH(8800),
	SQUADS(8900, HERO_ASTERIX, 60.0f), { 9000, S_FLAGEV, 0x578, 0, 0, 0, 0, "lock34 sphere" },
	SQUADS(12500, HERO_ASTERIX, 60.0f), SHOT(12600, "lock34t_1"), TAPS1(13000), TAPS1(18000), SHOT(23000, "lock34t_2"),
	END(23500),
};

// fight4h (-Level 4): fight4a at LV4-1's hiding Romans (squad 21: a hiding spot at -263.7 0.2 52.8).
const Step kFight4H[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 800, S_WATCHENEMIES, 0, 0, 0, -263.66f, 0.21f, "watch romans", 52.83f },
	PLACEAT(1000, HERO_ASTERIX, -263.66f, 0.9f, 52.83f), POTION(1200),
	TAPS1(3000), TAPS1(8000), TAPS1(13000), TAPS1(18000), TAPS1(23000), POTION(27000),
	TAPS1(28000), TAPS1(33000), TAPS1(38000), TAPS1(43000), TAPS1(48000),
	SHOT(53000, "fight4h_end"),
	END(54000),
};

// twistco2: like twistco, but player 2 (not leading) asks for the fusion with X.
const Step kTwistCo2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat" },
	{ 1500, S_P1COMBO, 4, 0, 0, 0, 0, "twister" },
	SHOT(2500, "twistco2_1"),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4100, 1, 0, 0, 0, "p2 release"),
	SHOT(5000, "twistco2_2"),
	SHOT(7000, "twistco2_3"),
	END(16000),
};

// boat49 (-Level 4): player 2's Obelix (not leading) walks up to sector 9's
// boat (-164.7 6.0 -642.2); he must not be sent into the boat's position.
const Step kBoat49[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 9),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -164.66f, 6.6f, -640.0f),
	PLACEAT(3200, HERO_OBELIX, -164.66f, 6.6f, -645.0f),
	GLIDEAT(4000, HERO_OBELIX, -165.41f, 6.3f, -652.0f, 2000),
	SHOT(7000, "boat_1"), SHOT(12000, "boat_2"),
	END(14000),
};
// circle45 (-Level 4): player 2's Obelix steps on sector 5's blue circle
// (-908.1 39.6 -400.6) while Asterix leads.
const Step kCircle45[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -900.1f, 40.5f, -400.6f), PLACEAT(3000, HERO_OBELIX, -903.1f, 40.5f, -400.6f),
	{ 3500, S_PLATES, 0, 0, 0, 0, 0, "plates" },
	GLIDEAT(4000, HERO_OBELIX, -908.1f, 40.2f, -400.59f, 1500),
	SHOT(7000, "circle_1"), { 8000, S_PLATES, 0, 0, 0, 0, 0, "plates" }, SHOT(12000, "circle_2"),
	END(14000),
};
// launch48 (-Level 4): player 2's Obelix (not leading) starts sector 8's
// launcher (-990.2 308.2 -981.0); Asterix next to it. Then both walk.
const Step kLaunch48[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -1006.3f, 274.0f, -989.6f), PLACEAT(3000, HERO_OBELIX, -1008.3f, 274.0f, -989.62f),
	VPAD(5000, 1, PAD_X, 0, 0, "p2 X"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	SHOT(8000, "launch48_1"), SHOT(20000, "launch48_2"), SHOT(35000, "launch48_3"),
	P1(36000, 0, 0, 1, "p1 forward"), VPAD(36000, 1, 0, 0, 1, "p2 forward"),
	P1(38000, 0, 0, 0, "p1 stop"), VPAD(38000, 1, 0, 0, 0, "p2 stop"),
	SHOT(39000, "launch48_4"),
	END(40000),
};

// launch48a: the same with the leader Asterix pressing X.
const Step kLaunch48A[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, -1006.3f, 274.0f, -989.6f), PLACEAT(3000, HERO_ASTERIX, -1008.3f, 274.0f, -989.62f),
	P1(5000, PAD_X, 0, 0, "p1 X"), P1(5200, 0, 0, 0, "p1 release"),
	SHOT(8000, "launch48_1"), SHOT(20000, "launch48_2"), SHOT(35000, "launch48_3"),
	P1(36000, 0, 0, 1, "p1 forward"), VPAD(36000, 1, 0, 0, 1, "p2 forward"),
	P1(38000, 0, 0, 0, "p1 stop"), VPAD(38000, 1, 0, 0, 0, "p2 stop"),
	SHOT(39000, "launch48_4"),
	END(40000),
};

// shine25 (level 2): sector 5 -- both heroes look at a x10 bonus
// (167.1 5.0 -380.5); then Asterix's potion. Its shine must look the same in
// both halves.
const Step kShine25[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, 166.93f, 2.8f, -371.8f), PLACEAT(3000, HERO_OBELIX, 168.51f, 2.7f, -372.3f),
	P1(3500, 0, 0, -1, "p1 face -z"), P1(3650, 0, 0, 0, "p1 stop"),
	VPAD(3500, 1, 0, 0, -1, "p2 face -z"), VPAD(3650, 1, 0, 0, 0, "p2 stop"),
	{ 4000, S_P2LOOK, 0, 0, 0, 167.1f, 5.0f, "p2 looks at the bonus", -380.5f },
	SHOT(6000, "shine25_1"), SHOT(6500, "shine25_2"),
	{ 6600, S_NODEDUMP, 0, 0, 0, 167.1f, 5.0f, "bonus nodes", -380.5f },
	POTION(7000),
	SHOT(9000, "shine25_3"),
	{ 9200, S_P2LOOK, 0, 0, 0, 166.93f, 3.3f, "p2 looks at asterix", -371.8f },
	SHOT(10500, "shine25_4"),
	END(11000),
};

// shine44 (-Level 4): sector 4 -- both heroes at -720.4 10.0 -179.6, where the
// game camera sees a x10 bonus at once; then Asterix's potion. Run with
// XXL_SPLITDBG=2 for player 1's camera in both halves.
const Step kShine44[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -720.4f, 10.5f, -179.6f), PLACEAT(2500, HERO_OBELIX, -718.4f, 10.5f, -179.6f),
	SHOT(5000, "shine44_1a"), SHOT(5300, "shine44_1b"), SHOT(5600, "shine44_1c"),
	POTION(6000),
	SHOT(8000, "shine44_2"),
	END(8500),
};

// cam32o (-Level 3): the same beach, but player 2's Obelix walks to x 140 while
// Asterix stays: player 2's copy of the game camera takes the fixed camera,
// player 1's stays on the follow camera. Then player 2 pulls the camera back.
const Step kCam32O[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 172.8f, 6.6f, -212.9f), PLACEAT(2500, HERO_OBELIX, 170.8f, 6.6f, -214.9f),
	SHOT(4000, "cam32o_1"),
	GLIDEAT(4500, HERO_OBELIX, 140.0f, 6.6f, -214.9f, 3000),
	SHOT(8000, "cam32o_2"),
	GLIDEAT(8500, HERO_OBELIX, 172.0f, 6.6f, -214.9f, 3000),
	SHOT(12000, "cam32o_3"),
	VPADR(12500, 1, 0.0f, -1.0f, "p2 pulls back"), SHOT(14000, "cam32o_4"),
	VPADR(14100, 1, 0.0f, 1.0f, "p2 pulls in"), SHOT(15600, "cam32o_5"),
	VPADR(15700, 1, 0.0f, 0.0f, "p2 stick free"),
	END(16000),
};

// cam32s (-Level 3): as cam32o, then leader switches (0x4801 Obelix leads,
// 0x4800 back): each half should keep its own camera across the switches.
const Step kCam32S[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 172.8f, 6.6f, -212.9f), PLACEAT(2500, HERO_OBELIX, 170.8f, 6.6f, -214.9f),
	GLIDEAT(3500, HERO_OBELIX, 150.0f, 6.6f, -214.9f, 2000),
	SHOT(6500, "cam32s_1"),
	{ 7000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	SHOT(7100, "cam32s_2"), SHOT(8500, "cam32s_3"),
	{ 9000, S_TRIOEV, 0x4800, 0, 0, 0, 0, "asterix leads" },
	SHOT(9100, "cam32s_4"), SHOT(10500, "cam32s_5"),
	END(11000),
};

// cable33 (-Level 3): the user's cable car spot in sector 3 -- Asterix in the
// car at 307.58 13.07 -209.15, player 2's Obelix below at the rope (y 10.34),
// holding X and walking towards -x to pull. Player 1 (Asterix, held) should get
// the follow camera, player 2 the game's.
const Step kCable33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	SHOT(4000, "cable33_1"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"), SHOT(5500, "cable33_2"),
	VPAD(6000, 1, PAD_X, -1.0f, 0, "p2 X + left"), SHOT(8000, "cable33_3"), SHOT(10000, "cable33_4"),
	VPAD(11000, 1, 0, 0, 0, "p2 release"), SHOT(12000, "cable33_5"),
	END(12500),
};

// carlead33 (-Level 3): Obelix leads (4801) when Asterix boards LV3-3's rope
// car; player 2's Obelix pulls it to the end. Player 1's camera must stay on
// Asterix (the car's own 4801 ran in his tick with his camera copy in).
const Step kCarLead33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1200, HERO_ASTERIX, 296.0f, 10.9f, -209.1f),   // walked over: a trail of safe spots
	GLIDEAT(1300, HERO_ASTERIX, 306.0f, 10.9f, -209.1f, 1000),
	PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	{ 2600, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(3200, HERO_ASTERIX, 307.582f, 13.073f, -209.154f),
	SHOT(4200, "carlead33_1"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(5500, HERO_OBELIX, 299.0f, 10.43f, -209.1f, 2000),
	VPAD(8000, 1, 0, 0, 0, "p2 release"), SHOT(8000, "carlead33_2"),
	GLIDEAT(8200, HERO_OBELIX, 286.0f, 10.43f, -209.1f, 2000),
	GLIDEAT(10500, HERO_OBELIX, 283.33f, 13.07f, -221.82f, 1500),   // through zone 0830: Asterix leads, freed
	GLIDEAT(12200, HERO_OBELIX, 282.28f, 13.14f, -250.0f, 2500),
	SHOT(15000, "carlead33_3"), SHOT(18000, "carlead33_4"),
	END(19000),
};

// carleadu33 (-Level 3): Obelix leads, Asterix boards LV3-3's rope car, player
// 1's R3 + B: the lead stays Obelix's, as before boarding.
const Step kCarLeadU33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, 300.0f, 11.0f, -209.154f), PLACEAT(2000, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	{ 2500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(3500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f),
	{ 6000, S_UNLOCKHERO, HERO_ASTERIX, 0, 0, 0, 0, "player 1's R3 + B" },
	SHOT(8000, "carleadu33"),
	END(9000),
};

// carunlock33 (-Level 3): the user's LV2 report on LV3-3's car -- Asterix boards
// (the car's code gives Obelix the lead, 4801), player 1's R3 + B: the lead must
// go back to Asterix. Then he boards again and player 2's Obelix pulls to the end.
const Step kCarUnlock33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	SHOT(4000, "carunlock33_1"),
	{ 5000, S_UNLOCKHERO, HERO_ASTERIX, 0, 0, 0, 0, "player 1's R3 + B" },
	SHOT(6500, "carunlock33_2"),
	PLACEAT(9000, HERO_ASTERIX, 307.582f, 13.073f, -209.154f),
	SHOT(10500, "carunlock33_3"),
	VPAD(11000, 1, PAD_X, 0, 0, "p2 X"),
	VPAD(12000, 1, PAD_X, -1.0f, 0, "p2 X + left"), SHOT(14000, "carunlock33_4"), SHOT(17000, "carunlock33_5"),
	VPAD(19000, 1, 0, 0, 0, "p2 release"), SHOT(20000, "carunlock33_6"),
	END(21000),
};

// cabled33 (-Level 3, XXL_DOWNAT=1:9000 XXL_UPAT=12000): cable33, Obelix falls
// with the car halfway -- it must be back at its start, then ride again.
const Step kCableD33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	SHOT(4000, "cabled33_1"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(5500, HERO_OBELIX, 287.0f, 10.43f, -209.1f, 3200), SHOT(8500, "cabled33_2"),
	VPAD(9000, 1, 0, 0, 0, "p2 release"), SHOT(10000, "cabled33_3"),
	PLACEAT(13000, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(13000, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	SHOT(14500, "cabled33_4"),
	VPAD(15000, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(16500, HERO_OBELIX, 299.0f, 10.43f, -209.1f, 2500), SHOT(19000, "cabled33_5"),
	VPAD(20000, 1, 0, 0, 0, "p2 release"),
	END(21000),
};

// liftcab33 (-Level 3, XXL_LIFTLOG=1): cabled33 with Obelix going onto the
// sector's lifter instead of falling, the car halfway -- the lifter waits for
// Asterix: the car back at its start, Asterix on solid ground, then ride again.
const Step kLiftCab33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	SHOT(4000, "liftcab33_1"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(5500, HERO_OBELIX, 287.0f, 10.43f, -209.1f, 3200), SHOT(8500, "liftcab33_2"),
	VPAD(9000, 1, 0, 0, 0, "p2 release"),
	PLACEAT(9500, HERO_OBELIX, 279.832f, 17.8f, -268.468f),
	SHOT(11000, "liftcab33_3"), SHOT(14000, "liftcab33_4"),
	PLACEAT(17000, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(17000, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	SHOT(18500, "liftcab33_5"),
	VPAD(19000, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(20500, HERO_OBELIX, 299.0f, 10.43f, -209.1f, 2500), SHOT(23000, "liftcab33_6"),
	VPAD(24000, 1, 0, 0, 0, "p2 release"),
	// the user's: after a short pull, Obelix onto the lifter -- the car stopped short of its start
	PLACEAT(24300, HERO_OBELIX, 279.832f, 17.8f, -268.468f),
	SHOT(26000, "liftcab33_7"), SHOT(30000, "liftcab33_8"),
	END(34000),
};

// liftcab33w (-Level 3, XXL_LIFTLOG=1): the user's play -- Obelix pulls the car
// partway (twice), lets go and walks off toward the lifter (the user's path,
// xxl_coop.lv3-3_lifter_car.log): the level's zone 0830 frees Asterix.
const Step kLiftCab33W[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(5500, HERO_OBELIX, 299.0f, 10.43f, -209.1f, 2000), SHOT(7600, "liftcab33w_1"),
	VPAD(8000, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(8200, HERO_OBELIX, 292.0f, 10.43f, -209.1f, 800),
	VPAD(9200, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(9600, HERO_OBELIX, 286.0f, 10.43f, -209.1f, 1500),
	VPAD(11300, 1, 0, 0, 0, "p2 release"), SHOT(11400, "liftcab33w_2"),
	GLIDEAT(11500, HERO_OBELIX, 283.33f, 13.07f, -221.82f, 1500),
	GLIDEAT(13200, HERO_OBELIX, 282.28f, 13.14f, -247.02f, 2500),
	GLIDEAT(15900, HERO_OBELIX, 280.1f, 13.0f, -264.2f, 2000),
	SHOT(18000, "liftcab33w_3"), SHOT(22000, "liftcab33w_4"),
	END(24000),
};

// cross33 (-Level 3, XXL_LIFTLOG=1): liftcab33w without the lifter -- Obelix
// stops past the cross detector that switches the leader (zone 0830); the car,
// stopped short of its start, is sent back, Asterix not taken again where he
// stands; then both come back and ride.
const Step kCross33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1200, HERO_ASTERIX, 296.0f, 10.9f, -209.1f),   // walked over: a trail of safe spots
	GLIDEAT(1300, HERO_ASTERIX, 306.0f, 10.9f, -209.1f, 1000),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(5500, HERO_OBELIX, 299.0f, 10.43f, -209.1f, 2000),
	VPAD(8000, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(8200, HERO_OBELIX, 286.0f, 10.43f, -209.1f, 2000),
	GLIDEAT(10500, HERO_OBELIX, 283.33f, 13.07f, -221.82f, 1500),
	GLIDEAT(12200, HERO_OBELIX, 282.28f, 13.14f, -250.0f, 2500),
	SHOT(16000, "cross33_1"), SHOT(20000, "cross33_2"),
	PLACEAT(22000, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	GLIDEAT(22000, HERO_ASTERIX, 307.582f, 13.073f, -209.154f, 1500),
	SHOT(25000, "cross33_3"),
	VPAD(25500, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(26500, HERO_OBELIX, 295.0f, 10.43f, -209.1f, 2000), SHOT(28600, "cross33_4"),
	VPAD(29000, 1, 0, 0, 0, "p2 release"),
	END(32000),
};

// cablefull33 (-Level 3, XXL_LIFTLOG=1): the car pulled all the way across
// (what a finished ride leaves).
const Step kCableFull33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"),
	GLIDEAT(5500, HERO_OBELIX, 287.0f, 10.43f, -209.1f, 3200),
	GLIDEAT(8800, HERO_OBELIX, 270.0f, 10.43f, -209.1f, 3000), SHOT(12000, "cablefull33_1"),
	VPAD(13000, 1, 0, 0, 0, "p2 release"),
	SHOT(16000, "cablefull33_2"),
	END(20000),
};

// p2dir (level 2): player 2 turns their camera (right stick) and then pushes
// the left stick up: Obelix must walk away from player 2's camera.
const Step kP2Dir[] = {
	MARK(0, "baseline"), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	SHOT(2000, "p2dir_0"),
	VPADR(2500, 1, 1.0f, 0.0f, "p2 turns camera"), VPADR(3700, 1, 0.0f, 0.0f, "p2 camera still"),
	SHOT(4000, "p2dir_1"),
	VPAD(4200, 1, 0, 0.0f, 1.0f, "p2 forward"), SHOT(5200, "p2dir_2"), VPAD(5700, 1, 0, 0, 0, "p2 stop"),
	VPAD(6000, 1, 0, 1.0f, 0.0f, "p2 right"), SHOT(7000, "p2dir_3"), VPAD(7500, 1, 0, 0, 0, "p2 stop"),
	END(8000),
};

// camjump (level 2): player 1 jumps, then player 2 jumps: neither camera
// should rise with its hero (copycam / gamecam heights in the log).
const Step kCamJump[] = {
	MARK(0, "baseline"), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	P1(3000, PAD_A, 0, 0, "p1 jump"), P1(3300, 0, 0, 0, "p1 release"),
	VPAD(5000, 1, PAD_A, 0, 0, "p2 jump"), VPAD(5300, 1, 0, 0, 0, "p2 release"),
	END(7000),
};

// slide22 (level 2): the user's LV2-2 run -- dialogues at the start, the belly
// slide, the Roman agent's dialogue at its end; then the right stick (player 2,
// leading) must still turn the camera.
const Step kSlide22[] = {
	MARK(0, "baseline"), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, -905.7f, 527.9f, -938.4f), PLACEAT(2000, HERO_OBELIX, -903.7f, 527.9f, -938.4f),
	GLIDEAT(3000, HERO_ASTERIX, -911.0f, 528.2f, -913.4f, 2500),
	CINE_SKIP(6000), CINE_SKIP(8000), CINE_SKIP(10000), CINE_SKIP(12000), CINE_SKIP(14000), CINE_SKIP(16000), CINE_SKIP(18000), CINE_SKIP(20000), CINE_SKIP(22000), CINE_SKIP(24000), CINE_SKIP(26000), CINE_SKIP(28000), CINE_SKIP(80000), CINE_SKIP(82000), CINE_SKIP(84000), CINE_SKIP(86000), CINE_SKIP(88000), CINE_SKIP(90000), CINE_SKIP(92000), CINE_SKIP(94000), CINE_SKIP(96000), CINE_SKIP(98000), CINE_SKIP(100000), CINE_SKIP(102000), CINE_SKIP(104000), CINE_SKIP(106000), CINE_SKIP(108000), CINE_SKIP(110000), 
	GLIDEAT(31000, HERO_ASTERIX, -907.3f, 526.8f, -903.6f, 1500),
	{ 112000, S_CAMDUMP, 0, 0, 0, 0, 0, "camera dump" }, SHOT(112100, "slide22_1"),
	VPADR(112500, 1, 1.0f, 0.0f, "p2 turns camera"), SHOT(113500, "slide22_2"), VPADR(114000, 1, 0.0f, 0.0f, "p2 still"),
	P1(114500, 0, 0, 0, "p1 idle"), { 115000, S_CAMDUMP, 0, 0, 0, 0, 0, "camera dump" },
	END(116000),
};

// slide2h (level 2): slide22s, Obelix (player 2) on the slide gets a shield:
// the one HUD shows his health then.
const Step kSlide2H[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, -905.7f, 527.9f, -938.4f), PLACEAT(2000, HERO_OBELIX, -903.7f, 527.9f, -938.4f),
	GLIDEAT(3000, HERO_ASTERIX, -911.0f, 528.2f, -913.4f, 2500),
	CINE_SKIP(6000), CINE_SKIP(8000), CINE_SKIP(10000), CINE_SKIP(12000), CINE_SKIP(14000), CINE_SKIP(16000), CINE_SKIP(18000), CINE_SKIP(20000), CINE_SKIP(22000), CINE_SKIP(24000), CINE_SKIP(26000), CINE_SKIP(28000),
	GLIDEAT(31000, HERO_ASTERIX, -907.3f, 526.8f, -903.6f, 1500),
	SHOT(36000, "slide2h_1"),
	{ 37000, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "obelix a shield" },
	SHOT(37500, "slide2h_2"), SHOT(38500, "slide2h_3"),
	{ 39000, S_GIVE, HERO_OBELIX, 0, 0, -5.0f, 0, "obelix hurt" }, SHOT(39500, "slide2h_4"),
	VPAD(40500, 1, PAD_START, 0, 0, "p2 start (ignored)"), VPAD(40700, 1, 0, 0, 0, "p2 release"),
	SHOT(43000, "slide2h_5"),
	END(44000),
};

// slide22s (level 2): the LV2-2 belly slide in co-op: player 2 (Obelix) steers
// it and turns the camera, player 1 (Asterix) jumps.
const Step kSlide22S[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, -905.7f, 527.9f, -938.4f), PLACEAT(2000, HERO_OBELIX, -903.7f, 527.9f, -938.4f),
	GLIDEAT(3000, HERO_ASTERIX, -911.0f, 528.2f, -913.4f, 2500),
	CINE_SKIP(6000), CINE_SKIP(8000), CINE_SKIP(10000), CINE_SKIP(12000), CINE_SKIP(14000), CINE_SKIP(16000), CINE_SKIP(18000), CINE_SKIP(20000), CINE_SKIP(22000), CINE_SKIP(24000), CINE_SKIP(26000), CINE_SKIP(28000),
	GLIDEAT(31000, HERO_ASTERIX, -907.3f, 526.8f, -903.6f, 1500),
	SHOT(36000, "slide22s_1"),
	VPAD(38000, 1, 0, -1.0f, 0.0f, "p2 steers left"), SHOT(40000, "slide22s_2"), VPAD(41000, 1, 0, 0, 0, "p2 release"),
	VPADR(41500, 1, 1.0f, 0.0f, "p2 turns camera"), SHOT(42500, "slide22s_3"), VPADR(43000, 1, 0.0f, 0.0f, "p2 still"),
	P1(44000, PAD_A, 0, 0, "p1 jump"), SHOT(44300, "slide22s_4"), P1(44400, 0, 0, 0, "p1 release"),
	END(47000),
};

// cam32 (-Level 3): sector 2's beach -- both heroes at 170.8 6.1 -214.9, then
// Asterix walks (glides) to x 140 and back: which camera the game uses where.
const Step kCam32[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 170.8f, 6.6f, -214.9f), PLACEAT(2500, HERO_OBELIX, 172.8f, 6.6f, -214.9f),
	{ 4000, S_CAMDUMP, 0, 0, 0, 0, 0, "camera dump" }, SHOT(4100, "cam32_1"),
	GLIDEAT(4500, HERO_ASTERIX, 140.0f, 6.6f, -214.9f, 3000),
	{ 8000, S_CAMDUMP, 0, 0, 0, 0, 0, "camera dump" }, SHOT(8100, "cam32_2"),
	GLIDEAT(8500, HERO_ASTERIX, 172.0f, 6.6f, -214.9f, 3000),
	{ 12000, S_CAMDUMP, 0, 0, 0, 0, 0, "camera dump" }, SHOT(12100, "cam32_3"),
	END(12500),
};

// lifterwait (level 2): player 2's Obelix waits by sector 1's lifter
// (-746.8 495.9 -991.7) while Asterix stays at the start; teleports logged.
const Step kLifterWait[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 800, S_WATCHRW, HERO_OBELIX, 0, 0, 2, 0, "obelix teleports" },
	PLACEAT(3500, HERO_OBELIX, -746.8f, 499.5f, -991.7f),
	VPAD(12000, 1, 0, 1, 0, "p2 walk off"), VPAD(13500, 1, 0, 0, 0, "p2 stop"),
	SHOT(5000, "lifterwait_1"), SHOT(20000, "lifterwait_2"), SHOT(38000, "lifterwait_3"),
	END(40000),
};

// detect58 (-Level 5): sector 8's zones (the lifter trigger near -378.7 17.6 -25.2).
// lift58o / lift58a (-Level 5): sector 8 -- the hero crosses the lifter
// trigger (-378.7 17.6 -25.2) along z, then x; o: player 2's Obelix (Asterix
// stays at the sector's entrance), a: the leader Asterix.
const Step kLift58O[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -324.78f, 10.3f, 9.6f),
	PLACEAT(3000, HERO_OBELIX, -378.65f, 18.3f, -40.0f),
	GLIDEAT(4000, HERO_OBELIX, -378.65f, 18.3f, -10.0f, 3000),
	GLIDEAT(8000, HERO_OBELIX, -395.0f, 18.3f, -25.24f, 1500),
	GLIDEAT(10000, HERO_OBELIX, -360.0f, 18.3f, -25.24f, 3000),
	SHOT(14000, "lift58_1"), SHOT(20000, "lift58_2"),
	END(22000),
};
const Step kLift58A[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, -324.78f, 10.3f, 9.6f),
	PLACEAT(3000, HERO_ASTERIX, -378.65f, 18.3f, -40.0f),
	GLIDEAT(4000, HERO_ASTERIX, -378.65f, 18.3f, -10.0f, 3000),
	GLIDEAT(8000, HERO_ASTERIX, -395.0f, 18.3f, -25.24f, 1500),
	GLIDEAT(10000, HERO_ASTERIX, -360.0f, 18.3f, -25.24f, 3000),
	SHOT(14000, "lift58_1"), SHOT(20000, "lift58_2"),
	END(22000),
};
// liftd58 (-Level 5, with XXL_DOWNAT=1:2000): Asterix walks onto sector 8's
// lifter (-375.46 0 -39.19, the user's spot) with Obelix down.
const Step kLiftD58[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_OBELIX, -324.78f, 10.3f, 9.6f),
	PLACEAT(3000, HERO_ASTERIX, -375.46f, 1.0f, -32.0f),
	GLIDEAT(4500, HERO_ASTERIX, -375.46f, 0.5f, -39.19f, 1500),
	SHOT(7000, "liftd58_1"), SHOT(10000, "liftd58_2"),
	END(12000),
};
// lead510 (-Level 5, with XXL_DOWNAT=1:2000): LV5-10, Obelix down on the ground
// by the keg; Asterix walks from the keg's yard (530 -196.6) back towards the keg
// (520 -196.6) through the cross rectangle at 525.7 -451.4 -197.2 (#97: trio 4800
// + 4803), twice: let through, no put-back.
const Step kLead510[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, 506.68f, -451.5f, -224.09f),
	PLACEAT(1500, HERO_ASTERIX, 530.0f, -450.5f, -196.6f),
	GLIDEAT(3500, HERO_ASTERIX, 519.5f, -453.5f, -196.6f, 2000),
	SHOT(6000, "lead510_1"),
	GLIDEAT(6500, HERO_ASTERIX, 530.0f, -450.5f, -196.6f, 2000),
	GLIDEAT(9000, HERO_ASTERIX, 519.5f, -453.5f, -196.6f, 2000),
	SHOT(11500, "lead510_2"),
	END(12500),
};
// lead510o (-Level 5, XXL_DOWNAT=1:2000 XXL_UPAT=12000): lead510 with Obelix
// leading when he falls: he still counts as the fallen leader (brought-to-leader
// checks), and after the revive Asterix keeps the lead the rectangle gave him.
const Step kLead510O[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 10),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(1500, HERO_OBELIX, 506.68f, -451.5f, -224.09f),
	PLACEAT(1500, HERO_ASTERIX, 530.0f, -450.5f, -196.6f),
	GLIDEAT(3500, HERO_ASTERIX, 519.5f, -453.5f, -196.6f, 2000),
	SHOT(6000, "lead510o_1"),
	SHOT(14000, "lead510o_2"),
	END(15000),
};
// lift58u (-Level 5): the user's repro -- both at the sector's entrance
// (-376.8 16.5 20.0), potion; player 2's Obelix (not leading) walks to
// z = -30.7 (the lifters' Romans then hang in the air); later Asterix walks to
// z = -19.1 (they land on the lifters at that moment).
const Step kLift58U[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -376.82f, 17.2f, 20.05f), PLACEAT(3000, HERO_OBELIX, -374.8f, 17.2f, 20.05f),
	POTION(3500),
	{ 3600, S_WATCHENEMIES, 0, 0, 0, -376.0f, 5.0f, "watch romans", -45.0f },
	GLIDEAT(4000, HERO_OBELIX, -376.82f, 17.5f, -30.68f, 4000),
	SHOT(9000, "lift58u_1"), SHOT(14000, "lift58u_2"),
	GLIDEAT(15000, HERO_ASTERIX, -376.82f, 17.5f, -19.07f, 3000),
	SHOT(19000, "lift58u_3"), SHOT(23000, "lift58u_4"),
	END(24000),
};
// lift58t (-Level 5): player 2's Obelix (not leading) on a sector 8 lifter
// (-375.5 0 -39.2) while Asterix stays at the entrance: the lifter's "sidekick
// not present" event must not keep teleporting him.
// lift58l: like lift58u, but Obelix is made the leader instead of Asterix walking.
const Step kLift58L[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -376.82f, 17.2f, 20.05f), PLACEAT(3000, HERO_OBELIX, -374.8f, 17.2f, 20.05f),
	POTION(3500),
	{ 3600, S_WATCHENEMIES, 0, 0, 0, -376.0f, 5.0f, "watch romans", -45.0f },
	GLIDEAT(4000, HERO_OBELIX, -376.82f, 17.5f, -30.68f, 4000),
	SHOT(9000, "lift58l_1"),
	{ 13000, S_WATCHRW, HERO_OBELIX, 0, 0, 3, 0, "roman height writes" },
	{ 15000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	SHOT(19000, "lift58l_2"),
	END(22000),
};
const Step kLift58T[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -376.82f, 17.2f, 20.05f), PLACEAT(3000, HERO_OBELIX, -375.46f, 0.8f, -39.19f),
	{ 3500, S_WATCHRW, HERO_OBELIX, 0, 0, 2, 0, "obelix teleports" },
	SHOT(6000, "lift58t_1"), SHOT(20000, "lift58t_2"),
	END(24000),
};
const Step kDetect58[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	{ 3000, S_DETECT, 0, 0, 0, 0, 0, "detect" },
	END(3500),
};

const Step kInputDump[] = {
	MARK(0, "baseline"), { 500, S_INPUTDUMP, 0, 0, 0, 0, 0, "input" },
	END(1000),
};

const Step kRegions[] = {
	MARK(0, "baseline"), { 2000, S_REGIONS, 0, 0, 0, 0, 0, "regions" },
	{ 2000, S_EVGROUP, 0x10D, 0, 0, 0, 0, "group 10D" }, { 2000, S_EVGROUP, 0x14D, 0, 0, 0, 0, "group 14D" },
	{ 2000, S_EVGROUP, 0x058, 0, 0, 0, 0, "group 058" }, { 2000, S_EVGROUP, 0x1A0, 0, 0, 0, 0, "group 1A0" },
	{ 2000, S_MUSICGROUPS, 0, 0, 0, 0, 0, "music groups" },
	END(2500),
};

const Step kJoinLead2[] = {
	MARK(0, "baseline"),
	{ 300, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	VPAD(1500, 1, PAD_START, 0, 0, "p2 start"), VPAD(1700, 1, 0, 0, 0, "p2 release"),
	VPAD(2500, 1, 0, 0, 1, "p2 up"), P1(2500, 0, 1, 0, "p1 right"),
	VPAD(4000, 1, 0, 0, 0, "p2 neutral"), P1(4000, 0, 0, 0, "p1 neutral"),
	VPAD(5000, 1, PAD_START, 0, 0, "p2 start (leave)"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	END(6500),
};

// warp: player 2 walks off, then Back + A brings Obelix back to Asterix; Back
// alone held long enough must still leave afterwards.
const Step kWarp[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(1000, 1, 0, 0, 1, "p2 up"), VPAD(4000, 1, 0, 0, 0, "p2 neutral"),
	VPAD(4500, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(4700, 1, PAD_BACK | PAD_A, 0, 0, "p2 back+A"),
	VPAD(4900, 1, PAD_BACK, 0, 0, "p2 back"), VPAD(6800, 1, 0, 0, 0, "p2 release (still in)"),
	SHOT(7000, "warp_01"),
	VPAD(7500, 1, PAD_BACK, 0, 0, "p2 hold back (leave)"), VPAD(9500, 1, 0, 0, 0, "p2 release"),
	END(10000),
};

// lifter2: player 2's Obelix goes to the lifter alone (player 1 stays at the
// start); zone events bound to the other hero should fire for him.
const Step kLifter2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	SECTORS(1000), { 1000, S_DETECT, 0, 0, 0, 0, 0, "detect" },
	{ 1500, S_PLACE, HERO_OBELIX, 5, 0, 0, 0, "obelix into the lifter zone" },
	SECTORS(2000),
	SHOT(2500, "lifter2_00"),
	VPAD(3000, 1, 0, -1, 0, "p2 left"), VPAD(4500, 1, 0, 0, 0, "p2 neutral"),
	SHOT(6000, "lifter2_01"),
	VPAD(6500, 1, 0, 1, 0, "p2 right"), VPAD(8000, 1, 0, 0, 0, "p2 neutral"),
	SHOT(12000, "lifter2_02"),
	END(13000),
};

// split1: split-screen smoke test. Player 2 joins (split on), the players walk
// apart, screenshots along the way, then player 2 leaves (back to one screen).
const Step kSplit1[] = {
	MARK(0, "baseline"), SHOT(100, "split_00_single"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	SHOT(1500, "split_01_joined"),
	P1(2000, 0, 1, 0, "p1 right"), VPAD(2000, 1, 0, -1, 0, "p2 left"),
	SHOT(3500, "split_02_apart"),
	P1(4000, 0, 0, 0, "p1 neutral"), VPAD(4000, 1, 0, 0, 1, "p2 up"),
	SHOT(5500, "split_03_p2up"),
	VPAD(6000, 1, 0, 0, 0, "p2 neutral"),
	VPAD(7000, 1, PAD_START, 0, 0, "p2 start (leave)"), VPAD(7200, 1, 0, 0, 0, "p2 release"),
	SHOT(8000, "split_04_left"),
	END(8500),
};

// split2: player 2's own camera. Player 2 turns it with the right stick, walks
// "forward" (must go away from player 2's camera, not player 1's), player 1
// jumps (player 2's view must not move), player 2 walks along the rail.
const Step kSplit2[] = {
	MARK(0, "baseline"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	SHOT(1500, "cam_00_joined"),
	VPADR(2000, 1, 1, 0, "p2 look right"), VPADR(3000, 1, 0, 0, "p2 look stop"),
	SHOT(3300, "cam_01_turned"),
	VPAD(3500, 1, 0, 0, 1, "p2 forward"), SHOT(4700, "cam_02_forward"), VPAD(5000, 1, 0, 0, 0, "p2 stop"),
	P1(5500, PAD_A, 0, 0, "p1 jump"), SHOT(5800, "cam_03_p1jump"), P1(5800, 0, 0, 0, "p1 release"),
	VPAD(6500, 1, 0, 1, 0, "p2 right"), SHOT(8000, "cam_04_right"), VPAD(8500, 1, 0, 0, 0, "p2 stop"),
	VPADR(8600, 1, 0, -1, "p2 look down"), VPADR(9200, 1, 0, 0, "p2 look stop"),
	SHOT(9500, "cam_05_lookdown"),
	END(10000),
};

// soak: one minute of both players wandering (p2 also turning the camera), to
// shake out rare crashes in the split renderer and player 2's camera.
const Step kSoak[] = {
	MARK(0, "baseline"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"),
	VPAD(700, 1, 0, 0, 0, "p2 release"),
	VPAD(1500, 1, 0, -0.7f, 0.7f, "p2 wander"),
	VPADR(1501, 1, 0.0f, -0.5f, "p2 look"),
	P1(1503, PAD_A, 0.0f, 1.0f, "p1 wander"),
	SHOT(1505, "soak_00"),
	VPAD(2296, 1, PAD_X, -0.7f, 0.7f, "p2 wander"),
	VPADR(2297, 1, -1.0f, 0.0f, "p2 look"),
	VPAD(3034, 1, PAD_A, 0.0f, 1.0f, "p2 wander"),
	VPADR(3035, 1, 0.0f, -0.5f, "p2 look"),
	P1(3037, 0, 0.0f, -1.0f, "p1 wander"),
	VPAD(4298, 1, 0, 0.7f, -0.7f, "p2 wander"),
	VPADR(4299, 1, 0.5f, -0.5f, "p2 look"),
	VPAD(5226, 1, PAD_X, 1.0f, 0.0f, "p2 wander"),
	VPADR(5227, 1, 0.5f, 0.5f, "p2 look"),
	P1(5229, 0, 1.0f, 0.0f, "p1 wander"),
	VPAD(5973, 1, 0, -1.0f, 0.0f, "p2 wander"),
	VPADR(5974, 1, 0.0f, 0.0f, "p2 look"),
	VPAD(7226, 1, PAD_X, 0.0f, 1.0f, "p2 wander"),
	VPADR(7227, 1, 0.0f, 0.0f, "p2 look"),
	P1(7229, PAD_A, 0.0f, 1.0f, "p1 wander"),
	VPAD(8510, 1, 0, 0.0f, -1.0f, "p2 wander"),
	VPADR(8511, 1, -1.0f, -0.5f, "p2 look"),
	VPAD(9787, 1, PAD_X, 1.0f, 0.0f, "p2 wander"),
	VPADR(9788, 1, -0.5f, 0.5f, "p2 look"),
	P1(9790, 0, 0.7f, -0.7f, "p1 wander"),
	SHOT(9792, "soak_08"),
	VPAD(10963, 1, 0, -0.7f, -0.7f, "p2 wander"),
	VPADR(10964, 1, 0.0f, 0.0f, "p2 look"),
	VPAD(12476, 1, 0, -1.0f, 0.0f, "p2 wander"),
	VPADR(12477, 1, -1.0f, 0.0f, "p2 look"),
	P1(12479, 0, -0.7f, -0.7f, "p1 wander"),
	VPAD(13922, 1, 0, -0.7f, -0.7f, "p2 wander"),
	VPADR(13923, 1, 0.5f, -0.5f, "p2 look"),
	VPAD(14742, 1, 0, 0.7f, -0.7f, "p2 wander"),
	VPADR(14743, 1, 0.0f, 0.0f, "p2 look"),
	P1(14745, 0, -0.7f, -0.7f, "p1 wander"),
	VPAD(15482, 1, PAD_X, 0.0f, 1.0f, "p2 wander"),
	VPADR(15483, 1, 0.5f, 0.0f, "p2 look"),
	VPAD(16530, 1, PAD_X, -0.7f, 0.7f, "p2 wander"),
	VPADR(16531, 1, 0.0f, 0.5f, "p2 look"),
	P1(16533, 0, 0.0f, 1.0f, "p1 wander"),
	VPAD(17506, 1, 0, -0.7f, -0.7f, "p2 wander"),
	VPADR(17507, 1, -1.0f, 0.0f, "p2 look"),
	VPAD(18868, 1, 0, -0.7f, -0.7f, "p2 wander"),
	VPADR(18869, 1, 1.0f, 0.5f, "p2 look"),
	P1(18871, 0, -0.7f, 0.7f, "p1 wander"),
	SHOT(18873, "soak_16"),
	VPAD(20040, 1, 0, -0.7f, 0.7f, "p2 wander"),
	VPADR(20041, 1, 0.5f, -0.5f, "p2 look"),
	VPAD(21245, 1, 0, 1.0f, 0.0f, "p2 wander"),
	VPADR(21246, 1, 0.0f, 0.0f, "p2 look"),
	P1(21248, 0, 0.0f, -1.0f, "p1 wander"),
	VPAD(22345, 1, 0, -0.7f, -0.7f, "p2 wander"),
	VPADR(22346, 1, -0.5f, 0.5f, "p2 look"),
	VPAD(23456, 1, 0, 0.7f, 0.7f, "p2 wander"),
	VPADR(23457, 1, 0.0f, 0.0f, "p2 look"),
	P1(23459, 0, 0.7f, -0.7f, "p1 wander"),
	VPAD(24855, 1, 0, 0.7f, -0.7f, "p2 wander"),
	VPADR(24856, 1, -0.5f, -0.5f, "p2 look"),
	VPAD(25735, 1, 0, -1.0f, 0.0f, "p2 wander"),
	VPADR(25736, 1, 1.0f, 0.0f, "p2 look"),
	P1(25738, 0, 1.0f, 0.0f, "p1 wander"),
	VPAD(27286, 1, 0, -1.0f, 0.0f, "p2 wander"),
	VPADR(27287, 1, 0.0f, -0.5f, "p2 look"),
	VPAD(28135, 1, PAD_X, 0.7f, -0.7f, "p2 wander"),
	VPADR(28136, 1, 0.0f, 0.0f, "p2 look"),
	P1(28138, PAD_A, -1.0f, 0.0f, "p1 wander"),
	SHOT(28140, "soak_24"),
	VPAD(29714, 1, PAD_A, 1.0f, 0.0f, "p2 wander"),
	VPADR(29715, 1, 1.0f, 0.5f, "p2 look"),
	VPAD(30821, 1, PAD_A, 0.7f, -0.7f, "p2 wander"),
	VPADR(30822, 1, -1.0f, 0.5f, "p2 look"),
	P1(30824, 0, 0.7f, -0.7f, "p1 wander"),
	VPAD(31716, 1, 0, 0.0f, 1.0f, "p2 wander"),
	VPADR(31717, 1, 0.0f, 0.0f, "p2 look"),
	VPAD(32528, 1, PAD_X, -0.7f, 0.7f, "p2 wander"),
	VPADR(32529, 1, -1.0f, -0.5f, "p2 look"),
	P1(32531, PAD_A, 1.0f, 0.0f, "p1 wander"),
	VPAD(33382, 1, 0, 0.0f, 1.0f, "p2 wander"),
	VPADR(33383, 1, 0.5f, -0.5f, "p2 look"),
	VPAD(34154, 1, PAD_X, 0.0f, -1.0f, "p2 wander"),
	VPADR(34155, 1, 0.0f, 0.0f, "p2 look"),
	P1(34157, 0, 0.7f, 0.7f, "p1 wander"),
	VPAD(35470, 1, PAD_A, -0.7f, 0.7f, "p2 wander"),
	VPADR(35471, 1, -1.0f, -0.5f, "p2 look"),
	VPAD(37039, 1, PAD_A, -0.7f, -0.7f, "p2 wander"),
	VPADR(37040, 1, 0.0f, 0.5f, "p2 look"),
	P1(37042, 0, 0.7f, 0.7f, "p1 wander"),
	SHOT(37044, "soak_32"),
	VPAD(37886, 1, 0, 0.0f, 1.0f, "p2 wander"),
	VPADR(37887, 1, 1.0f, 0.0f, "p2 look"),
	VPAD(39076, 1, PAD_X, -1.0f, 0.0f, "p2 wander"),
	VPADR(39077, 1, -1.0f, 0.0f, "p2 look"),
	P1(39079, 0, -0.7f, 0.7f, "p1 wander"),
	VPAD(40482, 1, PAD_X, 1.0f, 0.0f, "p2 wander"),
	VPADR(40483, 1, 0.0f, -0.5f, "p2 look"),
	VPAD(41894, 1, PAD_X, 0.7f, 0.7f, "p2 wander"),
	VPADR(41895, 1, 0.0f, 0.0f, "p2 look"),
	P1(41897, 0, -0.7f, 0.7f, "p1 wander"),
	VPAD(43139, 1, 0, -0.7f, 0.7f, "p2 wander"),
	VPADR(43140, 1, 0.5f, 0.0f, "p2 look"),
	VPAD(44664, 1, PAD_A, 0.0f, -1.0f, "p2 wander"),
	VPADR(44665, 1, 1.0f, 0.0f, "p2 look"),
	P1(44667, PAD_A, 0.0f, -1.0f, "p1 wander"),
	VPAD(45868, 1, 0, -0.7f, 0.7f, "p2 wander"),
	VPADR(45869, 1, -1.0f, 0.0f, "p2 look"),
	VPAD(47051, 1, 0, 0.7f, 0.7f, "p2 wander"),
	VPADR(47052, 1, 1.0f, 0.0f, "p2 look"),
	P1(47054, PAD_A, -0.7f, -0.7f, "p1 wander"),
	SHOT(47056, "soak_40"),
	VPAD(48108, 1, 0, -0.7f, 0.7f, "p2 wander"),
	VPADR(48109, 1, -0.5f, -0.5f, "p2 look"),
	VPAD(49040, 1, 0, -0.7f, -0.7f, "p2 wander"),
	VPADR(49041, 1, 0.0f, 0.0f, "p2 look"),
	P1(49043, PAD_A, -0.7f, -0.7f, "p1 wander"),
	VPAD(50364, 1, PAD_A, 1.0f, 0.0f, "p2 wander"),
	VPADR(50365, 1, 1.0f, 0.0f, "p2 look"),
	VPAD(51882, 1, 0, 0.0f, 1.0f, "p2 wander"),
	VPADR(51883, 1, 0.0f, 0.0f, "p2 look"),
	P1(51885, 0, -0.7f, -0.7f, "p1 wander"),
	VPAD(53026, 1, 0, -0.7f, 0.7f, "p2 wander"),
	VPADR(53027, 1, 1.0f, 0.5f, "p2 look"),
	VPAD(54200, 1, 0, 0.7f, -0.7f, "p2 wander"),
	VPADR(54201, 1, 1.0f, 0.0f, "p2 look"),
	P1(54203, 0, -1.0f, 0.0f, "p1 wander"),
	VPAD(54928, 1, PAD_X, -1.0f, 0.0f, "p2 wander"),
	VPADR(54929, 1, 0.0f, 0.0f, "p2 look"),
	VPAD(56254, 1, 0, -0.7f, -0.7f, "p2 wander"),
	VPADR(56255, 1, -0.5f, 0.0f, "p2 look"),
	P1(56257, 0, 1.0f, 0.0f, "p1 wander"),
	SHOT(56259, "soak_48"),
	VPAD(57772, 1, PAD_X, 0.0f, 1.0f, "p2 wander"),
	VPADR(57773, 1, 1.0f, 0.0f, "p2 look"),
	VPAD(58916, 1, 0, 0.0f, -1.0f, "p2 wander"),
	VPADR(58917, 1, -1.0f, 0.0f, "p2 look"),
	P1(58919, 0, 0.0f, -1.0f, "p1 wander"),
	P1(60500, 0, 0, 0, "p1 stop"),
	VPAD(60500, 1, 0, 0, 0, "p2 stop"),
	END(61000),
};

// helmet: player 2 (not the leader) is put on a bonus. Before the proximity
// layer only the leader could collect bonuses.
const Step kHelmet[] = {
	MARK(0, "baseline"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	SHOT(1400, "helmet_00_before"),
	TO_BONUS(1500, HERO_OBELIX),
	SHOT(2500, "helmet_01_on"),
	SHOT(4000, "helmet_02_after"),
	END(4500),
};

// beacon (level 2): player 2's Obelix goes to a helmet placed in the level, away
// from Asterix; it should fly to him (watch: p2Beacons, the bonus's target).
const Step kBeacon[] = {
	MARK(0, "baseline"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	TO_BEACON(1500, HERO_OBELIX),
	SHOT(2500, "beacon_00_on"),
	TO_BEACON(4500, HERO_OBELIX),
	SHOT(5500, "beacon_01_on"),
	END(7500),
};

// cine (run with -Level 4): the level's intro cutscene. Player 1 pushes the stick
// the whole time, so the moment Asterix starts moving is the moment the game
// gives control back; the probe logs every candidate signal around it.
const Step kCine[] = {
	PROBE(0, 1), MARK(0, "cine"), P1(0, 0, 0, 1, "p1 up (held)"),
	SHOT(500, "cine_00"), SHOT(4000, "cine_01"), SHOT(8000, "cine_02"), SHOT(12000, "cine_03"),
	SHOT(16000, "cine_04"), SHOT(20000, "cine_05"), SHOT(24000, "cine_06"),
	END(26000),
};

// bumper (level 2): player 1 dashes into the nearest spring (control), then
// player 2 does the same with Obelix. trace.cpp logs the spring's events.
const Step kBumper[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	NEAR_BUMPER(1500, HERO_ASTERIX),
	P1(1800, PAD_Y, 0, 0, "p1 dash"), P1(2000, 0, 0, 0, "p1 release"),
	SHOT(2300, "bumper_00_p1"), SHOT(3000, "bumper_01_p1"),
	NEAR_BUMPER(5000, HERO_OBELIX),
	VPAD(5300, 1, PAD_Y, 0, 0, "p2 dash"), VPAD(5500, 1, 0, 0, 0, "p2 release"),
	SHOT(5800, "bumper_02_p2"), SHOT(6500, "bumper_03_p2"),
	END(8500),
};

// crate (level 2): player 2's Obelix punches the nearest iron crate, then the
// nearest wooden one. CKHkCrate pieces (+8 & 7) drop by one per break.
const Step kCrate[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	NEAR_CRATE(1500, HERO_OBELIX, 1),
	VPAD(1900, 1, PAD_X, 0, 0, "p2 punch"), VPAD(2050, 1, 0, 0, 0, "p2 release"),
	VPAD(2500, 1, PAD_X, 0, 0, "p2 punch"), VPAD(2650, 1, 0, 0, 0, "p2 release"),
	VPAD(3100, 1, PAD_X, 0, 0, "p2 punch"), VPAD(3250, 1, 0, 0, 0, "p2 release"),
	SHOT(3600, "crate_00_iron"), CRATE_REPORT(4000),
	NEAR_CRATE(4500, HERO_OBELIX, 0),
	VPAD(4900, 1, PAD_X, 0, 0, "p2 punch"), VPAD(5050, 1, 0, 0, 0, "p2 release"),
	VPAD(5500, 1, PAD_X, 0, 0, "p2 punch"), VPAD(5650, 1, 0, 0, 0, "p2 release"),
	SHOT(6000, "crate_01_wood"), CRATE_REPORT(6500),
	END(7000),
};

// grab (level 2): each player walks up to a Roman and grabs it (B), then turns
// the stick around to spin it. Player 1 first as the control.
const Step kGrab[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	NEAR_ENEMY(1500, HERO_ASTERIX),
	P1(1600, PAD_X, 0, 0, "p1 punch"), P1(1700, 0, 0, 0, "p1 release"),
	P1(2000, PAD_X, 0, 0, "p1 punch"), P1(2100, 0, 0, 0, "p1 release"),
	P1(2400, PAD_X, 0, 0, "p1 punch"), P1(2500, 0, 0, 0, "p1 release"),
	P1(2900, PAD_B, 0, 0, "p1 grab"), P1(3100, 0, 0, 0, "p1 release"),
	P1(3500, 0, 1, 0, "p1 spin r"), P1(3700, 0, 0, -1, "p1 spin d"), P1(3900, 0, -1, 0, "p1 spin l"),
	P1(4100, 0, 0, 1, "p1 spin u"), P1(4300, 0, 1, 0, "p1 spin r"), P1(4500, 0, 0, 0, "p1 neutral"),
	SHOT(4600, "grab_00_p1"),
	NEAR_ENEMY(6500, HERO_OBELIX),
	VPAD(6600, 1, PAD_X, 0, 0, "p2 punch"), VPAD(6700, 1, 0, 0, 0, "p2 release"),
	VPAD(7000, 1, PAD_X, 0, 0, "p2 punch"), VPAD(7100, 1, 0, 0, 0, "p2 release"),
	VPAD(7400, 1, PAD_X, 0, 0, "p2 punch"), VPAD(7500, 1, 0, 0, 0, "p2 release"),
	VPAD(7900, 1, PAD_B, 0, 0, "p2 grab"), VPAD(8100, 1, 0, 0, 0, "p2 release"),
	VPAD(8500, 1, 0, 1, 0, "p2 spin r"), VPAD(8700, 1, 0, 0, -1, "p2 spin d"),
	VPAD(8900, 1, 0, -1, 0, "p2 spin l"), VPAD(9100, 1, 0, 0, 1, "p2 spin u"),
	VPAD(9300, 1, 0, 1, 0, "p2 spin r"), VPAD(9500, 1, 0, 0, 0, "p2 neutral"),
	SHOT(9600, "grab_01_p2"),
	END(11500),
};

// cutscene (level 2): player 2 joins, a cutscene with bars is started, both
// players push their sticks the whole time. Co-op should suspend for it (player
// 2's hero back to the AI) and resume after.
const Step kCutscene[] = {
	MARK(0, "baseline"), CINE_LIST(0),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	P1(1000, 0, 0, 1, "p1 up (held)"), VPAD(1000, 1, 0, 0, 1, "p2 up (held)"),
	PROBE(1400, 1), CINE_START(1500),
	SHOT(2500, "cutscene_00"), SHOT(5000, "cutscene_01"),
	CINE_STOP(6000), SHOT(7500, "cutscene_02"),
	PROBE(9000, 0),
	VPAD(9500, 1, 0, 1, 0, "p2 right (held)"), SHOT(11000, "cutscene_03"),
	END(12000),
};

// grab2 (level 2): both heroes next to the same Roman; player 2 hits it once
// (it is stunned for a few seconds) and grabs it, then spins.
const Step kGrab2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	NEAR_ENEMY(1500, HERO_OBELIX), { 1520, S_ENEMY, HERO_ASTERIX, 1, 0, 0, 0, "near enemy (far side)" },
	VPAD(1700, 1, PAD_X, 0, 0, "p2 punch"), VPAD(1800, 1, 0, 0, 0, "p2 release"),
	VPAD(2400, 1, PAD_B, 0, 0, "p2 grab"), VPAD(2600, 1, 0, 0, 0, "p2 release"),
	VPAD(3000, 1, 0, 1, 0, "p2 spin r"), VPAD(3200, 1, 0, 0, -1, "p2 spin d"),
	VPAD(3400, 1, 0, -1, 0, "p2 spin l"), VPAD(3600, 1, 0, 0, 1, "p2 spin u"),
	VPAD(3800, 1, 0, 0, 0, "p2 neutral"),
	SHOT(3900, "grab2_00"),
	END(6000),
};

// grab3 (level 2): like grab2 but Asterix stays at the start, ~65 units away.
const Step kGrab3[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	NEAR_ENEMY(1500, HERO_OBELIX),
	VPAD(1700, 1, PAD_X, 0, 0, "p2 punch"), VPAD(1800, 1, 0, 0, 0, "p2 release"),
	VPAD(2400, 1, PAD_B, 0, 0, "p2 grab"), VPAD(2600, 1, 0, 0, 0, "p2 release"),
	VPAD(3000, 1, 0, 1, 0, "p2 spin r"), VPAD(3200, 1, 0, 0, -1, "p2 spin d"),
	VPAD(3400, 1, 0, -1, 0, "p2 spin l"), VPAD(3600, 1, 0, 0, 1, "p2 spin u"),
	VPAD(3800, 1, 0, 0, 0, "p2 neutral"),
	END(6000),
};

// slidecross (level 2): from -905.96 527.36 -938.49 Asterix walks past LV2-2's
// cross detector (z -903.82, event #5): the belly slide starts.
const Step kSlideCross[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -905.96f, 528.2f, -938.49f), PLACEAT(3000, HERO_OBELIX, -903.96f, 528.2f, -938.49f),
	GLIDEAT(4500, HERO_ASTERIX, -905.96f, 524.0f, -896.0f, 2500),
	SHOT(8000, "slidecross_1"), SHOT(11000, "slidecross_2"),
	END(14000),
};

const Step kSlideCross3[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -905.96f, 528.2f, -938.49f), PLACEAT(3000, HERO_OBELIX, -903.96f, 528.2f, -938.49f),
	GLIDEAT(4500, HERO_ASTERIX, -905.96f, 524.0f, -896.0f, 2500),
	GLIDEAT(8000, HERO_ASTERIX, -903.0f, 524.0f, -896.0f, 2000),
	GLIDEAT(11000, HERO_ASTERIX, -909.0f, 524.0f, -896.0f, 2000),
	SHOT(14000, "slidecross3"),
	END(15000),
};

// slideend (level 2): Asterix walks down beside the belly slide (LV2-2,
// -916.8 519.6 -883.8 -> -906.6 511.1 -860.4): what starts the level's end there.
const Step kSlideEnd[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(3000, HERO_ASTERIX, -916.8f, 520.5f, -883.8f), PLACEAT(3000, HERO_OBELIX, -914.8f, 520.5f, -884.8f),
	GLIDEAT(4000, HERO_ASTERIX, -905.96f, 518.3f, -878.35f, 1500),
	GLIDEAT(6000, HERO_ASTERIX, -906.5f, 512.0f, -861.0f, 2500),
	SHOT(9500, "slideend_1"),
	END(14000),
};

// lockcircle (level 2, XXL_DOWNAT=1:6000): Asterix steps on the blue circle by
// the spinning platform (-778.45 516.45 -977.28): he waits there for Obelix;
// then Obelix goes down -- the wait must be undone.
const Step kLockCircle[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f),
	GLIDEAT(2500, HERO_ASTERIX, -778.45f, 516.8f, -977.28f, 1000),
	SHOT(5000, "lockcircle_1"), SHOT(9000, "lockcircle_2"),
	P1(10000, 0, 0, 1, "p1 walks"), P1(11500, 0, 0, 0, "p1 stops"),
	SHOT(12000, "lockcircle_3"),
	END(13000),
};

// lockagain (level 2, XXL_DOWNAT=1:6000 XXL_UPAT=9000): lockcircle, then
// Obelix revived and Asterix back onto the spinning platform's circle -- it must
// set off again (undone: its "set off" bit, its pillar, its detectors).
const Step kLockAgain[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f),
	GLIDEAT(2500, HERO_ASTERIX, -778.45f, 516.8f, -977.28f, 1000),
	SHOT(5000, "lockagain_1"), SHOT(7500, "lockagain_2"),
	PLACEAT(10000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f),
	GLIDEAT(11000, HERO_ASTERIX, -778.45f, 516.8f, -977.28f, 1000),
	SHOT(14000, "lockagain_3"),
	END(16000),
};

// corkdown (level 2, XXL_DOWNAT=1:10000 XXL_UPAT=13000): lockcircle, Obelix
// walks through the zone that frees Asterix (0x03C8, 4803) to the turnstile,
// then goes down before the platform's turn ends -- undone after the lock; the
// circle must set off again after the revive.
const Step kCorkDown[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f), PLACEAT(1800, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	PLACEAT(2000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f),
	GLIDEAT(2500, HERO_ASTERIX, -778.45f, 516.8f, -977.28f, 1000),
	GLIDEAT(5000, HERO_OBELIX, -762.0f, 515.8f, -979.2f, 1500),
	GLIDEAT(7000, HERO_OBELIX, -744.5f, 515.6f, -977.8f, 2000),
	SHOT(9500, "corkdown_1"), SHOT(11000, "corkdown_2"),
	PLACEAT(14000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f),
	GLIDEAT(15000, HERO_ASTERIX, -778.45f, 516.8f, -977.28f, 1000),
	SHOT(17500, "corkdown_3"),
	END(19000),
};

// lifterlock (level 2, XXL_LIFTLOG=1): the user's LV2-1 softlock. Both heroes ride
// the lifter up, Asterix goes onto the spinning platform's circle (locked, waits
// for Obelix at the turnstile), Obelix steps off the lifter (it goes down) and back
// on: the lifter waits for Asterix, who can't come.
const Step kLifterLock[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -746.697f, 506.5f, -992.08f), PLACEAT(1500, HERO_OBELIX, -746.697f, 506.5f, -990.5f),
	SHOT(4000, "lifterlock_0"),
	SHOT(9000, "lifterlock_1"),
	PLACEAT(9000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),   // walked over: a trail of safe spots
	GLIDEAT(9300, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(11000, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	GLIDEAT(12500, HERO_OBELIX, -745.719f, 514.5f, -979.123f, 1500),
	SHOT(15000, "lifterlock_2"),
	PLACEAT(19000, HERO_OBELIX, -746.697f, 517.0f, -992.08f),
	SHOT(21000, "lifterlock_3"), SHOT(30000, "lifterlock_4"),
	// the circle answers again (the lead back to Asterix): locked again, undone again
	PLACEAT(32000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(32300, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(34000, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(36000, "lifterlock_5"), SHOT(42000, "lifterlock_6"),
	END(44000),
};

// pull28 (level 2, sector 8; nolock on): the user's LV2-8 -- Asterix crosses the
// detector that lights the pillar (16.9 50.9 -133 -> -128), stands on the pillar
// on the platform (42.06 57.0 -120.10; "Obelix leads, Asterix waits", free in
// no-lock mode), player 2's Obelix holds X at the platform (37.92 51.0 -120.16)
// and pulls right, then down: the "Pull" prompt and the platform moving.
const Step kPull28[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, 16.913f, 51.4f, -133.0f),
	GLIDEAT(2300, HERO_ASTERIX, 16.913f, 51.4f, -127.5f, 1000),
	PLACEAT(4000, HERO_OBELIX, 37.923f, 51.5f, -120.156f),
	PLACEAT(4200, HERO_ASTERIX, 42.057f, 57.5f, -120.097f),
	SHOT(6000, "pull28_1"),
	VPAD(7000, 1, PAD_X, 0, 0, "p2 X"),
	VPAD(7500, 1, PAD_X, 1.0f, 0, "p2 X + right"), SHOT(9000, "pull28_2"),
	VPAD(10000, 1, PAD_X, 0, -1.0f, "p2 X + down"), SHOT(11500, "pull28_3"),
	VPAD(12500, 1, 0, 0, 0, "p2 release"), SHOT(13000, "pull28_4"),
	END(14000),
};

// prompt32 (-Level 3, sector 2): the user's LV3-2 (xxl_coop.lv3-2_prompt_light.log)
// -- player 2's Obelix pulls the big block (handles by 187.01 12.07 -193.73;
// X, then each stick direction in turn) to its end, Asterix away with a torch
// (177.68 12.07 -147.94, the hearth's "Light"), then by the hearth again: his
// "Light" must stay out of Obelix's box (a hero by a finished push/pull block
// gets no prompt write).
const Step kPrompt32[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, 180.21f, 12.5f, -188.79f),
	PLACEAT(1500, HERO_OBELIX, 187.13f, 12.5f, -184.0f),
	GLIDEAT(1700, HERO_OBELIX, 187.13f, 12.5f, -188.97f, 800),   // zone 00E0: Obelix leads
	GLIDEAT(3000, HERO_OBELIX, 187.01f, 12.5f, -193.73f, 800),
	{ 2500, S_TORCH, 0, 0, 0, 0, 0, "torches" },
	{ 2800, S_TORCH, 6, 0, 0, 0, 0, "cheat: give torch" },
	SHOT(4000, "prompt32_1"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"),
	VPAD(5000, 1, PAD_X, 1.0f, 0, "p2 X + right (pull)"),
	VPAD(8000, 1, 0, 0, 0, "p2 release"), SHOT(9000, "prompt32_2"),
	PLACEAT(10000, HERO_ASTERIX, 177.68f, 12.5f, -147.94f),   // the hearth: "Light"
	GLIDEAT(11000, HERO_OBELIX, 192.5f, 12.5f, -194.0f, 800),   // away from the block
	GLIDEAT(12500, HERO_OBELIX, 188.41f, 12.5f, -194.0f, 800),  // back by it
	SHOT(14000, "prompt32_3"),
	PLACEAT(15000, HERO_ASTERIX, 179.5f, 12.5f, -165.0f),     // off the hearth
	SHOT(17000, "prompt32_4"),
	END(18000),
};

// camfall28 (level 2, sector 8; nolock on): the user's LV2-8 -- the platform's
// pillar (Obelix leads), Obelix pulls the platform with Asterix on it, Asterix
// jumps off it (29.4 57.7 -117.4: the platform's camera, zone 02E9) and falls to
// the ground (31.1 51.0 -106.4: the reset, 03B8 -- the lead back to Asterix):
// player 1's camera must not stay on the platform's shot.
const Step kCamFall28[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, 16.913f, 51.4f, -133.0f),
	GLIDEAT(2300, HERO_ASTERIX, 16.913f, 51.4f, -127.5f, 1000),
	PLACEAT(4000, HERO_OBELIX, 37.923f, 51.5f, -120.156f),
	PLACEAT(4200, HERO_ASTERIX, 42.057f, 57.5f, -120.097f),
	VPAD(7000, 1, PAD_X, 0, 0, "p2 X"),
	VPAD(7500, 1, PAD_X, 1.0f, 0, "p2 X + right"),
	VPAD(17500, 1, 0, 0, 0, "p2 release"), SHOT(18000, "camfall28_1"),
	PLACEAT(18500, HERO_OBELIX, 36.2f, 51.6f, -103.2f),
	// (zone 02E9 -- the second block pushed into place -- switches on the
	// platforms' camera sector (15.5 36.7 -126.5)-(46.5 84.1 -98.8): as its 0x701)
	{ 19000, S_CAMSECTOR, 0, 0, 0, 17.0f, 45.0f, "the platforms' camera (02E9)", -124.0f },
	SHOT(21000, "camfall28_2"),
	GLIDEAT(22000, HERO_ASTERIX, 29.35f, 57.8f, -117.35f, 600),
	GLIDEAT(22700, HERO_ASTERIX, 31.08f, 51.2f, -106.4f, 800),   // off the platform: the reset, 03B8
	SHOT(25000, "camfall28_3"),
	GLIDEAT(25500, HERO_ASTERIX, 33.6f, 51.2f, -117.67f, 1500),
	GLIDEAT(27500, HERO_ASTERIX, 41.2f, 51.2f, -123.8f, 1500),
	SHOT(30000, "camfall28_4"),
	END(31000),
};

// lead28 (level 2, sector 8): the user's -- the platforms' sequence done
// (Obelix leads), Asterix waiting at the rotary beam's spot (41.0 58.4 -89.6), and
// player 2's Obelix jumping about by the turnstile (44.8 52.8 -69.3): Asterix's
// "fell off the platforms" zone (03B8: Asterix leads, the blocks back, the pillar
// lit) set off by Obelix in the other hero's pass took the lead from him.
const Step kLead28[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, 16.913f, 51.4f, -133.0f),
	GLIDEAT(2300, HERO_ASTERIX, 16.913f, 51.4f, -127.5f, 1000),
	PLACEAT(4000, HERO_OBELIX, 37.923f, 51.5f, -120.156f),
	PLACEAT(4200, HERO_ASTERIX, 42.057f, 57.5f, -120.097f),
	PLACEAT(6000, HERO_OBELIX, 38.0f, 57.5f, -92.0f),
	PLACEAT(6000, HERO_ASTERIX, 41.0f, 58.5f, -89.613f),
	PLACEAT(16000, HERO_OBELIX, 44.8f, 53.0f, -69.3f),
	PLACEAT(17000, HERO_OBELIX, 44.8f, 56.0f, -69.3f),   // up on the turnstile (the user: 55.5)
	PLACEAT(21000, HERO_OBELIX, 44.8f, 56.0f, -69.3f),
	PLACEAT(25000, HERO_OBELIX, 44.8f, 56.0f, -69.3f),
	VPAD(18000, 1, PAD_A, 0, 0, "p2 A"), VPAD(18300, 1, 0, 0, 0, "p2 release"),
	VPAD(18700, 1, PAD_A, 0, 0, "p2 A"), VPAD(19000, 1, 0, 0, 0, "p2 release"),
	VPAD(19400, 1, PAD_A, 0, 0, "p2 A"), VPAD(19700, 1, 0, 0, 0, "p2 release"),
	VPAD(20100, 1, PAD_A, 0, 0, "p2 A"), VPAD(20400, 1, 0, 0, 0, "p2 release"),
	VPAD(20800, 1, PAD_A, 0, 0, "p2 A"), VPAD(21100, 1, 0, 0, 0, "p2 release"),
	VPAD(21500, 1, PAD_A, 0, 0, "p2 A"), VPAD(21800, 1, 0, 0, 0, "p2 release"),
	VPAD(22200, 1, PAD_A, 0, 0, "p2 A"), VPAD(22500, 1, 0, 0, 0, "p2 release"),
	VPAD(22900, 1, PAD_A, 0, 0, "p2 A"), VPAD(23200, 1, 0, 0, 0, "p2 release"),
	VPAD(23600, 1, PAD_A, 0, 0, "p2 A"), VPAD(23900, 1, 0, 0, 0, "p2 release"),
	VPAD(24300, 1, PAD_A, 0, 0, "p2 A"), VPAD(24600, 1, 0, 0, 0, "p2 release"),
	VPAD(25000, 1, PAD_A, 0, 0, "p2 A"), VPAD(25300, 1, 0, 0, 0, "p2 release"),
	VPAD(25700, 1, PAD_A, 0, 0, "p2 A"), VPAD(26000, 1, 0, 0, 0, "p2 release"),
	VPAD(26400, 1, PAD_A, 0, 0, "p2 A"), VPAD(26700, 1, 0, 0, 0, "p2 release"),
	VPAD(27100, 1, PAD_A, 0, 0, "p2 A"), VPAD(27400, 1, 0, 0, 0, "p2 release"),
	VPAD(27800, 1, PAD_A, 0, 0, "p2 A"), VPAD(28100, 1, 0, 0, 0, "p2 release"),
	VPAD(28500, 1, PAD_A, 0, 0, "p2 A"), VPAD(28800, 1, 0, 0, 0, "p2 release"),
	VPAD(29200, 1, PAD_A, 0, 0, "p2 A"), VPAD(29500, 1, 0, 0, 0, "p2 release"),
	VPAD(29900, 1, PAD_A, 0, 0, "p2 A"), VPAD(30200, 1, 0, 0, 0, "p2 release"),
	SHOT(25000, "lead28_1"), SHOT(31000, "lead28_2"),
	END(32000),
};

// tele28 (level 2, sector 8): Asterix leading walks into LV2-8's beam cutscene
// zone (41.0 58.4 -89.6, ~8 s), then player 2's Obelix is put by the turnstile
// (45.52 52.58 -66.59): the user's -- he did not stay there unless leading.
const Step kTele28[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_OBELIX, 38.0f, 57.5f, -92.0f),
	PLACEAT(2000, HERO_ASTERIX, 41.0f, 58.5f, -89.613f),
	SHOT(6000, "tele28_1"),
	SECTORS(12500),
	PLACEAT(13000, HERO_OBELIX, 45.519f, 52.8f, -66.585f),
	SHOT(14000, "tele28_2"), SECTORS(14500), SHOT(17000, "tele28_3"),
	END(18000),
};

// beam28 (level 2, sector 8): the natural LV2-8 order -- the platform's pillar
// (Obelix leads, Asterix waits: AI order 3, free in no-lock mode), Asterix into
// the beam's cutscene zone, Obelix put by the turnstile hitting it (X), Asterix
// jumping now and then: the rotary beam must take him (nolock: his jump).
const Step kBeam28[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, 16.913f, 51.4f, -133.0f),
	GLIDEAT(2300, HERO_ASTERIX, 16.913f, 51.4f, -127.5f, 1000),
	PLACEAT(4000, HERO_OBELIX, 37.923f, 51.5f, -120.156f),
	PLACEAT(4200, HERO_ASTERIX, 42.057f, 57.5f, -120.097f),
	PLACEAT(6000, HERO_OBELIX, 38.0f, 57.5f, -92.0f),
	PLACEAT(6000, HERO_ASTERIX, 41.0f, 58.5f, -89.613f),
	SHOT(9000, "beam28_1"),
	{ 16000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 19000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 22000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 25000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 28000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 31000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 34000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 37000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 40000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 43000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	VPAD(17000, 1, PAD_X, 0, 0, "p2 X"), VPAD(17150, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X"), VPAD(18150, 1, 0, 0, 0, "p2 release"),
	VPAD(18500, 1, PAD_X, 0, 0, "p2 X"), VPAD(18650, 1, 0, 0, 0, "p2 release"),
	VPAD(19000, 1, PAD_X, 0, 0, "p2 X"), VPAD(19150, 1, 0, 0, 0, "p2 release"),
	VPAD(19500, 1, PAD_X, 0, 0, "p2 X"), VPAD(19650, 1, 0, 0, 0, "p2 release"),
	VPAD(20000, 1, PAD_X, 0, 0, "p2 X"), VPAD(20150, 1, 0, 0, 0, "p2 release"),
	VPAD(20500, 1, PAD_X, 0, 0, "p2 X"), VPAD(20650, 1, 0, 0, 0, "p2 release"),
	VPAD(21000, 1, PAD_X, 0, 0, "p2 X"), VPAD(21150, 1, 0, 0, 0, "p2 release"),
	VPAD(21500, 1, PAD_X, 0, 0, "p2 X"), VPAD(21650, 1, 0, 0, 0, "p2 release"),
	VPAD(22000, 1, PAD_X, 0, 0, "p2 X"), VPAD(22150, 1, 0, 0, 0, "p2 release"),
	VPAD(22500, 1, PAD_X, 0, 0, "p2 X"), VPAD(22650, 1, 0, 0, 0, "p2 release"),
	VPAD(23000, 1, PAD_X, 0, 0, "p2 X"), VPAD(23150, 1, 0, 0, 0, "p2 release"),
	VPAD(23500, 1, PAD_X, 0, 0, "p2 X"), VPAD(23650, 1, 0, 0, 0, "p2 release"),
	VPAD(24000, 1, PAD_X, 0, 0, "p2 X"), VPAD(24150, 1, 0, 0, 0, "p2 release"),
	VPAD(24500, 1, PAD_X, 0, 0, "p2 X"), VPAD(24650, 1, 0, 0, 0, "p2 release"),
	VPAD(25000, 1, PAD_X, 0, 0, "p2 X"), VPAD(25150, 1, 0, 0, 0, "p2 release"),
	VPAD(25500, 1, PAD_X, 0, 0, "p2 X"), VPAD(25650, 1, 0, 0, 0, "p2 release"),
	VPAD(26000, 1, PAD_X, 0, 0, "p2 X"), VPAD(26150, 1, 0, 0, 0, "p2 release"),
	VPAD(26500, 1, PAD_X, 0, 0, "p2 X"), VPAD(26650, 1, 0, 0, 0, "p2 release"),
	VPAD(27000, 1, PAD_X, 0, 0, "p2 X"), VPAD(27150, 1, 0, 0, 0, "p2 release"),
	VPAD(27500, 1, PAD_X, 0, 0, "p2 X"), VPAD(27650, 1, 0, 0, 0, "p2 release"),
	VPAD(28000, 1, PAD_X, 0, 0, "p2 X"), VPAD(28150, 1, 0, 0, 0, "p2 release"),
	VPAD(28500, 1, PAD_X, 0, 0, "p2 X"), VPAD(28650, 1, 0, 0, 0, "p2 release"),
	VPAD(29000, 1, PAD_X, 0, 0, "p2 X"), VPAD(29150, 1, 0, 0, 0, "p2 release"),
	VPAD(29500, 1, PAD_X, 0, 0, "p2 X"), VPAD(29650, 1, 0, 0, 0, "p2 release"),
	VPAD(30000, 1, PAD_X, 0, 0, "p2 X"), VPAD(30150, 1, 0, 0, 0, "p2 release"),
	VPAD(30500, 1, PAD_X, 0, 0, "p2 X"), VPAD(30650, 1, 0, 0, 0, "p2 release"),
	VPAD(31000, 1, PAD_X, 0, 0, "p2 X"), VPAD(31150, 1, 0, 0, 0, "p2 release"),
	VPAD(31500, 1, PAD_X, 0, 0, "p2 X"), VPAD(31650, 1, 0, 0, 0, "p2 release"),
	VPAD(32000, 1, PAD_X, 0, 0, "p2 X"), VPAD(32150, 1, 0, 0, 0, "p2 release"),
	VPAD(32500, 1, PAD_X, 0, 0, "p2 X"), VPAD(32650, 1, 0, 0, 0, "p2 release"),
	VPAD(33000, 1, PAD_X, 0, 0, "p2 X"), VPAD(33150, 1, 0, 0, 0, "p2 release"),
	VPAD(33500, 1, PAD_X, 0, 0, "p2 X"), VPAD(33650, 1, 0, 0, 0, "p2 release"),
	VPAD(34000, 1, PAD_X, 0, 0, "p2 X"), VPAD(34150, 1, 0, 0, 0, "p2 release"),
	VPAD(34500, 1, PAD_X, 0, 0, "p2 X"), VPAD(34650, 1, 0, 0, 0, "p2 release"),
	VPAD(35000, 1, PAD_X, 0, 0, "p2 X"), VPAD(35150, 1, 0, 0, 0, "p2 release"),
	VPAD(35500, 1, PAD_X, 0, 0, "p2 X"), VPAD(35650, 1, 0, 0, 0, "p2 release"),
	VPAD(36000, 1, PAD_X, 0, 0, "p2 X"), VPAD(36150, 1, 0, 0, 0, "p2 release"),
	VPAD(36500, 1, PAD_X, 0, 0, "p2 X"), VPAD(36650, 1, 0, 0, 0, "p2 release"),
	VPAD(37000, 1, PAD_X, 0, 0, "p2 X"), VPAD(37150, 1, 0, 0, 0, "p2 release"),
	VPAD(37500, 1, PAD_X, 0, 0, "p2 X"), VPAD(37650, 1, 0, 0, 0, "p2 release"),
	VPAD(38000, 1, PAD_X, 0, 0, "p2 X"), VPAD(38150, 1, 0, 0, 0, "p2 release"),
	VPAD(38500, 1, PAD_X, 0, 0, "p2 X"), VPAD(38650, 1, 0, 0, 0, "p2 release"),
	VPAD(39000, 1, PAD_X, 0, 0, "p2 X"), VPAD(39150, 1, 0, 0, 0, "p2 release"),
	VPAD(39500, 1, PAD_X, 0, 0, "p2 X"), VPAD(39650, 1, 0, 0, 0, "p2 release"),
	VPAD(40000, 1, PAD_X, 0, 0, "p2 X"), VPAD(40150, 1, 0, 0, 0, "p2 release"),
	VPAD(40500, 1, PAD_X, 0, 0, "p2 X"), VPAD(40650, 1, 0, 0, 0, "p2 release"),
	VPAD(41000, 1, PAD_X, 0, 0, "p2 X"), VPAD(41150, 1, 0, 0, 0, "p2 release"),
	VPAD(41500, 1, PAD_X, 0, 0, "p2 X"), VPAD(41650, 1, 0, 0, 0, "p2 release"),
	VPAD(42000, 1, PAD_X, 0, 0, "p2 X"), VPAD(42150, 1, 0, 0, 0, "p2 release"),
	VPAD(42500, 1, PAD_X, 0, 0, "p2 X"), VPAD(42650, 1, 0, 0, 0, "p2 release"),
	VPAD(43000, 1, PAD_X, 0, 0, "p2 X"), VPAD(43150, 1, 0, 0, 0, "p2 release"),
	VPAD(43500, 1, PAD_X, 0, 0, "p2 X"), VPAD(43650, 1, 0, 0, 0, "p2 release"),
	VPAD(44000, 1, PAD_X, 0, 0, "p2 X"), VPAD(44150, 1, 0, 0, 0, "p2 release"),
	VPAD(44500, 1, PAD_X, 0, 0, "p2 X"), VPAD(44650, 1, 0, 0, 0, "p2 release"),
	VPAD(45000, 1, PAD_X, 0, 0, "p2 X"), VPAD(45150, 1, 0, 0, 0, "p2 release"),
	VPAD(45500, 1, PAD_X, 0, 0, "p2 X"), VPAD(45650, 1, 0, 0, 0, "p2 release"),
	P1(20000, PAD_A, 0, 0, "p1 A"), P1(20250, 0, 0, 0, "p1 release"),
	P1(20700, PAD_A, 0, 0, "p1 A"), P1(20950, 0, 0, 0, "p1 release"),
	P1(21400, PAD_A, 0, 0, "p1 A"), P1(21650, 0, 0, 0, "p1 release"),
	P1(22100, PAD_A, 0, 0, "p1 A"), P1(22350, 0, 0, 0, "p1 release"),
	P1(22800, PAD_A, 0, 0, "p1 A"), P1(23050, 0, 0, 0, "p1 release"),
	P1(23500, PAD_A, 0, 0, "p1 A"), P1(23750, 0, 0, 0, "p1 release"),
	P1(24200, PAD_A, 0, 0, "p1 A"), P1(24450, 0, 0, 0, "p1 release"),
	P1(24900, PAD_A, 0, 0, "p1 A"), P1(25150, 0, 0, 0, "p1 release"),
	P1(25600, PAD_A, 0, 0, "p1 A"), P1(25850, 0, 0, 0, "p1 release"),
	P1(26300, PAD_A, 0, 0, "p1 A"), P1(26550, 0, 0, 0, "p1 release"),
	P1(27000, PAD_A, 0, 0, "p1 A"), P1(27250, 0, 0, 0, "p1 release"),
	P1(27700, PAD_A, 0, 0, "p1 A"), P1(27950, 0, 0, 0, "p1 release"),
	P1(28400, PAD_A, 0, 0, "p1 A"), P1(28650, 0, 0, 0, "p1 release"),
	P1(29100, PAD_A, 0, 0, "p1 A"), P1(29350, 0, 0, 0, "p1 release"),
	P1(29800, PAD_A, 0, 0, "p1 A"), P1(30050, 0, 0, 0, "p1 release"),
	P1(30500, PAD_A, 0, 0, "p1 A"), P1(30750, 0, 0, 0, "p1 release"),
	P1(31200, PAD_A, 0, 0, "p1 A"), P1(31450, 0, 0, 0, "p1 release"),
	P1(31900, PAD_A, 0, 0, "p1 A"), P1(32150, 0, 0, 0, "p1 release"),
	P1(32600, PAD_A, 0, 0, "p1 A"), P1(32850, 0, 0, 0, "p1 release"),
	P1(33300, PAD_A, 0, 0, "p1 A"), P1(33550, 0, 0, 0, "p1 release"),
	P1(34000, PAD_A, 0, 0, "p1 A"), P1(34250, 0, 0, 0, "p1 release"),
	P1(34700, PAD_A, 0, 0, "p1 A"), P1(34950, 0, 0, 0, "p1 release"),
	P1(35400, PAD_A, 0, 0, "p1 A"), P1(35650, 0, 0, 0, "p1 release"),
	P1(36100, PAD_A, 0, 0, "p1 A"), P1(36350, 0, 0, 0, "p1 release"),
	P1(36800, PAD_A, 0, 0, "p1 A"), P1(37050, 0, 0, 0, "p1 release"),
	P1(37500, PAD_A, 0, 0, "p1 A"), P1(37750, 0, 0, 0, "p1 release"),
	P1(38200, PAD_A, 0, 0, "p1 A"), P1(38450, 0, 0, 0, "p1 release"),
	P1(38900, PAD_A, 0, 0, "p1 A"), P1(39150, 0, 0, 0, "p1 release"),
	P1(39600, PAD_A, 0, 0, "p1 A"), P1(39850, 0, 0, 0, "p1 release"),
	P1(40300, PAD_A, 0, 0, "p1 A"), P1(40550, 0, 0, 0, "p1 release"),
	P1(41000, PAD_A, 0, 0, "p1 A"), P1(41250, 0, 0, 0, "p1 release"),
	P1(41700, PAD_A, 0, 0, "p1 A"), P1(41950, 0, 0, 0, "p1 release"),
	P1(42400, PAD_A, 0, 0, "p1 A"), P1(42650, 0, 0, 0, "p1 release"),
	P1(43100, PAD_A, 0, 0, "p1 A"), P1(43350, 0, 0, 0, "p1 release"),
	P1(43800, PAD_A, 0, 0, "p1 A"), P1(44050, 0, 0, 0, "p1 release"),
	P1(44500, PAD_A, 0, 0, "p1 A"), P1(44750, 0, 0, 0, "p1 release"),
	P1(45200, PAD_A, 0, 0, "p1 A"), P1(45450, 0, 0, 0, "p1 release"),
	P1(45900, PAD_A, 0, 0, "p1 A"), P1(46150, 0, 0, 0, "p1 release"),
	P1(46600, PAD_A, 0, 0, "p1 A"), P1(46850, 0, 0, 0, "p1 release"),
	P1(47300, PAD_A, 0, 0, "p1 A"), P1(47550, 0, 0, 0, "p1 release"),
	P1(48000, PAD_A, 0, 0, "p1 A"), P1(48250, 0, 0, 0, "p1 release"),
	P1(48700, PAD_A, 0, 0, "p1 A"), P1(48950, 0, 0, 0, "p1 release"),
	P1(49400, PAD_A, 0, 0, "p1 A"), P1(49650, 0, 0, 0, "p1 release"),
	SHOT(30000, "beam28_2"), SHOT(42000, "beam28_3"), SHOT(50000, "beam28_4"),
	END(52000),
};

// beam28b: beam28 with the lead switched to Asterix and back after the cutscene
// (the user's R3 + X): a switch sets the other hero's order to 0 -- the
// pillar's wait (3) gone, the beam's catch (trio 0x0C02, the companion with
// order 3) took nobody.
const Step kBeam28B[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 8),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2000, HERO_ASTERIX, 16.913f, 51.4f, -133.0f),
	GLIDEAT(2300, HERO_ASTERIX, 16.913f, 51.4f, -127.5f, 1000),
	PLACEAT(4000, HERO_OBELIX, 37.923f, 51.5f, -120.156f),
	PLACEAT(4200, HERO_ASTERIX, 42.057f, 57.5f, -120.097f),
	PLACEAT(6000, HERO_OBELIX, 38.0f, 57.5f, -92.0f),
	PLACEAT(6000, HERO_ASTERIX, 41.0f, 58.5f, -89.613f),
	SHOT(9000, "beam28b_1"),
	{ 16500, S_TRIOEV, 0x4800, 0, 0, 0, 0, "asterix leads (cheat)" },
	{ 16800, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (cheat): Asterix's wait order gone" },
	{ 16000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 19000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 22000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 25000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 28000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 31000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 34000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 37000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 40000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	{ 43000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile (on ice)" },
	VPAD(17000, 1, PAD_X, 0, 0, "p2 X"), VPAD(17150, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X"), VPAD(18150, 1, 0, 0, 0, "p2 release"),
	VPAD(18500, 1, PAD_X, 0, 0, "p2 X"), VPAD(18650, 1, 0, 0, 0, "p2 release"),
	VPAD(19000, 1, PAD_X, 0, 0, "p2 X"), VPAD(19150, 1, 0, 0, 0, "p2 release"),
	VPAD(19500, 1, PAD_X, 0, 0, "p2 X"), VPAD(19650, 1, 0, 0, 0, "p2 release"),
	VPAD(20000, 1, PAD_X, 0, 0, "p2 X"), VPAD(20150, 1, 0, 0, 0, "p2 release"),
	VPAD(20500, 1, PAD_X, 0, 0, "p2 X"), VPAD(20650, 1, 0, 0, 0, "p2 release"),
	VPAD(21000, 1, PAD_X, 0, 0, "p2 X"), VPAD(21150, 1, 0, 0, 0, "p2 release"),
	VPAD(21500, 1, PAD_X, 0, 0, "p2 X"), VPAD(21650, 1, 0, 0, 0, "p2 release"),
	VPAD(22000, 1, PAD_X, 0, 0, "p2 X"), VPAD(22150, 1, 0, 0, 0, "p2 release"),
	VPAD(22500, 1, PAD_X, 0, 0, "p2 X"), VPAD(22650, 1, 0, 0, 0, "p2 release"),
	VPAD(23000, 1, PAD_X, 0, 0, "p2 X"), VPAD(23150, 1, 0, 0, 0, "p2 release"),
	VPAD(23500, 1, PAD_X, 0, 0, "p2 X"), VPAD(23650, 1, 0, 0, 0, "p2 release"),
	VPAD(24000, 1, PAD_X, 0, 0, "p2 X"), VPAD(24150, 1, 0, 0, 0, "p2 release"),
	VPAD(24500, 1, PAD_X, 0, 0, "p2 X"), VPAD(24650, 1, 0, 0, 0, "p2 release"),
	VPAD(25000, 1, PAD_X, 0, 0, "p2 X"), VPAD(25150, 1, 0, 0, 0, "p2 release"),
	VPAD(25500, 1, PAD_X, 0, 0, "p2 X"), VPAD(25650, 1, 0, 0, 0, "p2 release"),
	VPAD(26000, 1, PAD_X, 0, 0, "p2 X"), VPAD(26150, 1, 0, 0, 0, "p2 release"),
	VPAD(26500, 1, PAD_X, 0, 0, "p2 X"), VPAD(26650, 1, 0, 0, 0, "p2 release"),
	VPAD(27000, 1, PAD_X, 0, 0, "p2 X"), VPAD(27150, 1, 0, 0, 0, "p2 release"),
	VPAD(27500, 1, PAD_X, 0, 0, "p2 X"), VPAD(27650, 1, 0, 0, 0, "p2 release"),
	VPAD(28000, 1, PAD_X, 0, 0, "p2 X"), VPAD(28150, 1, 0, 0, 0, "p2 release"),
	VPAD(28500, 1, PAD_X, 0, 0, "p2 X"), VPAD(28650, 1, 0, 0, 0, "p2 release"),
	VPAD(29000, 1, PAD_X, 0, 0, "p2 X"), VPAD(29150, 1, 0, 0, 0, "p2 release"),
	VPAD(29500, 1, PAD_X, 0, 0, "p2 X"), VPAD(29650, 1, 0, 0, 0, "p2 release"),
	VPAD(30000, 1, PAD_X, 0, 0, "p2 X"), VPAD(30150, 1, 0, 0, 0, "p2 release"),
	VPAD(30500, 1, PAD_X, 0, 0, "p2 X"), VPAD(30650, 1, 0, 0, 0, "p2 release"),
	VPAD(31000, 1, PAD_X, 0, 0, "p2 X"), VPAD(31150, 1, 0, 0, 0, "p2 release"),
	VPAD(31500, 1, PAD_X, 0, 0, "p2 X"), VPAD(31650, 1, 0, 0, 0, "p2 release"),
	VPAD(32000, 1, PAD_X, 0, 0, "p2 X"), VPAD(32150, 1, 0, 0, 0, "p2 release"),
	VPAD(32500, 1, PAD_X, 0, 0, "p2 X"), VPAD(32650, 1, 0, 0, 0, "p2 release"),
	VPAD(33000, 1, PAD_X, 0, 0, "p2 X"), VPAD(33150, 1, 0, 0, 0, "p2 release"),
	VPAD(33500, 1, PAD_X, 0, 0, "p2 X"), VPAD(33650, 1, 0, 0, 0, "p2 release"),
	VPAD(34000, 1, PAD_X, 0, 0, "p2 X"), VPAD(34150, 1, 0, 0, 0, "p2 release"),
	VPAD(34500, 1, PAD_X, 0, 0, "p2 X"), VPAD(34650, 1, 0, 0, 0, "p2 release"),
	VPAD(35000, 1, PAD_X, 0, 0, "p2 X"), VPAD(35150, 1, 0, 0, 0, "p2 release"),
	VPAD(35500, 1, PAD_X, 0, 0, "p2 X"), VPAD(35650, 1, 0, 0, 0, "p2 release"),
	VPAD(36000, 1, PAD_X, 0, 0, "p2 X"), VPAD(36150, 1, 0, 0, 0, "p2 release"),
	VPAD(36500, 1, PAD_X, 0, 0, "p2 X"), VPAD(36650, 1, 0, 0, 0, "p2 release"),
	VPAD(37000, 1, PAD_X, 0, 0, "p2 X"), VPAD(37150, 1, 0, 0, 0, "p2 release"),
	VPAD(37500, 1, PAD_X, 0, 0, "p2 X"), VPAD(37650, 1, 0, 0, 0, "p2 release"),
	VPAD(38000, 1, PAD_X, 0, 0, "p2 X"), VPAD(38150, 1, 0, 0, 0, "p2 release"),
	VPAD(38500, 1, PAD_X, 0, 0, "p2 X"), VPAD(38650, 1, 0, 0, 0, "p2 release"),
	VPAD(39000, 1, PAD_X, 0, 0, "p2 X"), VPAD(39150, 1, 0, 0, 0, "p2 release"),
	VPAD(39500, 1, PAD_X, 0, 0, "p2 X"), VPAD(39650, 1, 0, 0, 0, "p2 release"),
	VPAD(40000, 1, PAD_X, 0, 0, "p2 X"), VPAD(40150, 1, 0, 0, 0, "p2 release"),
	VPAD(40500, 1, PAD_X, 0, 0, "p2 X"), VPAD(40650, 1, 0, 0, 0, "p2 release"),
	VPAD(41000, 1, PAD_X, 0, 0, "p2 X"), VPAD(41150, 1, 0, 0, 0, "p2 release"),
	VPAD(41500, 1, PAD_X, 0, 0, "p2 X"), VPAD(41650, 1, 0, 0, 0, "p2 release"),
	VPAD(42000, 1, PAD_X, 0, 0, "p2 X"), VPAD(42150, 1, 0, 0, 0, "p2 release"),
	VPAD(42500, 1, PAD_X, 0, 0, "p2 X"), VPAD(42650, 1, 0, 0, 0, "p2 release"),
	VPAD(43000, 1, PAD_X, 0, 0, "p2 X"), VPAD(43150, 1, 0, 0, 0, "p2 release"),
	VPAD(43500, 1, PAD_X, 0, 0, "p2 X"), VPAD(43650, 1, 0, 0, 0, "p2 release"),
	VPAD(44000, 1, PAD_X, 0, 0, "p2 X"), VPAD(44150, 1, 0, 0, 0, "p2 release"),
	VPAD(44500, 1, PAD_X, 0, 0, "p2 X"), VPAD(44650, 1, 0, 0, 0, "p2 release"),
	VPAD(45000, 1, PAD_X, 0, 0, "p2 X"), VPAD(45150, 1, 0, 0, 0, "p2 release"),
	VPAD(45500, 1, PAD_X, 0, 0, "p2 X"), VPAD(45650, 1, 0, 0, 0, "p2 release"),
	P1(20000, PAD_A, 0, 0, "p1 A"), P1(20250, 0, 0, 0, "p1 release"),
	P1(20700, PAD_A, 0, 0, "p1 A"), P1(20950, 0, 0, 0, "p1 release"),
	P1(21400, PAD_A, 0, 0, "p1 A"), P1(21650, 0, 0, 0, "p1 release"),
	P1(22100, PAD_A, 0, 0, "p1 A"), P1(22350, 0, 0, 0, "p1 release"),
	P1(22800, PAD_A, 0, 0, "p1 A"), P1(23050, 0, 0, 0, "p1 release"),
	P1(23500, PAD_A, 0, 0, "p1 A"), P1(23750, 0, 0, 0, "p1 release"),
	P1(24200, PAD_A, 0, 0, "p1 A"), P1(24450, 0, 0, 0, "p1 release"),
	P1(24900, PAD_A, 0, 0, "p1 A"), P1(25150, 0, 0, 0, "p1 release"),
	P1(25600, PAD_A, 0, 0, "p1 A"), P1(25850, 0, 0, 0, "p1 release"),
	P1(26300, PAD_A, 0, 0, "p1 A"), P1(26550, 0, 0, 0, "p1 release"),
	P1(27000, PAD_A, 0, 0, "p1 A"), P1(27250, 0, 0, 0, "p1 release"),
	P1(27700, PAD_A, 0, 0, "p1 A"), P1(27950, 0, 0, 0, "p1 release"),
	P1(28400, PAD_A, 0, 0, "p1 A"), P1(28650, 0, 0, 0, "p1 release"),
	P1(29100, PAD_A, 0, 0, "p1 A"), P1(29350, 0, 0, 0, "p1 release"),
	P1(29800, PAD_A, 0, 0, "p1 A"), P1(30050, 0, 0, 0, "p1 release"),
	P1(30500, PAD_A, 0, 0, "p1 A"), P1(30750, 0, 0, 0, "p1 release"),
	P1(31200, PAD_A, 0, 0, "p1 A"), P1(31450, 0, 0, 0, "p1 release"),
	P1(31900, PAD_A, 0, 0, "p1 A"), P1(32150, 0, 0, 0, "p1 release"),
	P1(32600, PAD_A, 0, 0, "p1 A"), P1(32850, 0, 0, 0, "p1 release"),
	P1(33300, PAD_A, 0, 0, "p1 A"), P1(33550, 0, 0, 0, "p1 release"),
	P1(34000, PAD_A, 0, 0, "p1 A"), P1(34250, 0, 0, 0, "p1 release"),
	P1(34700, PAD_A, 0, 0, "p1 A"), P1(34950, 0, 0, 0, "p1 release"),
	P1(35400, PAD_A, 0, 0, "p1 A"), P1(35650, 0, 0, 0, "p1 release"),
	P1(36100, PAD_A, 0, 0, "p1 A"), P1(36350, 0, 0, 0, "p1 release"),
	P1(36800, PAD_A, 0, 0, "p1 A"), P1(37050, 0, 0, 0, "p1 release"),
	P1(37500, PAD_A, 0, 0, "p1 A"), P1(37750, 0, 0, 0, "p1 release"),
	P1(38200, PAD_A, 0, 0, "p1 A"), P1(38450, 0, 0, 0, "p1 release"),
	P1(38900, PAD_A, 0, 0, "p1 A"), P1(39150, 0, 0, 0, "p1 release"),
	P1(39600, PAD_A, 0, 0, "p1 A"), P1(39850, 0, 0, 0, "p1 release"),
	P1(40300, PAD_A, 0, 0, "p1 A"), P1(40550, 0, 0, 0, "p1 release"),
	P1(41000, PAD_A, 0, 0, "p1 A"), P1(41250, 0, 0, 0, "p1 release"),
	P1(41700, PAD_A, 0, 0, "p1 A"), P1(41950, 0, 0, 0, "p1 release"),
	P1(42400, PAD_A, 0, 0, "p1 A"), P1(42650, 0, 0, 0, "p1 release"),
	P1(43100, PAD_A, 0, 0, "p1 A"), P1(43350, 0, 0, 0, "p1 release"),
	P1(43800, PAD_A, 0, 0, "p1 A"), P1(44050, 0, 0, 0, "p1 release"),
	P1(44500, PAD_A, 0, 0, "p1 A"), P1(44750, 0, 0, 0, "p1 release"),
	P1(45200, PAD_A, 0, 0, "p1 A"), P1(45450, 0, 0, 0, "p1 release"),
	P1(45900, PAD_A, 0, 0, "p1 A"), P1(46150, 0, 0, 0, "p1 release"),
	P1(46600, PAD_A, 0, 0, "p1 A"), P1(46850, 0, 0, 0, "p1 release"),
	P1(47300, PAD_A, 0, 0, "p1 A"), P1(47550, 0, 0, 0, "p1 release"),
	P1(48000, PAD_A, 0, 0, "p1 A"), P1(48250, 0, 0, 0, "p1 release"),
	P1(48700, PAD_A, 0, 0, "p1 A"), P1(48950, 0, 0, 0, "p1 release"),
	P1(49400, PAD_A, 0, 0, "p1 A"), P1(49650, 0, 0, 0, "p1 release"),
	SHOT(30000, "beam28b_2"), SHOT(42000, "beam28b_3"), SHOT(50000, "beam28b_4"),
	END(52000),
};

// corklead21 (level 2): turnstile21 with Obelix leading (4801 first): Asterix
// on the spinning platform's circle sets it off all the same (its event 0050
// makes him wait); Obelix turns it, Asterix rides to the top (0058).
const Step kCorkLead21[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	{ 1600, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(5000, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(5300, HERO_OBELIX, -744.5f, 515.6f, -977.8f, 1500),
	GLIDEAT(6900, HERO_OBELIX, -775.878f, 515.614f, -990.0f, 2000),
	GLIDEAT(9000, HERO_OBELIX, -775.878f, 515.614f, -983.604f, 600),
	SHOT(9500, "corklead21_1"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X"), VPAD(10150, 1, 0, 0, 0, "p2 release"),
	VPAD(10500, 1, PAD_X, 0, 0, "p2 X"), VPAD(10650, 1, 0, 0, 0, "p2 release"),
	VPAD(11000, 1, PAD_X, 0, 0, "p2 X"), VPAD(11150, 1, 0, 0, 0, "p2 release"),
	VPAD(11500, 1, PAD_X, 0, 0, "p2 X"), VPAD(11650, 1, 0, 0, 0, "p2 release"),
	VPAD(12000, 1, PAD_X, 0, 0, "p2 X"), VPAD(12150, 1, 0, 0, 0, "p2 release"),
	VPAD(12500, 1, PAD_X, 0, 0, "p2 X"), VPAD(12650, 1, 0, 0, 0, "p2 release"),
	VPAD(13000, 1, PAD_X, 0, 0, "p2 X"), VPAD(13150, 1, 0, 0, 0, "p2 release"),
	VPAD(13500, 1, PAD_X, 0, 0, "p2 X"), VPAD(13650, 1, 0, 0, 0, "p2 release"),
	VPAD(14000, 1, PAD_X, 0, 0, "p2 X"), VPAD(14150, 1, 0, 0, 0, "p2 release"),
	VPAD(14500, 1, PAD_X, 0, 0, "p2 X"), VPAD(14650, 1, 0, 0, 0, "p2 release"),
	VPAD(15000, 1, PAD_X, 0, 0, "p2 X"), VPAD(15150, 1, 0, 0, 0, "p2 release"),
	VPAD(15500, 1, PAD_X, 0, 0, "p2 X"), VPAD(15650, 1, 0, 0, 0, "p2 release"),
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X"), VPAD(16150, 1, 0, 0, 0, "p2 release"),
	VPAD(16500, 1, PAD_X, 0, 0, "p2 X"), VPAD(16650, 1, 0, 0, 0, "p2 release"),
	VPAD(17000, 1, PAD_X, 0, 0, "p2 X"), VPAD(17150, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X"), VPAD(18150, 1, 0, 0, 0, "p2 release"),
	SHOT(19000, "corklead21_2"),
	END(24000),
};

// corkobelix21 (level 2): Obelix leads and walks onto the spinning platform's
// circle, Asterix 3 units off: the circle (made for Asterix: "Obelix leads,
// Asterix waits") must not go off for him.
const Step kCorkObelix21[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	{ 1600, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	GLIDEAT(1800, HERO_ASTERIX, -775.0f, 517.0f, -980.0f, 1500),
	GLIDEAT(1800, HERO_OBELIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_OBELIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(6000, "corkobelix21_1"),
	P1(7000, 0, -1.0f, 0, "p1 walks off"), P1(8500, 0, 0, 0, "p1 stops"),
	SHOT(9500, "corkobelix21_2"),
	END(10500),
};

// turnstile21 (level 2, XXL_LIFTLOG=1): the spinning platform as meant -- Asterix
// on its circle, Obelix through zone 03C8 to the turnstile (the user's spot) and
// X again and again: the platform rises with Asterix, nothing set back.
const Step kTurnstile21[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(5000, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(5300, HERO_OBELIX, -744.5f, 515.6f, -977.8f, 1500),   // through zone 03C8 (as lifterlock2)
	GLIDEAT(6900, HERO_OBELIX, -775.878f, 515.614f, -990.0f, 2000),
	GLIDEAT(9000, HERO_OBELIX, -775.878f, 515.614f, -983.604f, 600),   // the user's spot, walking towards the turnstile
	SHOT(9500, "turnstile21_1"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X"), VPAD(10150, 1, 0, 0, 0, "p2 release"),
	VPAD(10500, 1, PAD_X, 0, 0, "p2 X"), VPAD(10650, 1, 0, 0, 0, "p2 release"),
	VPAD(11000, 1, PAD_X, 0, 0, "p2 X"), VPAD(11150, 1, 0, 0, 0, "p2 release"),
	VPAD(11500, 1, PAD_X, 0, 0, "p2 X"), VPAD(11650, 1, 0, 0, 0, "p2 release"),
	VPAD(12000, 1, PAD_X, 0, 0, "p2 X"), VPAD(12150, 1, 0, 0, 0, "p2 release"),
	VPAD(12500, 1, PAD_X, 0, 0, "p2 X"), VPAD(12650, 1, 0, 0, 0, "p2 release"),
	VPAD(13000, 1, PAD_X, 0, 0, "p2 X"), VPAD(13150, 1, 0, 0, 0, "p2 release"),
	VPAD(13500, 1, PAD_X, 0, 0, "p2 X"), VPAD(13650, 1, 0, 0, 0, "p2 release"),
	SHOT(14000, "turnstile21_2"),
	VPAD(14500, 1, PAD_X, 0, 0, "p2 X"), VPAD(14650, 1, 0, 0, 0, "p2 release"),
	VPAD(15000, 1, PAD_X, 0, 0, "p2 X"), VPAD(15150, 1, 0, 0, 0, "p2 release"),
	VPAD(15500, 1, PAD_X, 0, 0, "p2 X"), VPAD(15650, 1, 0, 0, 0, "p2 release"),
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X"), VPAD(16150, 1, 0, 0, 0, "p2 release"),
	VPAD(16500, 1, PAD_X, 0, 0, "p2 X"), VPAD(16650, 1, 0, 0, 0, "p2 release"),
	VPAD(17000, 1, PAD_X, 0, 0, "p2 X"), VPAD(17150, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X"), VPAD(18150, 1, 0, 0, 0, "p2 release"),
	SHOT(19000, "turnstile21_3"),
	{ 20500, S_UNLOCKHERO, HERO_OBELIX, 0, 0, 0, 0, "R3 + B on Obelix walking to his marker (0058)" },
	SHOT(23000, "turnstile21_4"),
	END(24000),
};

// turnstile21c (level 2, XXL_LIFTLOG=1): turnstile21 to the platform's top (0058: Obelix walks to
// his marker, the level gives the lead back and forth), then Asterix walks far off --
// nothing may be "set back" (the walk's lock is no platform sequence) and Asterix keeps the lead.
const Step kTurnstile21c[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(5000, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(5300, HERO_OBELIX, -744.5f, 515.6f, -977.8f, 1500),   // through zone 03C8 (as lifterlock2)
	GLIDEAT(6900, HERO_OBELIX, -775.878f, 515.614f, -990.0f, 2000),
	GLIDEAT(9000, HERO_OBELIX, -775.878f, 515.614f, -983.604f, 600),   // the user's spot, walking towards the turnstile
	SHOT(9500, "turnstile21c_1"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X"), VPAD(10150, 1, 0, 0, 0, "p2 release"),
	VPAD(10500, 1, PAD_X, 0, 0, "p2 X"), VPAD(10650, 1, 0, 0, 0, "p2 release"),
	VPAD(11000, 1, PAD_X, 0, 0, "p2 X"), VPAD(11150, 1, 0, 0, 0, "p2 release"),
	VPAD(11500, 1, PAD_X, 0, 0, "p2 X"), VPAD(11650, 1, 0, 0, 0, "p2 release"),
	VPAD(12000, 1, PAD_X, 0, 0, "p2 X"), VPAD(12150, 1, 0, 0, 0, "p2 release"),
	VPAD(12500, 1, PAD_X, 0, 0, "p2 X"), VPAD(12650, 1, 0, 0, 0, "p2 release"),
	VPAD(13000, 1, PAD_X, 0, 0, "p2 X"), VPAD(13150, 1, 0, 0, 0, "p2 release"),
	VPAD(13500, 1, PAD_X, 0, 0, "p2 X"), VPAD(13650, 1, 0, 0, 0, "p2 release"),
	SHOT(14000, "turnstile21c_2"),
	VPAD(14500, 1, PAD_X, 0, 0, "p2 X"), VPAD(14650, 1, 0, 0, 0, "p2 release"),
	VPAD(15000, 1, PAD_X, 0, 0, "p2 X"), VPAD(15150, 1, 0, 0, 0, "p2 release"),
	VPAD(15500, 1, PAD_X, 0, 0, "p2 X"), VPAD(15650, 1, 0, 0, 0, "p2 release"),
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X"), VPAD(16150, 1, 0, 0, 0, "p2 release"),
	VPAD(16500, 1, PAD_X, 0, 0, "p2 X"), VPAD(16650, 1, 0, 0, 0, "p2 release"),
	VPAD(17000, 1, PAD_X, 0, 0, "p2 X"), VPAD(17150, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X"), VPAD(18150, 1, 0, 0, 0, "p2 release"),
	SHOT(19000, "turnstile21c_3"),
	SHOT(30000, "turnstile21c_1"),
	PLACEAT(31000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(31300, HERO_ASTERIX, -745.0f, 514.5f, -981.0f, 2500),   // far from the platform (the user: on to sector 2)
	SHOT(38000, "turnstile21c_2"),
	END(40000),
};

// turnstile21d (level 2): the user's LV2-1 run -- Obelix reaches the turnstile
// without zone 03C8 (so Asterix's circle lock never ends), the platform's ride to
// its top; later the level sends 4801 + 4803 (Obelix leads -- after the top's
// agent in the user's log) and Obelix walks off to the iron crates: nothing may
// be "set back", Obelix keeps the lead.
const Step kTurnstile21d[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(1500, HERO_OBELIX, -747.36f, 514.6f, -977.79f),   // (the user's: already there, so not through 03C8)
	GLIDEAT(5000, HERO_OBELIX, -774.59f, 515.51f, -981.24f, 4000),
	GLIDEAT(9200, HERO_OBELIX, -775.878f, 515.614f, -983.604f, 600),   // the turnstile spot
	SHOT(9500, "turnstile21d_1"),
	VPAD(10000, 1, PAD_X, 0, 0, "p2 X"), VPAD(10150, 1, 0, 0, 0, "p2 release"),
	VPAD(10500, 1, PAD_X, 0, 0, "p2 X"), VPAD(10650, 1, 0, 0, 0, "p2 release"),
	VPAD(11000, 1, PAD_X, 0, 0, "p2 X"), VPAD(11150, 1, 0, 0, 0, "p2 release"),
	VPAD(11500, 1, PAD_X, 0, 0, "p2 X"), VPAD(11650, 1, 0, 0, 0, "p2 release"),
	VPAD(12000, 1, PAD_X, 0, 0, "p2 X"), VPAD(12150, 1, 0, 0, 0, "p2 release"),
	VPAD(12500, 1, PAD_X, 0, 0, "p2 X"), VPAD(12650, 1, 0, 0, 0, "p2 release"),
	VPAD(13000, 1, PAD_X, 0, 0, "p2 X"), VPAD(13150, 1, 0, 0, 0, "p2 release"),
	VPAD(13500, 1, PAD_X, 0, 0, "p2 X"), VPAD(13650, 1, 0, 0, 0, "p2 release"),
	VPAD(14000, 1, PAD_X, 0, 0, "p2 X"), VPAD(14150, 1, 0, 0, 0, "p2 release"),
	VPAD(14500, 1, PAD_X, 0, 0, "p2 X"), VPAD(14650, 1, 0, 0, 0, "p2 release"),
	VPAD(15000, 1, PAD_X, 0, 0, "p2 X"), VPAD(15150, 1, 0, 0, 0, "p2 release"),
	VPAD(15500, 1, PAD_X, 0, 0, "p2 X"), VPAD(15650, 1, 0, 0, 0, "p2 release"),
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X"), VPAD(16150, 1, 0, 0, 0, "p2 release"),
	VPAD(16500, 1, PAD_X, 0, 0, "p2 X"), VPAD(16650, 1, 0, 0, 0, "p2 release"),
	VPAD(17000, 1, PAD_X, 0, 0, "p2 X"), VPAD(17150, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X"), VPAD(18150, 1, 0, 0, 0, "p2 release"),
	SHOT(19000, "turnstile21d_2"),
	{ 26000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (the level)" },
	{ 26000, S_TRIOEV, 0x4803, 0, 0, 0, 0, "sidekick free (the level)" },
	GLIDEAT(27000, HERO_OBELIX, -800.0f, 516.0f, -1000.0f, 2000),
	GLIDEAT(29500, HERO_OBELIX, -815.0f, 516.0f, -1015.0f, 2000),
	SHOT(40000, "turnstile21d_3"),
	END(42000),
};

// turnstile21e (level 2, XXL_DOWNAT=1:6000 XXL_UPAT=9000): the user's whole LV2-1
// run -- Asterix on the circle (locked), Obelix goes down (the lock undone) and is
// revived, Asterix back on the circle; then turnstile21d: Obelix to the turnstile
// from the east (not through 03C8), the ride to the top, the keg's 4801 + 4803,
// Obelix off to the iron crates -- nothing set back, Obelix keeps the lead.
const Step kTurnstile21e[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	PLACEAT(1500, HERO_OBELIX, -747.36f, 514.6f, -977.79f),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	GLIDEAT(11000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f, 1500),
	GLIDEAT(13000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(14700, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	GLIDEAT(16500, HERO_OBELIX, -774.59f, 515.51f, -981.24f, 4000),
	GLIDEAT(20700, HERO_OBELIX, -775.878f, 515.614f, -983.604f, 600),
	SHOT(21000, "turnstile21e_1"),
	VPAD(21500, 1, PAD_X, 0, 0, "p2 X"), VPAD(21650, 1, 0, 0, 0, "p2 release"),
	VPAD(22000, 1, PAD_X, 0, 0, "p2 X"), VPAD(22150, 1, 0, 0, 0, "p2 release"),
	VPAD(22500, 1, PAD_X, 0, 0, "p2 X"), VPAD(22650, 1, 0, 0, 0, "p2 release"),
	VPAD(23000, 1, PAD_X, 0, 0, "p2 X"), VPAD(23150, 1, 0, 0, 0, "p2 release"),
	VPAD(23500, 1, PAD_X, 0, 0, "p2 X"), VPAD(23650, 1, 0, 0, 0, "p2 release"),
	VPAD(24000, 1, PAD_X, 0, 0, "p2 X"), VPAD(24150, 1, 0, 0, 0, "p2 release"),
	VPAD(24500, 1, PAD_X, 0, 0, "p2 X"), VPAD(24650, 1, 0, 0, 0, "p2 release"),
	VPAD(25000, 1, PAD_X, 0, 0, "p2 X"), VPAD(25150, 1, 0, 0, 0, "p2 release"),
	VPAD(25500, 1, PAD_X, 0, 0, "p2 X"), VPAD(25650, 1, 0, 0, 0, "p2 release"),
	VPAD(26000, 1, PAD_X, 0, 0, "p2 X"), VPAD(26150, 1, 0, 0, 0, "p2 release"),
	VPAD(26500, 1, PAD_X, 0, 0, "p2 X"), VPAD(26650, 1, 0, 0, 0, "p2 release"),
	VPAD(27000, 1, PAD_X, 0, 0, "p2 X"), VPAD(27150, 1, 0, 0, 0, "p2 release"),
	VPAD(27500, 1, PAD_X, 0, 0, "p2 X"), VPAD(27650, 1, 0, 0, 0, "p2 release"),
	VPAD(28000, 1, PAD_X, 0, 0, "p2 X"), VPAD(28150, 1, 0, 0, 0, "p2 release"),
	VPAD(28500, 1, PAD_X, 0, 0, "p2 X"), VPAD(28650, 1, 0, 0, 0, "p2 release"),
	VPAD(29000, 1, PAD_X, 0, 0, "p2 X"), VPAD(29150, 1, 0, 0, 0, "p2 release"),
	VPAD(29500, 1, PAD_X, 0, 0, "p2 X"), VPAD(29650, 1, 0, 0, 0, "p2 release"),
	VPAD(30000, 1, PAD_X, 0, 0, "p2 X"), VPAD(30150, 1, 0, 0, 0, "p2 release"),
	VPAD(30500, 1, PAD_X, 0, 0, "p2 X"), VPAD(30650, 1, 0, 0, 0, "p2 release"),
	VPAD(31000, 1, PAD_X, 0, 0, "p2 X"), VPAD(31150, 1, 0, 0, 0, "p2 release"),
	VPAD(31500, 1, PAD_X, 0, 0, "p2 X"), VPAD(31650, 1, 0, 0, 0, "p2 release"),
	VPAD(32000, 1, PAD_X, 0, 0, "p2 X"), VPAD(32150, 1, 0, 0, 0, "p2 release"),
	VPAD(32500, 1, PAD_X, 0, 0, "p2 X"), VPAD(32650, 1, 0, 0, 0, "p2 release"),
	VPAD(33000, 1, PAD_X, 0, 0, "p2 X"), VPAD(33150, 1, 0, 0, 0, "p2 release"),
	VPAD(33500, 1, PAD_X, 0, 0, "p2 X"), VPAD(33650, 1, 0, 0, 0, "p2 release"),
	VPAD(34000, 1, PAD_X, 0, 0, "p2 X"), VPAD(34150, 1, 0, 0, 0, "p2 release"),
	SHOT(35000, "turnstile21e_2"),
	GLIDEAT(36000, HERO_ASTERIX, -782.8f, 518.5f, -992.67f, 1500),   // down to the keg (the user's spot)
	{ 38000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (the keg)" },
	{ 38000, S_TRIOEV, 0x4803, 0, 0, 0, 0, "sidekick free (the keg)" },
	GLIDEAT(39000, HERO_OBELIX, -800.0f, 516.0f, -1000.0f, 2000),
	GLIDEAT(41500, HERO_OBELIX, -815.0f, 516.0f, -1015.0f, 2000),
	SHOT(50000, "turnstile21e_3"),
	END(52000),
};

// turnstile21b (level 2, XXL_LIFTLOG=1): the user's cycle -- Asterix locked, Obelix
// walks off east through 03C8 (set back 3 s later, Asterix locked again), then
// comes back to the turnstile and spins it: the ride's zone 0528 and its camera.
const Step kTurnstile21b[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(5000, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(5300, HERO_OBELIX, -744.5f, 515.6f, -977.8f, 1500),   // through 03C8, then stays off
	SHOT(11000, "turnstile21b_1"),
	GLIDEAT(12000, HERO_OBELIX, -775.878f, 515.614f, -990.0f, 2500),
	GLIDEAT(14600, HERO_OBELIX, -775.878f, 515.614f, -983.604f, 600),
	VPAD(15500, 1, PAD_X, 0, 0, "p2 X"), VPAD(15650, 1, 0, 0, 0, "p2 release"),
	VPAD(16000, 1, PAD_X, 0, 0, "p2 X"), VPAD(16150, 1, 0, 0, 0, "p2 release"),
	VPAD(16500, 1, PAD_X, 0, 0, "p2 X"), VPAD(16650, 1, 0, 0, 0, "p2 release"),
	VPAD(17000, 1, PAD_X, 0, 0, "p2 X"), VPAD(17150, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(18000, 1, PAD_X, 0, 0, "p2 X"), VPAD(18150, 1, 0, 0, 0, "p2 release"),
	VPAD(18500, 1, PAD_X, 0, 0, "p2 X"), VPAD(18650, 1, 0, 0, 0, "p2 release"),
	VPAD(19000, 1, PAD_X, 0, 0, "p2 X"), VPAD(19150, 1, 0, 0, 0, "p2 release"),
	SHOT(19500, "turnstile21b_2"),
	VPAD(19500, 1, PAD_X, 0, 0, "p2 X"), VPAD(19650, 1, 0, 0, 0, "p2 release"),
	VPAD(20000, 1, PAD_X, 0, 0, "p2 X"), VPAD(20150, 1, 0, 0, 0, "p2 release"),
	VPAD(20500, 1, PAD_X, 0, 0, "p2 X"), VPAD(20650, 1, 0, 0, 0, "p2 release"),
	VPAD(21000, 1, PAD_X, 0, 0, "p2 X"), VPAD(21150, 1, 0, 0, 0, "p2 release"),
	VPAD(21500, 1, PAD_X, 0, 0, "p2 X"), VPAD(21650, 1, 0, 0, 0, "p2 release"),
	VPAD(22000, 1, PAD_X, 0, 0, "p2 X"), VPAD(22150, 1, 0, 0, 0, "p2 release"),
	VPAD(22500, 1, PAD_X, 0, 0, "p2 X"), VPAD(22650, 1, 0, 0, 0, "p2 release"),
	VPAD(23000, 1, PAD_X, 0, 0, "p2 X"), VPAD(23150, 1, 0, 0, 0, "p2 release"),
	SHOT(24000, "turnstile21b_3"), SHOT(28000, "turnstile21b_4"),
	END(29000),
};

// lifterlock2 (level 2, XXL_LIFTLOG=1): lifterlock, but Obelix walks off through
// the turnstile's zone (03C8: Asterix freed, the platform switched off until its
// turn ends) and then onto the lifter -- the user's first session; then the
// circle must answer again.
const Step kLifterLock2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_ASTERIX, -746.697f, 506.5f, -992.08f), PLACEAT(1500, HERO_OBELIX, -746.697f, 506.5f, -990.5f),
	PLACEAT(9000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(9300, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(11000, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(12500, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(13000, HERO_OBELIX, -744.5f, 515.6f, -977.8f, 2000),
	SHOT(15500, "lifterlock2_1"),
	PLACEAT(19000, HERO_OBELIX, -746.697f, 517.0f, -992.08f),
	SHOT(21000, "lifterlock2_2"), SHOT(27000, "lifterlock2_3"),
	PLACEAT(29000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(29300, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(31000, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(33000, "lifterlock2_4"), SHOT(38000, "lifterlock2_5"),
	END(40000),
};

// beamundo (-Level 4 sector 2..., XXL_DOWNAT=1:7000): the hearth by the rotary
// beam lit, Asterix onto its blue circle (he waits for Obelix), Obelix falls --
// undone; the circle's group and what it changed are logged.
const Step kBeamUndo[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(600, 2),
	PLACEAT(3000, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3100, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	PLACEAT(3500, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3600, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	{ 3800, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4000, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 4300, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	PLACEAT(4600, HERO_ASTERIX, -378.5f, 3.0f, -119.07f),
	GLIDEAT(5000, HERO_ASTERIX, -381.93f, 2.5f, -119.07f, 600),
	P1(6200, PAD_X, 0, 0, "p1 X: light the hearth"), P1(6400, 0, 0, 0, "p1 release"),
	SHOT(7500, "beamundo_0"),
	GLIDEAT(8500, HERO_ASTERIX, -394.73f, 5.2f, -119.08f, 1000),
	SHOT(11000, "beamundo_1"), SHOT(13500, "beamundo_2"),
	PLACEAT(16000, HERO_ASTERIX, -381.93f, 3.0f, -119.07f),
	GLIDEAT(17000, HERO_ASTERIX, -394.73f, 5.2f, -119.08f, 1000),
	SHOT(20000, "beamundo_3"),
	END(21000),
};

// beam42 (-Level 4, XXL_LIFTLOG=1, XXL_NOLOCK=0/1): LV4-2's rotary beam -- Asterix lights
// the hearth, stands on the blue circle; player 2's Obelix turns the turnstile
// (the user's spot) again and again: the beam swings over Asterix (locked: the AI
// jumps up and hangs on it; nolock: player 1 must jump to it).
const Step kBeam42[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(600, 2),
	PLACEAT(3000, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3100, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	PLACEAT(3500, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3600, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	{ 3800, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4000, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 4300, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	PLACEAT(4600, HERO_ASTERIX, -378.5f, 3.0f, -119.07f),
	GLIDEAT(5000, HERO_ASTERIX, -381.93f, 2.5f, -119.07f, 600),
	P1(6200, PAD_X, 0, 0, "p1 X: light the hearth"), P1(6400, 0, 0, 0, "p1 release"),
	GLIDEAT(8000, HERO_ASTERIX, -394.734f, 5.2f, -119.076f, 1000),
	SHOT(9500, "beam42_0"),
	PLACEAT(10500, HERO_OBELIX, -409.0f, 2.8f, -116.645f),
	GLIDEAT(11000, HERO_OBELIX, -414.612f, 2.281f, -116.645f, 800),
	VPAD(13000, 1, PAD_X, 0, 0, "p2 X"), VPAD(13150, 1, 0, 0, 0, "p2 release"),
	VPAD(13450, 1, PAD_X, 0, 0, "p2 X"), VPAD(13600, 1, 0, 0, 0, "p2 release"),
	VPAD(13900, 1, PAD_X, 0, 0, "p2 X"), VPAD(14050, 1, 0, 0, 0, "p2 release"),
	VPAD(14350, 1, PAD_X, 0, 0, "p2 X"), VPAD(14500, 1, 0, 0, 0, "p2 release"),
	VPAD(14800, 1, PAD_X, 0, 0, "p2 X"), VPAD(14950, 1, 0, 0, 0, "p2 release"),
	VPAD(15250, 1, PAD_X, 0, 0, "p2 X"), VPAD(15400, 1, 0, 0, 0, "p2 release"),
	VPAD(15700, 1, PAD_X, 0, 0, "p2 X"), VPAD(15850, 1, 0, 0, 0, "p2 release"),
	VPAD(16150, 1, PAD_X, 0, 0, "p2 X"), VPAD(16300, 1, 0, 0, 0, "p2 release"),
	VPAD(16600, 1, PAD_X, 0, 0, "p2 X"), VPAD(16750, 1, 0, 0, 0, "p2 release"),
	VPAD(17050, 1, PAD_X, 0, 0, "p2 X"), VPAD(17200, 1, 0, 0, 0, "p2 release"),
	VPAD(17500, 1, PAD_X, 0, 0, "p2 X"), VPAD(17650, 1, 0, 0, 0, "p2 release"),
	VPAD(17950, 1, PAD_X, 0, 0, "p2 X"), VPAD(18100, 1, 0, 0, 0, "p2 release"),
	VPAD(18400, 1, PAD_X, 0, 0, "p2 X"), VPAD(18550, 1, 0, 0, 0, "p2 release"),
	VPAD(18850, 1, PAD_X, 0, 0, "p2 X"), VPAD(19000, 1, 0, 0, 0, "p2 release"),
	VPAD(19300, 1, PAD_X, 0, 0, "p2 X"), VPAD(19450, 1, 0, 0, 0, "p2 release"),
	VPAD(19750, 1, PAD_X, 0, 0, "p2 X"), VPAD(19900, 1, 0, 0, 0, "p2 release"),
	VPAD(20200, 1, PAD_X, 0, 0, "p2 X"), VPAD(20350, 1, 0, 0, 0, "p2 release"),
	VPAD(20650, 1, PAD_X, 0, 0, "p2 X"), VPAD(20800, 1, 0, 0, 0, "p2 release"),
	VPAD(21100, 1, PAD_X, 0, 0, "p2 X"), VPAD(21250, 1, 0, 0, 0, "p2 release"),
	VPAD(21550, 1, PAD_X, 0, 0, "p2 X"), VPAD(21700, 1, 0, 0, 0, "p2 release"),
	VPAD(22000, 1, PAD_X, 0, 0, "p2 X"), VPAD(22150, 1, 0, 0, 0, "p2 release"),
	VPAD(22450, 1, PAD_X, 0, 0, "p2 X"), VPAD(22600, 1, 0, 0, 0, "p2 release"),
	VPAD(22900, 1, PAD_X, 0, 0, "p2 X"), VPAD(23050, 1, 0, 0, 0, "p2 release"),
	VPAD(23350, 1, PAD_X, 0, 0, "p2 X"), VPAD(23500, 1, 0, 0, 0, "p2 release"),
	VPAD(23800, 1, PAD_X, 0, 0, "p2 X"), VPAD(23950, 1, 0, 0, 0, "p2 release"),
	VPAD(24250, 1, PAD_X, 0, 0, "p2 X"), VPAD(24400, 1, 0, 0, 0, "p2 release"),
	VPAD(24700, 1, PAD_X, 0, 0, "p2 X"), VPAD(24850, 1, 0, 0, 0, "p2 release"),
	VPAD(25150, 1, PAD_X, 0, 0, "p2 X"), VPAD(25300, 1, 0, 0, 0, "p2 release"),
	VPAD(25600, 1, PAD_X, 0, 0, "p2 X"), VPAD(25750, 1, 0, 0, 0, "p2 release"),
	VPAD(26050, 1, PAD_X, 0, 0, "p2 X"), VPAD(26200, 1, 0, 0, 0, "p2 release"),
	VPAD(26500, 1, PAD_X, 0, 0, "p2 X"), VPAD(26650, 1, 0, 0, 0, "p2 release"),
	VPAD(26950, 1, PAD_X, 0, 0, "p2 X"), VPAD(27100, 1, 0, 0, 0, "p2 release"),
	VPAD(27400, 1, PAD_X, 0, 0, "p2 X"), VPAD(27550, 1, 0, 0, 0, "p2 release"),
	VPAD(27850, 1, PAD_X, 0, 0, "p2 X"), VPAD(28000, 1, 0, 0, 0, "p2 release"),
	VPAD(28300, 1, PAD_X, 0, 0, "p2 X"), VPAD(28450, 1, 0, 0, 0, "p2 release"),
	VPAD(28750, 1, PAD_X, 0, 0, "p2 X"), VPAD(28900, 1, 0, 0, 0, "p2 release"),
	VPAD(29200, 1, PAD_X, 0, 0, "p2 X"), VPAD(29350, 1, 0, 0, 0, "p2 release"),
	VPAD(29650, 1, PAD_X, 0, 0, "p2 X"), VPAD(29800, 1, 0, 0, 0, "p2 release"),
	P1(14200, PAD_A, 0, 0, "p1 A (jump)"), P1(14450, 0, 0, 0, "p1 release"),
	P1(14900, PAD_A, 0, 0, "p1 A (jump)"), P1(15150, 0, 0, 0, "p1 release"),
	P1(15600, PAD_A, 0, 0, "p1 A (jump)"), P1(15850, 0, 0, 0, "p1 release"),
	P1(16300, PAD_A, 0, 0, "p1 A (jump)"), P1(16550, 0, 0, 0, "p1 release"),
	P1(17000, PAD_A, 0, 0, "p1 A (jump)"), P1(17250, 0, 0, 0, "p1 release"),
	P1(17700, PAD_A, 0, 0, "p1 A (jump)"), P1(17950, 0, 0, 0, "p1 release"),
	P1(18400, PAD_A, 0, 0, "p1 A (jump)"), P1(18650, 0, 0, 0, "p1 release"),
	P1(19100, PAD_A, 0, 0, "p1 A (jump)"), P1(19350, 0, 0, 0, "p1 release"),
	P1(19800, PAD_A, 0, 0, "p1 A (jump)"), P1(20050, 0, 0, 0, "p1 release"),
	P1(20500, PAD_A, 0, 0, "p1 A (jump)"), P1(20750, 0, 0, 0, "p1 release"),
	SHOT(16000, "beam42_1"), SHOT(20000, "beam42_2"), SHOT(25000, "beam42_3"), SHOT(30500, "beam42_4"),
	END(32000),
};

// beam42f (-Level 4, XXL_P2HERO=asterix XXL_NOLOCK=1 XXL_LIFTLOG=1): beam42 in no-lock
// mode with the roles swapped -- player 2's Asterix (not locked) jumps again and
// again under the beam, player 1's Obelix turns the turnstile.
const Step kBeam42f[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(600, 2),
	PLACEAT(3000, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3100, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	PLACEAT(3500, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3600, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	{ 3800, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4000, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 4300, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	PLACEAT(4600, HERO_ASTERIX, -378.5f, 3.0f, -119.07f),
	GLIDEAT(5000, HERO_ASTERIX, -381.93f, 2.5f, -119.07f, 600),
	VPAD(6200, 1, PAD_X, 0, 0, "p2 X: light the hearth"), VPAD(6400, 1, 0, 0, 0, "p2 release"),
	VPAD(7000, 1, PAD_A, 0, 0, "p2 A (jump before the circle)"), VPAD(7300, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(8000, HERO_ASTERIX, -394.734f, 5.2f, -119.076f, 1000),
	SHOT(9500, "beam42f_0"),
	PLACEAT(10500, HERO_OBELIX, -409.0f, 2.8f, -116.645f),
	GLIDEAT(11000, HERO_OBELIX, -414.612f, 2.281f, -116.645f, 800),
	P1(13000, PAD_X, 0, 0, "p1 X"),
	P1(13150, 0, 0, 0, "p1 release"),
	P1(13450, PAD_X, 0, 0, "p1 X"),
	P1(13600, 0, 0, 0, "p1 release"),
	P1(13900, PAD_X, 0, 0, "p1 X"),
	VPAD(14000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(14050, 0, 0, 0, "p1 release"),
	VPAD(14200, 1, 0, 0, 0, "p2 release"),
	P1(14350, PAD_X, 0, 0, "p1 X"),
	VPAD(14400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(14500, 0, 0, 0, "p1 release"),
	VPAD(14600, 1, 0, 0, 0, "p2 release"),
	P1(14800, PAD_X, 0, 0, "p1 X"),
	VPAD(14800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(14950, 0, 0, 0, "p1 release"),
	VPAD(15000, 1, 0, 0, 0, "p2 release"),
	VPAD(15200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(15250, PAD_X, 0, 0, "p1 X"),
	P1(15400, 0, 0, 0, "p1 release"),
	VPAD(15400, 1, 0, 0, 0, "p2 release"),
	VPAD(15600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(15700, PAD_X, 0, 0, "p1 X"),
	VPAD(15800, 1, 0, 0, 0, "p2 release"),
	P1(15850, 0, 0, 0, "p1 release"),
	VPAD(16000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	SHOT(16000, "beam42f_1"),
	P1(16150, PAD_X, 0, 0, "p1 X"),
	VPAD(16200, 1, 0, 0, 0, "p2 release"),
	P1(16300, 0, 0, 0, "p1 release"),
	VPAD(16400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(16600, PAD_X, 0, 0, "p1 X"),
	VPAD(16600, 1, 0, 0, 0, "p2 release"),
	P1(16750, 0, 0, 0, "p1 release"),
	VPAD(16800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(17000, 1, 0, 0, 0, "p2 release"),
	P1(17050, PAD_X, 0, 0, "p1 X"),
	P1(17200, 0, 0, 0, "p1 release"),
	VPAD(17200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(17400, 1, 0, 0, 0, "p2 release"),
	P1(17500, PAD_X, 0, 0, "p1 X"),
	VPAD(17600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(17650, 0, 0, 0, "p1 release"),
	VPAD(17800, 1, 0, 0, 0, "p2 release"),
	P1(17950, PAD_X, 0, 0, "p1 X"),
	VPAD(18000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(18100, 0, 0, 0, "p1 release"),
	VPAD(18200, 1, 0, 0, 0, "p2 release"),
	P1(18400, PAD_X, 0, 0, "p1 X"),
	VPAD(18400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(18550, 0, 0, 0, "p1 release"),
	VPAD(18600, 1, 0, 0, 0, "p2 release"),
	VPAD(18800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(18850, PAD_X, 0, 0, "p1 X"),
	P1(19000, 0, 0, 0, "p1 release"),
	VPAD(19000, 1, 0, 0, 0, "p2 release"),
	VPAD(19200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(19300, PAD_X, 0, 0, "p1 X"),
	VPAD(19400, 1, 0, 0, 0, "p2 release"),
	P1(19450, 0, 0, 0, "p1 release"),
	VPAD(19600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(19750, PAD_X, 0, 0, "p1 X"),
	VPAD(19800, 1, 0, 0, 0, "p2 release"),
	P1(19900, 0, 0, 0, "p1 release"),
	VPAD(20000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	SHOT(20000, "beam42f_2"),
	P1(20200, PAD_X, 0, 0, "p1 X"),
	VPAD(20200, 1, 0, 0, 0, "p2 release"),
	P1(20350, 0, 0, 0, "p1 release"),
	VPAD(20400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(20600, 1, 0, 0, 0, "p2 release"),
	P1(20650, PAD_X, 0, 0, "p1 X"),
	P1(20800, 0, 0, 0, "p1 release"),
	VPAD(20800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(21000, 1, 0, 0, 0, "p2 release"),
	P1(21100, PAD_X, 0, 0, "p1 X"),
	VPAD(21200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(21250, 0, 0, 0, "p1 release"),
	VPAD(21400, 1, 0, 0, 0, "p2 release"),
	P1(21550, PAD_X, 0, 0, "p1 X"),
	VPAD(21600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(21700, 0, 0, 0, "p1 release"),
	VPAD(21800, 1, 0, 0, 0, "p2 release"),
	P1(22000, PAD_X, 0, 0, "p1 X"),
	VPAD(22000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(22150, 0, 0, 0, "p1 release"),
	VPAD(22200, 1, 0, 0, 0, "p2 release"),
	VPAD(22400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(22450, PAD_X, 0, 0, "p1 X"),
	P1(22600, 0, 0, 0, "p1 release"),
	VPAD(22600, 1, 0, 0, 0, "p2 release"),
	VPAD(22800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(22900, PAD_X, 0, 0, "p1 X"),
	VPAD(23000, 1, 0, 0, 0, "p2 release"),
	P1(23050, 0, 0, 0, "p1 release"),
	VPAD(23200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(23350, PAD_X, 0, 0, "p1 X"),
	VPAD(23400, 1, 0, 0, 0, "p2 release"),
	P1(23500, 0, 0, 0, "p1 release"),
	VPAD(23600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(23800, PAD_X, 0, 0, "p1 X"),
	VPAD(23800, 1, 0, 0, 0, "p2 release"),
	P1(23950, 0, 0, 0, "p1 release"),
	VPAD(24000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(24200, 1, 0, 0, 0, "p2 release"),
	P1(24250, PAD_X, 0, 0, "p1 X"),
	P1(24400, 0, 0, 0, "p1 release"),
	VPAD(24400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(24600, 1, 0, 0, 0, "p2 release"),
	P1(24700, PAD_X, 0, 0, "p1 X"),
	VPAD(24800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(24850, 0, 0, 0, "p1 release"),
	VPAD(25000, 1, 0, 0, 0, "p2 release"),
	SHOT(25000, "beam42f_3"),
	P1(25150, PAD_X, 0, 0, "p1 X"),
	VPAD(25200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(25300, 0, 0, 0, "p1 release"),
	VPAD(25400, 1, 0, 0, 0, "p2 release"),
	P1(25600, PAD_X, 0, 0, "p1 X"),
	VPAD(25600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(25750, 0, 0, 0, "p1 release"),
	VPAD(25800, 1, 0, 0, 0, "p2 release"),
	VPAD(26000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(26050, PAD_X, 0, 0, "p1 X"),
	P1(26200, 0, 0, 0, "p1 release"),
	VPAD(26200, 1, 0, 0, 0, "p2 release"),
	VPAD(26400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(26500, PAD_X, 0, 0, "p1 X"),
	VPAD(26600, 1, 0, 0, 0, "p2 release"),
	P1(26650, 0, 0, 0, "p1 release"),
	VPAD(26800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(26950, PAD_X, 0, 0, "p1 X"),
	VPAD(27000, 1, 0, 0, 0, "p2 release"),
	P1(27100, 0, 0, 0, "p1 release"),
	VPAD(27200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(27400, PAD_X, 0, 0, "p1 X"),
	VPAD(27400, 1, 0, 0, 0, "p2 release"),
	P1(27550, 0, 0, 0, "p1 release"),
	VPAD(27600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(27800, 1, 0, 0, 0, "p2 release"),
	P1(27850, PAD_X, 0, 0, "p1 X"),
	P1(28000, 0, 0, 0, "p1 release"),
	VPAD(28000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(28200, 1, 0, 0, 0, "p2 release"),
	P1(28300, PAD_X, 0, 0, "p1 X"),
	VPAD(28400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(28450, 0, 0, 0, "p1 release"),
	VPAD(28600, 1, 0, 0, 0, "p2 release"),
	P1(28750, PAD_X, 0, 0, "p1 X"),
	VPAD(28800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(28900, 0, 0, 0, "p1 release"),
	VPAD(29000, 1, 0, 0, 0, "p2 release"),
	P1(29200, PAD_X, 0, 0, "p1 X"),
	VPAD(29200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(29350, 0, 0, 0, "p1 release"),
	VPAD(29400, 1, 0, 0, 0, "p2 release"),
	VPAD(29600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(29650, PAD_X, 0, 0, "p1 X"),
	P1(29800, 0, 0, 0, "p1 release"),
	VPAD(29800, 1, 0, 0, 0, "p2 release"),
	VPAD(30000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(30100, PAD_X, 0, 0, "p1 X"),
	VPAD(30200, 1, 0, 0, 0, "p2 release"),
	P1(30250, 0, 0, 0, "p1 release"),
	VPAD(30400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	SHOT(30500, "beam42f_4"),
	P1(30550, PAD_X, 0, 0, "p1 X"),
	VPAD(30600, 1, 0, 0, 0, "p2 release"),
	P1(30700, 0, 0, 0, "p1 release"),
	VPAD(30800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(31000, PAD_X, 0, 0, "p1 X"),
	VPAD(31000, 1, 0, 0, 0, "p2 release"),
	P1(31150, 0, 0, 0, "p1 release"),
	VPAD(31200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(31400, 1, 0, 0, 0, "p2 release"),
	P1(31450, PAD_X, 0, 0, "p1 X"),
	P1(31600, 0, 0, 0, "p1 release"),
	VPAD(31600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(31800, 1, 0, 0, 0, "p2 release"),
	P1(31900, PAD_X, 0, 0, "p1 X"),
	VPAD(32000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(32050, 0, 0, 0, "p1 release"),
	VPAD(32200, 1, 0, 0, 0, "p2 release"),
	VPAD(32400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(32600, 1, 0, 0, 0, "p2 release"),
	VPAD(32800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(33000, 1, 0, 0, 0, "p2 release"),
	END(34000),
};

// beam42l (-Level 4, XXL_P2HERO=asterix XXL_NOLOCK=1): beam42f with the lead back
// to Asterix after the circle (4800, 4801, 4800: his wait order cleared too) -- the beam
// must take him while he leads, and drop him leading at the far end.
const Step kBeam42L[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	LOADSECTOR(600, 2),
	PLACEAT(3000, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3100, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	PLACEAT(3500, HERO_ASTERIX, -381.93f, 3.0f, -119.07f), PLACEAT(3600, HERO_OBELIX, -384.0f, 3.0f, -125.0f),
	{ 3800, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 4000, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 4300, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	PLACEAT(4600, HERO_ASTERIX, -378.5f, 3.0f, -119.07f),
	GLIDEAT(5000, HERO_ASTERIX, -381.93f, 2.5f, -119.07f, 600),
	VPAD(6200, 1, PAD_X, 0, 0, "p2 X: light the hearth"), VPAD(6400, 1, 0, 0, 0, "p2 release"),
	VPAD(7000, 1, PAD_A, 0, 0, "p2 A (jump before the circle)"), VPAD(7300, 1, 0, 0, 0, "p2 release"),
	GLIDEAT(8000, HERO_ASTERIX, -394.734f, 5.2f, -119.076f, 1000),
	SHOT(9500, "beam42l_0"),
	{ 10000, S_TRIOEV, 0x4800, 0, 0, 0, 0, "asterix leads" },
	{ 10300, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (Asterix's wait order 3 gone)" },
	{ 10600, S_TRIOEV, 0x4800, 0, 0, 0, 0, "asterix leads again" },
	PLACEAT(10500, HERO_OBELIX, -409.0f, 2.8f, -116.645f),
	GLIDEAT(11000, HERO_OBELIX, -414.612f, 2.281f, -116.645f, 800),
	P1(13000, PAD_X, 0, 0, "p1 X"),
	P1(13150, 0, 0, 0, "p1 release"),
	P1(13450, PAD_X, 0, 0, "p1 X"),
	P1(13600, 0, 0, 0, "p1 release"),
	P1(13900, PAD_X, 0, 0, "p1 X"),
	VPAD(14000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(14050, 0, 0, 0, "p1 release"),
	VPAD(14200, 1, 0, 0, 0, "p2 release"),
	P1(14350, PAD_X, 0, 0, "p1 X"),
	VPAD(14400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(14500, 0, 0, 0, "p1 release"),
	VPAD(14600, 1, 0, 0, 0, "p2 release"),
	P1(14800, PAD_X, 0, 0, "p1 X"),
	VPAD(14800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(14950, 0, 0, 0, "p1 release"),
	VPAD(15000, 1, 0, 0, 0, "p2 release"),
	VPAD(15200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(15250, PAD_X, 0, 0, "p1 X"),
	P1(15400, 0, 0, 0, "p1 release"),
	VPAD(15400, 1, 0, 0, 0, "p2 release"),
	VPAD(15600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(15700, PAD_X, 0, 0, "p1 X"),
	VPAD(15800, 1, 0, 0, 0, "p2 release"),
	P1(15850, 0, 0, 0, "p1 release"),
	VPAD(16000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	SHOT(16000, "beam42l_1"),
	P1(16150, PAD_X, 0, 0, "p1 X"),
	VPAD(16200, 1, 0, 0, 0, "p2 release"),
	P1(16300, 0, 0, 0, "p1 release"),
	VPAD(16400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(16600, PAD_X, 0, 0, "p1 X"),
	VPAD(16600, 1, 0, 0, 0, "p2 release"),
	P1(16750, 0, 0, 0, "p1 release"),
	VPAD(16800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(17000, 1, 0, 0, 0, "p2 release"),
	P1(17050, PAD_X, 0, 0, "p1 X"),
	P1(17200, 0, 0, 0, "p1 release"),
	VPAD(17200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(17400, 1, 0, 0, 0, "p2 release"),
	P1(17500, PAD_X, 0, 0, "p1 X"),
	VPAD(17600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(17650, 0, 0, 0, "p1 release"),
	VPAD(17800, 1, 0, 0, 0, "p2 release"),
	P1(17950, PAD_X, 0, 0, "p1 X"),
	VPAD(18000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(18100, 0, 0, 0, "p1 release"),
	VPAD(18200, 1, 0, 0, 0, "p2 release"),
	P1(18400, PAD_X, 0, 0, "p1 X"),
	VPAD(18400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(18550, 0, 0, 0, "p1 release"),
	VPAD(18600, 1, 0, 0, 0, "p2 release"),
	VPAD(18800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(18850, PAD_X, 0, 0, "p1 X"),
	P1(19000, 0, 0, 0, "p1 release"),
	VPAD(19000, 1, 0, 0, 0, "p2 release"),
	VPAD(19200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(19300, PAD_X, 0, 0, "p1 X"),
	VPAD(19400, 1, 0, 0, 0, "p2 release"),
	P1(19450, 0, 0, 0, "p1 release"),
	VPAD(19600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(19750, PAD_X, 0, 0, "p1 X"),
	VPAD(19800, 1, 0, 0, 0, "p2 release"),
	P1(19900, 0, 0, 0, "p1 release"),
	VPAD(20000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	SHOT(20000, "beam42l_2"),
	P1(20200, PAD_X, 0, 0, "p1 X"),
	VPAD(20200, 1, 0, 0, 0, "p2 release"),
	P1(20350, 0, 0, 0, "p1 release"),
	VPAD(20400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(20600, 1, 0, 0, 0, "p2 release"),
	P1(20650, PAD_X, 0, 0, "p1 X"),
	P1(20800, 0, 0, 0, "p1 release"),
	VPAD(20800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(21000, 1, 0, 0, 0, "p2 release"),
	P1(21100, PAD_X, 0, 0, "p1 X"),
	VPAD(21200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(21250, 0, 0, 0, "p1 release"),
	VPAD(21400, 1, 0, 0, 0, "p2 release"),
	P1(21550, PAD_X, 0, 0, "p1 X"),
	VPAD(21600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(21700, 0, 0, 0, "p1 release"),
	VPAD(21800, 1, 0, 0, 0, "p2 release"),
	P1(22000, PAD_X, 0, 0, "p1 X"),
	VPAD(22000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(22150, 0, 0, 0, "p1 release"),
	VPAD(22200, 1, 0, 0, 0, "p2 release"),
	VPAD(22400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(22450, PAD_X, 0, 0, "p1 X"),
	P1(22600, 0, 0, 0, "p1 release"),
	VPAD(22600, 1, 0, 0, 0, "p2 release"),
	VPAD(22800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(22900, PAD_X, 0, 0, "p1 X"),
	VPAD(23000, 1, 0, 0, 0, "p2 release"),
	P1(23050, 0, 0, 0, "p1 release"),
	VPAD(23200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(23350, PAD_X, 0, 0, "p1 X"),
	VPAD(23400, 1, 0, 0, 0, "p2 release"),
	P1(23500, 0, 0, 0, "p1 release"),
	VPAD(23600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(23800, PAD_X, 0, 0, "p1 X"),
	VPAD(23800, 1, 0, 0, 0, "p2 release"),
	P1(23950, 0, 0, 0, "p1 release"),
	VPAD(24000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(24200, 1, 0, 0, 0, "p2 release"),
	P1(24250, PAD_X, 0, 0, "p1 X"),
	P1(24400, 0, 0, 0, "p1 release"),
	VPAD(24400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(24600, 1, 0, 0, 0, "p2 release"),
	P1(24700, PAD_X, 0, 0, "p1 X"),
	VPAD(24800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(24850, 0, 0, 0, "p1 release"),
	VPAD(25000, 1, 0, 0, 0, "p2 release"),
	SHOT(25000, "beam42l_3"),
	P1(25150, PAD_X, 0, 0, "p1 X"),
	VPAD(25200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(25300, 0, 0, 0, "p1 release"),
	VPAD(25400, 1, 0, 0, 0, "p2 release"),
	P1(25600, PAD_X, 0, 0, "p1 X"),
	VPAD(25600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(25750, 0, 0, 0, "p1 release"),
	VPAD(25800, 1, 0, 0, 0, "p2 release"),
	VPAD(26000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(26050, PAD_X, 0, 0, "p1 X"),
	P1(26200, 0, 0, 0, "p1 release"),
	VPAD(26200, 1, 0, 0, 0, "p2 release"),
	VPAD(26400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(26500, PAD_X, 0, 0, "p1 X"),
	VPAD(26600, 1, 0, 0, 0, "p2 release"),
	P1(26650, 0, 0, 0, "p1 release"),
	VPAD(26800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(26950, PAD_X, 0, 0, "p1 X"),
	VPAD(27000, 1, 0, 0, 0, "p2 release"),
	P1(27100, 0, 0, 0, "p1 release"),
	VPAD(27200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(27400, PAD_X, 0, 0, "p1 X"),
	VPAD(27400, 1, 0, 0, 0, "p2 release"),
	P1(27550, 0, 0, 0, "p1 release"),
	VPAD(27600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(27800, 1, 0, 0, 0, "p2 release"),
	P1(27850, PAD_X, 0, 0, "p1 X"),
	P1(28000, 0, 0, 0, "p1 release"),
	VPAD(28000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(28200, 1, 0, 0, 0, "p2 release"),
	P1(28300, PAD_X, 0, 0, "p1 X"),
	VPAD(28400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(28450, 0, 0, 0, "p1 release"),
	VPAD(28600, 1, 0, 0, 0, "p2 release"),
	P1(28750, PAD_X, 0, 0, "p1 X"),
	VPAD(28800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(28900, 0, 0, 0, "p1 release"),
	VPAD(29000, 1, 0, 0, 0, "p2 release"),
	P1(29200, PAD_X, 0, 0, "p1 X"),
	VPAD(29200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(29350, 0, 0, 0, "p1 release"),
	VPAD(29400, 1, 0, 0, 0, "p2 release"),
	VPAD(29600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(29650, PAD_X, 0, 0, "p1 X"),
	P1(29800, 0, 0, 0, "p1 release"),
	VPAD(29800, 1, 0, 0, 0, "p2 release"),
	VPAD(30000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(30100, PAD_X, 0, 0, "p1 X"),
	VPAD(30200, 1, 0, 0, 0, "p2 release"),
	P1(30250, 0, 0, 0, "p1 release"),
	VPAD(30400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	SHOT(30500, "beam42l_4"),
	P1(30550, PAD_X, 0, 0, "p1 X"),
	VPAD(30600, 1, 0, 0, 0, "p2 release"),
	P1(30700, 0, 0, 0, "p1 release"),
	VPAD(30800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(31000, PAD_X, 0, 0, "p1 X"),
	VPAD(31000, 1, 0, 0, 0, "p2 release"),
	P1(31150, 0, 0, 0, "p1 release"),
	VPAD(31200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(31400, 1, 0, 0, 0, "p2 release"),
	P1(31450, PAD_X, 0, 0, "p1 X"),
	P1(31600, 0, 0, 0, "p1 release"),
	VPAD(31600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(31800, 1, 0, 0, 0, "p2 release"),
	P1(31900, PAD_X, 0, 0, "p1 X"),
	VPAD(32000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(32050, 0, 0, 0, "p1 release"),
	VPAD(32200, 1, 0, 0, 0, "p2 release"),
	VPAD(32400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(32600, 1, 0, 0, 0, "p2 release"),
	VPAD(32800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(33000, 1, 0, 0, 0, "p2 release"),
	END(34000),
};

// beam32 (-Level 3, XXL_P2HERO=asterix XXL_NOLOCK=1 XXL_LIFTLOG=1): the user's LV3-2 --
// Asterix lights the hearth (252.7 9.1 -90.5) and stands on the light pillar
// (266.18 11.07 -97.09); player 1's Obelix hits the turnstile (251.3 6.1 -97.8)
// until the rotary beam has carried Asterix across and let him go. He must be
// his player's once landed (the walk at the end).
const Step kBeam32[] = {
	MARK(0, "baseline"),
	LOADSECTOR(200, 2),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), PLACEAT(2500, HERO_ASTERIX, 255.5f, 9.6f, -90.5f),
	PLACEAT(2500, HERO_OBELIX, 258.0f, 9.6f, -94.0f), PLACEAT(3000, HERO_ASTERIX, 255.5f, 9.6f, -90.5f), PLACEAT(3000, HERO_OBELIX, 258.0f, 9.6f, -94.0f),
	{ 3300, S_TORCH, 0, 0, 0, 0, 0, "torches" },
	{ 3500, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 3800, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	GLIDEAT(4100, HERO_ASTERIX, 254.0f, 9.3f, -90.5f, 600),
	VPAD(5000, 1, PAD_X, 0, 0, "p2 X: light the hearth"), VPAD(5200, 1, 0, 0, 0, "p2 release"), SHOT(6000, "beam32_0"),
	GLIDEAT(6500, HERO_ASTERIX, 266.179f, 11.5f, -97.092f, 1500),
	SHOT(8500, "beam32_1"),
	{ 9000, S_NEAROBJ, HERO_OBELIX, 45, 0, 2.5f, 0, "obelix at the turnstile" },
	P1(10000, PAD_X, 0, 0, "p1 X"), P1(10150, 0, 0, 0, "p1 release"), P1(10450, PAD_X, 0, 0, "p1 X"),
	P1(10600, 0, 0, 0, "p1 release"), VPAD(10600, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(10800, 1, 0, 0, 0, "p2 release"),
	P1(10900, PAD_X, 0, 0, "p1 X"), VPAD(11000, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(11050, 0, 0, 0, "p1 release"),
	VPAD(11200, 1, 0, 0, 0, "p2 release"), P1(11350, PAD_X, 0, 0, "p1 X"), VPAD(11400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(11500, 0, 0, 0, "p1 release"), VPAD(11600, 1, 0, 0, 0, "p2 release"), P1(11800, PAD_X, 0, 0, "p1 X"),
	VPAD(11800, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(11950, 0, 0, 0, "p1 release"), VPAD(12000, 1, 0, 0, 0, "p2 release"),
	VPAD(12200, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(12250, PAD_X, 0, 0, "p1 X"), P1(12400, 0, 0, 0, "p1 release"),
	VPAD(12400, 1, 0, 0, 0, "p2 release"), VPAD(12600, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(12700, PAD_X, 0, 0, "p1 X"),
	VPAD(12800, 1, 0, 0, 0, "p2 release"), P1(12850, 0, 0, 0, "p1 release"), VPAD(13000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(13150, PAD_X, 0, 0, "p1 X"), VPAD(13200, 1, 0, 0, 0, "p2 release"), P1(13300, 0, 0, 0, "p1 release"),
	VPAD(13400, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(13600, PAD_X, 0, 0, "p1 X"), VPAD(13600, 1, 0, 0, 0, "p2 release"),
	P1(13750, 0, 0, 0, "p1 release"), VPAD(13800, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(14000, 1, 0, 0, 0, "p2 release"),
	P1(14050, PAD_X, 0, 0, "p1 X"), P1(14200, 0, 0, 0, "p1 release"), VPAD(14200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(14400, 1, 0, 0, 0, "p2 release"), P1(14500, PAD_X, 0, 0, "p1 X"), VPAD(14600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(14650, 0, 0, 0, "p1 release"), VPAD(14800, 1, 0, 0, 0, "p2 release"), P1(14950, PAD_X, 0, 0, "p1 X"),
	VPAD(15000, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(15100, 0, 0, 0, "p1 release"), VPAD(15200, 1, 0, 0, 0, "p2 release"),
	P1(15400, PAD_X, 0, 0, "p1 X"), VPAD(15400, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(15550, 0, 0, 0, "p1 release"),
	VPAD(15600, 1, 0, 0, 0, "p2 release"), VPAD(15800, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(15850, PAD_X, 0, 0, "p1 X"),
	P1(16000, 0, 0, 0, "p1 release"), VPAD(16000, 1, 0, 0, 0, "p2 release"), SHOT(16000, "beam32_2"),
	VPAD(16200, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(16300, PAD_X, 0, 0, "p1 X"), VPAD(16400, 1, 0, 0, 0, "p2 release"),
	P1(16450, 0, 0, 0, "p1 release"), VPAD(16600, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(16750, PAD_X, 0, 0, "p1 X"),
	VPAD(16800, 1, 0, 0, 0, "p2 release"), P1(16900, 0, 0, 0, "p1 release"), VPAD(17000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(17200, PAD_X, 0, 0, "p1 X"), VPAD(17200, 1, 0, 0, 0, "p2 release"), P1(17350, 0, 0, 0, "p1 release"),
	VPAD(17400, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(17600, 1, 0, 0, 0, "p2 release"), P1(17650, PAD_X, 0, 0, "p1 X"),
	P1(17800, 0, 0, 0, "p1 release"), VPAD(17800, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(18000, 1, 0, 0, 0, "p2 release"),
	P1(18100, PAD_X, 0, 0, "p1 X"), VPAD(18200, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(18250, 0, 0, 0, "p1 release"),
	VPAD(18400, 1, 0, 0, 0, "p2 release"), P1(18550, PAD_X, 0, 0, "p1 X"), VPAD(18600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(18700, 0, 0, 0, "p1 release"), VPAD(18800, 1, 0, 0, 0, "p2 release"), P1(19000, PAD_X, 0, 0, "p1 X"),
	VPAD(19000, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(19150, 0, 0, 0, "p1 release"), VPAD(19200, 1, 0, 0, 0, "p2 release"),
	VPAD(19400, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(19450, PAD_X, 0, 0, "p1 X"), P1(19600, 0, 0, 0, "p1 release"),
	VPAD(19600, 1, 0, 0, 0, "p2 release"), VPAD(19800, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(19900, PAD_X, 0, 0, "p1 X"),
	VPAD(20000, 1, 0, 0, 0, "p2 release"), P1(20050, 0, 0, 0, "p1 release"), VPAD(20200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(20350, PAD_X, 0, 0, "p1 X"), VPAD(20400, 1, 0, 0, 0, "p2 release"), P1(20500, 0, 0, 0, "p1 release"),
	VPAD(20600, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(20800, PAD_X, 0, 0, "p1 X"), VPAD(20800, 1, 0, 0, 0, "p2 release"),
	P1(20950, 0, 0, 0, "p1 release"), VPAD(21000, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(21200, 1, 0, 0, 0, "p2 release"),
	P1(21250, PAD_X, 0, 0, "p1 X"), P1(21400, 0, 0, 0, "p1 release"), VPAD(21400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(21600, 1, 0, 0, 0, "p2 release"), P1(21700, PAD_X, 0, 0, "p1 X"), VPAD(21800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(21850, 0, 0, 0, "p1 release"), VPAD(22000, 1, 0, 0, 0, "p2 release"), SHOT(22000, "beam32_3"),
	P1(22150, PAD_X, 0, 0, "p1 X"), VPAD(22200, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(22300, 0, 0, 0, "p1 release"),
	VPAD(22400, 1, 0, 0, 0, "p2 release"), P1(22600, PAD_X, 0, 0, "p1 X"), VPAD(22600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(22750, 0, 0, 0, "p1 release"), VPAD(22800, 1, 0, 0, 0, "p2 release"), VPAD(23000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(23050, PAD_X, 0, 0, "p1 X"), P1(23200, 0, 0, 0, "p1 release"), VPAD(23200, 1, 0, 0, 0, "p2 release"),
	VPAD(23400, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(23500, PAD_X, 0, 0, "p1 X"), VPAD(23600, 1, 0, 0, 0, "p2 release"),
	P1(23650, 0, 0, 0, "p1 release"), VPAD(23800, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(23950, PAD_X, 0, 0, "p1 X"),
	VPAD(24000, 1, 0, 0, 0, "p2 release"), P1(24100, 0, 0, 0, "p1 release"), VPAD(24200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(24400, PAD_X, 0, 0, "p1 X"), VPAD(24400, 1, 0, 0, 0, "p2 release"), P1(24550, 0, 0, 0, "p1 release"),
	VPAD(24600, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(24800, 1, 0, 0, 0, "p2 release"), P1(24850, PAD_X, 0, 0, "p1 X"),
	P1(25000, 0, 0, 0, "p1 release"), VPAD(25000, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(25200, 1, 0, 0, 0, "p2 release"),
	P1(25300, PAD_X, 0, 0, "p1 X"), VPAD(25400, 1, PAD_A, 0, 0, "p2 A (jump)"), P1(25450, 0, 0, 0, "p1 release"),
	VPAD(25600, 1, 0, 0, 0, "p2 release"), P1(25750, PAD_X, 0, 0, "p1 X"), VPAD(25800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	P1(25900, 0, 0, 0, "p1 release"), VPAD(26000, 1, 0, 0, 0, "p2 release"), P1(26200, PAD_X, 0, 0, "p1 X"),
	P1(26350, 0, 0, 0, "p1 release"), P1(26650, PAD_X, 0, 0, "p1 X"), P1(26800, 0, 0, 0, "p1 release"),
	P1(27100, PAD_X, 0, 0, "p1 X"), P1(27250, 0, 0, 0, "p1 release"), P1(27550, PAD_X, 0, 0, "p1 X"),
	P1(27700, 0, 0, 0, "p1 release"), P1(28000, PAD_X, 0, 0, "p1 X"), SHOT(28000, "beam32_4"),
	P1(28150, 0, 0, 0, "p1 release"), P1(28450, PAD_X, 0, 0, "p1 X"), P1(28600, 0, 0, 0, "p1 release"),
	P1(28900, PAD_X, 0, 0, "p1 X"), P1(29050, 0, 0, 0, "p1 release"), P1(29350, PAD_X, 0, 0, "p1 X"),
	P1(29500, 0, 0, 0, "p1 release"), P1(29800, PAD_X, 0, 0, "p1 X"), P1(29950, 0, 0, 0, "p1 release"),
	P1(30250, PAD_X, 0, 0, "p1 X"), P1(30400, 0, 0, 0, "p1 release"), P1(30700, PAD_X, 0, 0, "p1 X"),
	P1(30850, 0, 0, 0, "p1 release"), P1(31150, PAD_X, 0, 0, "p1 X"), P1(31300, 0, 0, 0, "p1 release"),
	P1(31600, PAD_X, 0, 0, "p1 X"), P1(31750, 0, 0, 0, "p1 release"), P1(32050, PAD_X, 0, 0, "p1 X"),
	P1(32200, 0, 0, 0, "p1 release"), P1(32500, PAD_X, 0, 0, "p1 X"), P1(32650, 0, 0, 0, "p1 release"),
	P1(32950, PAD_X, 0, 0, "p1 X"), P1(33100, 0, 0, 0, "p1 release"), P1(33400, PAD_X, 0, 0, "p1 X"),
	P1(33550, 0, 0, 0, "p1 release"), P1(33850, PAD_X, 0, 0, "p1 X"), P1(34000, 0, 0, 0, "p1 release"),
	SHOT(34000, "beam32_5"),
	VPAD(38000, 1, 0, 0, 1, "p2 walks (stick up)"), VPAD(39500, 1, 0, 0, 0, "p2 release"), SHOT(40000, "beam32_6"),
	END(42000),
};

// beam25 (level 2, XXL_P2HERO=asterix XXL_NOLOCK=1 XXL_LIFTLOG=1): beam32 at LV2-5's
// rotary beam (pivot 133.5 4.7 -481.3) -- Asterix at the nearest hearth and on the
// nearest light pillar; the beam turned by its event (0x0903, what a hit on its
// turnstile sends -- the turnstile is up on the beam's pole, not reached from the
// water, where S_NEAROBJ put Obelix).
const Step kBeam25[] = {
	MARK(0, "baseline"),
	LOADSECTOR(200, 5),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"), PLACEAT(2500, HERO_ASTERIX, 150.0f, 6.0f, -483.0f),
	PLACEAT(2500, HERO_OBELIX, 146.0f, 6.0f, -485.0f), PLACEAT(3000, HERO_ASTERIX, 150.0f, 6.0f, -483.0f), PLACEAT(3000, HERO_OBELIX, 146.0f, 6.0f, -485.0f),
	{ 3200, S_NEAROBJ, HERO_ASTERIX, 33, 0, 1.5f, 0, "asterix at the hearth" },
	{ 3300, S_TORCH, 0, 0, 0, 0, 0, "torches" },
	{ 3500, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 3800, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	{ 4100, S_NEAROBJ, HERO_ASTERIX, 33, 0, 1.3f, 0, "asterix at the hearth" },
	VPAD(5000, 1, PAD_X, 0, 0, "p2 X: light the hearth"), VPAD(5200, 1, 0, 0, 0, "p2 release"), SHOT(6000, "beam25_0"),
	{ 6500, S_NEAROBJ, HERO_ASTERIX, 60, 0, 0.3f, 0, "asterix onto the light pillar" },
	SHOT(8500, "beam25_1"),
	{ 10000, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, { 10450, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(10600, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(10800, 1, 0, 0, 0, "p2 release"),
	{ 10900, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(11000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(11200, 1, 0, 0, 0, "p2 release"), { 11350, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(11400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(11600, 1, 0, 0, 0, "p2 release"), { 11800, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(11800, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(12000, 1, 0, 0, 0, "p2 release"),
	VPAD(12200, 1, PAD_A, 0, 0, "p2 A (jump)"), { 12250, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(12400, 1, 0, 0, 0, "p2 release"), VPAD(12600, 1, PAD_A, 0, 0, "p2 A (jump)"), { 12700, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(12800, 1, 0, 0, 0, "p2 release"), VPAD(13000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	{ 13150, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(13200, 1, 0, 0, 0, "p2 release"),
	VPAD(13400, 1, PAD_A, 0, 0, "p2 A (jump)"), { 13600, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(13600, 1, 0, 0, 0, "p2 release"),
	VPAD(13800, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(14000, 1, 0, 0, 0, "p2 release"),
	{ 14050, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(14200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(14400, 1, 0, 0, 0, "p2 release"), { 14500, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(14600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(14800, 1, 0, 0, 0, "p2 release"), { 14950, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(15000, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(15200, 1, 0, 0, 0, "p2 release"),
	{ 15400, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(15400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(15600, 1, 0, 0, 0, "p2 release"), VPAD(15800, 1, PAD_A, 0, 0, "p2 A (jump)"), { 15850, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(16000, 1, 0, 0, 0, "p2 release"), SHOT(16000, "beam25_2"),
	VPAD(16200, 1, PAD_A, 0, 0, "p2 A (jump)"), { 16300, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(16400, 1, 0, 0, 0, "p2 release"),
	VPAD(16600, 1, PAD_A, 0, 0, "p2 A (jump)"), { 16750, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(16800, 1, 0, 0, 0, "p2 release"), VPAD(17000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	{ 17200, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(17200, 1, 0, 0, 0, "p2 release"),
	VPAD(17400, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(17600, 1, 0, 0, 0, "p2 release"), { 17650, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(17800, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(18000, 1, 0, 0, 0, "p2 release"),
	{ 18100, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(18200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(18400, 1, 0, 0, 0, "p2 release"), { 18550, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(18600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(18800, 1, 0, 0, 0, "p2 release"), { 19000, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(19000, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(19200, 1, 0, 0, 0, "p2 release"),
	VPAD(19400, 1, PAD_A, 0, 0, "p2 A (jump)"), { 19450, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(19600, 1, 0, 0, 0, "p2 release"), VPAD(19800, 1, PAD_A, 0, 0, "p2 A (jump)"), { 19900, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(20000, 1, 0, 0, 0, "p2 release"), VPAD(20200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	{ 20350, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(20400, 1, 0, 0, 0, "p2 release"),
	VPAD(20600, 1, PAD_A, 0, 0, "p2 A (jump)"), { 20800, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(20800, 1, 0, 0, 0, "p2 release"),
	VPAD(21000, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(21200, 1, 0, 0, 0, "p2 release"),
	{ 21250, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(21400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(21600, 1, 0, 0, 0, "p2 release"), { 21700, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(21800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(22000, 1, 0, 0, 0, "p2 release"), SHOT(22000, "beam25_3"),
	{ 22150, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(22200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(22400, 1, 0, 0, 0, "p2 release"), { 22600, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(22600, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(22800, 1, 0, 0, 0, "p2 release"), VPAD(23000, 1, PAD_A, 0, 0, "p2 A (jump)"),
	{ 23050, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(23200, 1, 0, 0, 0, "p2 release"),
	VPAD(23400, 1, PAD_A, 0, 0, "p2 A (jump)"), { 23500, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(23600, 1, 0, 0, 0, "p2 release"),
	VPAD(23800, 1, PAD_A, 0, 0, "p2 A (jump)"), { 23950, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(24000, 1, 0, 0, 0, "p2 release"), VPAD(24200, 1, PAD_A, 0, 0, "p2 A (jump)"),
	{ 24400, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(24400, 1, 0, 0, 0, "p2 release"),
	VPAD(24600, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(24800, 1, 0, 0, 0, "p2 release"), { 24850, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	VPAD(25000, 1, PAD_A, 0, 0, "p2 A (jump)"), VPAD(25200, 1, 0, 0, 0, "p2 release"),
	{ 25300, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(25400, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(25600, 1, 0, 0, 0, "p2 release"), { 25750, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, VPAD(25800, 1, PAD_A, 0, 0, "p2 A (jump)"),
	VPAD(26000, 1, 0, 0, 0, "p2 release"), { 26200, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 26650, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 27100, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, { 27550, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 28000, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, SHOT(28000, "beam25_4"),
	{ 28450, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 28900, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, { 29350, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 29800, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 30250, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, { 30700, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 31150, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 31600, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, { 32050, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 32500, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 32950, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" }, { 33400, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	{ 33850, S_OBJEV, 57, 0x903, 0, 0, 0, "the beam turns (0x0903, a turnstile hit)" },
	SHOT(34000, "beam25_5"),
	VPAD(38000, 1, 0, 0, 1, "p2 walks (stick up)"), VPAD(39500, 1, 0, 0, 0, "p2 release"), SHOT(40000, "beam25_6"),
	END(42000),
};

// keg23 (-Level 2): the user's crash (xxl_coop.crash-10-22b.log) -- Asterix
// crosses the detector at (-55.9 1.8 -569.6) -> (-48.1 2.2 -569.6) (a fight
// starts), then lights the powder keg at (-40.2 1.9 -549.7) (511E): the game
// crashed in a Roman's move (0x531CE5) during the keg's cutscene. Every
// Roman's component pointer is checked each frame.
const Step kKeg23[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_CPNTCHECK, 93, 18, 0, 0, 0, "component check; the crashing Roman's watched" },
	PLACEAT(2500, HERO_ASTERIX, -58.0f, 2.0f, -569.6f), PLACEAT(2500, HERO_OBELIX, -59.0f, 2.0f, -567.0f),
	PLACEAT(3000, HERO_ASTERIX, -58.0f, 2.0f, -569.6f), PLACEAT(3000, HERO_OBELIX, -59.0f, 2.0f, -567.0f),
	GLIDEAT(3500, HERO_ASTERIX, -48.058f, 2.179f, -569.581f, 1500),   // through the cross detector
	SHOT(5500, "keg23_0"), SHOT(9000, "keg23_1"),
	{ 10000, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 10200, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 10500, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	PLACEAT(11000, HERO_ASTERIX, -40.218f, 1.926f, -549.734f),   // the user's spot by the keg
	{ 11300, S_NEAROBJ, HERO_ASTERIX, 77, 0, 0.85f, 0, "asterix by the powder keg, facing it" },
	{ 11600, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	P1(11800, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(11950, 0, 0, 0, "p1 release"),
	P1(12200, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(12350, 0, 0, 0, "p1 release"),
	P1(12600, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(12750, 0, 0, 0, "p1 release"),
	SHOT(13500, "keg23_2"), SHOT(15000, "keg23_3"), SHOT(17000, "keg23_4"), SHOT(22000, "keg23_5"),
	END(30000),
};

// keg23s (-Level 2): keg23 in single player (player 2 never joins).
const Step kKeg23S[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	{ 1000, S_CPNTCHECK, 93, 18, 0, 0, 0, "component check; the crashing Roman's watched" },
	PLACEAT(2500, HERO_ASTERIX, -58.0f, 2.0f, -569.6f), PLACEAT(2500, HERO_OBELIX, -59.0f, 2.0f, -567.0f),
	PLACEAT(3000, HERO_ASTERIX, -58.0f, 2.0f, -569.6f), PLACEAT(3000, HERO_OBELIX, -59.0f, 2.0f, -567.0f),
	GLIDEAT(3500, HERO_ASTERIX, -48.058f, 2.179f, -569.581f, 1500),   // through the cross detector
	SHOT(5500, "keg23s_0"), SHOT(9000, "keg23s_1"),
	{ 10000, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 10200, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 10500, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	PLACEAT(11000, HERO_ASTERIX, -40.218f, 1.926f, -549.734f),   // the user's spot by the keg
	{ 11300, S_NEAROBJ, HERO_ASTERIX, 77, 0, 0.85f, 0, "asterix by the powder keg, facing it" },
	{ 11600, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	P1(11800, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(11950, 0, 0, 0, "p1 release"),
	P1(12200, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(12350, 0, 0, 0, "p1 release"),
	P1(12600, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(12750, 0, 0, 0, "p1 release"),
	SHOT(13500, "keg23s_2"), SHOT(15000, "keg23s_3"), SHOT(17000, "keg23s_4"), SHOT(22000, "keg23s_5"),
	END(30000),
};

// freeromans23 (level 2 sector 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_LIFTLOG=1): the
// user's -- Asterix lights the hearth by the doors (#423: the doors shut, Obelix
// walks to a marker and waits -- in no-lock mode not followed: Romans come for
// him), then lights the powder keg (#327: the crumbly wall opens, Obelix's wait
// lifted -- no more Romans).
const Step kFreeRomans23[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -44.030f, 1.0f, -541.286f), PLACEAT(2500, HERO_OBELIX, -57.214f, 1.2f, -534.656f),
	PLACEAT(3000, HERO_ASTERIX, -44.030f, 1.0f, -541.286f), PLACEAT(3000, HERO_OBELIX, -57.214f, 1.2f, -534.656f),   // outside the doors
	{ 3200, S_TORCH, 0, 0, 0, 0, 0, "torches" }, { 3400, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" },
	{ 3700, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	P1(4500, PAD_X, 0, 0, "p1 X: light the hearth"), P1(4700, 0, 0, 0, "p1 release"),
	SHOT(6000, "freeromans23_0"), SHOT(14000, "freeromans23_1"), SHOT(24000, "freeromans23_2"),
	{ 26000, S_TORCH, 2, 0, 0, 0, 0, "torch to asterix" }, { 26300, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	PLACEAT(26500, HERO_ASTERIX, -50.901f, 1.3f, -522.217f),   // the user's spot by the keg
	{ 26800, S_NEAROBJ, HERO_ASTERIX, 77, 0, 0.85f, 0, "asterix by the powder keg, facing it" },
	{ 27100, S_TORCH, 6, 0, 0, 0, 0, "cheat: give fire" },
	P1(27300, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(27450, 0, 0, 0, "p1 release"),
	P1(27700, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(27850, 0, 0, 0, "p1 release"),
	P1(28100, PAD_X, 0, 0, "p1 X: light the powder keg"), P1(28250, 0, 0, 0, "p1 release"),
	SHOT(29000, "freeromans23_3"), SHOT(36000, "freeromans23_4"),
	END(40000),
};

// pillar36 (-Level 3, sector 6; the user's with XXL_NOLOCK=1): the light
// pillar's circle (-175.3 3.8 -219.2, live only once the fight is won) hands
// Obelix the lead (4801) -- sent here directly; then Asterix rides the automatic
// cable car (CKHkTelepher) -- at its end it must jump off by itself (0C09).
const Step kPillar36[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -185.961f, 2.554f, -227.849f), PLACEAT(2500, HERO_OBELIX, -186.961f, 2.554f, -228.849f),
	PLACEAT(4000, HERO_OBELIX, -177.76f, 2.63f, -216.34f),
	{ 5000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (the pillar's circle)" },
	SHOT(9000, "pillar36_0"),
	PLACEAT(12000, HERO_ASTERIX, -159.605f, 11.772f, -209.331f),   // the cable car
	SHOT(20000, "pillar36_1"), SHOT(31000, "pillar36_2"), SHOT(36000, "pillar36_3"),
	END(40000),
};
// spitter44 (-Level 4, sector 4): Obelix hits the lever (CKHkActivator, the
// user's spot) that makes a CKHkBonusSpitter spit a magic potion flask, Asterix
// far away; the flask must wait for Asterix, then go to him.
const Step kSpitter44[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, -733.742f, 13.944f, -240.739f),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4150, 1, 0, 0, 0, "p2 release"),
	VPAD(4600, 1, PAD_X, 0, 0, "p2 X"), VPAD(4750, 1, 0, 0, 0, "p2 release"),
	SHOT(5500, "spitter44_0"),
	VPAD(5600, 1, PAD_X, 0, 0, "p2 X"), VPAD(5750, 1, 0, 0, 0, "p2 release"),
	VPAD(6600, 1, PAD_X, 0, 0, "p2 X"), VPAD(6750, 1, 0, 0, 0, "p2 release"),
	SHOT(8000, "spitter44_1"),
	PLACEAT(12000, HERO_ASTERIX, -731.5f, 13.944f, -244.0f),
	SHOT(13000, "spitter44_2"), SHOT(15000, "spitter44_3"),
	END(20000),
};
// spitter44o: the same with Obelix leading (4801 before the lever); l: Obelix
// takes the lead right after hitting it.
const Step kSpitter44l[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_OBELIX, -733.742f, 13.944f, -240.739f),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4150, 1, 0, 0, 0, "p2 release"),
	VPAD(4600, 1, PAD_X, 0, 0, "p2 X"), VPAD(4750, 1, 0, 0, 0, "p2 release"),
	VPAD(5600, 1, PAD_X, 0, 0, "p2 X"), VPAD(5750, 1, 0, 0, 0, "p2 release"),
	VPAD(6600, 1, PAD_X, 0, 0, "p2 X"), VPAD(6750, 1, 0, 0, 0, "p2 release"),
	{ 7500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	SHOT(8000, "spitter44l_1"),
	PLACEAT(12000, HERO_ASTERIX, -731.5f, 13.944f, -244.0f),
	SHOT(13000, "spitter44l_2"), SHOT(15000, "spitter44l_3"),
	END(20000),
};
// spitter44n: Obelix leads (4801); Asterix already on the ledge behind him when
// player 2's Obelix hits the lever: the flask goes to Asterix.
// spitter44a: Obelix leads; player 1's Asterix (not leading) hits the lever
// himself: the flask goes to him.
const Step kSpitter44n[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(2500, HERO_OBELIX, -733.742f, 13.944f, -240.739f), PLACEAT(2500, HERO_ASTERIX, -733.04f, 14.5f, -239.3f),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4150, 1, 0, 0, 0, "p2 release"),
	VPAD(4600, 1, PAD_X, 0, 0, "p2 X"), VPAD(4750, 1, 0, 0, 0, "p2 release"),
	SHOT(5500, "spitter44n_0"),
	VPAD(5600, 1, PAD_X, 0, 0, "p2 X"), VPAD(5750, 1, 0, 0, 0, "p2 release"),
	VPAD(6600, 1, PAD_X, 0, 0, "p2 X"), VPAD(6750, 1, 0, 0, 0, "p2 release"),
	VPAD(7600, 1, PAD_X, 0, 0, "p2 X"), VPAD(7750, 1, 0, 0, 0, "p2 release"),
	SHOT(9000, "spitter44n_1"), SHOT(12000, "spitter44n_2"),
	END(15000),
};
// spitter44d: spitter44n with lever 14 (-734.1 13.9 -241.6, event 0590 = group
// #178) worked as a punch does, instead of the punches, which miss it in some
// runs: the flask goes to Asterix, and the potion's dash is his alone -- Obelix,
// leading, keeps walking (he ran on for good before).
const Step kSpitter44d[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(2500, HERO_OBELIX, -733.742f, 13.944f, -240.739f), PLACEAT(2500, HERO_ASTERIX, -733.04f, 14.5f, -239.3f),
	{ 4000, S_PULLLEVER, 14, 0, 0, 0, 0, "the lever by the spitter" },
	SHOT(6000, "spitter44d_0"),
	VPAD(7000, 1, 0, 0, 1.0f, "p2 forward"), VPAD(8500, 1, 0, 0, 0, "p2 stop"),
	SHOT(9000, "spitter44d_1"),
	END(10000),
};
const Step kSpitter44a[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(2500, HERO_ASTERIX, -733.742f, 13.944f, -240.739f), PLACEAT(2500, HERO_OBELIX, -733.04f, 14.5f, -239.3f),
	P1(4000, PAD_X, 0, 0, "p1 X"), P1(4150, 0, 0, 0, "p1 release"),
	P1(4600, PAD_X, 0, 0, "p1 X"), P1(4750, 0, 0, 0, "p1 release"),
	SHOT(5500, "spitter44a_0"),
	P1(5600, PAD_X, 0, 0, "p1 X"), P1(5750, 0, 0, 0, "p1 release"),
	P1(6600, PAD_X, 0, 0, "p1 X"), P1(6750, 0, 0, 0, "p1 release"),
	P1(7600, PAD_X, 0, 0, "p1 X"), P1(7750, 0, 0, 0, "p1 release"),
	SHOT(9000, "spitter44a_1"), SHOT(12000, "spitter44a_2"),
	END(15000),
};
const Step kSpitter44o[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 2000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(2500, HERO_OBELIX, -733.742f, 13.944f, -240.739f),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4150, 1, 0, 0, 0, "p2 release"),
	VPAD(4600, 1, PAD_X, 0, 0, "p2 X"), VPAD(4750, 1, 0, 0, 0, "p2 release"),
	SHOT(5500, "spitter44o_0"),
	VPAD(5600, 1, PAD_X, 0, 0, "p2 X"), VPAD(5750, 1, 0, 0, 0, "p2 release"),
	VPAD(6600, 1, PAD_X, 0, 0, "p2 X"), VPAD(6750, 1, 0, 0, 0, "p2 release"),
	VPAD(7600, 1, PAD_X, 0, 0, "p2 X"), VPAD(7750, 1, 0, 0, 0, "p2 release"),
	SHOT(9000, "spitter44o_1"),
	PLACEAT(12000, HERO_ASTERIX, -731.5f, 13.944f, -244.0f),
	SHOT(13000, "spitter44o_2"), SHOT(15000, "spitter44o_3"),
	END(20000),
};
// cam14 (-Level 1, sector 4; the modded "Caesar's Challenge" levels, -GameDir):
// Obelix walks from the user's spot onto their camera trigger -- P2's copy of
// the game camera must keep its track, not start over every frame.
const Step kCam14[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 4),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, 998.42f, 200.08f, -23.08f), PLACEAT(2500, HERO_OBELIX, 1000.209f, 200.072f, -21.697f),
	{ 3500, S_CAMDUMP, 0, 0, 0, 0, 0, "camera" },
	GLIDEAT(4000, HERO_OBELIX, 1002.575f, 200.053f, -17.638f, 1500),
	SHOT(6000, "cam14_0"),
	{ 6500, S_CAMDUMP, 0, 0, 0, 0, 0, "camera" },
	SHOT(8000, "cam14_1"),
	END(9000),
};

// crates1cc (-Level 1 -GameDir "SPQR\Caesars Challenge"): the user's Caesar's
// Challenge level 1 -- its opening scene skipped, player 2 joins and warps
// Obelix to Asterix (R3 + A; Obelix's camera turns to a door, away from the
// crates), then Asterix walks past the crates to -257.7 29.8 609.2: crates there
// came in only as he got near, some as Obelix turned his camera.
#define CRATES1CC(t, n) { t, S_CRATE, 0, -3, 0, 0, 0, n }, SHOT(t, n)
const Step kCrates1CC[] = {
	MARK(0, "baseline"), CINE_ENTER(1500), { 0, S_WATCHCAM, 0, 0, 0, 0, 0, "watch camera" },
	VPAD(5000, 1, PAD_START, 0, 0, "p2 start"), VPAD(5200, 1, 0, 0, 0, "p2 release"),
	VPAD(7000, 1, PAD_RTHUMB, 0, 0, "p2 R3"), VPAD(7100, 1, PAD_RTHUMB | PAD_A, 0, 0, "p2 R3 + A"), VPAD(7300, 1, 0, 0, 0, "p2 release"),
	CRATES1CC(9500, "crates1cc_0"),
	PLACEAT(9800, HERO_ASTERIX, -268.43f, 30.2f, 583.74f),   // a step ahead: the warp left them on one spot
	GLIDEAT(10000, HERO_ASTERIX, -257.661f, 29.799f, 609.162f, 5000),
	CRATES1CC(11000, "crates1cc_1"), CRATES1CC(12500, "crates1cc_2"), CRATES1CC(14000, "crates1cc_3"),
	CRATES1CC(15500, "crates1cc_4"), CRATES1CC(17500, "crates1cc_5"),
	VPADR(18000, 1, 0.35f, 0.0f, "p2 camera right"),
	CRATES1CC(19000, "crates1cc_6"), CRATES1CC(20000, "crates1cc_7"), CRATES1CC(21000, "crates1cc_8"), CRATES1CC(22000, "crates1cc_9"),
	VPADR(22500, 1, 0.0f, 0.0f, "p2 camera still"),
	CRATES1CC(24000, "crates1cc_10"),
	END(25000),
};

// pillar36l: the same circle's lock (4801, 4802 sent as its group does) with
// Asterix on it: no-lock mode keeps it (a corkscrew's circle; off with
// XXL_NOLOCKCORK=0); R3 + B still frees him.
const Step kPillar36l[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(2500, HERO_ASTERIX, -185.961f, 2.554f, -227.849f), PLACEAT(2500, HERO_OBELIX, -186.961f, 2.554f, -228.849f),
	PLACEAT(4000, HERO_OBELIX, -177.76f, 2.63f, -216.34f),
	PLACEAT(4500, HERO_ASTERIX, -175.255f, 3.817f, -219.225f),
	{ 5000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (the pillar's circle)" },
	{ 5000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "asterix waits (the pillar's circle)" },
	P1(6000, 0, 1.0f, 0, "p1 walks off"), P1(7500, 0, 0, 0, "p1 stop"),
	SHOT(8000, "pillar36l_0"),
	{ 9000, S_UNLOCKHERO, HERO_ASTERIX, 0, 0, 0, 0, "p1 R3+B" },
	SHOT(11000, "pillar36l_1"),
	END(12000),
};

// cabled2 / spyd2 (level 2, with XXL_DOWNAT=1:1000): Obelix down, Asterix walks
// onto the cable car / glides to the second spy three times -- each time put
// back, the car never boarded, the talk never started.
const Step kCableD2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1600, S_PLACE, HERO_ASTERIX, 2, 0, 0, 0, "asterix before car" },
	P1(1800, 0, -1, 0, "p1 left"), P1(3500, 0, 0, 0, "p1 stop"),
	{ 4000, S_PLACE, HERO_ASTERIX, 0, 0, 0, 0, "asterix on car" },
	{ 7000, S_PLACE, HERO_ASTERIX, 0, 0, 0, 0, "asterix on car" },
	{ 10000, S_PLACE, HERO_ASTERIX, 0, 0, 0, 0, "asterix on car" },
	SHOT(12500, "cabled2"),
	END(14000),
};
const Step kSpyD2[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	PLACEAT(2000, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(3000, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	GLIDEAT(7000, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	GLIDEAT(11000, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(14000, "spyd2"),
	END(16000),
};

// spyrev (level 2, XXL_DOWNAT=1:2800 XXL_UPAT=6000): the second spy's talk held
// back while Obelix is down; revived, Asterix walks up again -- it must start.
const Step kSpyRev[] = {
	MARK(0, "baseline"), { 0, S_WATCHCLUE, 0, 0, 0, 0, 0, "watch spy" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_WATCHCLUE, 1, 0, 0, 0, 0, "open the spies' gate" },
	PLACEAT(1500, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	PLACEAT(2000, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(3000, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	PLACEAT(7000, HERO_ASTERIX, -752.0f, 508.0f, -1012.0f),
	GLIDEAT(8000, HERO_ASTERIX, -746.46f, 507.8f, -1007.37f, 1000),
	SHOT(11000, "spyrev"),
	END(13000),
};

// swing (level 2): Asterix walks onto the cable car, player 2's Obelix pulls.
const Step kSwing[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PROBE(1000, 1),
	{ 1500, S_PLACE, HERO_OBELIX, 1, 0, 0, 0, "obelix to rope" },
	{ 1600, S_PLACE, HERO_ASTERIX, 2, 0, 0, 0, "asterix before car" },
	P1(1800, 0, -1, 0, "p1 left (held)"), P1(2300, 0, 0, 0, "p1 neutral"),
	{ 2400, S_PLACE, HERO_ASTERIX, 0, 0, 0, 0, "asterix on car" },
	{ 3000, S_SCATTER, HERO_OBELIX, 0, 0, 0, 0, "scatter" },
	{ 3600, S_SCATTER, HERO_OBELIX, 0, 0, 0, 0, "scatter" },
	SHOT(3800, "swing_00"),
	VPAD(3850, 1, PAD_X, 0, 0, "p2 X (held)"), SHOT(4400, "swing_00b"), VPAD(5850, 1, 0, 0, 0, "p2 release"),
	VPAD(4000, 1, PAD_X, 0, 0, "p2 X"), VPAD(4150, 1, 0, 0, 0, "p2 release"),
	VPAD(4600, 1, PAD_X, 0, 0, "p2 X"), VPAD(4750, 1, 0, 0, 0, "p2 release"),
	SHOT(5000, "swing_01"),
	VPAD(5200, 1, PAD_X, 0, 0, "p2 X"), VPAD(5350, 1, 0, 0, 0, "p2 release"),
	VPAD(5800, 1, PAD_X, 0, 0, "p2 X"), VPAD(5950, 1, 0, 0, 0, "p2 release"),
	VPAD(6400, 1, PAD_X, 0, 0, "p2 X"), VPAD(6550, 1, 0, 0, 0, "p2 release"),
	SHOT(7000, "swing_02"),
	END(9000),
};

// unlockpillar / unlockcar / unlockseq (level 2, XXL_P2HERO=asterix XXL_LIFTLOG=1):
// player 2's Asterix, locked by the level, presses R3 + B -- undone as for a
// fallen partner, Asterix on his last solid ground; then it works again.
#define UNLOCK(t) VPAD(t, 1, PAD_RTHUMB, 0, 0, "p2 R3"), VPAD(t + 200, 1, PAD_RTHUMB | PAD_B, 0, 0, "p2 R3+B"), \
                  VPAD(t + 400, 1, 0, 0, 0, "p2 release")
const Step kUnlockPillar[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(5500, "unlockpillar_0"),
	UNLOCK(6500),
	SHOT(8000, "unlockpillar_1"),
	GLIDEAT(10000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(11700, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(14000, "unlockpillar_2"),
	END(15000),
};
const Step kUnlockCar[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1400, HERO_ASTERIX, -782.0f, 511.5f, -1059.9f),
	GLIDEAT(1500, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	VPAD(2800, 1, 0, -1.0f, 0.0f, "p2 left"), VPAD(3300, 1, 0, 0, 0, "p2 neutral"),
	PLACEAT(3400, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),
	SHOT(5000, "unlockcar_0"),
	UNLOCK(6000),
	SHOT(7500, "unlockcar_1"),
	{ 10000, S_PLACE, HERO_OBELIX, 1, 0, 0, 0, "obelix to rope" },
	GLIDEAT(10000, HERO_ASTERIX, -778.0f, 511.5f, -1059.9f, 1000),
	GLIDEAT(11200, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	VPAD(12500, 1, 0, -1.0f, 0.0f, "p2 left"), VPAD(13000, 1, 0, 0, 0, "p2 neutral"),
	PLACEAT(13100, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),
	SHOT(15000, "unlockcar_2"),
	END(16000),
};
const Step kUnlockSeq[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, -746.5f, 514.5f, -981.4f),
	PLACEAT(1500, HERO_ASTERIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(5000, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	GLIDEAT(5300, HERO_OBELIX, -754.0f, 515.6f, -977.8f, 1000),   // through 03C8 (Asterix freed, platform off)
	UNLOCK(7000),
	SHOT(8500, "unlockseq_1"),
	GLIDEAT(10000, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(11700, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(14000, "unlockseq_2"),
	END(15000),
};

// unlockcar33 (-Level 3, XXL_LIFTLOG=1): the user's -- player 2's Obelix pulls
// Asterix partway (not past the leader-switching detector) and player 1's
// Asterix presses R3 + B (S_UNLOCKHERO): his camera must follow him again, not the car.
const Step kUnlockCar33[] = {
	MARK(0, "baseline"), LOADSECTOR(200, 3),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1200, HERO_ASTERIX, 296.0f, 10.9f, -209.1f),   // walked over: a trail of safe spots
	GLIDEAT(1300, HERO_ASTERIX, 306.0f, 10.9f, -209.1f, 1000),
	PLACEAT(2500, HERO_ASTERIX, 307.582f, 13.073f, -209.154f), PLACEAT(2500, HERO_OBELIX, 307.582f, 10.34f, -209.154f),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X (held)"),
	GLIDEAT(5000, HERO_OBELIX, 292.0f, 10.43f, -209.1f, 2500),
	SHOT(7700, "unlockcar33_0"),
	{ 8000, S_UNLOCKHERO, HERO_ASTERIX, 0, 0, 0, 0, "player 1's R3 + B" },
	VPAD(8100, 1, 0, 0, 0, "p2 release"),
	SHOT(9000, "unlockcar33_1"), SHOT(12000, "unlockcar33_2"),
	GLIDEAT(12500, HERO_ASTERIX, 296.0f, 10.9f, -209.1f, 1500),
	SHOT(14500, "unlockcar33_3"),
	END(15500),
};

// unlockcar21 (level 2, XXL_LIFTLOG=1): the user's -- player 2's Obelix pulls the
// cable car far (X again and again), player 1's Asterix presses R3 + B
// (S_UNLOCKHERO): his camera must be his usual one again.
const Step kUnlockCar21[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_OBELIX, 1, 0, 0, 0, "obelix to rope" },
	PLACEAT(1600, HERO_ASTERIX, -782.0f, 511.5f, -1059.9f),
	GLIDEAT(1700, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	P1(3000, 0, -1, 0, "p1 left (held)"), P1(3500, 0, 0, 0, "p1 neutral"),
	PLACEAT(3600, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),
	{ 4500, S_SCATTER, HERO_OBELIX, 0, 0, 0, 0, "scatter" },
	SHOT(5500, "unlockcar21_0"),
	{ 6000, S_CARPOS, HERO_ASTERIX, 0, 0, 6.0f, 0, "car along its rope" },
	{ 8000, S_CARPOS, HERO_ASTERIX, 0, 0, 12.0f, 0, "car along its rope" },
	{ 10000, S_CARPOS, HERO_ASTERIX, 0, 0, 18.0f, 0, "car along its rope" },
	SHOT(13200, "unlockcar21_1"),
	{ 13500, S_UNLOCKHERO, HERO_ASTERIX, 0, 0, 0, 0, "player 1's R3 + B" },
	SHOT(14500, "unlockcar21_2"), SHOT(17000, "unlockcar21_3"),
	END(18000),
};

// carpause (level 2, XXL_LIFTLOG=1): Asterix sits in the cable car, nobody
// pulls; the pause menu 20 s, then play -- he stays in the car (order 9)
// throughout (no timeout lets go of him).
const Step kCarPause[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1400, HERO_ASTERIX, -782.0f, 511.5f, -1059.9f),
	GLIDEAT(1500, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	P1(2800, 0, -1, 0, "p1 left (held)"), P1(3300, 0, 0, 0, "p1 neutral"),
	PLACEAT(3400, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),
	SHOT(4500, "carpause_0"),
	{ 6000, S_POKE8, -3, 2, 0, 0, 0, "the pause menu (game manager state 2)" },
	{ 6000, S_POKE8, -2, 0x5E, 0, (float)0x10, 0, "its bars (game manager +0x5C bit 20) on" },
	SHOT(16000, "carpause_1"),
	{ 26000, S_POKE8, -3, 1, 0, 0, 0, "back to the game (state 1; the game clears the bars itself)" },
	SHOT(28000, "carpause_2"), SHOT(36000, "carpause_3"),
	GLIDEAT(38000, HERO_ASTERIX, -778.0f, 511.5f, -1059.9f, 1500),
	GLIDEAT(40000, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	P1(41300, 0, -1, 0, "p1 left (held)"), P1(41800, 0, 0, 0, "p1 neutral"),
	PLACEAT(41900, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),
	SHOT(43500, "carpause_4"),
	END(44500),
};

// liftercar (level 2, XXL_LIFTLOG=1): Asterix sits in the cable car (locked,
// waits for Obelix at the rope), Obelix goes onto the lifter instead: undone as
// for a fallen Obelix. Then the car is worked as in swing: still working.
const Step kLifterCar[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1400, HERO_ASTERIX, -782.0f, 511.5f, -1059.9f),   // walked over: a trail of safe spots
	GLIDEAT(1500, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	P1(2800, 0, -1, 0, "p1 left (held)"), P1(3300, 0, 0, 0, "p1 neutral"),
	PLACEAT(3400, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),
	SHOT(4500, "liftercar_0"),
	PLACEAT(5000, HERO_OBELIX, -746.697f, 506.5f, -992.08f),
	SHOT(7500, "liftercar_1"), SHOT(10000, "liftercar_2"),
	GLIDEAT(12000, HERO_ASTERIX, -778.0f, 511.5f, -1059.9f, 1500),   // away from the car, as a player walks off
	{ 14000, S_PLACE, HERO_OBELIX, 1, 0, 0, 0, "obelix to rope" },
	GLIDEAT(14100, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	P1(15400, 0, -1, 0, "p1 left (held)"), P1(15900, 0, 0, 0, "p1 neutral"),
	PLACEAT(16000, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),
	{ 16600, S_SCATTER, HERO_OBELIX, 0, 0, 0, 0, "scatter" },
	SHOT(17200, "liftercar_3"),
	VPAD(17300, 1, PAD_X, 0, 0, "p2 X"), VPAD(17450, 1, 0, 0, 0, "p2 release"),
	VPAD(17900, 1, PAD_X, 0, 0, "p2 X"), VPAD(18050, 1, 0, 0, 0, "p2 release"),
	VPAD(18500, 1, PAD_X, 0, 0, "p2 X"), VPAD(18650, 1, 0, 0, 0, "p2 release"),
	VPAD(19100, 1, PAD_X, 0, 0, "p2 X"), VPAD(19250, 1, 0, 0, 0, "p2 release"),
	VPAD(19700, 1, PAD_X, 0, 0, "p2 X"), VPAD(19850, 1, 0, 0, 0, "p2 release"),
	SHOT(20500, "liftercar_4"),
	END(22000),
};

// swingundo (level 2, XXL_DOWNAT=1:3500): Asterix on the cable car, Obelix
// falls before pulling -- the car let go and back at its start, still shown.
const Step kSwingUndo[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_OBELIX, 1, 0, 0, 0, "obelix to rope" },
	{ 1600, S_PLACE, HERO_ASTERIX, 2, 0, 0, 0, "asterix before car" },
	P1(1800, 0, -1, 0, "p1 left (held)"), P1(2300, 0, 0, 0, "p1 neutral"),
	{ 2400, S_PLACE, HERO_ASTERIX, 0, 0, 0, 0, "asterix on car" },
	SHOT(3300, "swingundo_0"), SHOT(4500, "swingundo_1"), SHOT(7000, "swingundo_2"),
	END(8000),
};

// swingsp (level 2): the same without player 2 -- player 1 drives whoever the
// game makes leader (Obelix, once Asterix sits in the cable car).
const Step kSwingSp[] = {
	MARK(0, "baseline"), PROBE(1000, 1),
	{ 1600, S_PLACE, HERO_ASTERIX, 2, 0, 0, 0, "asterix before car" },
	{ 2400, S_PLACE, HERO_ASTERIX, 0, 0, 0, 0, "asterix on car" },
	{ 4500, S_PLACE, HERO_OBELIX, 1, 0, 0, 0, "obelix to rope (now leader)" },
	SHOT(5000, "swingsp_00"),
	P1(5500, PAD_X, 0, 0, "p1 X"), P1(5650, 0, 0, 0, "p1 release"),
	P1(6100, PAD_X, 0, 0, "p1 X"), P1(6250, 0, 0, 0, "p1 release"),
	SHOT(6600, "swingsp_01"),
	P1(7000, PAD_X, 0, 0, "p1 X"), P1(7150, 0, 0, 0, "p1 release"),
	P1(7600, PAD_X, 0, 0, "p1 X"), P1(7750, 0, 0, 0, "p1 release"),
	SHOT(8500, "swingsp_02"),
	END(10000),
};

// lead2 (level 2): the trio switches to player 2's Obelix while player 1's
// Asterix stays free: player 1's half gets a follow camera of its own.
const Step kLead2[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "switch to Obelix" },
	P1(2000, 0, 0, 1, "p1 up (held)"), VPAD(2000, 1, 0, 1, 0, "p2 right (held)"),
	SHOT(3500, "lead2_00"),
	P1(3600, 0, 0, 0, "p1 neutral"), VPAD(3600, 1, 0, 0, 0, "p2 neutral"),
	VPAD(4000, 1, PAD_A, 0, 0, "p2 jump"), VPAD(4150, 1, 0, 0, 0, "p2 release"),
	SHOT(4400, "lead2_01"),
	{ 5500, S_TRIOEV, 0x4800, 0, 0, 0, 0, "switch to Asterix" },
	SHOT(6500, "lead2_02"),
	END(7000),
};

// circle (level 2): player 2's Obelix where he set off the spinning platform's
// blue circle in a player's session, Asterix nearby.
const Step kCircle[] = {
	MARK(0, "baseline"), { 0, S_PLATES, 0, 0, 0, 0, 0, "plates" },
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	GLIDE(1000, HERO_ASTERIX, 3, 6000), GLIDE(1000, HERO_OBELIX, 3, 6000), GLIDE(1000, HERO_IDEFIX, 3, 6000),
	SHOT(7500, "circle_00"),
	{ 7600, S_PLATES, 0, 0, 0, 0, 0, "plates" },
	END(8000),
};

// borrow (level 2): the trio tells the companion (player 2's Obelix) to wait
// (0x4802), then to follow again (0x4803): he should go to the AI and back.
const Step kBorrow[] = {
	MARK(0, "baseline"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_TRIOEV, 0x4802, 0, 0, 0, 0, "companion wait" },
	VPAD(1600, 1, 0, 1, 0, "p2 right (held)"),
	{ 3500, S_TRIOEV, 0x4803, 0, 0, 0, 0, "companion follow" },
	VPAD(5500, 1, 0, 0, 0, "p2 neutral"),
	END(6500),
};

// survey: log which group-action objects the level has, then leave.
const Step kBossScan[] = {
	MARK(0, "bossscan"), { 3000, S_BOSSSCAN, 0, 0, 0, 0, 0, "boss scan" }, END(3500),
};
const Step kSurvey[] = {
	MARK(0, "survey"), { 0, S_DETECT, 0, 0, 0, 0, 0, "detect" }, SHOT(100, "survey"), END(300),
};

// towerscan: the level's mobile towers (CKHkMobileTower, 2,176), Obelix put by the nearest.
const Step kTowerScan[] = {
	MARK(0, "towerscan"), { 2000, S_NEAROBJ, HERO_OBELIX, 176, 0, 6.0f, 0, "obelix by a mobile tower" }, SHOT(2500, "tower"), END(3000),
};

// A stand-in for an area hit's source (S_AREAHIT): it answers "the hit's
// damage" (0x0C0A: byte 0 the points, dword 1 the kind) as a mobile tower does
// (CKHkMobileTower::sendEvent, from its parameters +0x54: +9, +4 & 7). A level's
// own tower stays in its pool, parameters unset, until its squad sends it.
int __fastcall StandInTowerEvent(void* self, void* edx, int event, uint32_t* reply) {
	if ((event & 0xFFFF) == 0x0C0A && reply) { reply[0] = 3; reply[1] = 0; }
	return 0;
}
void* g_standInTowerVt[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, (void*)StandInTowerEvent, nullptr, nullptr };
void* g_standInTower = g_standInTowerVt;   // the object: its vtable at +0

// towerhit: the mobile tower's shockwave (trio event 0x4C14) hits Obelix,
// player 2's, not leading -- his health goes down, not Asterix's
// (caesar_challenge_24_10.log: Obelix's hits drained Asterix to a game over).
const Step kTowerHit[] = {
	MARK(0, "towerhit"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 5.0f, 0, "obelix 5 units ahead of asterix" },
	{ 2500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" }, SHOT(2600, "hit1"),
	{ 4500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" }, SHOT(4600, "hit2"),
	{ 6500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" },
	{ 8500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" }, SHOT(8600, "hit4"),
	{ 10500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" },
	{ 12500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" }, SHOT(12600, "hit6"),
	{ 14500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" },
	{ 16500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" }, SHOT(16600, "hit8"),
	{ 18500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" },
	{ 20500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "shockwave on obelix" }, SHOT(20600, "hit10"),
	SHOT(23000, "after"), END(24000),
};

// shieldjump: player 1's Asterix with five shields, the helmet counter counting
// up to 1985 in the middle (four digits: a wide plate), then sliding out past
// the health row (a hit, then points back: the fifth shield part full, as in the
// user's) -- with xxl_widescreen2d.asi the last shield jumped right as the
// counter passed it.
const Step kShieldJump[] = {
	MARK(0, "shieldjump"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 800, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 5.0f, 0, "obelix 5 units ahead of asterix" },
	{ 1000, S_POKE8, -1, 0x68, 0, 193.0f, 0, "1985 helmets: the counter shows, counting up" }, { 1000, S_POKE8, -1, 0x69, 0, 7.0f, 0, "" },
	{ 1100, S_GIVE, HERO_ASTERIX, 1, 0, 0, 0, "a shield" }, { 1150, S_GIVE, HERO_ASTERIX, 1, 0, 0, 0, "a shield" },
	{ 1200, S_GIVE, HERO_ASTERIX, 0, 0, 10.0f, 0, "health" },
	{ 2500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit: the health row stays up" }, SHOT(3000, "s00"), SHOT(3100, "s01"), SHOT(3200, "s02"), SHOT(3300, "s03"), SHOT(3400, "s04"), SHOT(3500, "s05"),
	SHOT(3600, "s06"), SHOT(3700, "s07"), SHOT(3800, "s08"), SHOT(3900, "s09"), { 4000, S_GIVE, HERO_ASTERIX, 0, 0, 1.0f, 0, "a point back: the health row stays up, the fifth shield part full" }, SHOT(4000, "s10"), SHOT(4100, "s11"),
	SHOT(4200, "s12"), SHOT(4300, "s13"), SHOT(4400, "s14"), SHOT(4500, "s15"), SHOT(4600, "s16"), SHOT(4700, "s17"), SHOT(4800, "s18"),
	SHOT(4900, "s19"), SHOT(5000, "s20"), SHOT(5100, "s21"), SHOT(5200, "s22"), SHOT(5300, "s23"), SHOT(5400, "s24"), { 5500, S_GIVE, HERO_ASTERIX, 0, 0, 1.0f, 0, "a point back: the health row stays up, the fifth shield part full" },
	SHOT(5500, "s25"), SHOT(5600, "s26"), SHOT(5700, "s27"), SHOT(5800, "s28"), SHOT(5900, "s29"), SHOT(6000, "s30"), SHOT(6100, "s31"),
	SHOT(6200, "s32"), SHOT(6300, "s33"), SHOT(6400, "s34"), SHOT(6500, "s35"), SHOT(6600, "s36"), SHOT(6700, "s37"), SHOT(6800, "s38"),
	SHOT(6900, "s39"), { 7000, S_GIVE, HERO_ASTERIX, 0, 0, 1.0f, 0, "a point back: the health row stays up, the fifth shield part full" }, SHOT(7000, "s40"), SHOT(7100, "s41"), SHOT(7200, "s42"), SHOT(7300, "s43"), SHOT(7400, "s44"),
	SHOT(7500, "s45"), SHOT(7600, "s46"), SHOT(7700, "s47"), SHOT(7800, "s48"), SHOT(7900, "s49"), SHOT(8000, "s50"), SHOT(8100, "s51"),
	SHOT(8200, "s52"), SHOT(8300, "s53"), SHOT(8400, "s54"), { 8500, S_GIVE, HERO_ASTERIX, 0, 0, 1.0f, 0, "a point back: the health row stays up, the fifth shield part full" }, SHOT(8500, "s55"), SHOT(8600, "s56"), SHOT(8700, "s57"),
	SHOT(8800, "s58"), SHOT(8900, "s59"), SHOT(9000, "s60"), SHOT(9100, "s61"), SHOT(9200, "s62"), SHOT(9300, "s63"), SHOT(9400, "s64"),
	SHOT(9500, "s65"), SHOT(9600, "s66"), SHOT(9700, "s67"), SHOT(9800, "s68"), SHOT(9900, "s69"),
	END(10500),
};

// downcombo: player 1's Asterix with a full combo bar goes down (area hits);
// his bar drains to empty in half a second, "full" and all -- the HUD's bar
// too; Obelix's (part full) runs down as before. Player 2's Obelix revives
// him: Asterix starts with an empty bar.
const Step kDownCombo[] = {
	MARK(0, "downcombo"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 5.0f, 0, "obelix 5 units ahead of asterix" },
	{ 2000, S_GIVE, HERO_ASTERIX, 3, 0, 30.0f, 0, "asterix's combo bar: 30 hits" },
	{ 2500, S_GIVE, HERO_ASTERIX, 3, 0, 10.0f, 0, "10 more: full (the bar sees it fill)" },
	{ 2000, S_GIVE, HERO_OBELIX, 3, 0, 8.0f, 0, "obelix's combo bar part full" },
	SHOT(3000, "full"),
	{ 3500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" }, { 5000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" },
	{ 6500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" }, { 8000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" },
	SHOT(9400, "d_before"), { 9500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit: down" },
	SHOT(9560, "d00"), SHOT(9650, "d01"), SHOT(9750, "d02"), SHOT(9850, "d03"), SHOT(9950, "d04"), SHOT(10100, "d05"), SHOT(10300, "d06"), SHOT(10600, "d07"), SHOT(11000, "d08"), SHOT(12000, "d09"), SHOT(13500, "d10"), SHOT(15500, "d11"),
	{ 16000, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 0, 0, "obelix to asterix" },
	VPAD(16500, 1, PAD_B, 0, 0, "p2 holds B"), VPAD(22500, 1, 0, 0, 0, "p2 release"),
	SHOT(23500, "revived"), END(24000),
};
// downcombop: downcombo with Asterix's bar part full (two levels lit): the
// glows go out one by one as it drains, then the bar hides.
const Step kDownComboP[] = {
	MARK(0, "downcombop"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 5.0f, 0, "obelix 5 units ahead of asterix" },
	{ 2000, S_GIVE, HERO_OBELIX, 3, 0, 8.0f, 0, "obelix's combo bar part full" },
	{ 3500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" }, { 5000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" },
	{ 6500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" }, { 8000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" },
	{ 8300, S_GIVE, HERO_ASTERIX, 3, 0, 25.0f, 0, "asterix's combo bar part full (25 hits, a second before the fall)" },
	SHOT(9400, "d_before"), { 9500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit: down" },
	SHOT(9560, "d00"), SHOT(9650, "d01"), SHOT(9750, "d02"), SHOT(9850, "d03"), SHOT(9950, "d04"), SHOT(10100, "d05"), SHOT(10300, "d06"), SHOT(10600, "d07"), SHOT(11000, "d08"), SHOT(12000, "d09"), SHOT(13500, "d10"), SHOT(15500, "d11"),
	{ 16000, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 0, 0, "obelix to asterix" },
	VPAD(16500, 1, PAD_B, 0, 0, "p2 holds B"), VPAD(22500, 1, 0, 0, 0, "p2 release"),
	SHOT(23500, "revived"), END(24000),
};


// downcomboo: downcombo's other half -- player 2's Obelix (his HUD the copy)
// with a full bar goes down; his bar empties.
const Step kDownComboO[] = {
	MARK(0, "downcomboo"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 5.0f, 0, "obelix 5 units ahead of asterix" },
	{ 2000, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix's combo bar full" },
	VPAD(2500, 1, PAD_BACK, 0, 0, "p2 holds back (his HUD up)"), SHOT(3000, "full"),
	{ 3500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" }, { 5000, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" },
	{ 6500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" }, { 8000, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" },
	{ 9500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit: down" },
	SHOT(9600, "down1"), SHOT(11000, "down2"), VPAD(11500, 1, 0, 0, 0, "p2 release"),
	END(12000),
};

// overcombo (level 2): Asterix goes down, then player 2's Obelix with a full
// combo bar: the game over. After the respawn player 2's bar is empty, as
// player 1's (user: Obelix kept his full bar through a game over -- player 2's
// HUD copy put its full state back over the interface's reset).
const Step kOverCombo[] = {
	MARK(0, "overcombo"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 5.0f, 0, "obelix 5 units ahead of asterix" },
	{ 3500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" }, { 5000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" },
	{ 6500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" }, { 8000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit" },
	{ 9500, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "hit: asterix down" },
	{ 10000, S_GIVE, HERO_OBELIX, 3, 0, 20.0f, 0, "obelix's combo bar: 20 hits (full at 30 with Asterix down)" },
	{ 10500, S_GIVE, HERO_OBELIX, 3, 0, 20.0f, 0, "20 more: full (the bar sees it fill)" },
	SHOT(11800, "full"),
	{ 12000, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" }, { 13500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" },
	{ 15000, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" }, { 16500, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit" },
	{ 18000, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "hit: obelix down, game over" },
	SHOT(19000, "over1"), SHOT(22000, "over2"),
	SHOT(27000, "respawn1"), VPAD(28000, 1, PAD_BACK, 0, 0, "p2 holds back (his HUD up)"),
	SHOT(29000, "respawn2"), SHOT(31000, "respawn3"), VPAD(31500, 1, 0, 0, 0, "p2 release"),
	SHOT(33000, "respawn4"),
	END(34000),
};

// catapultcombo (level 3): player 2's Obelix (not leading) hits LV3-1's Roman
// catapult (CKHkCatapult 2,130; user: 106.0 1.0 11.5) -- his combo bar takes
// every hit, Asterix's (leading, far off at the level's start) none
// (catapult_shared_combo.log: both bars went up).
const Step kCatapultCombo[] = {
	MARK(0, "catapultcombo"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	PLACEAT(1500, HERO_OBELIX, 106.008f, 0.951f, 11.521f),
	{ 1800, S_NEAROBJ, HERO_OBELIX, 130, 0, 2.5f, 0, "obelix by the catapult" }, SHOT(2200, "before"),
	VPAD(2500, 1, PAD_X, 0, 0, "p2 X"), VPAD(2700, 1, 0, 0, 0, "p2 release"),
	VPAD(3500, 1, PAD_X, 0, 0, "p2 X"), VPAD(3700, 1, 0, 0, 0, "p2 release"), SHOT(3900, "hit2"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"), VPAD(4700, 1, 0, 0, 0, "p2 release"),
	VPAD(5500, 1, PAD_X, 0, 0, "p2 X"), VPAD(5700, 1, 0, 0, 0, "p2 release"), SHOT(5900, "hit4"),
	VPAD(6500, 1, PAD_X, 0, 0, "p2 X"), VPAD(6700, 1, 0, 0, 0, "p2 release"),
	VPAD(7500, 1, PAD_X, 0, 0, "p2 X"), VPAD(7700, 1, 0, 0, 0, "p2 release"), SHOT(7900, "hit6"),
	END(8500),
};
// catapultcomboa (level 3, XXL_P2HERO=asterix): the same with player 2's
// Asterix, Obelix leading.
const Step kCatapultComboA[] = {
	MARK(0, "catapultcomboa"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	PLACEAT(1500, HERO_ASTERIX, 106.008f, 0.951f, 11.521f),
	{ 1800, S_NEAROBJ, HERO_ASTERIX, 130, 0, 2.0f, 0, "asterix by the catapult" }, SHOT(2200, "before"),
	VPAD(2500, 1, PAD_X, 0, 0, "p2 X"), VPAD(2700, 1, 0, 0, 0, "p2 release"),
	VPAD(3500, 1, PAD_X, 0, 0, "p2 X"), VPAD(3700, 1, 0, 0, 0, "p2 release"), SHOT(3900, "hit2"),
	VPAD(4500, 1, PAD_X, 0, 0, "p2 X"), VPAD(4700, 1, 0, 0, 0, "p2 release"),
	VPAD(5500, 1, PAD_X, 0, 0, "p2 X"), VPAD(5700, 1, 0, 0, 0, "p2 release"), SHOT(5900, "hit4"),
	END(6500),
};

// idle: just boot to gameplay, log and screenshot. Smoke test for the harness.
const Step kIdle[] = {
	MARK(0, "baseline"), SHOT(100, "idle_00"), SHOT(3000, "idle_01"), END(3500),
};

// p2busy: player 1 changes direction every 200 ms and taps A while player 2
// moves and attacks on its own rhythm. Player 2 must stay joined: a busy
// player 1 must not make player 2's pad look like the game's.
const Step kP2Busy[] = {
	MARK(0, "baseline"),
	VPAD(500, 1, PAD_START, 0, 0, "p2 start"), VPAD(700, 1, 0, 0, 0, "p2 release"),
	P1(1500, PAD_A, 1, 0, "p1 right"),
	VPAD(1570, 1, PAD_X, -1, 0, "p2 left"),
	P1(1700, 0, 0, 1, "p1 up"),
	P1(1900, PAD_A, -1, 0, "p1 left"),
	VPAD(1900, 1, 0, 0, -1, "p2 down"),
	P1(2100, 0, 0, -1, "p1 down"),
	VPAD(2230, 1, 0, 1, 0, "p2 right"),
	P1(2300, PAD_A, 1, 0, "p1 right"),
	P1(2500, 0, 0, 1, "p1 up"),
	VPAD(2560, 1, PAD_X, 0, 1, "p2 up"),
	P1(2700, PAD_A, -1, 0, "p1 left"),
	VPAD(2890, 1, 0, -1, 0, "p2 left"),
	P1(2900, 0, 0, -1, "p1 down"),
	P1(3100, PAD_A, 1, 0, "p1 right"),
	VPAD(3220, 1, 0, 0, -1, "p2 down"),
	P1(3300, 0, 0, 1, "p1 up"),
	P1(3500, PAD_A, -1, 0, "p1 left"),
	VPAD(3550, 1, PAD_X, 1, 0, "p2 right"),
	P1(3700, 0, 0, -1, "p1 down"),
	VPAD(3880, 1, 0, 0, 1, "p2 up"),
	P1(3900, PAD_A, 1, 0, "p1 right"),
	P1(4100, 0, 0, 1, "p1 up"),
	VPAD(4210, 1, 0, -1, 0, "p2 left"),
	P1(4300, PAD_A, -1, 0, "p1 left"),
	P1(4500, 0, 0, -1, "p1 down"),
	VPAD(4540, 1, PAD_X, 0, -1, "p2 down"),
	P1(4700, PAD_A, 1, 0, "p1 right"),
	VPAD(4870, 1, 0, 1, 0, "p2 right"),
	P1(4900, 0, 0, 1, "p1 up"),
	P1(5100, PAD_A, -1, 0, "p1 left"),
	VPAD(5200, 1, 0, 0, 1, "p2 up"),
	P1(5300, 0, 0, -1, "p1 down"),
	P1(5500, PAD_A, 1, 0, "p1 right"),
	VPAD(5530, 1, PAD_X, -1, 0, "p2 left"),
	P1(5700, 0, 0, 1, "p1 up"),
	VPAD(5860, 1, 0, 0, -1, "p2 down"),
	P1(5900, PAD_A, -1, 0, "p1 left"),
	P1(6100, 0, 0, -1, "p1 down"),
	VPAD(6190, 1, 0, 1, 0, "p2 right"),
	P1(6300, PAD_A, 1, 0, "p1 right"),
	P1(6500, 0, 0, 1, "p1 up"),
	VPAD(6520, 1, PAD_X, 0, 1, "p2 up"),
	P1(6700, PAD_A, -1, 0, "p1 left"),
	VPAD(6850, 1, 0, -1, 0, "p2 left"),
	P1(6900, 0, 0, -1, "p1 down"),
	P1(7100, PAD_A, 1, 0, "p1 right"),
	VPAD(7180, 1, 0, 0, -1, "p2 down"),
	P1(7300, 0, 0, 1, "p1 up"),
	P1(7500, PAD_A, -1, 0, "p1 left"),
	VPAD(7510, 1, PAD_X, 1, 0, "p2 right"),
	P1(7700, 0, 0, -1, "p1 down"),
	VPAD(7840, 1, 0, 0, 1, "p2 up"),
	P1(7900, PAD_A, 1, 0, "p1 right"),
	P1(8100, 0, 0, 1, "p1 up"),
	VPAD(8170, 1, 0, -1, 0, "p2 left"),
	P1(8300, PAD_A, -1, 0, "p1 left"),
	P1(8500, 0, 0, -1, "p1 down"),
	VPAD(8500, 1, PAD_X, 0, -1, "p2 down"),
	P1(8700, PAD_A, 1, 0, "p1 right"),
	VPAD(8830, 1, 0, 1, 0, "p2 right"),
	P1(8900, 0, 0, 1, "p1 up"),
	P1(9100, PAD_A, -1, 0, "p1 left"),
	VPAD(9160, 1, 0, 0, 1, "p2 up"),
	P1(9300, 0, 0, -1, "p1 down"),
	P1(9700, 0, 0, 0, "p1 stop"), VPAD(9750, 1, 0, 0, 0, "p2 stop"),
	SHOT(10000, "busy_end"),
	END(10500),
};

// hudhints: the players' hint boxes and the HUD round them (the potion's flask,
// both combo bars part full): player 1's box at the left (stacked split: the
// top half's right), player 2's at the right; a combo hint's buttons lit one by
// one with sparkles and the combo bar's level sound. XXL_HUDPOS=1 logs where the
// HUD's pieces are.
const Step kHudHints[] = {
	MARK(0, "hudhints"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	POTION(1500),
	{ 1600, S_GIVE, HERO_ASTERIX, 3, 0, 20.0f, 0, "asterix's combo bar: 20 hits" },
	{ 1600, S_GIVE, HERO_OBELIX, 3, 0, 8.0f, 0, "obelix's combo bar: 8 hits" },
	{ 2000, S_HINTTEST, 0, 0, 0, 0, 0, "p1: the revive hint (two lines)" },
	{ 2000, S_HINTTEST, 1, 1, 0, 0, 0, "p2: one line" },
	SHOT(3500, "boxes"),
	{ 4000, S_HINTTEST, 0, 2, 0, 0.0f, 0, "p1: the Twister's buttons, none lit" },
	{ 4000, S_HINTTEST, 1, 3, 0, 0, 0, "p2: three lines" },
	SHOT(5000, "combo0"),
	{ 5200, S_HINTTEST, 0, 2, 0, 1.0f, 0, "p1: one lit" }, { 5200, S_HINTSPARK, 0, 0, 0, 0, 0, "sparkle 1" },
	SHOT(5280, "spark1a"), SHOT(5450, "spark1b"),
	{ 6000, S_HINTTEST, 0, 2, 0, 2.0f, 0, "p1: two lit" }, { 6000, S_HINTSPARK, 0, 1, 0, 0, 0, "sparkle 2" },
	SHOT(6080, "spark2a"), SHOT(6250, "spark2b"),
	{ 6800, S_HINTTEST, 0, 2, 0, 4.0f, 0, "p1: all lit" }, { 6800, S_HINTSPARK, 0, 3, 0, 0, 0, "sparkle 4" },
	SHOT(6880, "spark4a"), SHOT(7600, "lit"),
	{ 7700, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "asterix hit: his shields show" },
	{ 7700, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "obelix hit: his shields show" },
	{ 8000, S_HINTTEST, 1, 2, 0, 1.0f, 0, "p2: the Twister's buttons, one lit" }, { 8000, S_HINTSPARK, 1, 0, 0, 0, 0, "p2 sparkle 1" },
	SHOT(8060, "p2spark1a"), SHOT(8200, "p2spark1b"),
	{ 8800, S_HINTTEST, 1, 2, 0, 2.0f, 0, "p2: two lit" }, { 8800, S_HINTSPARK, 1, 1, 0, 0, 0, "p2 sparkle 2" },
	SHOT(8860, "p2spark2a"), SHOT(9000, "p2spark2b"),
	VPAD(9000, 1, PAD_BACK, 0, 0, "p2 holds Back: player 2's HUD shows"),
	SHOT(9500, "p2combo"), VPAD(9700, 1, 0, 0, 0, "p2 release"),
	END(10500),
};

// combohint2: every combo learned (the cheat: the combos "bought" in this level
// wait in line, the Club first); player 2's Obelix next to a Roman with a full
// combo bar gets the Club's hint, and does it on player 2's pad (A A X B), each
// button lit as pressed; the Club done, its hint goes (the Power Hammer's next
// time the bar is full by a Roman).
#define VTAP(t, b, what) VPAD(t, 1, b, 0, 0, what), VPAD(t + 90, 1, 0, 0, 0, "p2 release")
const Step kComboHint2[] = {
	MARK(0, "combohint2"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1500, S_ENEMY, HERO_OBELIX, 1, 0, 0, 0, "obelix next to a roman" },
	{ 1800, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix's combo bar full" },
	SHOT(3000, "club_hint"),
	VTAP(3500, PAD_A, "p2 A"), SHOT(3620, "pressed_a"),
	VTAP(3750, PAD_A, "p2 A"), SHOT(3870, "pressed_aa"),
	VTAP(4000, PAD_X, "p2 X"), SHOT(4120, "pressed_aax"),
	VTAP(4250, PAD_B, "p2 B"), SHOT(4370, "pressed_aaxb"),
	SHOT(5000, "club_done"), SHOT(7000, "after"),
	{ 7500, S_ENEMY, HERO_OBELIX, 1, 0, 0, 0, "obelix next to a roman" },
	{ 7600, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix's combo bar full again" },
	SHOT(9000, "hammer_hint"),
	END(10000),
};
// combohint2l: the Club's hint (combohint2's start); player 2's A lights its first
// button and Obelix jumps; he lands without the rest: the buttons go dark again.
const Step kComboHint2L[] = {
	MARK(0, "combohint2l"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1500, S_ENEMY, HERO_OBELIX, 1, 0, 0, 0, "obelix next to a roman" },
	{ 1800, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix's combo bar full" },
	VTAP(3500, PAD_A, "p2 A"), SHOT(3700, "jumped_a"), SHOT(5500, "landed"),
	VTAP(6000, PAD_A, "p2 A"), VTAP(6250, PAD_A, "p2 A (double jump)"), SHOT(6450, "jumped_aa"), SHOT(8500, "landed_aa"),
	END(9000),
};
#undef VTAP
// combohint2b: every combo learned, the Mole first in line (test aid); player 1's
// Asterix by a Roman with a full bar gets the Mole's hint; the Mole done (its
// action injected), the hint lights all its buttons and goes; the Club's comes
// with the next full bar by a Roman.
const Step kComboHint2B[] = {
	MARK(0, "combohint2b"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1300, S_COMBOHINT, 104, 0, 0, 0, 0, "the Mole first in line" },
	{ 1500, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman" },
	{ 1800, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full" },
	SHOT(3000, "mole_hint"),
	{ 3500, S_P1COMBO, 3, 0, 0, 0, 0, "player 1: the Mole" },
	SHOT(3700, "mole_done"), SHOT(5200, "after"),
	{ 9000, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman" },
	{ 9200, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full again" },
	SHOT(10500, "club_hint"),
	END(11500),
};

// crowd36 (-Level 3): LV3-6's fight (200 Romans to beat in co-op), the Twister
// learned earlier (no hint line of its own): player 2's Obelix's combo bar
// fills -- the crowd's Twister hint, 12 s; two of its buttons pressed in order
// (A, X on player 2's pad): 8 s more (20 s in all).
const Step kCrowd36[] = {
	MARK(0, "crowd36"), LOADSECTOR(200, 6),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1000, S_COMBOHINT, -2, 0, 0, 0, 0, "the Twister learned (no line)" },
	PLACEAT(3000, HERO_ASTERIX, -218.05f, 6.0f, -251.66f), PLACEAT(3000, HERO_OBELIX, -216.0f, 6.0f, -251.0f),
	PLACEAT(4000, HERO_ASTERIX, -179.3f, 3.2f, -232.8f), PLACEAT(4000, HERO_OBELIX, -185.5f, 3.2f, -228.2f),
	POTION(4600),
	{ 8000, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix's combo bar full" },
	SHOT(9000, "crowd_hint"),
	VPAD(10000, 1, PAD_A, 0, 0, "p2 A"), VPAD(10090, 1, 0, 0, 0, "p2 release"), SHOT(10150, "crowd_a"),
	VPAD(10300, 1, PAD_X, 0, 0, "p2 X"), VPAD(10390, 1, 0, 0, 0, "p2 release"), SHOT(10450, "crowd_ax"),
	SHOT(19000, "crowd_19s"), SHOT(26500, "crowd_26s"), SHOT(29500, "crowd_29s"),
	END(30000),
};

// fuse2: every combo learned; the heroes side by side, both bars full; player
// 1's Twister (injected) sets both spinning -- both players' boxes: X fuses
// them; player 2's X: the fused tornado, its parts (who steers what) in each
// player's box for as long as it lasts.
const Step kFuse2[] = {
	MARK(0, "fuse2"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1300, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 1.5f, 0, "obelix 1.5 units ahead of asterix (the trio's fusion range)" },
	{ 1500, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full" },
	{ 1500, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix's combo bar full" },
	{ 2500, S_P1COMBO, 4, 0, 0, 0, 0, "player 1: the Twister" },
	// Player 2's own (individual heroes: no Twister is copied), fast enough to
	// be done before the jump (A) leaves the ground.
	VPAD(2550, 1, PAD_A, 0, 0, "p2 A"), VPAD(2583, 1, 0, 0, 0, "p2 r"), VPAD(2600, 1, PAD_X, 0, 0, "p2 X"),
	VPAD(2633, 1, 0, 0, 0, "p2 r"), VPAD(2650, 1, PAD_B, 0, 0, "p2 B"), VPAD(2683, 1, 0, 0, 0, "p2 r"),
	VPAD(2700, 1, PAD_Y, 0, 0, "p2 Y"), VPAD(2733, 1, 0, 0, 0, "p2 r"),
	SHOT(3300, "both_start"), SHOT(5300, "both_spin"),   // (the Twister's start step 0x22 lasts some 2 s)
	VPAD(5800, 1, PAD_X, 0, 0, "p2 X"), VPAD(5900, 1, 0, 0, 0, "p2 release"),
	SHOT(6050, "fuse_x"), SHOT(7500, "fused"), SHOT(10000, "fused_later"),
	END(13000),
};
// rage2: every combo learned, the Rage first in line (test aid); player 2's
// Obelix by a Roman with a full bar gets the Rage's hint; RB on player 2's pad:
// Idefix's rage (his bite with that bar), the Rage done for player 2.
const Step kRage2[] = {
	MARK(0, "rage2"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1300, S_COMBOHINT, 105, 0, 0, 0, 0, "the Rage first in line" },
	{ 1500, S_ENEMY, HERO_OBELIX, 2, 0, 0, 0, "obelix 6 from a roman" },
	{ 1800, S_GIVE, HERO_OBELIX, 2, 0, 0, 0, "obelix's combo bar full" },
	SHOT(3000, "rage_hint"),
	VPAD(3500, 1, PAD_RB, 0, 0, "p2 RB"), VPAD(3600, 1, 0, 0, 0, "p2 release"),
	SHOT(3700, "rage_rb"), SHOT(4500, "rage_done"), SHOT(6000, "rage_after"),
	END(7000),
};

// flaskbox (user: xxl_coop.flask_offscreen.log): the potion's flask under player
// 1's box -- it rises over a hint, and as the hint goes (the box sliding out to
// the left) it has to come down where it was, not be dragged off the screen
// (xxl_widescreen2d.asi pulled HUD models over a plate towards its anchor).
const Step kFlaskBox[] = {
	MARK(0, "flaskbox"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	POTION(1500),
	{ 2500, S_HINTTEST, 0, 0, 0, 0, 3000.0f, "p1: the revive hint for 3 s" },
	SHOT(4500, "raised"),
	SHOT(5520, "out_1"), SHOT(5560, "out_2"), SHOT(5600, "out_3"), SHOT(5650, "out_4"), SHOT(5750, "out_5"),
	SHOT(7000, "down"), SHOT(9000, "later"),
	END(9500),
};
// launchboth: both heroes on sector 2's launcher -- both players' boxes show its
// "Activate" (it offered itself to the nearer hero only).
const Step kLaunchBoth[] = {
	MARK(0, "launchboth"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	{ 1500, S_PLACE, HERO_ASTERIX, 10, 0, 0, 0, "asterix into the sas (sector 2)" },
	{ 4000, S_PLACE, HERO_OBELIX, 17, 0, 0, 0, "obelix on the launcher" },
	{ 4000, S_PLACE, HERO_ASTERIX, 18, 0, 0, 0, "asterix next to it" },
	SHOT(5500, "one_on"),
	{ 6000, S_PLACE, HERO_ASTERIX, 17, 0, 0, 0, "asterix on the launcher too" },
	SHOT(7500, "both_on"),
	{ 8000, S_PLACE, HERO_ASTERIX, 18, 0, 0, 0, "asterix off it again" },
	SHOT(9500, "one_again"),
	END(10000),
};
// gunboth33 (-Level 3): both heroes by the LV3-3 machine gun before it is taken --
// both boxes show its "Use"; Asterix on it alone, Obelix at its handles: his box
// shows "Use" (his X joins).
const Step kGunBoth33[] = {
	MGUN33_SETUP,
	PLACEAT(4000, HERO_ASTERIX, 289.3f, 31.92f, -325.6f), PLACEAT(4000, HERO_OBELIX, 287.0f, 31.92f, -327.5f),
	SHOT(5500, "both_near"),
	PLACEAT(6000, HERO_OBELIX, 278.0f, 31.92f, -326.0f),
	P1(6500, PAD_X, 0, 0, "p1 X"), P1(6700, 0, 0, 0, "p1 release"),
	SHOT(8500, "solo"),
	PLACEAT(9000, HERO_OBELIX, 293.9f, 31.92f, -326.5f),
	SHOT(10500, "obelix_at_handles"),
	END(11000),
};

// obelixgun33 (-Level 3; obelixgun33l: Obelix leading): player 2's Obelix takes
// the LV3-3 machine gun alone (X), turns it on his half (stick left), presses X
// (fires nothing); player 1's Asterix far off: X does nothing; by the gun his X
// joins as the gunner (one screen); his A leaves (both off, split back); Obelix
// takes it again and leaves with A.
#define OBELIXGUN33_STEPS \
	PLACEAT(4000, HERO_OBELIX, 289.3f, 31.92f, -325.6f), PLACEAT(4000, HERO_ASTERIX, 276.0f, 31.92f, -326.0f), \
	VPAD(6000, 1, PAD_X, 0, 0, "p2 X: obelix takes the gun"), VPAD(6200, 1, 0, 0, 0, "p2 release"), \
	SHOT(8000, "on_alone"), \
	VPAD(8500, 1, 0, -1.0f, 0, "p2 turns left"), SHOT(9200, "turning"), VPAD(9300, 1, 0, 1.0f, 0, "p2 turns right"), \
	VPAD(10000, 1, 0, 0, 0, "p2 release"), SHOT(10300, "turned"), \
	VPAD(10500, 1, PAD_X, 0, 0, "p2 X (no fire)"), VPAD(10700, 1, 0, 0, 0, "p2 release"), \
	P1(11000, PAD_X, 0, 0, "p1 X far off"), P1(11200, 0, 0, 0, "p1 release"), SHOT(11800, "asterix_far"), \
	PLACEAT(12000, HERO_ASTERIX, 288.0f, 31.92f, -324.0f), SHOT(13000, "asterix_by_it"), \
	P1(13500, PAD_X, 0, 0, "p1 X: asterix joins"), P1(13700, 0, 0, 0, "p1 release"), \
	SHOT(15500, "joined"), \
	P1(16000, 0, -1.0f, 0, "p1 stick (ignored)"), VPAD(16000, 1, 0, 1.0f, 0, "p2 turns right"), \
	P1(16500, PAD_X, 0, 0, "p1 fires"), P1(16900, 0, 0, 0, "p1 release"), VPAD(17000, 1, 0, 0, 0, "p2 release"), \
	SHOT(17200, "joined_fired"), \
	P1(18000, PAD_A, 0, 0, "p1 A: leaves"), P1(18200, 0, 0, 0, "p1 release"), \
	SHOT(20000, "after_join"), \
	PLACEAT(20500, HERO_OBELIX, 294.9f, 31.92f, -326.2f), PLACEAT(20500, HERO_ASTERIX, 276.0f, 31.92f, -326.0f), \
	VPAD(22000, 1, PAD_X, 0, 0, "p2 X: obelix again"), VPAD(22200, 1, 0, 0, 0, "p2 release"), \
	SHOT(24000, "on_again"), \
	VPAD(24500, 1, PAD_A, 0, 0, "p2 A: leaves"), VPAD(24700, 1, 0, 0, 0, "p2 release"), \
	SHOT(26500, "left"), \
	VPAD(27000, 1, 0, 0, 1.0f, "p2 walks"), VPAD(28500, 1, 0, 0, 0, "p2 stop"), SHOT(29000, "walked"), \
	END(29500)
const Step kObelixGun33[] = { MGUN33_SETUP, OBELIXGUN33_STEPS };
const Step kObelixGun33L[] = { MGUN33_SETUP, { 4500, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" }, OBELIXGUN33_STEPS };
#undef OBELIXGUN33_STEPS

// ---- single player ([single_player]; XXL_SPISOLATION=0 and each one's env aid) ----
// spswitch2 (level 2, XXL_SPSWITCH=1): the hero switch button: Obelix leads and
// walks (Asterix follows); back to Asterix; refused while Obelix is told to wait
// (trio 0x4802) and while Asterix is in the air; Obelix again once free.
const Step kSpSwitch2[] = {
	MARK(0, "spswitch2"),
	SHOT(1000, "start"),
	{ 1500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch" },
	SHOT(2500, "obelix_leads"),
	P1(3000, 0, 0, 1.0f, "p1 forward"), P1(4500, 0, 0, 0, "p1 stop"),
	SHOT(5000, "obelix_walked"),
	{ 5500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch back" },
	SHOT(6500, "asterix_leads"),
	{ 7000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix told to wait" },
	{ 7500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch (refused: obelix waits)" },
	{ 8000, S_TRIOEV, 0x4803, 0, 0, 0, 0, "obelix follows again" },
	P1(8500, PAD_A, 0, 0, "p1 jump"), { 8650, S_SPSWITCH, 0, 0, 0, 0, 0, "switch (refused: in the air)" },
	P1(8700, 0, 0, 0, "p1 release"),
	{ 10500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch" },
	SHOT(11500, "obelix_again"),
	END(12000),
};
// spcork21 (level 2, XXL_SPSWITCH=1): LV2-1's spinning platform (its circle makes
// Asterix wait): Obelix leading walks onto it -- nothing; the switch to Asterix,
// who walks onto it: he waits there, Obelix leads; the switch then refused.
const Step kSpCork21[] = {
	MARK(0, "spcork21"),
	FORCE(800, HERO_OBELIX, 1), PLACEAT(1000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1000, HERO_OBELIX, -763.0f, 516.5f, -980.5f), FORCE(1400, HERO_OBELIX, 0),
	{ 2000, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	GLIDEAT(2500, HERO_OBELIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(4200, HERO_OBELIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(6000, "obelix_on_circle"),
	FORCE(6300, HERO_ASTERIX, 1), PLACEAT(6500, HERO_ASTERIX, -775.0f, 517.0f, -980.0f), FORCE(6900, HERO_ASTERIX, 0),
	{ 7500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to asterix" },
	FORCE(7800, HERO_OBELIX, 1), PLACEAT(8000, HERO_OBELIX, -762.0f, 516.5f, -979.2f), FORCE(8400, HERO_OBELIX, 0),
	GLIDEAT(8500, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1000),
	GLIDEAT(9700, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	SHOT(11500, "asterix_on_circle"),
	{ 12500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch (refused: asterix waits)" },
	SHOT(13500, "after"),
	END(14000),
};
// splever25 (level 2, XXL_SPSWITCH=1): sector 5's lever behind the iron crates
// ("switch to Asterix"): Asterix leading punches it -- safe sequence, nothing;
// the switch to Obelix, who punches it: the lead goes to Asterix.
#define SPLEVER_PUNCHES(t, hero) \
	{ t, S_NEAROBJ, hero, 52, 0, 1.2f, 0, "facing the lever" }, \
	P1(t + 400, PAD_X, 0, 0, "p1 X"), P1(t + 600, 0, 0, 0, "p1 release"), \
	P1(t + 1400, PAD_X, 0, 0, "p1 X"), P1(t + 1600, 0, 0, 0, "p1 release")
const Step kSpLever25[] = {
	MARK(0, "splever25"),
	LOADSECTOR(200, 5),
	{ 2500, S_LEVERS, 0, 0, 0, 0, 0, "levers" },
	FORCE(2800, HERO_OBELIX, 1),
	{ 3000, S_PLACE, HERO_ASTERIX, 15, 0, 0, 0, "asterix at the lever" },
	{ 3000, S_PLACE, HERO_OBELIX, 16, 0, 0, 0, "obelix at the hearth" },
	FORCE(3400, HERO_OBELIX, 0),
	SPLEVER_PUNCHES(3600, HERO_ASTERIX),
	SHOT(5800, "asterix_punched"),
	{ 6500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	FORCE(6800, HERO_ASTERIX, 1),
	{ 7000, S_PLACE, HERO_OBELIX, 15, 0, 0, 0, "obelix at the lever" },
	{ 7000, S_PLACE, HERO_ASTERIX, 16, 0, 0, 0, "asterix at the hearth" },
	FORCE(7400, HERO_ASTERIX, 0),
	SPLEVER_PUNCHES(7600, HERO_OBELIX),
	{ 9600, S_NEAROBJ, HERO_OBELIX, 52, 0, 1.6f, 0, "obelix 1.6 from the lever" },
	P1(10000, PAD_X, 0, 0, "p1 X"), P1(10200, 0, 0, 0, "p1 release"),
	{ 11000, S_NEAROBJ, HERO_OBELIX, 52, 0, 2.0f, 0, "obelix 2.0 from the lever" },
	P1(11400, PAD_X, 0, 0, "p1 X"), P1(11600, 0, 0, 0, "p1 release"),
	SHOT(12500, "obelix_punched"),
	END(13500),
};
#undef SPLEVER_PUNCHES
// spcombo2 (level 2, XXL_SPCOMBOHINTS=1): single player's combo hints: every
// combo learned, Asterix by a Roman with a full bar -- the Club's hint in
// player 1's box (the game's place), its buttons lit as pressed; the Club done,
// it goes; the Power Hammer's the next time.
#define P1TAP(t, b, what) P1(t, b, 0, 0, what), P1(t + 90, 0, 0, 0, "p1 release")
const Step kSpCombo2[] = {
	MARK(0, "spcombo2"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1500, S_ENEMY, HERO_ASTERIX, 1, 0, 0, 0, "asterix next to a roman" },
	{ 1800, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full" },
	SHOT(3000, "club_hint"),
	P1TAP(3500, PAD_A, "p1 A"), SHOT(3620, "pressed_a"),
	POTION(4800),   // (invincible: the camp's Romans knocked him about, no combo could start)
	{ 5000, S_COMBOHINT, 104, 0, 0, 0, 0, "the Mole first in line" },
	{ 5100, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman (no hits)" },
	{ 5400, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full" },
	SHOT(6500, "mole_hint"),
	{ 7000, S_P1COMBO, 3, 0, 0, 0, 0, "player 1: the Mole" },
	SHOT(7200, "mole_done"), SHOT(9000, "after"),
	END(10000),
};
#undef P1TAP
// spicons33 (-Level 3, XXL_SPICONS=1 / 0): single player, Asterix at sector 3's
// machine gun: its "Use" prompt with player 1's button; the pause menu's texts.
const Step kSpIcons33[] = {
	MARK(0, "spicons33"),
	LOADSECTOR(200, 3),
	PLACEAT(3000, HERO_ASTERIX, 288.0f, 31.92f, -324.0f), PLACEAT(3000, HERO_OBELIX, 280.0f, 31.92f, -326.0f),
	SHOT(5000, "prompt"),
	{ 6000, S_POKE8, -3, 2, 0, 0, 0, "pause menu" },
	SHOT(7500, "pause"),
	{ 8000, S_POKE8, -3, 1, 0, 0, 0, "unpause" },
	END(9000),
};
// sppotion2 (level 2, XXL_SPISOLATION=0, XXL_OBELIXPOTION=1 / 0): single player, Obelix leading runs
// without, then with the magic potion; with it, by a Roman for a while (hits).
const Step kSpPotion2[] = {
	MARK(0, "sppotion2"),
	{ 1000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads" },
	{ 1500, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields" },
	P1(2000, 0, 0, 1, "p1 run"), { 3500, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields running" }, P1(4000, 0, 0, 0, "p1 stop"),
	POTION(4500),
	{ 5000, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields potion" },
	P1(5500, 0, 0, -1, "p1 run back"), { 7000, S_HEROFIELDS, 0, 0, 0, 0, 0, "fields potion running" }, P1(7500, 0, 0, 0, "p1 stop"),
	SHOT(7600, "potion_ran"),
	{ 8000, S_ENEMY, HERO_OBELIX, 1, 0, 0, 0, "obelix next to a roman" },
	SHOT(12000, "potion_roman"),
	END(16000),
};
// spsecond2 (level 2, XXL_SPSECOND=1 XXL_DOWNAT=0:3000 XXL_REVIVEHOLD=2):
// single player's second chance: Asterix (leading) goes down -- player 1 plays
// Obelix on, Asterix stays down; Obelix holds B by him: Asterix back up and
// leading again, Obelix the AI companion following him.
const Step kSpSecond2[] = {
	MARK(0, "spsecond2"),
	SHOT(3500, "asterix_down"),
	P1(4500, 0, 0, 1.0f, "p1 forward (obelix)"), P1(6500, 0, 0, 0, "p1 stop"),
	SHOT(7000, "obelix_walked"),
	{ 7200, S_SPSWITCH, 0, 0, 0, 0, 0, "switch (refused: asterix down; sp_hero_switch on)" },
	GLIDEBY(7500, HERO_OBELIX, 1.0f, 0.0f, 600),
	P1(8500, PAD_B, 0, 0, "p1 holds B"), SHOT(9500, "reviving"), P1(11500, 0, 0, 0, "p1 release"),
	SHOT(12500, "revived"),
	P1(13000, 0, 0, 1.0f, "p1 forward (asterix)"), P1(15000, 0, 0, 0, "p1 stop"),
	SHOT(16000, "walked_on"),
	END(17000),
};
// spsecond2c (level 2, XXL_SPSECOND=1 XXL_DOWNAT=1:3000 XXL_REVIVEHOLD=2): the
// AI companion Obelix goes down -- he stays there as Asterix walks off and back;
// Asterix's B by him gets him up, following again.
const Step kSpSecond2C[] = {
	MARK(0, "spsecond2c"),
	SHOT(3500, "obelix_down"),
	P1(4500, 0, 0, 1.0f, "p1 forward"), P1(6500, 0, 0, 0, "p1 stop"),
	SHOT(7000, "asterix_walked"),
	GLIDEBY(7500, HERO_ASTERIX, 1.0f, 0.0f, 600),
	P1(8500, PAD_B, 0, 0, "p1 holds B"), P1(11500, 0, 0, 0, "p1 release"),
	SHOT(12500, "revived"),
	P1(13000, 0, 0, -1.0f, "p1 back"), P1(15000, 0, 0, 0, "p1 stop"),
	SHOT(16000, "follows"),
	END(17000),
};
// spsecond2g (level 2, XXL_SPSECOND=1 XXL_DOWNAT=1:3000 XXL_DOWNAT2=0:6000):
// both heroes down -- the game over, the respawn with both up.
const Step kSpSecond2G[] = {
	MARK(0, "spsecond2g"),
	SHOT(4000, "obelix_down"),
	SHOT(7000, "both_down"),
	SHOT(12000, "game_over"),
	SHOT(20000, "respawned"),
	P1(21000, 0, 0, 1.0f, "p1 forward"), P1(23000, 0, 0, 0, "p1 stop"),
	SHOT(24000, "after"),
	END(25000),
};
// spsecond2h (level 2, XXL_SPSECOND=1 XXL_HPAT=0:2500:1): a real hit takes
// Asterix's last point (the trio's area hit): he goes down, Obelix plays on.
const Step kSpSecond2H[] = {
	MARK(0, "spsecond2h"),
	{ 3000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "shockwave on asterix (his last point)" },
	SHOT(3500, "hit"),
	P1(4500, 0, 0, 1.0f, "p1 forward (obelix)"), P1(6000, 0, 0, 0, "p1 stop"),
	SHOT(7000, "after"),
	END(8000),
};
// spcork21d (level 2, XXL_SPSECOND=1 XXL_DOWNAT=1:9000 XXL_REVIVEHOLD=2): Asterix
// leading onto LV2-1's spinning platform: he waits there, Obelix leads and walks
// through zone 03C8 (Asterix freed, the platform's turn under way); Obelix goes
// down -- the sequence set back (the pillar on), Asterix leading comes over and
// revives Obelix. spcork21e: Obelix stays put (Asterix still locked on the
// circle) -- the lock undone: the level as before the circle's event, Asterix
// off it on his last solid ground, leading.
#define SPCORK21D_START \
	FORCE(800, HERO_OBELIX, 1), PLACEAT(1000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1000, HERO_OBELIX, -763.0f, 516.5f, -980.5f), FORCE(1400, HERO_OBELIX, 0), \
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500), \
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000), \
	PLACEAT(5000, HERO_OBELIX, -762.0f, 516.5f, -979.2f), \
	SHOT(6000, "asterix_waits")
#define SPCORK21D_END \
	SHOT(9500, "obelix_down"), \
	SHOT(11000, "asterix_freed"), \
	GLIDEBY(11500, HERO_ASTERIX, 1.0f, 0.0f, 1500), \
	P1(13500, PAD_B, 0, 0, "p1 holds B"), P1(16500, 0, 0, 0, "p1 release"), \
	SHOT(17500, "revived"), \
	END(18500)
const Step kSpCork21D[] = {
	MARK(0, "spcork21d"),
	SPCORK21D_START,
	P1(6500, 0, 0, 1.0f, "p1 forward (obelix)"), P1(7500, 0, 0, 0, "p1 stop"),
	SPCORK21D_END,
};
const Step kSpCork21E[] = {
	MARK(0, "spcork21e"),
	SPCORK21D_START,
	SPCORK21D_END,
};
#undef SPCORK21D_START
#undef SPCORK21D_END

// spbump2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=1 XXL_DOWNAT=0:3000): second
// chance -- Asterix down, player 1's Obelix runs into him (user: the hero
// walking up to revive hopped over the downed one). spbump2b (no XXL_DOWNAT):
// the same with Asterix leading into the AI companion Obelix -- its hop out of
// the way stays (user).
const Step kSpBump2[] = {
	MARK(0, "spbump2"),
	{ 1500, S_WATCHRW, HERO_OBELIX, 0, 0, 7, 0, "obelix vertical speed (armed on the watchdog's next tick)" },
	P1(5000, 0, 0, 1.0f, "p1 forward"),
	{ 5500, S_NEXTTO, HERO_ASTERIX, HERO_OBELIX, 0, 2.5f, 0, "downed asterix 2.5 ahead of running obelix" },
	SHOT(5800, "bump"), SHOT(6100, "bump_2"),
	P1(7500, 0, 0, 0, "p1 stop"),
	{ 7600, S_HEROFIELDS, HERO_OBELIX, 0, 0, 0, 0, "obelix" },
	SHOT(8000, "after"),
	END(8500),
};
const Step kSpBump2B[] = {
	MARK(0, "spbump2b"),
	{ 1500, S_WATCHRW, HERO_OBELIX, 0, 0, 7, 0, "obelix vertical speed (armed on the watchdog's next tick)" },
	P1(5000, 0, 0, 1.0f, "p1 forward"),
	{ 5500, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 2.5f, 0, "obelix 2.5 ahead of running asterix" },
	SHOT(5800, "bump"), SHOT(6100, "bump_2"),
	P1(7500, 0, 0, 0, "p1 stop"),
	END(8500),
};
// splock21 (level 2, XXL_SPISOLATION=0 XXL_SPSWITCH=1; sp_hero_switch_lock on):
// Asterix leading onto LV2-1's spinning platform -- its event (4801 + 4802)
// locks him: kept, Obelix leads; Obelix walks through zone 03C8 (4803: Asterix
// free, the platform off till its turn ends) -- Obelix keeps the lead; at the
// turnstile (zone 0528) Asterix, still the companion, walks onto the platform.
const Step kSpLock21[] = {
	MARK(0, "splock21"),
	FORCE(800, HERO_OBELIX, 1), PLACEAT(1000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1000, HERO_OBELIX, -763.0f, 516.5f, -980.5f), FORCE(1400, HERO_OBELIX, 0),
	GLIDEAT(1800, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(3500, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	PLACEAT(5000, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	SHOT(6000, "asterix_waits"),
	P1(6500, 0, 0, 1.0f, "p1 forward (obelix)"), P1(7500, 0, 0, 0, "p1 stop"),
	SHOT(8500, "asterix_freed"),
	GLIDEAT(9500, HERO_OBELIX, -775.9f, 515.6f, -983.6f, 1500),   // to the turnstile: zone 0528 walks the companion onto the platform
	SHOT(12000, "at_turnstile"), SHOT(15000, "after"),
	END(16000),
};
// sptele36 (-Level 3, XXL_SPISOLATION=0 XXL_SPSWITCH=1): Asterix leading takes
// LV3-6's automatic cable car (CKHkTelepher): no switch (user: its camera follows
// the car) -- he keeps the lead through the ride; the switch refused on it.
const Step kSpTele36[] = {
	MARK(0, "sptele36"), LOADSECTOR(200, 6),
	PLACEAT(2500, HERO_ASTERIX, -185.961f, 2.554f, -227.849f),
	FORCE(2300, HERO_OBELIX, 1), PLACEAT(2500, HERO_OBELIX, -186.961f, 2.554f, -228.849f), FORCE(2900, HERO_OBELIX, 0),
	PLACEAT(4000, HERO_ASTERIX, -159.605f, 11.772f, -209.331f),   // the cable car
	SHOT(7000, "riding"),
	{ 8000, S_SPSWITCH, 0, 0, 0, 0, 0, "switch (refused: riding)" },
	SHOT(14000, "riding_2"), SHOT(22000, "landed"),
	END(24000),
};
// spguard41 (-Level 4, XXL_SPISOLATION=0 XXL_SPSECOND=1 XXL_DOWNAT=0:13000
// XXL_OBELIXPOTION=0): guard41a in single player -- Asterix down among LV4-1's
// lock fight Romans at 13 s; Obelix (now leading, healed every 2.5 s) 12 units
// off: the Romans stay round Asterix as his guards (romans_guard_downed), go for
// Obelix 3 units off, and back to their spots when he goes 14 units off.
#define HEALO(t) { t, S_GIVE, HERO_OBELIX, 0, 0, 10.0f, 0, "heal obelix" }
const Step kSpGuard41[] = {
	MARK(0, "spguard41"),
	{ 1500, S_WATCHENEMIES, 0, 0, 0, -288.15f, 0.5f, "watch romans", 117.27f },
	FORCE(2300, HERO_OBELIX, 1), PLACEAT(2500, HERO_ASTERIX, -288.15f, 0.8f, 124.0f), PLACEAT(2600, HERO_OBELIX, -286.15f, 0.8f, 118.0f),
	GLIDEAT(3000, HERO_ASTERIX, -288.15f, 0.5f, 117.27f, 800),
	HEALO(4500), HEALO(7000),
	PLACEAT(8000, HERO_ASTERIX, -296.0f, 0.5f, 92.0f), PLACEAT(8100, HERO_OBELIX, -294.0f, 0.5f, 90.0f), FORCE(8300, HERO_OBELIX, 0),
	HEALO(9500), HEALO(12000), PLACEAT(13300, HERO_OBELIX, -306.0f, 0.5f, 92.0f),
	HEALO(14500), HEALO(17000), SHOT(17500, "spguard41_0"),
	GLIDEBY(18000, HERO_OBELIX, -3.0f, 0.0f, 1500), HEALO(19500), SHOT(21000, "spguard41_1"),
	HEALO(22000), HEALO(24500), SHOT(25000, "spguard41_2"),
	GLIDEBY(25500, HERO_OBELIX, -14.0f, 0.0f, 1500), HEALO(27000),
	HEALO(29500), SHOT(30000, "spguard41_3"), END(31000),
};
#undef HEALO
// spmod2 (level 2, XXL_SPISOLATION=0; [debug] cheats on): single player's mod
// buttons -- Obelix told to wait (4802): R3 + B frees him; R3 + A brings him over
// from 25 units away; R3 + X gives him the lead; R3 + Y fills the potion.
const Step kSpMod2[] = {
	MARK(0, "spmod2"),
	{ 1000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix told to wait" },
	P1(1500, 0, 0, 1.0f, "p1 forward"), P1(3000, 0, 0, 0, "p1 stop"),
	SHOT(3200, "obelix_waits"),
	{ 3500, S_SPMOD, PAD_B, 0, 0, 0, 0, "R3 + B" },
	SHOT(4500, "obelix_freed"),
	FORCE(5400, HERO_OBELIX, 1), PLACEAT(5500, HERO_OBELIX, -830.0f, 508.0f, -1060.0f),
	{ 6000, S_SPMOD, PAD_A, 0, 0, 0, 0, "R3 + A" }, FORCE(6200, HERO_OBELIX, 0),
	SHOT(6500, "obelix_brought"),
	{ 7000, S_SPMOD, PAD_X, 0, 0, 0, 0, "R3 + X" },
	SHOT(8000, "obelix_leads"),
	{ 8500, S_SPMOD, PAD_Y, 0, 0, 0, 0, "R3 + Y" },
	END(9500),
};
// splockcar21 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=0; sp_hero_switch_lock
// on): the user's -- Asterix leading boards LV2-1's rope cable car (its boarding
// hands Obelix the lead: kept, Asterix locked); the car pulled to its rope's end,
// Asterix lands through zone 0020 ("switch to Asterix" + "the sidekick may
// move"): the switch is to the hero you play -- kept, Asterix leads again.
const Step kSpLockCar21[] = {
	MARK(0, "splockcar21"),
	PLACEAT(1400, HERO_ASTERIX, -782.0f, 511.5f, -1059.9f),
	GLIDEAT(1500, HERO_ASTERIX, -790.45f, 510.6f, -1059.94f, 1200),
	P1(2800, 0, -1, 0, "p1 left (held)"), P1(3300, 0, 0, 0, "p1 neutral"),
	PLACEAT(3600, HERO_ASTERIX, -793.761f, 511.812f, -1060.196f),   // the car's seat
	SHOT(4500, "in_the_car"),
	{ 6000, S_CARPOS, HERO_ASTERIX, 0, 0, 6.0f, 0, "car along its rope" },
	{ 7000, S_CARPOS, HERO_ASTERIX, 0, 0, 12.0f, 0, "car along its rope" },
	{ 8000, S_CARPOS, HERO_ASTERIX, 0, 0, 18.0f, 0, "car along its rope" },
	{ 9000, S_CARPOS, HERO_ASTERIX, 0, 0, 24.0f, 0, "car along its rope" },
	{ 10000, S_CARPOS, HERO_ASTERIX, 0, 0, 30.0f, 0, "car along its rope" },
	SHOT(12000, "end"), SHOT(15000, "after"),
	END(16000),
};
// spfindkeg2 (level 2): the event groups sending the trio Idefix's follow (4878)
// and "switch to Asterix" (4800) -- the keg's and the rope car's end.
const Step kSpFindKeg2[] = {
	MARK(0, "spfindkeg2"),
	{ 2000, S_FINDGROUPS, 0x6256A0, 0x4878, 0, 2.0f, 0, "groups: idefix follows" },
	{ 2100, S_FINDGROUPS, 0x6256A0, 0x4800, 0, 2.0f, 0, "groups: switch to asterix" },
	END(3000),
};
// splockev2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=0; sp_hero_switch_lock on):
// the user's two -- (1) Asterix held by a lock that gave Obelix the lead (4801 +
// 4802, as a rope car's boarding), then LV2-1's landing zone 0020 (its group
// 004: "switch to Asterix" + "the sidekick may move"): the switch is to the hero
// you play -- kept, Asterix leads at once. (2) Asterix leading, Obelix held
// (4802), then the groups sending Idefix's follow (069, 0AA -- the keg's: 4801 +
// 4803 + 4878): the switch to Obelix dropped, Asterix keeps the lead, no hop.
const Step kSpLockEv2[] = {
	MARK(0, "splockev2"),
	{ 2000, S_TRIOEV, 0x4801, 0, 0, 0, 0, "obelix leads (a lock's switch)" },
	{ 2000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "asterix waits (the lock)" },
	SHOT(3000, "asterix_held"),
	{ 4000, S_FLAGEV, 0x0020, 0, 0, 0, 0, "zone 0020: switch to asterix + the sidekick may move" },
	SHOT(5000, "asterix_leads"),
	{ 6000, S_TRIOEV, 0x4802, 0, 0, 0, 0, "obelix waits" },
	{ 7000, S_FLAGEV, 0x0348, 0, 0, 0, 0, "group 069 (sends 4878)" },
	{ 8500, S_FLAGEV, 0x0550, 0, 0, 0, 0, "group 0AA (sends 4878)" },
	SHOT(9500, "after_keg"),
	END(10500),
};
// sptwist2 / sptwist2j (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=0): single
// player, Asterix's Twister by a Roman -- its camera (pulled back: a wider field
// of view, a shake); j: player 2 joins and leaves first (the user's LV3-5: the
// Twister's camera looked normal after a join).
#define SPTWIST_STEPS \
	{ 4500, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman" }, \
	{ 4700, S_P1COMBO, -1, 0, 0, 0, 0, "every combo learned" }, \
	{ 4900, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "combo bar full" }, \
	{ 5000, S_CAMFOV, 0, 0, 0, 0, 0, "camera before" }, SHOT(5000, "before"), \
	{ 5500, S_P1COMBO, 4, 0, 0, 0, 0, "twister" }, \
	{ 6000, S_CAMFOV, 0, 0, 0, 0, 0, "camera 0.5 s in" }, SHOT(6000, "twister_1"), \
	{ 6600, S_CAMFOV, 0, 0, 0, 0, 0, "camera 1.1 s in" }, SHOT(6600, "twister_2"), \
	{ 7500, S_CAMFOV, 0, 0, 0, 0, 0, "camera 2 s in" }, SHOT(7500, "twister_3"), \
	{ 9500, S_CAMFOV, 0, 0, 0, 0, 0, "camera after" }, \
	END(10000)
const Step kSpTwist2[] = { MARK(0, "sptwist2"), SPTWIST_STEPS };
const Step kSpTwist2J[] = {
	MARK(0, "sptwist2j"),
	VPAD(300, 1, PAD_START, 0, 0, "p2 start (joins)"), VPAD(500, 1, 0, 0, 0, "p2 release"),
	VPAD(3000, 1, PAD_START, 0, 0, "p2 start (leaves)"), VPAD(3200, 1, 0, 0, 0, "p2 release"),
	SPTWIST_STEPS,
};
#undef SPTWIST_STEPS
// spslide2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=1 XXL_DOWNAT=0:9000): second
// chance, LV2-2's belly slide (slidecross: Asterix past its cross detector, z
// -903.82) -- Asterix down on it: the game over at once, as in co-op (user: a
// revive began).
const Step kSpSlide2[] = {
	MARK(0, "spslide2"), LOADSECTOR(200, 2),
	PLACEAT(3000, HERO_ASTERIX, -905.96f, 528.2f, -938.49f), PLACEAT(3000, HERO_OBELIX, -903.96f, 528.2f, -938.49f),
	GLIDEAT(4500, HERO_ASTERIX, -905.96f, 524.0f, -896.0f, 2500),
	SHOT(8500, "sliding"), SHOT(10000, "down"), SHOT(14000, "after"),
	END(18000),
};
// spslidemode2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=1 XXL_DOWNAT=0:3000): the
// rule alone (the slide itself needs LV2-2's opening): Obelix's flags in the belly
// slide's mode (3, f18 0x0D) as Asterix goes down -- the game over, no revive.
const Step kSpSlideMode2[] = {
	MARK(0, "spslidemode2"),
	{ 2950, S_SETF18, HERO_OBELIX, 0x0D, 0, 0, 0, "obelix in the slide's mode" },
	{ 3100, S_SETF18, HERO_OBELIX, 0x23, 0, 0, 0, "obelix back" },
	SHOT(4000, "after"),
	END(12000),
};
// splockord2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=0; sp_hero_switch_lock on):
// you play Obelix (the switch); zone 0020's group ("Asterix leads" + "the
// sidekick may move") flagged: dropped; then LV2-1's turnstile box 0528 ("the
// sidekick walks to markers"): round 202 had Asterix lead for it first and walked
// Obelix; the box is bound to Obelix, and here no lock sequence of Asterix's goes
// on (no circle: splockcork21 is the real one, 0528 walks Asterix there) -- the
// walk means Obelix, leading: dropped, no switch.
const Step kSpLockOrd2[] = {
	MARK(0, "splockord2"),
	{ 2000, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	{ 3000, S_FLAGEV, 0x0020, 0, 0, 0, 0, "zone 0020: switch to asterix + the sidekick may move" },
	SHOT(4000, "obelix_kept"),
	{ 5000, S_FLAGEV, 0x0528, 0, 0, 0, 0, "box 0528: the sidekick walks to markers" },
	SHOT(6000, "order"), SHOT(12000, "after"), SHOT(20000, "later"),
	END(24000),
};
// splock43 (-Level 4, XXL_SPISOLATION=0 XXL_SPSECOND=0; sp_hero_switch_lock on):
// the user's LV4-3 light pillar (-607 -1 -148), you play Obelix: zone 00D9's
// group 01B ("Asterix leads" + "the sidekick may move": dropped), then group 0E7
// (event 0738: the same + 487D the sidekick's catch-up + 4811 Asterix's torch
// used) -- was kept, Asterix led for 0.3 s; now dropped too, Obelix keeps it.
const Step kSpLock43[] = {
	MARK(0, "splock43"), LOADSECTOR(200, 3),
	FORCE(2800, HERO_OBELIX, 1),
	PLACEAT(3000, HERO_ASTERIX, -593.8f, 4.8f, -142.1f), PLACEAT(3000, HERO_OBELIX, -593.4f, 4.8f, -144.5f),
	FORCE(3400, HERO_OBELIX, 0),
	{ 4500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	{ 6000, S_FLAGEV, 0x00D9, 0, 0, 0, 0, "zone 00D9: the pillar's switch to asterix" },
	{ 7500, S_FLAGEV, 0x0738, 0, 0, 0, 0, "event 0738: its second group (unless it followed)" },
	SHOT(8000, "after"), SHOT(10000, "later"),
	END(11000),
};
// splockcar44 (-Level 4, XXL_SPISOLATION=0 XXL_SPSECOND=0; sp_hero_switch_lock
// on): the user's LV4-4, you play Asterix: the lever (-681.8 9.4 -214.4), the
// automatic cable car beside it (-684.9 8.6 -216.0: its boarding hands Obelix the
// lead, kept) -- the ride ends in sphere #4 (-733.9 18.5 -237.9, bound to Asterix,
// event 0650: the fight lock on the spitter's lever up there). It answered Obelix
// while he led for the ride: no fight. Now it fires as Asterix arrives.
const Step kSpLockCar44[] = {
	MARK(0, "splockcar44"), LOADSECTOR(200, 4),
	FORCE(2800, HERO_OBELIX, 1),
	PLACEAT(3000, HERO_ASTERIX, -681.05f, 8.6f, -214.74f), PLACEAT(3000, HERO_OBELIX, -680.0f, 6.0f, -222.0f),
	FORCE(3400, HERO_OBELIX, 0),
	SHOT(4000, "by_the_lever"),
	{ 4500, S_PULLLEVER, -1, 0, 0, -681.844f, 9.447f, "the lever (fight-locked: its fight starts)", -214.416f },
	ENDFIGHTWON(7000),
	{ 9000, S_PULLLEVER, -1, 0, 0, -681.844f, 9.447f, "the lever: the car comes back", -214.416f },
	SHOT(13000, "car_back"),
	GLIDEAT(15000, HERO_ASTERIX, -684.85f, 8.6f, -216.0f, 1000),
	SHOT(17000, "in_the_car"),
	// (a rope car: Obelix pulls it in the game -- moved along its rope here, as splockcar21)
	{ 19000, S_CARPOS, HERO_ASTERIX, 0, 0, 8.0f, 0, "car along its rope" },
	{ 20000, S_CARPOS, HERO_ASTERIX, 0, 0, 16.0f, 0, "car along its rope" },
	{ 21000, S_CARPOS, HERO_ASTERIX, 0, 0, 24.0f, 0, "car along its rope" },
	{ 22000, S_CARPOS, HERO_ASTERIX, 0, 0, 32.0f, 0, "car along its rope" },
	{ 23000, S_CARPOS, HERO_ASTERIX, 0, 0, 40.0f, 0, "car along its rope" },
	{ 24000, S_CARPOS, HERO_ASTERIX, 0, 0, 48.0f, 0, "car along its rope" },
	{ 25000, S_CARPOS, HERO_ASTERIX, 0, 0, 56.0f, 0, "car along its rope" },
	{ 26000, S_CARPOS, HERO_ASTERIX, 0, 0, 64.0f, 0, "car along its rope" },
	SHOT(24500, "riding"), SHOT(28000, "top"), SHOT(34000, "top_2"),
	END(38000),
};
// splock47 (-Level 4, XXL_SPISOLATION=0 XXL_SPSECOND=0; sp_hero_switch_lock on):
// the user's LV4-7 (#137 at -972.8 247.5 -1069.1, plane #17 bound to Obelix),
// you play Obelix: event 0448 ("the sidekick walks to marker 5") -- Asterix was
// given the lead for it and Obelix walked (or, the level's last switch meaning
// Obelix, Asterix walked). Now dropped: Obelix keeps the lead, walks along the
// top himself and crosses his plane #20 (0480: "Obelix leads" + the cable car),
// then #21 (0488: the companion, Asterix, waits there).
const Step kSpLock47[] = {
	MARK(0, "splock47"), LOADSECTOR(200, 7),
	FORCE(2800, HERO_OBELIX, 1),
	PLACEAT(3000, HERO_ASTERIX, -956.57f, 229.82f, -1048.24f), PLACEAT(3000, HERO_OBELIX, -956.74f, 230.34f, -1051.1f),
	FORCE(3400, HERO_OBELIX, 0),
	{ 4500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	{ 6000, S_FLAGEV, 0x0448, 0, 0, 0, 0, "zone 0448: the sidekick walks to marker 5" },
	SHOT(7000, "after_0448"), SHOT(10000, "after_0448_2"),
	// Obelix walks to the car's foot himself (the AI walk's path): plane #20, his,
	// 0480 -- "Obelix leads" (already) + the car
	FORCE(10300, HERO_ASTERIX, 1),
	PLACEAT(10500, HERO_OBELIX, -966.97f, 251.05f, -1027.78f), PLACEAT(10500, HERO_ASTERIX, -968.0f, 251.05f, -1031.0f),
	FORCE(10900, HERO_ASTERIX, 0),
	GLIDEAT(11500, HERO_OBELIX, -950.63f, 251.05f, -1018.97f, 1500),
	GLIDEAT(13100, HERO_OBELIX, -932.45f, 253.32f, -1025.28f, 1500),
	GLIDEAT(14700, HERO_OBELIX, -927.0f, 250.0f, -1022.0f, 800),
	SHOT(16000, "cable_car_foot"),
	GLIDEAT(18000, HERO_OBELIX, -924.87f, 227.55f, -1019.54f, 800),   // (the car's seat: the walked Obelix's spot, user's log)
	SHOT(20000, "on_the_car"), SHOT(26000, "later"), SHOT(34000, "later_2"),
	END(38000),
};
// sptwist34 (-Level 3, XXL_SPISOLATION=1 / 0, XXL_SPSECOND=0): the user's LV3-4
// arena (104.6 -1.3 -318.5): Asterix's Twister by its Romans -- does the camera
// pull back and shake there (vanilla: isolated)?
const Step kSpTwist34[] = {
	MARK(0, "sptwist34"), LOADSECTOR(200, 4),
	FORCE(2800, HERO_OBELIX, 1),
	PLACEAT(3000, HERO_ASTERIX, 104.1f, -0.8f, -310.0f), PLACEAT(3000, HERO_OBELIX, 106.1f, -0.8f, -310.0f),
	FORCE(3400, HERO_OBELIX, 0),
	{ 4000, S_FLAGEV, 0x0578, 0, 0, 0, 0, "zone 0578: the arena's fight" },
	{ 6000, S_CAMDUMP, 0, 0, 0, 0, 0, "camera" },
	{ 14000, S_ENEMY, HERO_ASTERIX, 2, 0, 0, 0, "asterix 6 from a roman" },
	{ 14200, S_P1COMBO, -1, 0, 0, 0, 0, "every combo learned" },
	{ 14400, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "combo bar full" },
	{ 14500, S_CAMFOV, 0, 0, 0, 0, 0, "camera before" }, SHOT(14500, "before"),
	{ 15000, S_P1COMBO, 4, 0, 0, 0, 0, "twister" },
	{ 15500, S_CAMFOV, 0, 0, 0, 0, 0, "camera 0.5 s in" }, SHOT(15500, "twister_1"),
	{ 16500, S_CAMFOV, 0, 0, 0, 0, 0, "camera 1.5 s in" }, SHOT(16500, "twister_2"),
	{ 18000, S_CAMFOV, 0, 0, 0, 0, 0, "camera 3 s in" },
	END(19000),
};
// spgun33 (-Level 3, XXL_SPISOLATION=0 XXL_SPSECOND=0): single player, Obelix
// leading takes LV3-3's machine gun (X): he turns it, nobody fires; Asterix, the
// AI companion 13 units off, runs to it and joins as its gunner (the lead his,
// player 1 fires); A leaves -- the lead back to Obelix.
const Step kSpGun33[] = {
	MARK(0, "spgun33"), LOADSECTOR(200, 3),
	FORCE(2800, HERO_OBELIX, 1), PLACEAT(3000, HERO_ASTERIX, 284.4f, 31.92f, -325.9f), PLACEAT(3000, HERO_OBELIX, 289.3f, 31.92f, -325.6f),
	FORCE(3400, HERO_OBELIX, 0), SECTORS(3500),
	{ 4000, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	FORCE(4300, HERO_ASTERIX, 1), PLACEAT(4500, HERO_ASTERIX, 276.0f, 31.92f, -326.0f), PLACEAT(4500, HERO_OBELIX, 289.3f, 31.92f, -325.6f),
	SHOT(5500, "by_the_gun"),
	P1(6000, PAD_X, 0, 0, "p1 X: obelix takes the gun (asterix held back)"), P1(6200, 0, 0, 0, "p1 release"),
	SHOT(7000, "obelix_on"),
	P1(7200, 0, -1.0f, 0, "p1 turns left"), P1(7800, 0, 0, 0, "p1 release"),
	P1(8000, PAD_X, 0, 0, "p1 X (alone: no fire)"), P1(8400, 0, 0, 0, "p1 release"),
	SHOT(8600, "alone_x"),
	FORCE(9000, HERO_ASTERIX, 0),
	SHOT(9800, "asterix_coming"), SHOT(12000, "joined"),
	P1(12500, PAD_X, 0, 0, "p1 fires"), P1(12900, 0, 0, 0, "p1 release"),
	SHOT(13100, "fired"),
	P1(14000, PAD_A, 0, 0, "p1 A: leaves"), P1(14200, 0, 0, 0, "p1 release"),
	SHOT(15500, "left"),
	END(16500),
};
// spfuse2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=1 / 0): single player, every
// combo learned, the bar full; player 1's Twister, the AI Obelix beside spins
// along; X while both spin: the Twister Fusion (user: none with second chance).
const Step kSpFuse2[] = {
	MARK(0, "spfuse2"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1300, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 1.5f, 0, "obelix 1.5 units ahead of asterix (the trio's fusion range)" },
	{ 1500, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full" },
	{ 2400, S_WATCHRW, HERO_OBELIX, 0x189, 0, 0, 0, "obelix's 'partner near' (+0x189): who reads / writes it" },
	{ 2500, S_P1COMBO, 4, 0, 0, 0, 0, "player 1: the Twister" },
	SHOT(3300, "both_start"), SHOT(4800, "both_spin"),
	TAP1(5000), TAP1(5400), TAP1(5800), TAP1(6200), TAP1(6600),
	SHOT(7000, "fuse_x"), SHOT(8500, "fused"), SHOT(11000, "fused_later"),
	END(13000),
};
// spbar2 (level 2; XXL_SPISOLATION=1 the game's own, or XXL_SPSECOND=1):
// spfuse2 without X -- the Twister's spend, the bar left after it and how long
// the companion stays Fusion-ready (LogTick: bar, tw).
const Step kSpBar2[] = {
	MARK(0, "spbar2"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1300, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 1.5f, 0, "obelix 1.5 units ahead of asterix (the trio's fusion range)" },
	{ 1500, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full" },
	{ 2500, S_P1COMBO, 4, 0, 0, 0, 0, "player 1: the Twister" },
	SHOT(3300, "both_start"), SHOT(6000, "spin"), SHOT(9000, "spin_2"), SHOT(14000, "after"),
	END(18000),
};
// spbarx2 (as spbar2): X once the bar has run out (9.2 s; the companion back in
// the plain Twister at 11.2 s) -- no Fusion without combo left (user: the game
// fused, free, and the tornado ran its full time from there).
const Step kSpBarX2[] = {
	MARK(0, "spbarx2"),
	{ 1200, S_P1COMBO, -1, 0, 0, 0, 0, "combo cheat: every combo learned" },
	{ 1300, S_NEXTTO, HERO_OBELIX, HERO_ASTERIX, 0, 1.5f, 0, "obelix 1.5 units ahead of asterix (the trio's fusion range)" },
	{ 1500, S_GIVE, HERO_ASTERIX, 2, 0, 0, 0, "asterix's combo bar full" },
	{ 2500, S_P1COMBO, 4, 0, 0, 0, 0, "player 1: the Twister" },
	SHOT(6000, "spin"),
	TAP1(12000), TAP1(12400), TAP1(12800), TAP1(13200),
	SHOT(13500, "after_x"), SHOT(17000, "later"), SHOT(22000, "later_2"),
	END(26000),
};
// spboss210 (level 2, XXL_SPISOLATION=0; sp_hero_switch_lock on): you play
// Obelix; sector 10's boss gate (#34 at x 1672.3: the intro cutscene, "Asterix
// leads"; #4 at 1687.1: the boss wakes, "the sidekick walks to marker 22"),
// both bound to Asterix, crossed by Obelix with Asterix 25 units back (user:
// nothing for Obelix, the AI Asterix behind him set both off); p1's stick
// during the intro (user: Obelix free in it).
const Step kSpBoss210[] = {
	MARK(0, "spboss210"), LOADSECTOR(200, 10),
	PLACEAT(2500, HERO_ASTERIX, 1664.54f, -743.6f, -1210.02f), PLACEAT(2500, HERO_OBELIX, 1665.95f, -743.6f, -1210.66f),
	{ 4500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	PLACEAT(5000, HERO_ASTERIX, 1640.0f, -742.5f, -1210.1f),
	GLIDEAT(5300, HERO_OBELIX, 1678.0f, -742.8f, -1210.5f, 1500),
	SHOT(7100, "intro"),
	P1(7300, 0, 0, 1.0f, "p1 forward (during the intro)"), P1(8800, 0, 0, 0, "p1 stop"),
	SHOT(8900, "intro_moved"),
	CINE_ENTER(9300),
	GLIDEAT(10800, HERO_OBELIX, 1695.0f, -742.8f, -1210.5f, 2000),
	SHOT(13300, "boss_wake"), SHOT(16000, "boss_2"), SHOT(20000, "boss_3"),
	END(23000),
};
// spboss36 (-Level 3, XXL_SPISOLATION=0; sp_hero_switch_lock on): spboss210 on
// LV3-6 -- you play Obelix, Asterix 10 units back; Obelix crosses the rectangle
// that loads the boss (-208.3 -> -226), drops onto #46 (wakes it, the sidekick
// walked to a marker) and runs on to x -248.
const Step kSpBoss36[] = {
	MARK(0, "spboss36"), LOADSECTOR(200, 6),
	PLACEAT(2500, HERO_ASTERIX, -208.28f, 11.5f, -200.22f), PLACEAT(2500, HERO_OBELIX, -208.28f, 11.5f, -199.22f),
	{ 4500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	PLACEAT(5000, HERO_ASTERIX, -198.0f, 11.5f, -200.22f),
	GLIDEAT(5300, HERO_OBELIX, -226.0f, 11.5f, -199.22f, 1500),
	SHOT(7500, "loaded"),
	PLACEAT(9000, HERO_OBELIX, -232.0f, 16.0f, -173.9f),
	SHOT(10500, "woken"),
	GLIDEAT(11000, HERO_OBELIX, -248.0f, 11.0f, -173.4f, 3000),
	SHOT(14500, "run"), SHOT(18000, "run_2"), SHOT(23000, "run_3"),
	END(26000),
};
// spboss36j (-Level 3, XXL_SPISOLATION=0; sp_hero_switch_lock on): you play
// Obelix; the far jump off the platform (user: -229.6 13.6 -181.4) -- past #143's
// plane (x -235.8, the boss's "move to the other end", Obelix's) above the arena,
// then through #46 (flat, y 9.8: the boss wakes, #143 switched on, the sidekick
// walked to marker 17). The boss must roll once the walked Asterix passes #143,
// as the walked Obelix does in the game (user: it never moved).
const Step kSpBoss36J[] = {
	MARK(0, "spboss36j"), LOADSECTOR(200, 6),
	PLACEAT(2500, HERO_ASTERIX, -208.28f, 11.5f, -200.22f), PLACEAT(2500, HERO_OBELIX, -208.28f, 11.5f, -199.22f),
	{ 4500, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	GLIDEAT(5000, HERO_OBELIX, -226.0f, 11.5f, -199.22f, 1500),
	PLACEAT(7000, HERO_ASTERIX, -228.0f, 13.7f, -185.0f), PLACEAT(7000, HERO_OBELIX, -229.575f, 13.7f, -181.374f),
	SHOT(8500, "on_the_platform"),
	GLIDEAT(9000, HERO_OBELIX, -238.0f, 11.5f, -178.0f, 500),
	GLIDEAT(9550, HERO_OBELIX, -240.0f, 5.6f, -177.0f, 300),
	SHOT(10500, "landed"), SHOT(13000, "walk"), SHOT(16000, "rolls"), SHOT(20000, "rolls_2"),
	END(22000),
};
// sphud2 / spdownhud2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=1
// XXL_DOWNAT=1:2000 XXL_HUDPOS=1): second chance, Obelix down; Asterix takes
// three area hits -- each shows both health rows (user: Obelix's row went down
// each time, as if the HUD had just come up).
const Step kSpDownHud2[] = {
	MARK(0, "spdownhud2"),
	{ 4000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "shockwave on asterix" },
	SHOT(4200, "hit_1"), SHOT(4600, "hit_1b"),
	{ 9000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "shockwave on asterix" },
	SHOT(9200, "hit_2"), SHOT(9600, "hit_2b"),
	{ 14000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "shockwave on asterix" },
	SHOT(14200, "hit_3"),
	END(18000),
};
// spsq24 (level 2, XXL_SPISOLATION=0 XXL_GATELOG=1): LV2-4's arena fight (zone
// 00D8: 2 squads, 60 Romans behind the gate) -- the squads round it, which the
// gate counts (+0x154 >= -1) and which are active, as it goes (user: Romans came
// that the counter did not count).
const Step kSpSq24[] = {
	MARK(0, "spsq24"), LOADSECTOR(200, 4),
	PLACEAT(2500, HERO_ASTERIX, -8.0f, 3.0f, -570.5f), PLACEAT(2500, HERO_OBELIX, -6.9f, 3.0f, -571.1f),
	{ 3500, S_SQUADS, HERO_ASTERIX, 0, 0, 120.0f, 0, "squads before" },
	{ 4000, S_FLAGEV, 0x00D8, 0, 0, 0, 0, "zone 00d8: the arena's fight" },
	{ 9000, S_SQUADS, HERO_ASTERIX, 0, 0, 120.0f, 0, "squads 5 s in" }, SHOT(9000, "fight"),
	{ 20000, S_SQUADS, HERO_ASTERIX, 0, 0, 120.0f, 0, "squads 16 s in" }, SHOT(20000, "fight_2"),
	END(21000),
};
// spsq24n (as spsq24; XXL_SPISOLATION=1 the game's own): Asterix walks to the
// arena's north edge where the user's Obelix stood at 711 s (-24.1 2.1 -548.5)
// -- inside squad 03DC9A3C's box (z -549.1..-489.7, not the gate's).
const Step kSpSq24N[] = {
	MARK(0, "spsq24n"), LOADSECTOR(200, 4),
	PLACEAT(2500, HERO_ASTERIX, -8.0f, 3.0f, -570.5f), PLACEAT(2500, HERO_OBELIX, -6.9f, 3.0f, -571.1f),
	{ 4000, S_FLAGEV, 0x00D8, 0, 0, 0, 0, "zone 00d8: the arena's fight" },
	{ 9000, S_SQUADS, HERO_ASTERIX, 0, 0, 30.0f, 0, "squads 5 s in" },
	GLIDEAT(10000, HERO_ASTERIX, -24.0f, 2.4f, -556.0f, 2000),
	GLIDEAT(12500, HERO_ASTERIX, -24.14f, 2.1f, -548.52f, 1000),
	SHOT(14000, "north_edge"),
	{ 16000, S_SQUADS, HERO_ASTERIX, 0, 0, 30.0f, 0, "squads at the north edge" }, SHOT(16000, "north_edge_2"),
	SHOT(20000, "north_edge_3"),
	{ 21000, S_SQUADS, HERO_ASTERIX, 0, 0, 30.0f, 0, "squads later" },
	END(22000),
};
// splockcork35 (-Level 3, XXL_SPISOLATION=0 XXL_SPSECOND=1; sp_hero_switch_lock
// on): you play Asterix; LV3-5's cable car end -- zone 0C38 (flagged): Asterix
// leads, Obelix put on the corkscrew, waits; Asterix hits the turnstile (user's
// spot -143.4 5.5 -257.7), Obelix rises, jumps off at the top (0C08's 484C) into
// box 0340 (4801 "Obelix leads" + 4803): the switch dropped -- Obelix must follow
// after (user: held there for good).
const Step kSpLockCork35[] = {
	MARK(0, "splockcork35"), LOADSECTOR(200, 5),
	FORCE(2800, HERO_OBELIX, 1),
	PLACEAT(3000, HERO_ASTERIX, -168.0f, 5.6f, -255.5f), PLACEAT(3000, HERO_OBELIX, -174.3f, -0.6f, -261.6f),
	FORCE(3400, HERO_OBELIX, 0),
	{ 4000, S_FLAGEV, 0x0C38, 0, 0, 0, 0, "zone 0c38: asterix leads, obelix on the corkscrew waits" },
	SHOT(5000, "obelix_waits"),
	GLIDEAT(5500, HERO_ASTERIX, -150.0f, 5.55f, -257.2f, 1200),
	GLIDEAT(6800, HERO_ASTERIX, -143.39f, 5.54f, -257.725f, 900),
	SHOT(7900, "at_the_turnstile"),
	TAP1(8000), TAP1(8450), TAP1(8900), TAP1(9350), TAP1(9800), TAP1(10250), TAP1(10700), TAP1(11150), TAP1(11600), TAP1(12050), TAP1(12500), TAP1(12950), TAP1(13400), TAP1(13850), TAP1(14300), TAP1(14750), TAP1(15200), TAP1(15650), TAP1(16100), TAP1(16550), TAP1(17000), TAP1(17450), TAP1(17900), TAP1(18350), TAP1(18800), TAP1(19250), TAP1(19700), TAP1(20150), TAP1(20600), TAP1(21050), TAP1(21500), TAP1(21950),
	SHOT(14000, "rising"), SHOT(18000, "rising_2"), SHOT(22500, "after_turns"),
	SHOT(26000, "after"), SHOT(30000, "later"),
	END(33000),
};
// splockcork21 (level 2, XXL_SPISOLATION=0; sp_hero_switch_lock on):
// splockcork35's LV2-1 mirror -- you play Obelix; Asterix (put there) on the
// spinning platform's circle waits; Obelix through zone 03C8, to the turnstile
// (0528: Asterix walks onto the platform), hits it: Asterix rises, jumps off at
// the top (0530's 484C) into box 0058 ("Asterix leads"): dropped -- Asterix
// must follow after.
const Step kSpLockCork21[] = {
	MARK(0, "splockcork21"),
	FORCE(800, HERO_OBELIX, 1), PLACEAT(1000, HERO_ASTERIX, -762.0f, 516.5f, -979.2f), PLACEAT(1000, HERO_OBELIX, -763.0f, 516.5f, -980.5f), FORCE(1400, HERO_OBELIX, 0),
	{ 1800, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	FORCE(2200, HERO_ASTERIX, 1),
	GLIDEAT(2400, HERO_ASTERIX, -778.45f, 518.0f, -982.0f, 1500),
	GLIDEAT(4000, HERO_ASTERIX, -777.997f, 517.84f, -977.562f, 1000),
	{ 5100, S_FLAGEV, 0x0050, 0, 0, 0, 0, "the circle's landing event 0050: obelix leads, asterix waits" },
	FORCE(5200, HERO_ASTERIX, 0),
	PLACEAT(5400, HERO_OBELIX, -762.0f, 516.5f, -979.2f),
	SHOT(6000, "asterix_waits"),
	GLIDEAT(6300, HERO_OBELIX, -744.5f, 515.6f, -977.8f, 1500),   // through zone 03C8 (turnstile21c)
	GLIDEAT(8000, HERO_OBELIX, -775.878f, 515.614f, -990.0f, 2000),
	GLIDEAT(10200, HERO_OBELIX, -775.878f, 515.614f, -983.604f, 600),
	SHOT(12000, "at_turnstile"),
	TAP1(13000), TAP1(13500), TAP1(14000), TAP1(14500), TAP1(15000), TAP1(15500), TAP1(16000), TAP1(16500), TAP1(17000), TAP1(17500), TAP1(18000), TAP1(18500), TAP1(19000), TAP1(19500), TAP1(20000), TAP1(20500), TAP1(21000), TAP1(21500), TAP1(22000), TAP1(22500), TAP1(23000), TAP1(23500), TAP1(24000), TAP1(24500), TAP1(25000),
	SHOT(17000, "rising"), SHOT(21000, "rising_2"), SHOT(25500, "after_turns"),
	SHOT(29000, "after"), SHOT(33000, "later"),
	END(35000),
};
// sphud2 (level 2, XXL_SPISOLATION=0 XXL_SPSECOND=1 XXL_SPSWITCH=1): second
// chance's two health rows -- Asterix's (red) first, Obelix's (blue) under it:
// shown by a hit on Obelix, then one on Asterix; the switch to Obelix (the rows
// stay where they are, the combo bar Obelix's), a hit on Asterix, a shield for
// Obelix.
const Step kSpHud2[] = {
	MARK(0, "sphud2"),
	{ 2000, S_AREAHIT, HERO_OBELIX, 0, 0, 0, 0, "a hit on obelix" },
	SHOT(2600, "rows"), SHOT(3200, "rows_2"),
	{ 4000, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "a hit on asterix" },
	SHOT(4600, "rows_3"),
	{ 9000, S_SPSWITCH, 0, 0, 0, 0, 0, "switch to obelix" },
	{ 9800, S_AREAHIT, HERO_ASTERIX, 0, 0, 0, 0, "a hit on asterix" },
	SHOT(10400, "rows_obelix_leads"), SHOT(11000, "rows_obelix_leads_2"),
	{ 11500, S_GIVE, HERO_OBELIX, 1, 0, 0, 0, "a shield for obelix" },
	SHOT(12100, "rows_shield"),
	END(16000),
};

struct Scenario { const char* name; const Step* steps; int count; };
#define SCENARIO(n, arr) { n, arr, (int)(sizeof(arr) / sizeof(arr[0])) }
const Scenario kScenarios[] = {
	SCENARIO("idle", kIdle),
	SCENARIO("spswitch2", kSpSwitch2),
	SCENARIO("spcork21", kSpCork21),
	SCENARIO("splever25", kSpLever25),
	SCENARIO("spcombo2", kSpCombo2),
	SCENARIO("spicons33", kSpIcons33),
	SCENARIO("sppotion2", kSpPotion2),
	SCENARIO("spsecond2", kSpSecond2),
	SCENARIO("spsecond2c", kSpSecond2C),
	SCENARIO("spsecond2g", kSpSecond2G),
	SCENARIO("spsecond2h", kSpSecond2H),
	SCENARIO("spcork21d", kSpCork21D),
	SCENARIO("spcork21e", kSpCork21E),
	SCENARIO("spbump2", kSpBump2),
	SCENARIO("spbump2b", kSpBump2B),
	SCENARIO("splock21", kSpLock21),
	SCENARIO("sptele36", kSpTele36),
	SCENARIO("spguard41", kSpGuard41),
	SCENARIO("spmod2", kSpMod2),
	SCENARIO("sphud2", kSpHud2),
	SCENARIO("spfuse2", kSpFuse2),
	SCENARIO("splockcork35", kSpLockCork35),
	SCENARIO("spbar2", kSpBar2),
	SCENARIO("spbarx2", kSpBarX2),
	SCENARIO("spboss210", kSpBoss210),
	SCENARIO("spboss36", kSpBoss36),
	SCENARIO("spboss36j", kSpBoss36J),
	SCENARIO("spdownhud2", kSpDownHud2),
	SCENARIO("spsq24", kSpSq24),
	SCENARIO("spsq24n", kSpSq24N),
	SCENARIO("splockcork21", kSpLockCork21),
	SCENARIO("spgun33", kSpGun33),
	SCENARIO("spslide2", kSpSlide2),
	SCENARIO("spslidemode2", kSpSlideMode2),
	SCENARIO("splockord2", kSpLockOrd2),
	SCENARIO("splock43", kSpLock43),
	SCENARIO("splockcar44", kSpLockCar44),
	SCENARIO("splock47", kSpLock47),
	SCENARIO("sptwist34", kSpTwist34),
	SCENARIO("sptwist2", kSpTwist2),
	SCENARIO("sptwist2j", kSpTwist2J),
	SCENARIO("splockev2", kSpLockEv2),
	SCENARIO("spfindkeg2", kSpFindKeg2),
	SCENARIO("splockcar21", kSpLockCar21),
	SCENARIO("hudhints", kHudHints),
	SCENARIO("combohint2", kComboHint2),
	SCENARIO("combohint2b", kComboHint2B),
	SCENARIO("combohint2l", kComboHint2L),
	SCENARIO("crowd36", kCrowd36),
	SCENARIO("flaskbox", kFlaskBox),
	SCENARIO("launchboth", kLaunchBoth),
	SCENARIO("gunboth33", kGunBoth33),
	SCENARIO("obelixgun33", kObelixGun33),
	SCENARIO("obelixgun33l", kObelixGun33L),
	SCENARIO("fuse2", kFuse2),
	SCENARIO("rage2", kRage2),
	SCENARIO("towerscan", kTowerScan),
	SCENARIO("towerhit", kTowerHit),
	SCENARIO("shieldjump", kShieldJump),
	SCENARIO("downcombo", kDownCombo),
	SCENARIO("downcombop", kDownComboP),
	SCENARIO("downcomboo", kDownComboO),
	SCENARIO("overcombo", kOverCombo),
	SCENARIO("catapultcombo", kCatapultCombo),
	SCENARIO("catapultcomboa", kCatapultComboA),
	SCENARIO("survey", kSurvey),
	SCENARIO("bossscan", kBossScan),
	SCENARIO("lifter", kLifter),
	SCENARIO("split1", kSplit1),
	SCENARIO("split2", kSplit2),
	SCENARIO("soak", kSoak),
	SCENARIO("helmet", kHelmet),
	SCENARIO("beacon", kBeacon),
	SCENARIO("cine", kCine),
	SCENARIO("bumper", kBumper),
	SCENARIO("crate", kCrate),
	SCENARIO("grab", kGrab),
	SCENARIO("grab2", kGrab2),
	SCENARIO("borrow", kBorrow),
	SCENARIO("grab3", kGrab3),
	SCENARIO("swing", kSwing),
	SCENARIO("lead2", kLead2),
	SCENARIO("circle", kCircle),
	SCENARIO("swingsp", kSwingSp),
	SCENARIO("cutscene", kCutscene),
	SCENARIO("exp1", kExp1),
	SCENARIO("p2basic", kP2Basic),
	SCENARIO("kbd", kKbd),
	SCENARIO("lifter2", kLifter2),
	SCENARIO("warp", kWarp),
	SCENARIO("potion", kPotion),
	SCENARIO("pursuito", kPursuitO),
	SCENARIO("dash", kDash),
	SCENARIO("joinlead2", kJoinLead2),
	SCENARIO("regions", kRegions),
	SCENARIO("spyo", kSpyO),
	SCENARIO("icons21", kIcons21),
	SCENARIO("iconsgun", kIconsGun),
	SCENARIO("spya", kSpyA),
	SCENARIO("spy2o", kSpy2O),
	SCENARIO("spywalk", kSpyWalk),
	SCENARIO("sas", kSas),
	SCENARIO("sectorzone", kSectorZone),
	SCENARIO("sasnl21", kSasNl21),
	SCENARIO("sasnl21w", kSasNl21w),
	SCENARIO("warp25", kWarp25),
	SCENARIO("cine1", kCine1),
	SCENARIO("boot0", kBoot0),
	SCENARIO("boot1", kBoot1),
	SCENARIO("boot2", kBoot2),
	SCENARIO("skip1", kSkip1),
	SCENARIO("intro1", kIntro1),
	SCENARIO("torch", kTorch),
	SCENARIO("torchgrab", kTorchGrab),
	SCENARIO("torchnl2", kTorchNL2),
	SCENARIO("torchl2", kTorchL2),
	SCENARIO("lever", kLever),
	SCENARIO("launchp2", kLaunchP2),
	SCENARIO("launcha", kLaunchA),
	SCENARIO("lvl1", kLvl1),
	SCENARIO("lvl1obelix", kLvl1Obelix),
	SCENARIO("obelix1s", kObelix1S),
	SCENARIO("obelix1f", kObelix1F),
	SCENARIO("potionmove", kPotionMove),
	SCENARIO("hurto", kHurtO),
	SCENARIO("hurtop", kHurtOP),
	SCENARIO("spygroups", kSpyGroups),
	SCENARIO("savestate", kSaveState),
	SCENARIO("musica", kMusicA),
	SCENARIO("musico", kMusicO),
	SCENARIO("musica2", kMusicA2),
	SCENARIO("bossbutton", kBossButton),
	SCENARIO("lifterp2", kLifterP2),
	SCENARIO("repair", kRepair),
	SCENARIO("release", kRelease),
	SCENARIO("pursuitlead2", kPursuitLead2),
	SCENARIO("flask", kFlask),
	SCENARIO("pursuita", kPursuitA),
	SCENARIO("p2mirror", kP2Mirror),
	SCENARIO("p2busy", kP2Busy),
	SCENARIO("plate3", kPlate3),
	SCENARIO("plate26", kPlate26),
	SCENARIO("beach12o", kBeach12O),
	SCENARIO("boss210", kBoss210),
	SCENARIO("boss210t", kBoss210t),
	SCENARIO("boss210s", kBoss210s),
	SCENARIO("boss210f", kBoss210f),
	SCENARIO("boss210u", kBoss210u),
	SCENARIO("boss210v", kBoss210v),
	SCENARIO("boss36", kBoss36),
	SCENARIO("boss36s", kBoss36s),
	SCENARIO("sky2", kSky2),
	SCENARIO("shadow33", kShadow33),
	SCENARIO("shadow3s", kShadow3S),
	SCENARIO("shadow29", kShadow29),
	SCENARIO("shadow29d", kShadow29D),
	SCENARIO("crates29", kCrates29),
	SCENARIO("crates44", kCrates44),
	SCENARIO("anim2", kAnim2),
	SCENARIO("lbsp", kLbSp),
	SCENARIO("rbp2", kRbP2),
	SCENARIO("grab22", kGrab22),
	SCENARIO("pass22", kPass22),
	SCENARIO("lb2", kLb2),
	SCENARIO("whistle22", kWhistle22),
	SCENARIO("lbwait", kLbWait),
	SCENARIO("preset0", kPreset0),
	SCENARIO("preset1", kPreset1),
	SCENARIO("presetopts", kPresetOpts),
	SCENARIO("fog21", kFog21),
	SCENARIO("spy21o", kSpy21O),
	SCENARIO("spy21a", kSpy21A),
	SCENARIO("mgun3a", kMgun3A),
	SCENARIO("mgun3n", kMgun3N),
	SCENARIO("mgun3o", kMgun3O),
	SCENARIO("mgun33o", kMgun33O),
	SCENARIO("mgun33a", kMgun33A),
	SCENARIO("mgun33j", kMgun33J),
	SCENARIO("mgun33l", kMgun33L),
	SCENARIO("mgun33d", kMgun33D),
	SCENARIO("boat49o", kBoat49O),
	SCENARIO("boat49a", kBoat49A),
	SCENARIO("iconsboat49", kIconsBoat49),
	SCENARIO("hintjoin", kHintJoin),
	SCENARIO("spy3n", kSpy3N),
	SCENARIO("hudvanish", kHudVanish),
	SCENARIO("hudlayout", kHudLayout),
	SCENARIO("hudreq", kHudReq),
	SCENARIO("hudx3", kHudX3),
	SCENARIO("spy3p", kSpy3P),
	SCENARIO("catapultp", kCatapultP),
	SCENARIO("catapulta", kCatapultA),
	SCENARIO("shop", kShop),
	SCENARIO("shopo", kShopO),
	SCENARIO("shopa", kShopA),
	SCENARIO("shopox", kShopOX),
	SCENARIO("shopsplit", kShopSplit),
	SCENARIO("shoppause", kShopPause),
	SCENARIO("shopsplita", kShopSplitA),
	SCENARIO("pit26o", kPit26O),
	SCENARIO("hit2o", kHit2O),
	SCENARIO("down2a", kDown2A),
	SCENARIO("hud2", kHud2),
	SCENARIO("bump2", kBump2),
	SCENARIO("bump2i", kBump2I),
	SCENARIO("shield2o", kShield2O),
	SCENARIO("revshield", kRevShield),
	SCENARIO("combofull2", kComboFull2),
	SCENARIO("combomix", kComboMix),
	SCENARIO("pushkeg5", kPushKeg5),
	SCENARIO("leave2", kLeave2),
	SCENARIO("launch53", kLaunch53),
	SCENARIO("cork53", kCork53),
	SCENARIO("cork53b", kCork53B),
	SCENARIO("molefov", kMoleFov),
	SCENARIO("shields6", kShields6),
	SCENARIO("shieldfull", kShieldFull),
	SCENARIO("hudorder", kHudOrder),
	SCENARIO("shopspy", kShopSpy),
	SCENARIO("shopspyb", kShopSpyB),
	SCENARIO("spyonly", kSpyOnly),
	SCENARIO("shopspyw", kShopSpyW),
	SCENARIO("fight4h", kFight4H),
	SCENARIO("lock4", kLock4),
	SCENARIO("rules4d", kRules4D),
	SCENARIO("rules4n", kRules4N),
	SCENARIO("rules4t", kRules4T),
	SCENARIO("rules4p", kRules4P),
	SCENARIO("rules4l", kRules4L),
	SCENARIO("lock34", kLock34),
	SCENARIO("lock34f", kLock34F),
	SCENARIO("lock34g", kLock34G),
	SCENARIO("lock34t", kLock34T),
	SCENARIO("lockcircle", kLockCircle),
	SCENARIO("slidecross3", kSlideCross3),
	SCENARIO("slidecross", kSlideCross),
	SCENARIO("slideend", kSlideEnd),
	SCENARIO("cabled2", kCableD2),
	SCENARIO("spyd2", kSpyD2),
	SCENARIO("spyrev", kSpyRev),
	SCENARIO("swingundo", kSwingUndo),
	SCENARIO("lockagain", kLockAgain),
	SCENARIO("shopjoin", kShopJoin),
	SCENARIO("shopsas", kShopSas),
	SCENARIO("shopsas0", kShopSas0),
	SCENARIO("shoplaunch4", kShopLaunch4),
	SCENARIO("shoplaunch4b", kShopLaunch4b),
	SCENARIO("shoplaunch4c", kShopLaunch4c),
	SCENARIO("shoplaunch4d", kShopLaunch4d),
	SCENARIO("shoplift", kShopLift),
	SCENARIO("shopbossrun", kShopBossRun),
	SCENARIO("shopboss", kShopBoss),
	SCENARIO("combodrain", kComboDrain),
	SCENARIO("shopzone", kShopZone),
	SCENARIO("shopsave", kShopSave),
	SCENARIO("shopbuyo", kShopBuyO),
	SCENARIO("shopsavea", kShopSaveA),
	SCENARIO("shopsaveo", kShopSaveO),
	SCENARIO("cabled33", kCableD33),
	SCENARIO("liftcab33", kLiftCab33),
	SCENARIO("liftcab33w", kLiftCab33W),
	SCENARIO("cablefull33", kCableFull33),
	SCENARIO("cross33", kCross33),
	SCENARIO("corkdown", kCorkDown),
	SCENARIO("lifterlock", kLifterLock),
	SCENARIO("lifterlock2", kLifterLock2),
	SCENARIO("turnstile21", kTurnstile21),
	SCENARIO("corklead21", kCorkLead21),
	SCENARIO("pull28", kPull28),
	SCENARIO("prompt32", kPrompt32),
	SCENARIO("lead28", kLead28),
	SCENARIO("camfall28", kCamFall28),
	SCENARIO("tele28", kTele28),
	SCENARIO("beam28", kBeam28),
	SCENARIO("beam28b", kBeam28B),
	SCENARIO("corkobelix21", kCorkObelix21),
	SCENARIO("turnstile21c", kTurnstile21c),
	SCENARIO("turnstile21d", kTurnstile21d),
	SCENARIO("turnstile21e", kTurnstile21e),
	SCENARIO("beam42", kBeam42),
	SCENARIO("beam42f", kBeam42f),
	SCENARIO("beam42l", kBeam42L),
	SCENARIO("beam32", kBeam32),
	SCENARIO("beam25", kBeam25),
	SCENARIO("freeromans23", kFreeRomans23),
	SCENARIO("keg23", kKeg23),
	SCENARIO("keg23s", kKeg23S),
	SCENARIO("pillar36", kPillar36),
	SCENARIO("spitter44", kSpitter44),
	SCENARIO("spitter44o", kSpitter44o),
	SCENARIO("spitter44n", kSpitter44n),
	SCENARIO("spitter44a", kSpitter44a),
	SCENARIO("spitter44d", kSpitter44d),
	SCENARIO("spitter44l", kSpitter44l),
	SCENARIO("pillar36l", kPillar36l),
	SCENARIO("cam14", kCam14),
	SCENARIO("crates1cc", kCrates1CC),
	SCENARIO("turnstile21b", kTurnstile21b),
	SCENARIO("liftercar", kLifterCar),
	SCENARIO("carpause", kCarPause),
	SCENARIO("unlockpillar", kUnlockPillar),
	SCENARIO("unlockcar", kUnlockCar),
	SCENARIO("unlockseq", kUnlockSeq),
	SCENARIO("unlockcar33", kUnlockCar33),
	SCENARIO("unlockcar21", kUnlockCar21),
	SCENARIO("beamundo", kBeamUndo),
	SCENARIO("liftd58", kLiftD58),
	SCENARIO("lead510", kLead510),
	SCENARIO("lead510o", kLead510O),
	SCENARIO("reload2", kReload2),
	SCENARIO("fight64", kFight64),
	SCENARIO("fight56", kFight56),
	SCENARIO("gatedone", kGateDone),
	SCENARIO("gatedonew", kGateDoneW),
	SCENARIO("marker56", kMarker56),
	SCENARIO("pf26", kPf26),
	SCENARIO("pf23", kPf23),
	SCENARIO("reach26", kReach26),
	SCENARIO("reach26o", kReach26O),
	SCENARIO("reach26c", kReach26C),
	SCENARIO("reach26f", kReach26F),
	SCENARIO("reach26s", kReach26S),
	SCENARIO("zonejump26", kZoneJump26),
	SCENARIO("zonejump26j", kZoneJump26J),
	SCENARIO("unreach26", kUnreach26),
	SCENARIO("revive41", kRevive41),
	SCENARIO("catrevive31", kCatRevive31),
	SCENARIO("catrate31", kCatRate31),
	SCENARIO("liftdown21", kLiftDown21),
	SCENARIO("liftdown21r", kLiftDown21R),
	SCENARIO("liftdie21a", kLiftDie21A),
	SCENARIO("liftdie21b", kLiftDie21B),
	SCENARIO("reach26b", kReach26B),
	SCENARIO("reach23", kReach23),
	SCENARIO("reach23g", kReach23G),
	SCENARIO("reach26w", kReach26W),
	SCENARIO("relight23", kRelight23),
	SCENARIO("relight23n", kRelight23N),
	SCENARIO("pausedown", kPauseDown),
	SCENARIO("reach26p", kReach26P),
	SCENARIO("warppit26", kWarpPit26),
	SCENARIO("warplift58", kWarpLift58),
	SCENARIO("sq36a", kSq36a),
	SCENARIO("over56", kOver56),
	SCENARIO("sq36b", kSq36b),
	SCENARIO("sq36c", kSq36c),
	SCENARIO("guard36", kGuard36),
	SCENARIO("guard41", kGuard41),
	SCENARIO("guard41m", kGuard41M),
	SCENARIO("guard41a", kGuard41A),
	SCENARIO("pool36", kPool36),
	SCENARIO("poolkinds", kPoolKinds),
	SCENARIO("spots26", kSpots26),
	SCENARIO("twistroman", kTwistRoman),
	SCENARIO("twistromana", kTwistRomanA),
	SCENARIO("twistend", kTwistEnd),
	SCENARIO("twistenda", kTwistEndA),
	SCENARIO("req410", kReq410),
	SCENARIO("bump2l", kBump2L),
	SCENARIO("bump2w", kBump2W),
	SCENARIO("bump1", kBump1),
	SCENARIO("huddiff", kHudDiff),
	SCENARIO("combo2", kCombo2),
	SCENARIO("pit26b", kPit26B),
	SCENARIO("pit26c", kPit26C),
	SCENARIO("rejoin2", kRejoin2),
	SCENARIO("pit26a", kPit26A),
	SCENARIO("inputdump", kInputDump),
	SCENARIO("combop2", kComboP2),
	SCENARIO("combop1", kComboP1),
	SCENARIO("helmets3", kHelmets3),
	SCENARIO("helmets12", kHelmets12),
	SCENARIO("helmets12d", kHelmets12D),
	SCENARIO("music5a", kMusic5A),
	SCENARIO("music5o", kMusic5O),
	SCENARIO("group38", kGroup38),
	SCENARIO("music5l", kMusic5L),
	SCENARIO("camp2", kCamp2),
	SCENARIO("twistsp", kTwistSP),
	SCENARIO("twistco", kTwistCo),
	SCENARIO("twistco2", kTwistCo2),
	SCENARIO("boat49", kBoat49),
	SCENARIO("lifterwait", kLifterWait),
	SCENARIO("detect58", kDetect58),
	SCENARIO("lift58o", kLift58O),
	SCENARIO("lift58a", kLift58A),
	SCENARIO("lift58u", kLift58U),
	SCENARIO("lift58t", kLift58T),
	SCENARIO("lift58l", kLift58L),
	SCENARIO("circle45", kCircle45),
	SCENARIO("launch48", kLaunch48),
	SCENARIO("launch48a", kLaunch48A),
	SCENARIO("shine25", kShine25),
	SCENARIO("shine44", kShine44),
	SCENARIO("cam32", kCam32),
	SCENARIO("cam32o", kCam32O),
	SCENARIO("slide22", kSlide22),
	SCENARIO("slide22s", kSlide22S),
	SCENARIO("slide2h", kSlide2H),
	SCENARIO("camjump", kCamJump),
	SCENARIO("p2dir", kP2Dir),
	SCENARIO("cable33", kCable33),
	SCENARIO("carunlock33", kCarUnlock33),
	SCENARIO("carlead33", kCarLead33),
	SCENARIO("carleadu33", kCarLeadU33),
	SCENARIO("cam32s", kCam32S),
	SCENARIO("fight4o", kFight4O),
	SCENARIO("camfight", kCamFight),
	SCENARIO("fight4a", kFight4A),
	SCENARIO("fight36", kFight36),
	SCENARIO("fight36v", kFight36V),
	SCENARIO("join36", kJoin36),
	SCENARIO("nolockmark36", kNoLockMark36),
	SCENARIO("nolock34", kNoLock34),
	SCENARIO("nolock34r", kNoLock34R),
	SCENARIO("nolock36w", kNoLock36W),
	SCENARIO("nolock36x", kNoLock36X),
	SCENARIO("nolock34m", kNoLock34M),
	SCENARIO("nolock36e", kNoLock36E),
	SCENARIO("nolock36f", kNoLock36F),
	SCENARIO("nolock36s", kNoLock36S),
	SCENARIO("nolock36u", kNoLock36U),
	SCENARIO("nolock36v", kNoLock36V),
	SCENARIO("nolock36y", kNoLock36Y),
};

// Fixed spots (level 2), from players' logs.
const float kSpots[][3] = {
	{ -793.89f, 512.30f, -1060.49f },   // 0: the cable car (swing) seat
	{ -794.40f, 508.40f, -1060.40f },   // 1: next to the cable car's rope
	{ -790.45f, 510.60f, -1059.94f },   // 2: just before the cable car
	{ -771.90f, 515.60f, -976.80f },    // 3: Asterix near the spinning platform
	{ -777.20f, 515.90f, -977.00f },    // 4: where player 2's Obelix set off the circle
	{ -744.00f, 509.00f, -1006.00f },   // 5: the lifter's zone (sphere bound to Asterix, event 0408)
	{ -8.00f, 45.50f, -146.00f },       // 6: sector 8's entrance (where the user's sector switched)
	{ -815.00f, 507.60f, -1061.00f },   // 7: past the spy's dialogue trigger (level 2 start area)
	{ 16.00f, 51.50f, -124.50f },       // 8: where the music starts for the leader (checkpoint)
	{ -759.40f, 508.50f, -1014.80f },   // 9: where the user's Obelix waited for the lifter area's spy
	{ -811.50f, 531.00f, -970.00f },    // 10: sector 1's Sas box that streams in sector 2
	{ -803.00f, 532.00f, -987.00f },    // 11: before the rectangles (#75/#76, at -811 533 -987) that switch to sector 2
	{ -819.00f, 532.00f, -987.00f },    // 12: past them
	{ -783.30f, 517.40f, -990.00f },    // 13: where the user's Asterix stayed behind in sector 1
	{ -744.80f, 514.60f, -980.60f },    // 14: next to the level 2 torch (-744.3 514.2 -981.4)
	{ 134.20f, 12.60f, -572.30f },      // 15: at sector 5's lever behind the iron crates (134.2 12.1 -573.7)
	{ 144.30f, 2.60f, -566.50f },       // 16: sector 5, next to the hearth (142.8 2.1 -568.2)
	{ -264.30f, 13.20f, -521.00f },     // 17: on top of sector 2's launcher (CKHkTrioCatapult, -264.3 12.3 -521.0)
	{ -270.00f, 9.00f, -521.00f },      // 18: next to that launcher
	{ -2.22f, -2.30f, 58.62f },         // 19: level 1, the zone that streams in sector 2
	{ 46.50f, 5.60f, 105.84f },         // 20: level 1, before the rectangle (52.1 5.5 105.8) where Asterix finds Obelix
	{ 57.50f, 5.60f, 105.84f },         // 21: past it
};

const int kNumSpots = (int)(sizeof(kSpots) / sizeof(kSpots[0]));

struct AutotestState {
	const Scenario* sc = nullptr;
	enum Phase { OFF, WAIT, RUN, DONE } phase = OFF;
	DWORD bootTick = 0, liveSince = 0, runStart = 0, lastLog = 0;
	int   next = 0;
	int   forceHero = -1;
	int   forceReverts = 0;

	bool     p1Active = false;
	int      p1Combo = -1;          // P1COMBO: combo k's action for player 1 next frame
	int      bind[3] = { -1, -1, -1 };   // S_BIND: bindings pressed in the game's values
	int      bindFrames = 0;            // ... for that many more frames
	PadState p1;
	uint16_t p1Prev = 0;
	PadState vpad[kMaxPads];

	HWND    hwnd = nullptr;
	WNDPROC prevProc = nullptr;
	char*   watchBonus = nullptr;   // CKHkBasicBonus hook being watched
	char*   watchCrate = nullptr;   // CKHkCrate chosen by NEAR_CRATE
	char*   watchEnemy = nullptr;   // CKHkBasicEnemy chosen by NEAR_ENEMY
	char*   watchClue = nullptr;    // CKHkClueMan (the spy), WATCHCLUE
	char*   watchTorch = nullptr;   // CKHkTorch nearest Asterix, TORCH
	bool    watchCam = false;       // the game camera (service +0x5B0 pos, +0x5BC look, track), WATCHCAM
	bool    watchCine = false;      // every cinematic scene's +0x20 / +0x30 changes, WATCHCINE
	bool    watchBeacons = false;   // beacon bonuses near beaconPoint, WATCHBEACONS
	bool    watchEnemies = false;   // Romans and squads near enemyPoint, WATCHENEMIES
	bool    enemiesPrimed = false;
	bool    cpntCheck = false;      // every Roman's component (+0x54) checked each frame, CPNTCHECK
	Vector3 enemyPoint;
	Vector3 beaconPoint;
	int     cineScene = -1;         // scene started by CINE_START
	bool    probe = false;          // log cutscene-related engine state each tick
	struct Glide { bool on; Vector3 from, to; DWORD start, ms; } glide[HERO_COUNT] = {};
} at;

void SetHeroPos(Hero h, const Vector3& to) {
	h.Pos() = to;
	if (h.hook->node && h.hook->node->matrix) {
		float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
		hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
	}
}

void RunGlides(DWORD now) {
	Trio t = GetTrio();
	for (int i = 0; i < HERO_COUNT; i++) {
		auto& g = at.glide[i];
		Hero h = t ? t.GetHero(i) : Hero();
		if (!g.on || !h) continue;
		float f = (float)(now - g.start) / (float)(g.ms ? g.ms : 1);
		if (f >= 1.0f) { f = 1.0f; g.on = false; Log("autotest: %s glide done", HeroName(i)); }
		Vector3 p(g.from.x + (g.to.x - g.from.x) * f, g.from.y + (g.to.y - g.from.y) * f,
		          g.from.z + (g.to.z - g.from.z) * f);
		SetHeroPos(h, p);
	}
}

LRESULT CALLBACK AutotestWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	// The game pauses when it loses focus; a test run must not stall because
	// somebody clicked another window.
	if (m == WM_ACTIVATEAPP && w == FALSE) return 0;
	if (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) return 0;
	return CallWindowProcA(at.prevProc, h, m, w, l);
}

void SubclassWindow() {
	if (at.prevProc) return;
	HWND hwnd = GameStartInfo()->hwnd;
	if (!hwnd || !IsWindow(hwnd)) return;
	at.hwnd = hwnd;
	at.prevProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)AutotestWndProc);
	bool fg = GetForegroundWindow() == hwnd;
	Log("autotest: window %p subclassed (foreground=%d)", hwnd, fg);
	if (!fg) {
		SetForegroundWindow(hwnd);
		CallWindowProcA(at.prevProc, hwnd, WM_ACTIVATEAPP, TRUE, 0);
	}
}

void Shot(const char* name) {
	if (!at.hwnd) return;
	char dir[MAX_PATH], file[MAX_PATH], path[MAX_PATH];
	GameDirPath(dir, sizeof(dir), "shots");
	CreateDirectoryA(dir, nullptr);
	_snprintf_s(file, sizeof(file), _TRUNCATE, "shots\\%s.png", name);
	GameDirPath(path, sizeof(path), file);
	CaptureWindowPng(at.hwnd, path, 2);
}

void Finish(const char* why) {
	at.phase = AutotestState::DONE;
	for (int i = 0; i < kMaxPads; i++) PadsSetVirtual(i, nullptr);
	CoopLogState("final");
	Log("autotest: finished (%s); counters: asterixTicks=%ld obelixTicks=%ld p2Routed=%ld "
		"enforced=%ld trioEvents=%ld forceReverts=%d",
		why, g_hookCounters.asterixTicks, g_hookCounters.obelixTicks, g_hookCounters.p2Routed,
		g_hookCounters.enforced, g_hookCounters.trioEvents, at.forceReverts);
	LogClose();
	TerminateProcess(GetCurrentProcess(), 0);
}

// Pool kinds (S_POOLKINDS): an enemy component's class by its vtable.
const char* CpntClass(uintptr_t vt) {
	switch (vt) {
	case 0x627B10: return "CKSquadEnemyCpnt";
	case 0x627B40: return "CKSeizableEnemyCpnt";
	case 0x627BA0: return "CKSquadSeizableEnemyCpnt";
	case 0x627BD0: return "CKBasicEnemyCpnt";
	case 0x627C00: return "CKEnemyCpnt";
	case 0x627C30: return "CKBasicEnemyLeaderCpnt";
	case 0x627C90: return "CKJetPackRomanCpnt";
	case 0x627CC0: return "CKJumpingRomanCpnt";
	case 0x627CF0: return "CKRomanArcherCpnt";
	case 0x627D20: return "CKMobileTowerCpnt";
	case 0x627D50: return "CKTriangularTurtleCpnt";
	case 0x627DB8: return "CKSquareTurtleCpnt";
	case 0x627DEC: return "CKDonutTurtleCpnt";
	case 0x627E20: return "CKPyramidalTurtleCpnt";
	}
	return "?";
}

// HUD diagnostics (S_HUDDIFF): the in-game interface (2,132) and every object
// it points at; from the start, which bytes of each ever change (per frame).
struct HudObj { char* p; int size; char tag[40]; uint8_t prev[0x600]; uint8_t changed[0x600]; };
HudObj* g_hud = nullptr;
int g_nHud = 0;
bool g_hudDiff = false;

bool IsVt(uintptr_t v) { return v >= 0x620000 && v < 0x630000; }

void HudAdd(char* p, int size, const char* fmt, ...) {
	if (!p || g_nHud >= 400 || IsBadReadPtr(p, size)) return;
	for (int i = 0; i < g_nHud; i++) if (g_hud[i].p == p) return;
	HudObj& o = g_hud[g_nHud++];
	o.p = p;
	o.size = size;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(o.tag, sizeof(o.tag), fmt, ap);
	va_end(ap);
	memcpy(o.prev, p, size);
	memset(o.changed, 0, sizeof(o.changed));
}

void HudDiffStart() {
	if (!g_hud) g_hud = new HudObj[400];
	g_nHud = 0;
	char* itf = (char*)GetLvlObject(2, 132, 0);
	if (!itf) { Log("autotest: no in-game interface"); return; }
	HudAdd(itf, 0x600, "interface");
	for (int o = 0; o < 0x600; o += 4) {
		char* q = *(char**)(itf + o);
		if (!q || IsBadReadPtr(q, 0x60)) continue;
		uintptr_t vt = *(uintptr_t*)q;
		if (!IsVt(vt)) continue;
		Log("autotest: interface +%03X -> %p vt %08X", o, q, (unsigned)vt);
		HudAdd(q, 0x100, "+%03X obj vt %X", o, (unsigned)vt);
		char* fr = *(char**)(q + 4);
		if (vt >= 0x6230D4 && vt <= 0x623348 && fr && !IsBadReadPtr(fr, 0xA0)) {
			HudAdd(fr, 0xA0, "+%03X frame", o);
			char* at = *(char**)(q + 0x48);
			if (at && !IsBadReadPtr(at, 0x80)) HudAdd(at, 0x80, "+%03X +48", o);
		}
	}
	g_hudDiff = true;
	Log("autotest: watching %d HUD objects", g_nHud);
}

void HudDiffFrame() {
	if (!g_hudDiff) return;
	for (int i = 0; i < g_nHud; i++) {
		HudObj& o = g_hud[i];
		for (int k = 0; k < o.size; k++) {
			uint8_t v = o.p[k];
			if (v != o.prev[k]) { o.changed[k] = 1; o.prev[k] = v; }
		}
	}
}

void HudDiffReport() {
	if (!g_hudDiff) return;
	for (int i = 0; i < g_nHud; i++) {
		HudObj& o = g_hud[i];
		char buf[1600];
		LineBuf line(buf);
		line.Add("autotest: hud %s %p changed:", o.tag, o.p);
		int n = 0;
		for (int k = 0; k < o.size; k += 4) {
			if (!(o.changed[k] | o.changed[k + 1] | o.changed[k + 2] | o.changed[k + 3])) continue;
			if (++n < 120) line.Add(" %X", k);
		}
		if (n) Log("%s", buf);
	}
}

void LogTick(float t) {
	Trio tr = GetTrio();
	if (!tr) return;
	char buf[512];
	LineBuf line(buf);
	line.Add("at %6.2f", t);
	for (int i = HERO_ASTERIX; i <= HERO_OBELIX; i++) {
		Hero h = tr.GetHero(i);
		if (!h) continue;
		char* hn = (char*)h.hook->node;
		line.Add(" | %c%s f18=%02X L=%02X sit=%02X body=%02X ai=%02X atk=%02X/%02X pos=(%7.2f %6.2f %7.2f)",
			i == HERO_ASTERIX ? 'A' : 'O', hn && (*(uint8_t*)(hn + 0x10) & 2) ? "(hidden)" : "", h.Flags18(), LifeLastState(i),
			*(uint8_t*)(h.raw() + 0x3E9), *(uint8_t*)(h.raw() + 0xE5),
			*(uint8_t*)(h.raw() + 0x561), *(uint8_t*)(h.raw() + 0x135), *(uint8_t*)(h.raw() + 0x38E) & 0x3F,
			h.Pos().x, h.Pos().y, h.Pos().z);
		if (getenv("XXL_MOVELOG")) {   // movement fields
			const float* f = (const float*)h.raw();
			line.Add(" 38C %08X 22C %.2f %.2f", *(uint32_t*)(h.raw() + 0x38C), f[0x22C / 4], f[0x234 / 4]);
			line.Add(" mv 8C %.2f 94 %.2f gn %.2f %.2f %.2f 2E4 %.2f %.2f %.2f 284 %p 5BC %p", f[0x8C / 4], f[0x94 / 4], f[0x288 / 4],
			         f[0x28C / 4], f[0x290 / 4], f[0x2E4 / 4], f[0x2E8 / 4], f[0x2EC / 4], *(void**)(h.raw() + 0x284), *(void**)(h.raw() + 0x5BC));
		}
		{   // what it stands on (+0x280) and the on-ground bits (+0x384)
			char* gr = *(char**)(h.raw() + 0x280);
			line.Add(" gr %p/%p %02X", gr, gr && !IsBadReadPtr(gr, 4) ? *(void**)gr : nullptr, *(uint8_t*)(h.raw() + 0x384));
		}
		char* tgt = *(char**)(h.raw() + 0xF8);   // the enemy the hero is after / holding, and its state (+0x21)
		if (tgt && !IsBadReadPtr(tgt, 0x24)) line.Add(" tgt %p st %d", tgt, *(uint8_t*)(tgt + 0x21));
		uint8_t atk = *(uint8_t*)(h.raw() + 0x135);
		char* clk = *(char**)(h.raw() + 0x368);
		if ((atk == 9 || atk == 10) && clk)   // a Twister: its spin's age (+0x164 start), partner near +0x189, fusion asked +0x188
			line.Add(" tw %.2f n%d r%d", *(float*)(clk + 4) - *(float*)(h.raw() + 0x164), *(uint8_t*)(h.raw() + 0x189),
			         *(uint8_t*)(h.raw() + 0x188));
	}
	line.Add(" | lead=%d p2=%s", tr.Leader(), CoopP2Joined() ? HeroName(CoopP2Hero()) : "-");
	{   // shared health (game manager +0x40 -> +0x50 points, +0x51 shields) and each hero's dead bit (+0x390 bit 2)
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
		Hero a = tr.GetHero(HERO_ASTERIX), o = tr.GetHero(HERO_OBELIX);
		if (cb) line.Add(" | hp=%d/%d dead=%d%d bar %.1f/%.1f%s", *(uint8_t*)(cb + 0x50), *(uint8_t*)(cb + 0x51),
		                 a ? (*(uint8_t*)(a.raw() + 0x390) >> 2) & 1 : 9, o ? (*(uint8_t*)(o.raw() + 0x390) >> 2) & 1 : 9,
		                 *(float*)(gm + 0xA4), *(float*)(gm + 0xA0), (*(uint32_t*)(cb + 0x10) & 0x4000) ? " full" : "");
		int hp[2], sh[2], hits[2];
		float gauge[2];
		if (IndivStats(HERO_ASTERIX, &hp[0], &sh[0], &hits[0], &gauge[0]) && IndivStats(HERO_OBELIX, &hp[1], &sh[1], &hits[1], &gauge[1]))
			line.Add(" | indiv A %d/%d hits %d gauge %.1f, O %d/%d hits %d gauge %.1f", hp[0], sh[0], hits[0], gauge[0], hp[1], sh[1],
			         hits[1], gauge[1]);
	}
	{   // the button prompt (CKGrpMeca, game manager +0x18: owner +0x90; gm +0x5C bit 0, kind +0x68)
		// and the trio's machine offer (+0x48, +0x3F4 bit 8)
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		char* meca = gm ? *(char**)(gm + 0x18) : nullptr;
		if (gm) line.Add(" | hud prompt %X", (*(uint32_t*)(gm + 0xCC) >> 4) & 0xF);
		if (meca) line.Add(" | prompt %p kind %d on %d trio48 %p/%d", *(void**)(meca + 0x90), *(uint8_t*)(gm + 0x68),
		                   *(uint32_t*)(gm + 0x5C) & 1, *(void**)(tr.raw() + 0x48), (*(uint32_t*)(tr.raw() + 0x3F4) >> 3) & 1);
	}
	if (getenv("XXL_LIFTLOG"))   // lifters: flags +8 (state (>> 1) & 3), height, wait time +0x34, platform +0x50
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 47); i < n && i < 4; i++) {
			KHook* l = (KHook*)GetLvlObject(2, 47, i);
			if (!l || !l->node || !l->node->matrix) continue;
			char* r = (char*)l;
			line.Add(" | lifter %u f8 %08X y %.2f w %.1f pl %p", i, *(uint32_t*)(r + 8), ((float*)((char*)l->node->matrix + 0x40))[1],
			         *(float*)(r + 0x34), *(void**)(r + 0x50));
		}
	if (getenv("XXL_LIFTLOG")) {   // rope cable cars (CKHkTowedTelepher 2,159): flags +0x7C, zone answer +0x20 vs +0x84
		char cbuf[1024];   // a line of its own (the tick line is full)
		LineBuf cars(cbuf);
		cars.Add("cars at %6.2f", t);
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 159); i < n && i < 4; i++) {
			char* c = (char*)GetLvlObject(2, 159, i);
			char* node = c ? *(char**)(c + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (c) cars.Add(" | car %u 7C %08X 20 %p 84 %p 30 %.2f at %.1f %.1f %.1f", i, *(uint32_t*)(c + 0x7C), *(void**)(c + 0x20),
			                *(void**)(c + 0x84), *(float*)(c + 0x30), fr ? ((float*)(fr + 0x80))[0] : 0, fr ? ((float*)(fr + 0x80))[1] : 0,
			                fr ? ((float*)(fr + 0x80))[2] : 0);
		}
		// turnstiles (CKHkTurnstile 2,45): flags +8; spinning platforms (2,44) near: flags +8, +0x28
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 45); i < n && i < 4; i++) {
			char* k = (char*)GetLvlObject(2, 45, i);
			char* node = k ? *(char**)(k + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!fr) continue;
			const float* m = (const float*)(fr + 0x80);
			cars.Add(" | turnstile %u f8 %08X at %.1f %.1f %.1f", i, *(uint32_t*)(k + 8), m[0], m[1], m[2]);
		}
		{   // LV2-1's platform zones: box-enter #13 (0528, the ride + camera), plane-cross #1B (03C8)
			CKYellowPages* yp = YellowPages();
			char* det = yp ? *(char**)((char*)yp + 0x54) : nullptr;
			if (det && *(uintptr_t*)det == 0x62504C && *(uint16_t*)(det + 0x30) > 0x13 && *(uint16_t*)(det + 0x40) > 0x1B)
				cars.Add(" | box13 %u cross1B %u", *(uint8_t*)(*(char**)(det + 0x2C) + 8 * 0x13 + 4) & 1,
				         *(uint8_t*)(*(char**)(det + 0x3C) + 8 * 0x1B + 4) & 1);
		}
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 32); i < n && i < 16; i++) {   // carried torches (CKHkTorch 2,32)
			char* tc = (char*)GetLvlObject(2, 32, i);
			if (tc && (*(uint8_t*)(tc + 0x3E) & 3) == 1)
				cars.Add(" | torch %u 3E %02X fire %d timer %.2f lead %d", i, *(uint8_t*)(tc + 0x3E), (*(uint8_t*)(tc + 0x3E) >> 2) & 7,
				         *(float*)(tc + 0x1C), tr.Leader());
		}
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 57); i < n && i < 4; i++) {   // rotary beams (CKHkRotaryBeam 2,57)
			char* b = (char*)GetLvlObject(2, 57, i);
			char* node = b ? *(char**)(b + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (fr) cars.Add(" | beam %u f8 %08X at %.1f %.1f %.1f", i, *(uint32_t*)(b + 8), ((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1],
			                  ((float*)(fr + 0x80))[2]);
			// its angle (+0x28) and target (+0x38); rest / over the circle / far end (+0x4C/+0x50/+0x54); Asterix's distance (x-z)
			Hero ha = tr.GetHero(HERO_ASTERIX);
			if (fr && ha) cars.Add(" ang %.2f to %.2f (%.2f %.2f %.2f) A %.1f", *(float*)(b + 0x28), *(float*)(b + 0x38), *(float*)(b + 0x4C),
			                       *(float*)(b + 0x50), *(float*)(b + 0x54),
			                       sqrtf(powf(ha.Pos().x - ((float*)(fr + 0x80))[0], 2) + powf(ha.Pos().z - ((float*)(fr + 0x80))[2], 2)));
		}
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 44); i < n && i < 8; i++) {
			char* c = (char*)GetLvlObject(2, 44, i);
			if (c && (*(uint32_t*)(c + 8) & 3)) cars.Add(" | cork %u f8 %08X 28 %.2f", i, *(uint32_t*)(c + 8), *(float*)(c + 0x28));
		}
		// the cameras' targets (service +0xE8; a hero's is its +0x10): the copy's and the game's
		for (int hh = 0; hh < 2; hh++)
			if (const char* cs = GameCamCopyState(hh)) {
				Hero hx = tr.GetHero(hh);
				cars.Add(" | %s's cam target %p (hero %p)", HeroName(hh), *(void* const*)(cs + 0xE8), hx ? *(void**)(hx.raw() + 0x10) : nullptr);
			}
		{
			CKYellowPages* yp = YellowPages();
			char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
			Hero hl = tr.GetHero(tr.Leader());
			if (svc) cars.Add(" | game cam target %p (leader %p)", *(void**)(svc + 0xE8), hl ? *(void**)(hl.raw() + 0x10) : nullptr);
		}
		Log("%s", cars.c_str());
	}
	if (at.probe) {
		// Everything that might say "a cutscene has the player": cinematic scenes,
		// the camera service's track and flags, game manager / trio flags, the
		// hero command block, and the input bindings and values.
		__try {
			CKYellowPages* yp = YellowPages();
			char* cin = *(char**)((char*)yp + 0x80);
			char* cam = *(char**)((char*)yp + 0x68);
			char* gm = (char*)yp->gameManager;
			char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
			char* inp = *(char**)((char*)yp + 0x14);
			line.Add(" | bars=%d gm160=%08X cin:", cin ? (*(uint8_t*)(cin + 0xC) & 1) : -1,
				gm ? *(unsigned*)(gm + 0x160) : 0);
			char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
			unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
			int shown = 0;
			for (unsigned i = 0; scenes && i < n && i < 64 && shown < 6; i++) {
				unsigned f20 = *(unsigned short*)(scenes[i] + 0x20), f30 = *(uint8_t*)(scenes[i] + 0x30);
				if ((f20 & 0x8001) || f30) { line.Add(" %u:%04X/%02X", i, f20, f30); shown++; }
			}
			char* track = cam ? *(char**)(cam + 0x38) : nullptr;
			line.Add(" | cam trk=%08X fl=%08X | gm5C=%08X trio3F4=%08X", track ? *(unsigned*)track : 0,
				cam ? *(unsigned*)(cam + 0xC) : 0, gm ? *(unsigned*)(gm + 0x5C) : 0,
				tr ? *(unsigned*)(tr.raw() + 0x3F4) : 0);
			if (cb) line.Add(" | cb1C=%05X cb5C=%d cb8C=%.2f", *(unsigned*)(cb + 0x1C) & 0xFFFFF,
				(int)*(signed char*)(cb + 0x5C), *(float*)(cb + 0x8C));
			if (inp) {
				unsigned mask = 0;
				char* binds = *(char**)(inp + 0x18);
				for (int i = 0; i < 16 && binds; i++) if (binds[i * 0x20 + 4] == 1) mask |= 1u << i;
				uint8_t* v = *(uint8_t**)(inp + 0x1C);
				line.Add(" | bind=%04X v=%02X%02X%02X%02X in68=%d", mask, v ? v[0] : 0, v ? v[1] : 0,
					v ? v[2] : 0, v ? v[3] : 0, *(uint8_t*)(inp + 0x68));
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			line.Add(" | probe fault");
		}
	}
	if (at.cpntCheck) {   // a Roman's +0x54 is its pool's component (CK*Cpnt, vtables 0x627B10..0x627E20) or 0
		const int ids[] = { 75, 93, 95, 148, 164 };
		static char* s_obj[1024];
		static char* s_last[1024];
		static int s_n = 0, s_bad = 0;
		for (int id : ids)
			for (unsigned i = 0, n = GetNumLvlClassInstances(2, id); i < n; i++) {
				char* e = (char*)GetLvlObject(2, id, i);
				if (!e) continue;
				char* c = *(char**)(e + 0x54);
				int k = 0;
				while (k < s_n && s_obj[k] != e) k++;
				if (k == s_n) {
					if (s_n == 1024) continue;
					s_obj[s_n] = e;
					s_last[s_n++] = nullptr;
					Log("autotest: cpnt Roman %p class %d #%u: +54 %p", e, id, i, c);
				}
				char*& last = s_last[k];
				if (c == last) continue;
				uintptr_t vt = c && !IsBadReadPtr(c, 4) ? *(uintptr_t*)c : 0;
				bool ok = !c || (vt >= 0x627B10 && vt <= 0x627E20);
				if (!ok && s_bad < 40) {
					s_bad++;
					Log("autotest: CPNT Roman %p (class %d, vt %08X) +54 %p -> %p (vt %08X) -- not a component", e, id,
					    *(uint32_t*)e, last, c, (unsigned)vt);
				} else if (ok && s_bad < 40 && c && last) {
					Log("autotest: cpnt Roman %p +54 %p -> %p", e, last, c);
				}
				last = c;
			}
		// cutscene pathfinding blocks (CKPathFindingCinematicBloc, class 8,4): +0x98 path slot
		// (0..19, -1 none), +0x2C flags (0x40 running), +0x14 state, +0x48 the character
		static uint32_t s_bloc[256];
		static int s_blocLogs = 0;
		for (unsigned i = 0, n = GetNumLvlClassInstances(8, 4); i < n && i < 256; i++) {
			char* b = (char*)GetLvlObject(8, 4, i);
			if (!b) continue;
			uint32_t st = *(uint8_t*)(b + 0x98) | (uint32_t)*(uint8_t*)(b + 0x2C) << 8 | (*(uint32_t*)(b + 0x14) & 0xFF) << 16;
			if (st == s_bloc[i] || s_blocLogs >= 200) continue;
			s_bloc[i] = st;
			s_blocLogs++;
			Log("autotest: path block #%u %p: slot %d flags %02X state %02X, character %p%s", i, b, (int)*(int8_t*)(b + 0x98),
			    *(uint8_t*)(b + 0x2C), *(uint32_t*)(b + 0x14) & 0xFF, *(void**)(b + 0x48),
			    *(int8_t*)(b + 0x98) == -1 && (*(uint8_t*)(b + 0x2C) & 0x40) ? " -- RUNNING WITHOUT A SLOT" : "");
		}
	}
	if (at.watchEnemies) {   // live Romans near the point; squads near it (flags, member groups)
		int live = 0, nearA = 0, nearO = 0;   // within 7 units of each hero: fighting / chasing it
		Trio wt = GetTrio();
		Hero wa = wt ? wt.GetHero(HERO_ASTERIX) : Hero(), wo = wt ? wt.GetHero(HERO_OBELIX) : Hero();
		const int ids[2] = { 93, 148 };
		static char* seen[256];
		static int nSeen = 0;
		char* now[256];
		int nNow = 0;
		for (int id : ids)
			for (unsigned i = 0, n = GetNumLvlClassInstances(2, id); i < n; i++) {
				char* e = (char*)GetLvlObject(2, id, i);
				char* node = e ? *(char**)(e + 0x10) : nullptr;
				char* fr = node ? *(char**)(node + 4) : nullptr;
				if (!fr || (*(uint8_t*)(node + 0x10) & 2)) continue;
				Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
				if ((p - at.enemyPoint).len2() >= 30.0f * 30.0f) continue;
				live++;
				if (wa && (p - wa.Pos()).len2() < 7.0f * 7.0f) nearA++;
				if (wo && (p - wo.Pos()).len2() < 7.0f * 7.0f) nearO++;
				if (nNow < 256) now[nNow++] = e;
				bool old = false;
				for (int k = 0; k < nSeen; k++) old = old || seen[k] == e;
				if (!old && at.enemiesPrimed) Log("autotest: Roman %p appeared at (%.1f %.1f %.1f)", e, p.x, p.y, p.z);
				if (live <= 6) line.Add(" r(%.0f %.1f %.0f)", p.x, p.y, p.z);
			}
		memcpy(seen, now, nNow * sizeof(char*));
		nSeen = nNow;
		bool primed = at.enemiesPrimed;
		at.enemiesPrimed = true;
		line.Add(" | romans near %d (by A %d, by O %d)", live, nearA, nearO);
		static int s_classTick = 0;
		if (!primed || ++s_classTick % 50 == 0)
			for (int id : ids) {
				unsigned n = GetNumLvlClassInstances(2, id), shown = 0;
				for (unsigned i = 0; i < n; i++) {
					char* e = (char*)GetLvlObject(2, id, i);
					char* node = e ? *(char**)(e + 0x10) : nullptr;
					if (node && !(*(uint8_t*)(node + 0x10) & 2)) shown++;
				}
				Log("autotest: class (2,%d): %u in the level, %u shown", id, n, shown);
			}
		if (!primed || s_classTick % 50 == 0)
			for (unsigned i = 0, n = GetNumLvlClassInstances(4, 44); i < n; i++) {
				char* pool = (char*)GetLvlObject(4, 44, i);
				if (!pool) continue;
				int nFree = 0, nHeld = 0;
				uintptr_t vt = 0;
				char* node = nullptr;
				__try {
					typedef char** (__fastcall* ListFn)(char* self, void* edx);
					char** head = ((ListFn)(*(void***)pool)[0x30 / 4])(pool, nullptr);
					for (char* e = head ? *head : nullptr; e && nFree < 500; e = *(char**)(e + 4)) {
						if (!nFree) { vt = *(uintptr_t*)e; node = *(char**)(e + 0x10); }
						nFree++;
					}
					for (char* e = *(char**)(pool + 0xC); e && nHeld < 500; e = *(char**)(e + 4)) nHeld++;
				} __except (EXCEPTION_EXECUTE_HANDLER) { nFree = -1; }
				Log("autotest: Roman pool %u %p: %d free, %d in its +0C list; first free vt %X node %p; pool +04 %p "
				    "+10 %X +14 %p %p %p +24 %X +28 %p", i, pool, nFree, nHeld, (unsigned)vt, node, *(void**)(pool + 4),
				    *(unsigned*)(pool + 0x10), *(void**)(pool + 0x14), *(void**)(pool + 0x18), *(void**)(pool + 0x1C),
				    *(unsigned*)(pool + 0x24), *(void**)(pool + 0x28));
				// the squads' pool records drawing on it, with Romans alive / queued
				char usersBuf[700];
				LineBuf users(usersBuf);
				const int squadClasses[2] = { 26, 64 };
				for (int c : squadClasses)
					for (unsigned k = 0, nk = GetNumLvlClassInstances(4, c); k < nk; k++) {
						char* sq = (char*)GetLvlObject(4, c, k);
						char* gr = sq ? *(char**)(sq + 0x144) : nullptr;
						unsigned ng = sq ? *(unsigned*)(sq + 0x14C) : 0;
						for (unsigned g2 = 0; gr && g2 < ng && g2 < 8; g2++) {
							char* e = gr + g2 * 0x1C;
							if (*(char**)e != pool) continue;
							int members = 0;   // taken from the pool: the record's list (+8, chained +0x50)
							for (char* m = *(char**)(e + 8); m && members < 500; m = *(char**)(m + 0x50)) members++;
							if (!members && !*(uint8_t*)(e + 0x14)) continue;
							users.Add(" sq%u/%u %p.%u members %d alive %u queued %u", c, k, sq, g2, members,
							          *(uint8_t*)(e + 0x11), *(uint8_t*)(e + 0x14));
						}
					}
				Log("autotest:   pool %u used by%s", i, users.c_str());
			}
		static int s_sqTick = 0;
		bool sqDetail = (s_sqTick++ % 20) == 0;
		for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
			char* sq = (char*)GetLvlObject(4, 26, i);
			if (!sq) continue;
			const float* f = (const float*)sq;
			Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
			if ((c - at.enemyPoint).len2() > 60.0f * 60.0f) continue;
			line.Add(" sq%u %p f%04X", i, sq, *(uint16_t*)(sq + 0x152));
			if (!primed) {
				Log("autotest: squad %u %p box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f", i, sq, f[0x4A], f[0x47], f[0x4B],
				    f[0x48], f[0x4C], f[0x49]);
				Log("autotest: squad %u fight zone x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f", i, f[0x44], f[0x41], f[0x45],
				    f[0x42], f[0x46], f[0x43]);
			}
			unsigned ng = *(unsigned*)(sq + 0x14C);
			char* gr = *(char**)(sq + 0x144);
			for (unsigned g2 = 0; gr && g2 < ng && g2 < 4; g2++) {
				char* e = gr + g2 * 0x1C;
				line.Add(" [%u %u %u %u %d]", *(uint8_t*)(e + 0x11), *(uint8_t*)(e + 0x14), *(uint8_t*)(e + 0x15),
				         *(uint16_t*)(e + 0x16), *(int16_t*)(e + 0x18));
			}
			// every 2 s: target mode (+0x152 bits 2-4; 5 = the trio's leader), formation centre
			// (node +0x30), the heroes in the fight zone (max +0x104, min +0x110), each member
			// Roman (pool records' lists) with its slot (component 0x46 +0) and distance to each hero
			if (!sqDetail) continue;
			char detailBuf[1500];
			LineBuf d(detailBuf);
			char* centre = *(char**)(sq + 0x30);
			auto inZone = [&](Hero h) {
				if (!h) return false;
				Vector3 p = h.Pos();
				return p.x >= f[0x44] && p.x <= f[0x41] && p.y >= f[0x45] && p.y <= f[0x42] && p.z >= f[0x46] && p.z <= f[0x43];
			};
			d.Add("autotest: sq%u f%04X target %u centre (%.1f %.1f %.1f) zone A%d O%d:", i, *(uint16_t*)(sq + 0x152),
			      (*(uint8_t*)(sq + 0x152) >> 2) & 7, centre ? *(float*)(centre + 0x30) : 0.0f,
			      centre ? *(float*)(centre + 0x34) : 0.0f, centre ? *(float*)(centre + 0x38) : 0.0f, inZone(wa), inZone(wo));
			for (unsigned g2 = 0; gr && g2 < ng && g2 < 8; g2++)
				for (char* m = *(char**)(gr + g2 * 0x1C + 8); m; m = *(char**)(m + 0x50)) {
					typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
					char* slot = ((CpntFn)(*(void***)m)[12])(m, nullptr, 0x46);
					Vector3 rp;
					char* node = *(char**)(m + 0x10);
					char* fr = node ? *(char**)(node + 4) : nullptr;
					if (fr) rp = Vector3(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
					d.Add(" %u:(%.0f %.0f)A%.0fO%.0f", g2, rp.x, rp.z, wa ? sqrtf((rp - wa.Pos()).len2()) : -1.0f,
					      wo ? sqrtf((rp - wo.Pos()).len2()) : -1.0f);
					if (slot) d.Add("s(%.0f %.0f)", *(float*)slot, *(float*)(slot + 8));
				}
			Log("%s", d.c_str());
		}
	}
	if (at.watchBeacons) {   // bonuses the pools placed on beacons: near the watched point / all
		int nearCount = 0, all = 0;
		const int kPoolIds[2] = { 61, 63 };
		for (int k = 0; k < 2; k++)
			for (unsigned i = 0, n = GetNumLvlClassInstances(4, kPoolIds[k]); i < n; i++) {
				char* pool = (char*)GetLvlObject(4, kPoolIds[k], i);
				for (char* b = pool ? *(char**)(pool + 0x34) : nullptr; b; b = *(char**)(b + 0x14)) {
					char* node = *(char**)(b + 0x10);
					char* frame = node ? *(char**)(node + 4) : nullptr;
					if (!frame) continue;
					all++;
					Vector3 p(((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1], ((float*)(frame + 0x80))[2]);
					if ((p - at.beaconPoint).len2() < 12.0f * 12.0f) nearCount++;
				}
			}
		line.Add(" | beacon bonuses near %d of %d", nearCount, all);
	}
	if (at.watchBonus) {
		char* node = *(char**)(at.watchBonus + 0x10);
		char* life = *(char**)(at.watchBonus + 0x0C);
		typedef char(__fastcall* BoolFn)(void* self, void* edx);
		char following = ((BoolFn)(*(void***)at.watchBonus)[19])(at.watchBonus, nullptr);
		line.Add(" | bonus hidden=%d following=%d target=%p", node ? (*(uint8_t*)(node + 0x10) & 2) != 0 : -1,
			following, *(void**)(at.watchBonus + 0x20));
		(void)life;
	}
	if (at.watchCam) {
		CKYellowPages* yp = YellowPages();
		char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
		if (cs) {
			const float* cp = (const float*)(cs + 0x5B0);
			const float* cl = (const float*)(cs + 0x5BC);
			char* tr = *(char**)(cs + 0x38);
			float dx = cp[0] - cl[0], dz = cp[2] - cl[2];
			line.Add(" | gamecam (%.1f %.1f %.1f) look (%.1f %.1f %.1f) yaw %.2f dist %.1f track vt %08X", cp[0], cp[1], cp[2],
			         cl[0], cl[1], cl[2], atan2f(dz, dx), sqrtf(dx * dx + dz * dz), tr ? (unsigned)*(uintptr_t*)tr : 0);
			float cm[16], cw[2];
			if (GameCamView(CoopNonLeaderPlayer(), cm, cw))
				line.Add(" | copycam (%.2f %.2f %.2f) at (%.2f %.2f %.2f)", cm[12], cm[13], cm[14], cm[8], cm[9], cm[10]);
		}
	}
	if (at.watchTorch) {
		Trio t = GetTrio();
		Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero();
		char* node = *(char**)(at.watchTorch + 0x10);
		char* fr = node ? *(char**)(node + 4) : nullptr;
		line.Add(" | torch +3E=%02X at (%.1f %.1f %.1f) A+464=%p A+3EC=%08X trio+4C=%p", *(uint8_t*)(at.watchTorch + 0x3E),
		         fr ? ((float*)(fr + 0x80))[0] : 0, fr ? ((float*)(fr + 0x80))[1] : 0, fr ? ((float*)(fr + 0x80))[2] : 0,
		         a ? *(void**)(a.raw() + 0x464) : nullptr, a ? *(uint32_t*)(a.raw() + 0x3EC) : 0,
		         t ? *(void**)((char*)t.raw() + 0x4C) : nullptr);
	}
	if (at.watchClue) {
		line.Add(" | spies");
		{   // CKHkClueMan::vf15's gates: [[gm+0x18]+0x24]+0x64 -> +0x10 bit 1, [gm+0x40]+0x50
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			char* a = gm ? *(char**)(gm + 0x18) : nullptr;
			char* b = a ? *(char**)(a + 0x24) : nullptr;
			char* c = b ? *(char**)(b + 0x64) : nullptr;
			char* d = gm ? *(char**)(gm + 0x40) : nullptr;
			line.Add(" gate=%d/%p clue=%d", c ? (*(uint8_t*)(c + 0x10) >> 1) & 1 : -1, c, d ? *(uint8_t*)(d + 0x50) : -1);
		}
		unsigned n = GetNumLvlClassInstances(2, 161);
		for (unsigned i = 0; i < n && i < 6; i++) {
			char* c = (char*)GetLvlObject(2, 161, i);
			if (c && i < 3 && *(uint8_t*)(c + 0x14) & 0x80) continue;   // (the far, done ones: room for the rest)
			char* node = c ? *(char**)(c + 0x10) : nullptr;
			char* frame = node ? *(char**)(node + 4) : nullptr;
			const float* p = frame ? (const float*)(frame + 0x80) : nullptr;
			if (!c) continue;
			line.Add(" %u:%s%s f14=%02X f15=%02X f16=%02X", i, node ? "node" : "-", node && (*(uint8_t*)(node + 0x10) & 2) ? "(hidden)" : "",
			         *(uint8_t*)(c + 0x14), *(uint8_t*)(c + 0x15), *(uint8_t*)(c + 0x16));
			if (p) line.Add("@(%.0f %.0f %.0f)", p[0], p[1], p[2]);
		}
	}
	if (at.watchEnemy) {
		char* node = *(char**)(at.watchEnemy + 0x10);
		char* frame = node ? *(char**)(node + 4) : nullptr;
		if (frame)
			line.Add(" | roman (%.2f %.2f %.2f)", ((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1],
				((float*)(frame + 0x80))[2]);
	}
	Log("%s", line.c_str());
}

// Whom a camera service state follows (its target +0xE8: a hero's +0x10).
const char* CamFollows(Trio t, const char* svc) {
	void* tg = *(void* const*)(svc + 0xE8);
	for (int k = 0; k < 2; k++) {
		Hero x = t.GetHero(k);
		if (x && *(void**)(x.raw() + 0x10) == tg) return HeroName(k);
	}
	return "something else";
}

// S_STEERCHECK: hero `hero`'s walk since its place was noted (report 0),
// against its player's view -- its camera copy, else the game's camera.
float g_steerFrom[2][3];
void SteerCheck(int hero, int report) {
	Trio t = GetTrio();
	Hero h = t && hero >= 0 && hero < 2 ? t.GetHero(hero) : Hero();
	if (!h) return;
	Vector3 p = h.Pos();
	float* from = g_steerFrom[hero];
	if (!report) { from[0] = p.x, from[1] = p.y, from[2] = p.z; return; }
	float m[16];
	bool own = hero == CoopP1Hero() ? P1CamMatrix(m) : P2CamMatrix(m);
	if (!own) {
		CKYellowPages* yp = YellowPages();
		char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
		char* cam = cs ? *(char**)(cs + 0x1C) : nullptr;
		char* frame = cam ? *(char**)(cam + 4) : nullptr;
		if (!frame) return;
		memcpy(m, frame + 0x10, sizeof(m));
	}
	float dx = p.x - from[0], dz = p.z - from[2];
	float len = sqrtf(dx * dx + dz * dz);
	float fl = sqrtf(m[8] * m[8] + m[10] * m[10]), rl = sqrtf(m[0] * m[0] + m[2] * m[2]);
	if (len < 0.1f || fl < 1e-3f || rl < 1e-3f) {
		Log("autotest: %s walked %.2f units (too little to tell)", HeroName(hero), len);
		return;
	}
	float along = (dx * m[8] + dz * m[10]) / (len * fl), side = (dx * m[0] + dz * m[2]) / (len * rl);
	Log("autotest: %s walked %.1f units: %.2f along %s forward, %.2f along its right", HeroName(hero), len, along,
	    own ? "its own camera's" : "the game's camera's", side);
}

void RunStep(const Step& s) {
	switch (s.kind) {
	case S_MARK: {
		Log("autotest: --- %s ---", s.name);
		CoopLogState(s.name);
		struct { int grp, id; const char* name; } kinds[] = {
			{ 2, 47, "CKHkLifter" }, { 2, 44, "CKHkCorkscrew" }, { 2, 190, "CKHkTrioCatapult" },
			{ 2, 39, "CKHkBoat" }, { 2, 147, "CKHkPushPullAsterix" }, { 2, 193, "checkpoint" },
		};
		char buf[256];
		LineBuf line(buf);
		line.Add("autotest: level objects:");
		for (auto& k : kinds) line.Add(" %s=%u", k.name, GetNumLvlClassInstances(k.grp, k.id));
		Log("%s", line.c_str());
		// Render objects: CKGraphic = yellowPages+0x08, view = CKGraphic+4,
		// renderer = view+8. Vtables are matched to classes offline (vtables.tsv).
		__try {
			char* yp = (char*)YellowPages();
			char* gfx = yp ? *(char**)(yp + 0x08) : nullptr;
			char* view = gfx ? *(char**)(gfx + 0x04) : nullptr;
			if (view) {
				char rb[512];
				LineBuf r(rb);
				r.Add("autotest: render gfx=%p vt=%08X view=%p renderer=%p builder(view+0C)=%p vt=%08X",
					gfx, *(unsigned*)gfx, view, *(void**)(view + 8), *(void**)(view + 0xC),
					*(void**)(view + 0xC) ? **(unsigned**)(view + 0xC) : 0);
				Log("%s", r.c_str());
				char lb[512];
				LineBuf l(lb);
				l.Add("autotest: render lists vt:");
				for (int i = 0; i < 15; i++) {
					void* o = *(void**)(view + 0x1C + 4 * i);
					l.Add(" %s%08X", i == 11 ? "fx:" : i == 12 ? "hud:" : "", o ? *(unsigned*)o : 0);
				}
				Log("%s", l.c_str());
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Log("autotest: render probe faulted");
		}
		for (unsigned i = 0; i < GetNumLvlClassInstances(2, 47) && i < 8; i++) {
			KHook* lifter = (KHook*)GetLvlObject(2, 47, i);
			if (!lifter || !lifter->node || !lifter->node->matrix) continue;
			float* m = (float*)((char*)lifter->node->matrix + 0x40);   // RW matrix position
			Log("autotest:   lifter %u at (%.2f %.2f %.2f)", i, m[0], m[1], m[2]);
		}
		break;
	}
	case S_SHOT:
		Shot(s.name);
		break;
	case S_FORCE: {
		Log("autotest: %s %s", s.name, HeroName(s.a));
		if (s.b) {
			at.forceHero = s.a;
		} else {
			at.forceHero = -1;
			Trio t = GetTrio();
			Hero h = t ? t.GetHero(s.a) : Hero();
			if (h && t.Leader() != s.a && HeroModePlayable(h.Flags18()))
				*(uint8_t*)(h.raw() + 0x18) = HeroAiVariant(h.Flags18());
		}
		break;
	}
	case S_P1:
		Log("autotest: %s", s.name);
		at.p1Active = true;
		at.p1.connected = true;
		at.p1.buttons = s.buttons;
		at.p1.lx = s.x;
		at.p1.ly = s.y;
		CoopLogState(s.name);
		break;
	case S_P1R:
		Log("autotest: %s", s.name);
		at.p1Active = true;
		at.p1.connected = true;
		at.p1.rx = s.x;
		at.p1.ry = s.y;
		break;
	case S_KEY: {
		Log("autotest: %s", s.name);
		UINT sc = MapVirtualKeyA((UINT)s.a, 0 /* MAPVK_VK_TO_VSC */);
		bool ext = (s.a >= 0x21 && s.a <= 0x2E) || s.a == 0x6F;   // nav cluster, numpad /
		keybd_event((BYTE)s.a, (BYTE)sc, (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (s.b ? 0 : KEYEVENTF_KEYUP), 0);
		CoopLogState(s.name);
		break;
	}
	case S_SECTORS: {
		CKYellowPages* yp = YellowPages();
		char* mgr = yp ? *(char**)((char*)yp + 0x90) : nullptr;
		if (!mgr) { Log("autotest: no sector manager"); break; }
		char** secs = *(char***)(mgr + 4);
		Log("autotest: sector manager %p: +0x1C %08X +0x20 %08X current(+0x24) %d +0x28 %08X +0x2C %08X +0x30 %08X",
		    mgr, *(uint32_t*)(mgr + 0x1C), *(uint32_t*)(mgr + 0x20), *(int*)(mgr + 0x24), *(uint32_t*)(mgr + 0x28),
		    *(uint32_t*)(mgr + 0x2C), *(uint32_t*)(mgr + 0x30));
		Trio t = GetTrio();
		for (int i = 0; secs && i < 64; i++) {
			char* sec = secs[i];
			if (!sec || IsBadReadPtr(sec, 0x40)) break;
			const float* b = (const float*)(sec + 0x18);
			char who[32] = "";
			for (int h = 0; h < HERO_COUNT && t; h++) {
				Hero hero = t.GetHero(h);
				if (!hero) continue;
				Vector3 p = hero.Pos();
				if (p.x <= b[0] && p.x > b[3] && p.y <= b[1] && p.y > b[4] && p.z <= b[2] && p.z > b[5]) {
					strcat(who, " ");
					strcat(who, HeroName(h));
				}
			}
			Log("autotest:   sector %d %p idx=%d flags=%04X vt=%p box max(%.0f %.0f %.0f) min(%.0f %.0f %.0f)%s", i, sec,
			    *(int16_t*)(sec + 4), *(uint16_t*)(sec + 6), *(void**)sec, b[0], b[1], b[2], b[3], b[4], b[5], who);
		}
		// Sas (CKSas, 12,17), listed per sector (+0x10, count +0x14): two boxes
		// each; the leader inside the first streams in sector +4, inside the second
		// sector +8. Only the current sector's list (and a loading one's) is checked
		// (CKLevel::vf17 -> FUN_00410DA0 with ECX = the sector).
		for (int i = 0; secs && i < 64; i++) {
			char* sec = secs[i];
			if (!sec || IsBadReadPtr(sec, 0x40) || *(void**)sec != *(void**)secs[0]) break;
			char** sas = *(char***)(sec + 0x10);
			unsigned ns = *(unsigned*)(sec + 0x14);
			for (unsigned k = 0; sas && k < ns && k < 64; k++) {
				char* a = sas[k];
				const float* b = (const float*)(a + 0xC);
				Log("autotest:   sector %d sas %u: -> %d in max(%.0f %.0f %.0f) min(%.0f %.0f %.0f), -> %d in max(%.0f %.0f %.0f) min(%.0f %.0f %.0f)",
				    i, k, *(int*)(a + 4), b[0], b[1], b[2], b[3], b[4], b[5], *(int*)(a + 8), b[6], b[7], b[8], b[9], b[10], b[11]);
			}
		}
		break;
	}
	case S_LOADSECTOR: {
		CKYellowPages* yp = YellowPages();
		char* lvl = yp ? *(char**)((char*)yp + 0x90) : nullptr;
		if (!lvl) break;
		Log("autotest: loading sector %d (current %d)", s.a, *(int*)(lvl + 0x24));
		*(int*)(lvl + 0x34) = s.a;
		break;
	}
	case S_CINEDUMP: {   // cinematic scene a (-1: the one playing): its nodes, edges and events
		CKYellowPages* yp = YellowPages();
		char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
		char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
		unsigned ns = cin ? *(unsigned*)(cin + 0x14) : 0;
		char* sc = nullptr;
		int si = -1;
		for (unsigned i = 0; scenes && i < ns; i++)
			if ((s.a < 0 && (*(uint16_t*)(scenes[i] + 0x20) & 1) && (*(uint16_t*)(scenes[i] + 0x20) & 0x180) != 0x80) ||
			    (int)i == s.a) { sc = scenes[i]; si = (int)i; break; }
		if (!sc) { Log("autotest: no cinematic scene %d (of %u)", s.a, ns); break; }
		char** edges = *(char***)(sc + 4);
		unsigned ne = *(unsigned*)(sc + 8);
		Log("autotest: scene %d %p flags %04X +30 %02X running %p start door %p end event %04X, %u edges", si, sc,
		    *(uint16_t*)(sc + 0x20), *(uint8_t*)(sc + 0x30), *(void**)(sc + 0xC), *(void**)(sc + 0x28), *(uint16_t*)(sc + 0x64), ne);
		struct { unsigned id; const char* name; } kinds[] = {
			{ 1, "And" }, { 2, "Or" }, { 11, "RandDoor" }, { 17, "StartDoor" }, { 9, "AttachObjects" }, { 22, "DisplayPicture" },
			{ 5, "FlaggedPath" }, { 7, "GroupBloc" }, { 27, "Lightning" }, { 23, "ManageCamera" }, { 15, "ManageEvent" },
			{ 16, "ManagerEventStop" }, { 12, "Particle" }, { 4, "PathFinding" }, { 32, "Class32" }, { 3, "PlayAnim" },
			{ 28, "PlaySound" }, { 20, "Sekensor" }, { 25, "StartEvent" }, { 13, "StreamAlone" }, { 10, "Stream" },
			{ 14, "StreamGroupBloc" }, { 26, "Sky" } };
		CKYellowPages* y2 = YellowPages();
		char* ev = y2 ? *(char**)((char*)y2 + 0x6C) : nullptr;
		auto targets = [&](unsigned id, char* out, size_t cap) {
			out[0] = 0;
			unsigned g = (id & 0xFFFF) >> 3;
			if (!ev || g == 0x1FFF) return;
			unsigned n = (unsigned)*(uint16_t*)(ev + 0x1C) + *(uint16_t*)(ev + 0x1E) + *(uint16_t*)(ev + 0x20);
			if (g >= n) return;
			uint8_t* tab = *(uint8_t**)(ev + 0xC);
			char** objs = *(char***)(ev + 0x10);
			uint16_t* msgs = *(uint16_t**)(ev + 0x14);
			unsigned first = 0;
			for (unsigned k = 0; k < g; k++) first += tab[k * 3];
			for (unsigned k = 0; k < tab[g * 3] && strlen(out) + 24 < cap; k++) {
				char* o = objs[first + k];
				sprintf(out + strlen(out), " %06X<-%04X", o ? (unsigned)*(uintptr_t*)o & 0xFFFFFF : 0, msgs[first + k]);
			}
		};
		for (auto& k : kinds) {
			for (unsigned i = 0, n = GetNumLvlClassInstances(8, k.id); i < n && i < 4096; i++) {
				char* nd = (char*)GetLvlObject(8, k.id, i);
				bool door = k.id == 1 || k.id == 2 || k.id == 11 || k.id == 17;
				if (!nd || *(char**)(nd + 0x1C) != sc) continue;
				uint16_t st = *(uint16_t*)(nd + 8), fi = *(uint16_t*)(nd + 0xA), cnt = *(uint16_t*)(nd + 0xC);
				char out[512] = "", tg[256] = "";
				// start-edge targets (started when this node finishes), then finish-edge ones (stopped)
				unsigned a0 = st, n1 = st == 0xFFFF ? 0 : fi == 0xFFFF ? cnt : (unsigned)(fi - st);
				unsigned b0 = fi, n2 = fi == 0xFFFF ? 0 : st == 0xFFFF ? cnt : (unsigned)(cnt - (fi - st));
				for (unsigned e = 0; e < n1 && a0 + e < ne && strlen(out) < 400; e++) sprintf(out + strlen(out), " +%p", edges[a0 + e]);
				for (unsigned e = 0; e < n2 && b0 + e < ne && strlen(out) < 400; e++) sprintf(out + strlen(out), " x%p", edges[b0 + e]);
				if (k.id == 25) targets(*(uint16_t*)(nd + 0x2C), tg, sizeof(tg));
				Log("autotest:   %-16s %p flags %08X grp %p%s%s%s%s", k.name, nd, *(uint32_t*)(nd + 0x14), *(void**)(nd + 0x18),
				    door ? "" : "", out, k.id == 25 ? " | event" : "", tg);
				if (door) Log("autotest:     door: inputs %d of %d", *(int*)(nd + 0x20), *(int*)(nd + 0x24));
				else if (k.id == 25) Log("autotest:     event %04X, waits for sekens %d line %d", *(uint16_t*)(nd + 0x2C), *(int*)(nd + 0x20), *(int*)(nd + 0x24));
				else if (*(int*)(nd + 0x20) != -1) Log("autotest:     waits for sekens %d line %d", *(int*)(nd + 0x20), *(int*)(nd + 0x24));
			}
		}
		break;
	}
	case S_PULLLEVER: {   // lever a (CKHkActivator 2,52) worked as a punch on it does (FUN_0053B610, thiscall)
		char* l = s.a >= 0 ? (char*)GetLvlObject(2, 52, (unsigned)s.a) : nullptr;
		float bd = 1e30f;
		for (unsigned i = 0, n = s.a < 0 ? GetNumLvlClassInstances(2, 52) : 0; i < n; i++) {   // a < 0: the nearest (x, y, z)
			char* o = (char*)GetLvlObject(2, 52, i);
			char* node = o ? *(char**)(o + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!fr) continue;
			Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
			float d = (p - Vector3(s.x, s.y, s.z)).len2();
			if (d < bd) { bd = d; l = o; }
		}
		if (!l) break;
		((void(__fastcall*)(char*, void*))0x53B610)(l, nullptr);
		Log("autotest: lever %d %p worked (events %04X/%04X)", s.a, l, *(uint16_t*)(l + 0x28), *(uint16_t*)(l + 0x2A));
		break;
	}
	case S_FLAGEV: {   // event a flagged in the event service (FUN_00460C20), as a lever or a zone does
		CKYellowPages* yp = YellowPages();
		char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		if (!ev) break;
		((void(__fastcall*)(void*, void*, unsigned))0x460C20)(ev, nullptr, (unsigned)s.a);
		Log("autotest: event %04X flagged", (unsigned)s.a);
		break;
	}
	case S_OBJEV: {   // event b sent to the object of class (2, a) nearest Asterix
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(HERO_ASTERIX) : Hero();
		if (!h) break;
		char* best = nullptr;
		float bd = 1e30f;
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, s.a); i < n; i++) {
			char* o = (char*)GetLvlObject(2, s.a, i);
			char* node = o ? *(char**)(o + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!fr) continue;
			Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
			float d = (p - h.Pos()).len2();
			if (d < bd) { bd = d; best = o; }
		}
		if (!best) { Log("autotest: no (2,%d) object", s.a); break; }
		typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
		int r = ((SendEventFn)(*(void***)best)[5])(best, nullptr, s.b, nullptr);
		Log("autotest: event %04X to (2,%d) %p, %.1f units away -> %d", s.b, s.a, best, sqrtf(bd), r);
		break;
	}
	case S_PFDUMP: {   // the pathfinding graph, with the cells within s.x units of hero a as a map
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) { PfDump(nullptr, 0.0f); break; }
		Vector3 p = h.Pos();
		PfDump(&p, s.x);
		break;
	}
	case S_CARPOS: {   // the rope cable car nearest hero a moved along its rope (+0x14 = x), as a pull does
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		char* best = nullptr;
		float bestD = 1e30f;
		for (unsigned i = 0, n = h ? GetNumLvlClassInstances(2, 159) : 0; i < n; i++) {
			char* c = (char*)GetLvlObject(2, 159, i);
			char* node = c ? *(char**)(c + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!fr) continue;
			const float* p = (const float*)(fr + 0x80);
			float d = (h.Pos() - Vector3(p[0], p[1], p[2])).len2();
			if (d < bestD) { bestD = d; best = c; }
		}
		if (best) {
			Log("autotest: cable car %p along its rope %.1f -> %.1f", best, *(float*)(best + 0x14), s.x);
			*(float*)(best + 0x14) = s.x;
		}
		break;
	}
	case S_UNLOCKHERO:   // a player's R3 + B on hero a (the mod's unlock)
		Log("autotest: R3 + B for %s", HeroName(s.a));
		CoopUnlockHero(s.a);
		break;
	case S_REACH: {   // can each hero walk to the other (the graph)
		Trio t = GetTrio();
		Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero(), o = t ? t.GetHero(HERO_OBELIX) : Hero();
		if (!a || !o) break;
		static const char* kName[] = { "unknown", "no", "yes" };
		Log("autotest: reach obelix->asterix %s, asterix->obelix %s (asterix %.1f %.1f %.1f, obelix %.1f %.1f %.1f)",
		    kName[PfReachable(o.Pos(), a.Pos()) + 1], kName[PfReachable(a.Pos(), o.Pos()) + 1], a.Pos().x, a.Pos().y,
		    a.Pos().z, o.Pos().x, o.Pos().y, o.Pos().z);
		break;
	}
	case S_ENDFIGHT: {   // the fights behind gates won: each squad a gate counts (+0x154 >= -1) put away (vf34), nothing
		int n = 0;       // left to send or replace, and its end as the game gives it (FUN_00530570: 0D16, +0x154 = -2)
		if (s.b == 3) {  // squad #a's fight won, wherever it is: its members taken off as beaten (FUN_00530300), every
		                 // pool beaten to its count (sent = count, at least 1), ended (+0x154 = -2; no 0D16: the level's
		                 // scripts left alone)
			char* sq = s.a >= 0 && (unsigned)s.a < GetNumLvlClassInstances(4, 26) ? (char*)GetLvlObject(4, 26, s.a) : nullptr;
			if (!sq) break;
			char* off[64];
			int nOff = 0;
			char** head = ((char**(__fastcall*)(char*, void*))(*(void***)sq)[12])(sq, nullptr);
			for (char* m = head ? *head : nullptr; m && nOff < 64; m = *(char**)(m + 4)) off[nOff++] = m;
			for (int k = 0; k < nOff; k++) ((void(__fastcall*)(char*, void*, char*))0x530300)(sq, nullptr, off[k]);
			char* recs = *(char**)(sq + 0x144);
			for (unsigned r = 0, nr = *(unsigned*)(sq + 0x14C); recs && r < nr && r < 16; r++) {
				char* rec = recs + r * 0x1C;
				rec[0x11] = 0;   // (alive: an asleep squad's starting Romans, placed as it wakes)
				rec[0x14] = 0;
				rec[0x15] = 0;
				int16_t count = *(int16_t*)(rec + 0x18);
				uint16_t sent = *(uint16_t*)(rec + 0x16);
				if (count <= 0) count = sent ? (int16_t)sent : 1;
				*(int16_t*)(rec + 0x18) = count;
				*(uint16_t*)(rec + 0x16) = (uint16_t)count;
			}
			*(uint8_t*)(sq + 0x154) = 0xFE;
			const float* f = (const float*)sq;
			Log("autotest: squad #%d %p's fight won: %d member(s) taken off, box x %.1f..%.1f z %.1f..%.1f, comes back after "
			    "%.1f s (asleep %.1f s), active %d", s.a, sq, nOff, f[0x4A], f[0x47], f[0x4C], f[0x49], f[0x4F], f[0x4E],
			    (*(uint8_t*)(sq + 0x152) >> 1) & 1);
			break;
		}
		for (unsigned i = 0, k = GetNumLvlClassInstances(4, 26); i < k; i++) {
			char* sq = (char*)GetLvlObject(4, 26, i);
			if (!sq || *(int8_t*)(sq + 0x154) < -1) continue;
			void** vt = *(void***)sq;
			if (s.b == 1 || s.b == 2) {   // members (vf12, chained +4) away from hero a (2: all) taken off as beaten ones are (FUN_00530300)
				Trio t = GetTrio();
				Hero keep = t && s.b == 1 ? t.GetHero(s.a) : Hero();
				char* off[64];
				int nOff = 0, kept = 0;
				char** head = ((char**(__fastcall*)(char*, void*))vt[12])(sq, nullptr);
				for (char* m = head ? *head : nullptr; m && nOff < 64; m = *(char**)(m + 4)) {
					char* node = *(char**)(m + 0x10);
					char* fr = node ? *(char**)(node + 4) : nullptr;
					Vector3 p = fr ? Vector3(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]) : Vector3();
					if (keep && fr && (p - keep.Pos()).len2() < 15.0f * 15.0f) kept++;
					else off[nOff++] = m;
				}
				for (int k = 0; k < nOff; k++) ((void(__fastcall*)(char*, void*, char*))0x530300)(sq, nullptr, off[k]);
				char* recs = *(char**)(sq + 0x144);
				for (unsigned r = 0, nr = *(unsigned*)(sq + 0x14C); recs && r < nr && r < 16; r++) {
					char* rec = recs + r * 0x1C;
					rec[0x14] = 0;
					rec[0x15] = 0;
					*(int16_t*)(rec + 0x18) = (int16_t)*(uint16_t*)(rec + 0x16);
				}
				Log("autotest: squad %p: %d Romans taken off, %d by %s kept; nothing left to send", sq, nOff, kept, HeroName(s.a));
				if (s.b == 2) {   // its end as the game gives it as its last Roman goes
					((void(__fastcall*)(char*, void*, int, int))vt[5])(sq, nullptr, 0xD16, 0);
					*(uint8_t*)(sq + 0x154) = 0xFE;
				}
				n++;
				continue;
			}
			((void(__fastcall*)(char*, void*))vt[34])(sq, nullptr);
			char* recs = *(char**)(sq + 0x144);
			for (unsigned r = 0, nr = *(unsigned*)(sq + 0x14C); recs && r < nr && r < 16; r++) {
				char* rec = recs + r * 0x1C;
				rec[0x14] = 0;
				rec[0x15] = 0;
				*(int16_t*)(rec + 0x18) = (int16_t)*(uint16_t*)(rec + 0x16);
			}
			((void(__fastcall*)(char*, void*, int, int))vt[5])(sq, nullptr, 0xD16, 0);
			*(uint8_t*)(sq + 0x154) = 0xFE;
			n++;
		}
		Log("autotest: %d fight squad(s) ended", n);
		break;
	}
	case S_SQUADRESET: {   // every active squad (4,26 / 4,64) put away by its vf34
		const int classes[2] = { 26, 64 };
		int n = 0;
		for (int c : classes)
			for (unsigned i = 0, k = GetNumLvlClassInstances(4, c); i < k; i++) {
				char* sq = (char*)GetLvlObject(4, c, i);
				if (!sq || !(*(uint8_t*)(sq + 0x152) & 2)) continue;
				typedef void(__fastcall* ResetFn)(char* self, void* edx);
				((ResetFn)(*(void***)sq)[34])(sq, nullptr);
				n++;
			}
		Log("autotest: %d active squad(s) put away", n);
		break;
	}
	case S_GUARDSPOTS: {   // the guards' spots round hero a, were it down where it stands (guard.cpp)
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		char buf[300];
		GuardSpots(h.Pos(), buf, sizeof(buf));
		Log("autotest: guard spots round %s at (%.1f %.1f %.1f): %s", HeroName(s.a), h.Pos().x, h.Pos().y, h.Pos().z, buf);
		break;
	}
	case S_POOLKINDS: {   // every squad's pools: the component (class, flags, health, shield points), a Roman of it (bones, textures), may it guard
		const int classes[2] = { 26, 64 };
		for (int c : classes)
			for (unsigned i = 0, k = GetNumLvlClassInstances(4, c); i < k; i++) {
				char* sq = (char*)GetLvlObject(4, c, i);
				char* recs = sq ? *(char**)(sq + 0x144) : nullptr;
				for (unsigned r = 0, nr = recs ? *(unsigned*)(sq + 0x14C) : 0; r < nr && r < 8; r++) {
					char* rec = recs + 0x1C * r;
					char* pool = *(char**)rec;
					char* cp = *(char**)(rec + 4);
					if (!cp || IsBadReadPtr(cp, 0x130)) continue;
					uintptr_t vt = *(uintptr_t*)cp;
					bool seiz = vt != 0x627B10 && vt != 0x627C00 && vt != 0x627C90 && vt != 0x627D20 && vt < 0x627D50;
					char* e = nullptr;   // a Roman: one out, else the pool's first free one
					if (*(char**)(rec + 8)) e = *(char**)(rec + 8);
					else if (pool) {
						__try {
							typedef char** (__fastcall* ListFn)(char* self, void* edx);
							char** head = ((ListFn)(*(void***)pool)[12])(pool, nullptr);
							e = head ? *head : nullptr;
						} __except (EXCEPTION_EXECUTE_HANDLER) { e = nullptr; }
					}
					char* node = e && !IsBadReadPtr(e, 0x60) ? *(char**)(e + 0x10) : nullptr;
					char* hier = node && !IsBadReadPtr(node, 0x60) ? *(char**)(node + 0x28) : nullptr;   // its skeleton
					char tex[160] = {};
					if (e) GuardModelTextures(e, tex, sizeof(tex));
					Log("autotest: squad (4,%d) #%u %p pool %u (%p): %d to defeat, cpnt %p %s flags %08X health %u damage %u"
					    " shield points %d stun %.2f cover %.2f | Roman %p vt %X, bones %d, textures%s | guards %s",
					    c, i, sq, r, pool, *(int16_t*)(rec + 0x18), cp, CpntClass(vt), *(uint32_t*)(cp + 4),
					    *(uint8_t*)(cp + 8), *(uint8_t*)(cp + 9), seiz ? *(uint8_t*)(cp + 0x110) : -1,
					    seiz ? *(float*)(cp + 0x120) : 0.0f, seiz ? *(float*)(cp + 0x114) : 0.0f, e, e ? *(unsigned*)e : 0,
					    hier && !IsBadReadPtr(hier, 8) ? *(uint8_t*)(hier + 4) : -1, tex, e && GuardEligible(rec, e) ? "yes" : "no");
				}
			}
		break;
	}
	case S_SQUADS: {   // squads (4,26 / 4,64) whose box is within s.x units of hero a
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		const Vector3& p = h.Pos();
		const int classes[2] = { 26, 64 };
		for (int c : classes)
			for (unsigned i = 0, k = GetNumLvlClassInstances(4, c); i < k; i++) {
				char* sq = (char*)GetLvlObject(4, c, i);
				if (!sq) continue;
				const float* f = (const float*)sq;   // box: max +0x11C, min +0x128
				float dx = p.x < f[0x4A] ? f[0x4A] - p.x : p.x > f[0x47] ? p.x - f[0x47] : 0.0f;
				float dz = p.z < f[0x4C] ? f[0x4C] - p.z : p.z > f[0x49] ? p.z - f[0x49] : 0.0f;
				if (dx * dx + dz * dz > s.x * s.x) continue;
				char buf[300];
				int len = _snprintf(buf, sizeof(buf), "autotest: squad %p box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f (%s from %s), active %d, gate %d:",
				                    sq, f[0x4A], f[0x47], f[0x4B], f[0x48], f[0x4C], f[0x49], dx || dz ? "away" : "in", HeroName(s.a),
				                    (*(uint8_t*)(sq + 0x152) >> 1) & 1, *(int8_t*)(sq + 0x154));
				char* recs = *(char**)(sq + 0x144);
				for (unsigned r = 0, nr = *(unsigned*)(sq + 0x14C); recs && r < nr && r < 4 && len > 0 && len < (int)sizeof(buf); r++)
					len += _snprintf(buf + len, sizeof(buf) - len, " [%u: %d to defeat, sent %u, alive %u, asked %u/%u]", r,
					                 *(int16_t*)(recs + 0x1C * r + 0x18), *(uint16_t*)(recs + 0x1C * r + 0x16),
					                 *(uint8_t*)(recs + 0x1C * r + 0x11), *(uint8_t*)(recs + 0x1C * r + 0x14),
					                 *(uint8_t*)(recs + 0x1C * r + 0x15));
				buf[sizeof(buf) - 1] = 0;
				Log("%s", buf);
			}
		break;
	}
	case S_DOORS: {   // swing doors (2,98) and slide doors (2,100) within s.x units of hero a
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		const Vector3& p = h.Pos();
		const int classes[2] = { 98, 100 };
		for (int c : classes)
			for (unsigned i = 0, k = GetNumLvlClassInstances(2, c); i < k; i++) {
				char* d = (char*)GetLvlObject(2, c, i);
				if (!d || IsBadReadPtr(d, 0x50)) continue;
				Vector3 at[2];
				bool ok = true;
				for (int n = 0; n < 2 && ok; n++) {   // swing: its node, leaves +0x2C / +0x30; slide: nodes +0x10 / +0x14
					char* node = *(char**)(d + (c == 98 ? 0x2C + 4 * n : 0x10 + 4 * n));
					char* fr = node && !IsBadReadPtr(node, 8) ? *(char**)(node + 4) : nullptr;
					if (!fr || IsBadReadPtr(fr, 0x8C)) { ok = false; break; }
					at[n] = Vector3(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
				}
				if (!ok) continue;
				float dx = at[0].x - p.x, dz = at[0].z - p.z;
				if (dx * dx + dz * dz > s.x * s.x) continue;
				if (c == 98)
					Log("autotest: swing door %p leaves at %.1f %.1f %.1f / %.1f %.1f %.1f: open %.2f, step %u/%u, leaf cos %.2f / %.2f",
					    d, at[0].x, at[0].y, at[0].z, at[1].x, at[1].y, at[1].z, *(float*)(d + 0x18), *(uint8_t*)(d + 0x2A),
					    *(uint8_t*)(d + 0x28), *(float*)(*(char**)(*(char**)(d + 0x2C) + 4) + 0x10),
					    *(float*)(*(char**)(*(char**)(d + 0x30) + 4) + 0x10));
				else
					Log("autotest: slide door %p at %.1f %.1f %.1f, moving part %.1f %.1f %.1f: open %.2f, step %u/%u (start %u), "
					    "flags %02X, ends %.1f %.1f %.1f / %.1f %.1f %.1f", d, at[0].x, at[0].y, at[0].z, at[1].x, at[1].y, at[1].z,
					    *(float*)(d + 0x18), *(uint8_t*)(d + 0x2A), *(uint8_t*)(d + 0x28), *(uint8_t*)(d + 0x29), *(uint8_t*)(d + 0x2B),
					    ((float*)(d + 0x2C))[0], ((float*)(d + 0x2C))[1], ((float*)(d + 0x2C))[2], ((float*)(d + 0x38))[0],
					    ((float*)(d + 0x38))[1], ((float*)(d + 0x38))[2]);
			}
		break;
	}
	case S_APARTSPOT: {   // hero a where hero b can't walk to nor come from (s.x..s.y units off)
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero(), f = t ? t.GetHero(s.b) : Hero();
		Vector3 at;
		if (!h || !f || !PfSpotCutOff(f.Pos(), s.x, s.y, &at)) {
			Log("autotest: no spot cut off from %s within %.0f..%.0f units", HeroName(s.b), s.x, s.y);
			break;
		}
		h.Pos() = at;
		if (h.hook->node && h.hook->node->matrix) {
			float* m = (float*)((char*)h.hook->node->matrix + 0x40);
			m[0] = at.x; m[1] = at.y; m[2] = at.z;
		}
		Log("autotest: %s put at %.1f %.1f %.1f, cut off from %s (%.1f %.1f %.1f)", HeroName(s.a), at.x, at.y, at.z,
		    HeroName(s.b), f.Pos().x, f.Pos().y, f.Pos().z);
		break;
	}
	case S_NEAROBJ: {   // hero a put s.x units from the object of class (2, b) nearest it (buttons: nearest hero buttons - 1), facing it
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		unsigned n = GetNumLvlClassInstances(2, s.b);
		char* best = nullptr;
		Vector3 bp;
		float bd = 1e30f;
		for (unsigned i = 0; i < n; i++) {
			char* o = (char*)GetLvlObject(2, s.b, i);
			char* node = o ? *(char**)(o + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!fr || (s.buttons && (*(uint8_t*)(node + 0x10) & 2))) continue;   // (in a pool: hidden)
			Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
			if (!s.buttons) Log("autotest:   class (2,%d) #%u %p at (%.1f %.1f %.1f)", s.b, i, o, p.x, p.y, p.z);
			Hero from = s.buttons ? t.GetHero(s.buttons - 1) : h;
			float d = (p - (from ? from.Pos() : h.Pos())).len2();
			if (d < bd) { bd = d; best = o; bp = p; }
		}
		if (!best) { Log("autotest: no (2,%d) object with a node", s.b); break; }
		Vector3 dir = h.Pos() - bp;
		dir.y = 0;
		float l = sqrtf(dir.len2());
		dir = l > 0.01f ? dir * (1.0f / l) : Vector3(1, 0, 0);
		float dist = s.x > 0 ? s.x : 1.5f;
		Vector3 to(bp.x + dir.x * dist, bp.y + 0.5f, bp.z + dir.z * dist);
		SetHeroPos(h, to);
		Vector3 face = dir * -1.0f;   // facing it, as NEAR_CRATE does
		h.Dir() = face;
		float yaw = atan2f(face.z, face.x);
		if (yaw < 0) yaw += 6.2831855f;
		*(float*)(h.raw() + 0xCC) = yaw;
		*(float*)(h.raw() + 0xD4) = yaw;
		Log("autotest: %s put %.1f from (2,%d) %p at (%.1f %.1f %.1f)", HeroName(s.a), dist, s.b, best, bp.x, bp.y, bp.z);
		break;
	}
	case S_INPUTDUMP: {   // CKInput: bindings (source +0xC, filter +8) and the named views' action tables
		InputManager im = GetInputManager();
		char* in = im.raw();
		if (!in) break;
		unsigned nb = im.NumActions();
		char* binds = *(char**)(in + 0x18);
		for (unsigned i = 0; binds && i < nb; i++) {
			char* b = binds + i * 0x20;
			char* src = *(char**)(b + 0xC);
			char* flt = *(char**)(b + 8);
			Log("autotest: binding %2u: flags %02X %02X %02X %02X analog %u src %p vt %p (raw %p) filter %p vt %p", i,
			    (uint8_t)b[4], (uint8_t)b[5], (uint8_t)b[6], (uint8_t)b[7], (uint8_t)b[0x10], src, src ? *(void**)src : nullptr,
			    src ? *(void**)(src + 0x14) : nullptr, flt, flt ? *(void**)flt : nullptr);
		}
		unsigned nv = *(uint8_t*)(in + 0x38);
		char** views = *(char***)(in + 0x40);
		for (unsigned v = 0; views && v < nv; v++) {
			char* view = views[v] ? views[v] + 0x10 : nullptr;
			uint8_t* remap = view ? *(uint8_t**)(view + 8) : nullptr;
			char line[256];
			int k = 0;
			for (int a = 0; remap && a < 24; a++) k += sprintf(line + k, " %u", remap[a]);
			Log("autotest: view %u (%p, name hash %08X):%s", v, view, *(uint32_t*)(*(char**)(in + 0x5C) + 4 * v),
			    remap ? line : " -");
		}
		Trio t = GetTrio();
		char* hv = t ? *(char**)(t.raw() + 0x28) : nullptr;
		uint8_t* hr = hv ? *(uint8_t**)(hv + 8) : nullptr;
		char line[256];
		int k = 0;
		for (int a = 0; hr && a < 24; a++) k += sprintf(line + k, " %u", hr[a]);
		Log("autotest: heroes' view (trio +0x28) %p:%s", hv, hr ? line : " -");
		// Combos (CKInput +0x34: +4 on, +5 count, +8 entries of 0x14: +4 on, +5 steps, +8 steps of
		// 0x10: +4 items, +5 forbidden, +8 items / +0xC forbidden of 0x10: +0 binding, +1 threshold,
		// +4 hold time; entry +0xC time allowed)
		char* cm = *(char**)(in + 0x34);
		unsigned nc = cm ? *(uint8_t*)(cm + 5) : 0;
		Log("autotest: combo manager %p vt %p on %u, %u combos", cm, cm ? *(void**)cm : nullptr, cm ? *(uint8_t*)(cm + 4) : 0, nc);
		for (unsigned c = 0; cm && c < nc; c++) {
			char* e = *(char**)(cm + 8) + c * 0x14;
			unsigned ns = *(uint8_t*)(e + 5);
			char buf[512];
			int k2 = sprintf(buf, "combo %u (value %u): on %u, time %.2f:", c, nb + c, *(uint8_t*)(e + 4), *(float*)(e + 0xC));
			for (unsigned s2 = 0; s2 < ns && k2 < 440; s2++) {
				char* st = *(char**)(e + 8) + s2 * 0x10;
				k2 += sprintf(buf + k2, " [");
				for (unsigned it = 0; it < *(uint8_t*)(st + 4) && k2 < 440; it++) {
					char* item = *(char**)(st + 8) + it * 0x10;
					k2 += sprintf(buf + k2, " %u>=%u", *(uint8_t*)item, *(uint8_t*)(item + 1));
					if (*(float*)(item + 4) != 0.0f) k2 += sprintf(buf + k2, " held %.2f", *(float*)(item + 4));
				}
				for (unsigned it = 0; it < *(uint8_t*)(st + 5) && k2 < 440; it++) {
					char* item = *(char**)(st + 0xC) + it * 0x10;
					k2 += sprintf(buf + k2, " !%u>%u", *(uint8_t*)item, *(uint8_t*)(item + 1));
				}
				k2 += sprintf(buf + k2, " ]");
			}
			Log("autotest: %s", buf);
		}
		break;
	}
	case S_P1COMBO:
		if (s.a < 0) { CombosCheat(); Log("autotest: combo cheat"); break; }
		at.p1Active = true;
		at.p1.connected = true;
		at.p1Combo = s.a;
		Log("autotest: player 1 combo %d", s.a);
		break;
	case S_WATCHGROUP2: {   // write watch on the flag bytes of event groups a and b
		CKYellowPages* yp = YellowPages();
		char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		if (!ev) break;
		uint8_t* tab = *(uint8_t**)(ev + 0xC);
		const void* w[2] = { tab + s.a * 3 + 2, tab + s.b * 3 + 2 };
		WatchBytes(w, 2, 0x46B180, 0x46B331);
		Log("autotest: watching groups %03X and %03X", s.a, s.b);
		break;
	}
	case S_FINDGROUPS: {   // event groups messaging an object of vtable a with message b; watch the first's flag byte
		CKYellowPages* yp = YellowPages();
		char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		if (!ev) break;
		unsigned n = (unsigned)*(uint16_t*)(ev + 0x1C) + *(uint16_t*)(ev + 0x1E) + *(uint16_t*)(ev + 0x20);
		uint8_t* tab = *(uint8_t**)(ev + 0xC);
		char** objs = *(char***)(ev + 0x10);
		uint16_t* msgs = *(uint16_t**)(ev + 0x14);
		const void* watch[4];
		int nw = 0;
		unsigned first = 0;
		for (unsigned gi = 0; gi < n; first += tab[gi * 3], gi++)
			for (unsigned k = 0; k < tab[gi * 3]; k++) {
				char* o = objs[first + k];
				if (!o || *(uintptr_t*)o != (uintptr_t)(unsigned)s.a || (s.b && msgs[first + k] != s.b)) continue;
				Log("autotest: group %03X messages %p (vt %08X) with %04X; flag byte %p", gi, o, s.a, msgs[first + k],
				    tab + gi * 3 + 2);
				if (nw < 4) watch[nw++] = tab + gi * 3 + 2;
			}
		if (nw && s.x == 0) WatchBytes(watch, nw, 0x46B180, 0x46B331);   // not CKSrvEvent::vf17 clearing them
		if (s.x == 1) {   // instead: hook the target class's sendEvent and log who gets the message
			static uintptr_t s_vt = 0;
			static unsigned s_msg = 0;
			typedef int(__fastcall* SendFn)(void*, void*, int, void*);
			static SendFn s_orig = nullptr;
			struct H { static int __fastcall Hook(void* self, void* edx, int ev, void* p) {
				if ((unsigned)(ev & 0xFFFF) == s_msg) {
					Log("autotest: %p (vt %08X) got %04X", self, (unsigned)s_vt, ev & 0xFFFF);
					LogBacktrace("autotest: message", (uintptr_t)_AddressOfReturnAddress());
				}
				return s_orig(self, edx, ev, p);
			} };
			if (!s_orig) {
				s_vt = (uintptr_t)(unsigned)s.a;
				s_msg = (unsigned)s.b;
				DWORD old;
				void** slot = (void**)s_vt + 5;
				VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old);
				s_orig = (SendFn)*slot;
				*slot = (void*)&H::Hook;
				VirtualProtect(slot, 4, old, &old);
			}
		}
		break;
	}
	case S_WATCHRW: {   // every reader/writer of hero a's byte at +b
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		if (s.x == 9) {   // writes to "sent" (+0x16) of the member groups of the squad with spawn markers holding the hero
			              // (a fight gate's first: +0x154 >= -1), with stacks
			char* pick = nullptr;
			for (int pass = 0; pass < 2 && !pick; pass++)
				for (unsigned i = 0, k = GetNumLvlClassInstances(4, 26); i < k && !pick; i++) {
					char* sq = (char*)GetLvlObject(4, 26, i);
					const float* f = (const float*)sq;   // box: max +0x11C, min +0x128
					const Vector3& p = h.Pos();
					if (!sq || !*(char**)(sq + 0x144) || !*(unsigned*)(sq + 0x64) || p.x < f[0x4A] || p.x > f[0x47] ||
					    p.z < f[0x4C] || p.z > f[0x49] || (pass == 0 && *(int8_t*)(sq + 0x154) < -1))
						continue;
					pick = sq;
				}
			if (pick) {
				char* recs = *(char**)(pick + 0x144);
				const void* w[4];
				int nw = 0;
				for (unsigned r = 0, nr = *(unsigned*)(pick + 0x14C); r < nr && nw < 4; r++) w[nw++] = recs + 0x1C * r + 0x16;
				WatchBytes(w, nw, 0, 0);
				Log("autotest: watching writes to 'sent' of squad %p's %d member groups", pick, nw);
			}
			break;
		}
		if (s.x == 8) {   // writes to the level's requested sector (yellow pages +0x90, +0x34), with stacks
			CKYellowPages* yp = YellowPages();
			char* lvl = yp ? *(char**)((char*)yp + 0x90) : nullptr;
			if (!lvl) break;
			const void* w[1] = { lvl + 0x34 };
			WatchBytes(w, 1, 0, 0);
			Log("autotest: watching writes to the requested sector (now %d)", *(int*)(lvl + 0x34));
			break;
		}
		if (s.x == 7) {   // the hero's vertical speed (+0xA8) rising: a jump's impulse, with stacks
			WatchFloatRise((const float*)(h.raw() + 0xA8));
			Log("autotest: watching %s's vertical speed rise", HeroName(s.a));
			break;
		}
		if (s.x == 6) {   // the hero's height rising (a jump starting), with stacks
			WatchFloatRise(&h.Pos().y);
			Log("autotest: watching %s's height rise", HeroName(s.a));
			break;
		}
		if (s.x == 2) {   // teleports of the hero (x jumps by more than 10)
			WatchFloatJump(&h.Pos().x);
			Log("autotest: watching %s's teleports", HeroName(s.a));
			break;
		}
		if (s.x == 3) {   // writes to the height of the Roman nearest the hero (node LTM y), with stacks
			char* best = nullptr;
			float bd = 1e9f;
			const int ids[2] = { 93, 148 };
			for (int id : ids)
				for (unsigned i = 0, n = GetNumLvlClassInstances(2, id); i < n; i++) {
					char* e = (char*)GetLvlObject(2, id, i);
					char* node = e ? *(char**)(e + 0x10) : nullptr;
					char* fr = node ? *(char**)(node + 4) : nullptr;
					if (!fr || (*(uint8_t*)(node + 0x10) & 2)) continue;
					Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
					float d = (p - h.Pos()).len2();
					if (d < bd) { bd = d; best = e; }
				}
			if (!best) break;
			char* fr = *(char**)(*(char**)(best + 0x10) + 4);
			const void* w[1] = { fr + 0x84 };
			WatchBytes(w, 1, 0, 0);
			Log("autotest: watching writes to Roman %p's height (%.1f), %.1f from %s", best, *(float*)(fr + 0x84), sqrtf(bd),
			    HeroName(s.a));
			break;
		}
		if (s.x == 4) {   // writes to byte +b of the archer (2,95) nearest the hero, with stacks
			char* best = nullptr;
			float bd = 1e9f;
			for (unsigned i = 0, n = GetNumLvlClassInstances(2, 95); i < n; i++) {
				char* e = (char*)GetLvlObject(2, 95, i);
				char* node = e ? *(char**)(e + 0x10) : nullptr;
				char* fr = node ? *(char**)(node + 4) : nullptr;
				if (!fr) continue;
				Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
				float d = (p - h.Pos()).len2();
				if (d < bd) { bd = d; best = e; }
			}
			if (!best) break;
			const void* w[1] = { best + s.b };
			WatchBytes(w, 1, 0, 0);
			Log("autotest: watching writes to archer %p +%X", best, s.b);
			break;
		}
		if (s.x == 5) {   // writes to the shared health (game manager +0x40 -> +0x50) and to hero a's +b, with stacks
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
			if (!cb) break;
			const void* w[2] = { cb + 0x50, h.raw() + s.b };
			WatchBytes(w, 2, 0, 0);
			Log("autotest: watching writes to the health and %s +%03X", HeroName(s.a), s.b);
			break;
		}
		if (s.x == 1) {   // writes only, with stacks
			const void* w[1] = { h.raw() + s.b };
			WatchBytes(w, 1, 0, 0);
			Log("autotest: watching writes to %s +%03X", HeroName(s.a), s.b);
			break;
		}
		WatchBytesRW(h.raw() + s.b);
		Log("autotest: watching every access to %s +%03X", HeroName(s.a), s.b);
		break;
	}
	case S_NEXTTO: {   // hero a put 1.5 units beside hero b
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero(), o = t ? t.GetHero(s.b) : Hero();
		if (!h || !o) break;
		Vector3 p = o.Pos();
		if (s.x != 0.0f) {   // x units in front of hero b (its facing)
			Vector3 d = o.Dir();
			p.x += d.x * s.x;
			p.z += d.z * s.x;
		} else {
			p.x += 1.5f;
		}
		p.y += 0.3f;
		h.Pos() = p;
		if (h.hook->node && h.hook->node->matrix) {
			float* m = (float*)((char*)h.hook->node->matrix + 0x40);
			m[0] = p.x; m[1] = p.y; m[2] = p.z;
		}
		Log("autotest: %s put beside %s at (%.1f %.1f %.1f)", HeroName(s.a), HeroName(s.b), p.x, p.y, p.z);
		break;
	}
	case S_PLATES: {
		unsigned n = GetNumLvlClassInstances(2, 21);
		Log("autotest: %u pressure stones (blue circles)", n);
		for (unsigned k = 0; k < n && k < 64; k++) {
			KHook* hk = (KHook*)GetLvlObject(2, 21, k);
			const float* p = hk && hk->node && hk->node->matrix ? (const float*)((char*)hk->node->matrix + 0x80) : nullptr;
			if (p) Log("autotest:   stone %u %p at (%.2f %.2f %.2f) flags %08X ground %08X events %04X/%04X", k, hk, p[0], p[1], p[2],
			           *(uint32_t*)((char*)hk + 8), *(uint32_t*)((char*)hk + 0x28), *(uint16_t*)((char*)hk + 0x30),
			           *(uint16_t*)((char*)hk + 0x32));
		}
		break;
	}
	case S_POKE8: {   // hero a, byte offset b, value x (a = -1: the game manager's +0x40 block)
		if (s.a <= -10) {   // hero -10 - a's own stats (individual heroes): byte +b of the +0x40 block
			int hero = -10 - s.a;
			int prev = IndivEnter(hero);
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
			if (cb) {
				Log("autotest: %s's +0x40 block +%03X %02X -> %02X", HeroName(hero), s.b, *(uint8_t*)(cb + s.b), (int)s.x);
				*(uint8_t*)(cb + s.b) = (uint8_t)(int)s.x;
			}
			IndivLeave(prev);
			break;
		}
		if (s.a == -3) {   // the game manager's state (+0xCC bits 0-1; FUN_00503D80, thiscall(gm, state), ret 4): 2 = the pause menu
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			if (gm) {
				Log("autotest: game manager state %u -> %d", *(uint8_t*)(gm + 0xCC) & 3, s.b);
				((void(__fastcall*)(char*, void*, unsigned))0x503D80)(gm, nullptr, (unsigned)s.b);
			}
			break;
		}
		if (s.a == -2) {   // the game manager's own byte +b, bits x flipped
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			if (gm) {
				Log("autotest: game manager +%03X %02X ^ %02X", s.b, *(uint8_t*)(gm + s.b), (int)s.x);
				*(uint8_t*)(gm + s.b) ^= (uint8_t)(int)s.x;
			}
			break;
		}
		if (s.a < 0) {
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
			if (cb) {
				Log("autotest: game manager block +%03X %02X -> %02X", s.b, *(uint8_t*)(cb + s.b), (int)s.x);
				*(uint8_t*)(cb + s.b) = (uint8_t)(int)s.x;
			}
			break;
		}
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		Log("autotest: %s +%03X %02X -> %02X", HeroName(s.a), s.b, *(uint8_t*)(h.raw() + s.b), (int)s.x);
		*(uint8_t*)(h.raw() + s.b) = (uint8_t)(int)s.x;
		break;
	}
	case S_EVGROUP: {   // event service group a: kind, mask, flags, and the detector tests feeding it
		CKYellowPages* yp = YellowPages();
		char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		if (!ev) break;
		unsigned n1 = *(uint16_t*)(ev + 0x1C), n2 = *(uint16_t*)(ev + 0x1E), n3 = *(uint16_t*)(ev + 0x20);
		uint8_t* tab = *(uint8_t**)(ev + 0xC);
		unsigned gi = (unsigned)s.a;
		const char* kind = gi < n1 ? "OR" : gi < n1 + n2 ? "AND (accumulating)" : gi < n1 + n2 + n3 ? "AND (same frame)" : "?";
		Log("autotest: event groups %u/%u/%u; group %03X (%s): %u targets, mask %02X, flags %02X", n1, n2, n3, gi, kind,
		    tab[gi * 3], tab[gi * 3 + 1], tab[gi * 3 + 2]);
		// Targets: +0x10 object pointers and +0x14 message ids, both in group order.
		unsigned first = 0;
		for (unsigned k = 0; k < gi; k++) first += tab[k * 3];
		char** objs = *(char***)(ev + 0x10);
		uint16_t* msgs = *(uint16_t**)(ev + 0x14);
		for (unsigned k = 0; k < tab[gi * 3]; k++) {
			char* o = objs ? objs[first + k] : nullptr;
			Log("autotest:   target %u: %p vt %08X message %04X", k, o, o ? (unsigned)*(uintptr_t*)o : 0,
			    msgs ? msgs[first + k] : 0);
		}
		break;
	}
	case S_TORCH: {   // a: 0 list/watch, 1 give (fields), 2 move the torch to Asterix, 3 drop (0x513C)
		Trio t = GetTrio();
		Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero();
		if (!a) break;
		unsigned n = GetNumLvlClassInstances(2, 32);
		if (s.a == 0) {
			HMODULE insp = GetModuleHandleA("apd3d9.dll"), self = GetModuleHandleA("xxl_coop.asi");
			Log("autotest: inspector %p wants-keyboard export %p; xxl_coop.asi %p torch export %p", insp,
			    insp ? (void*)GetProcAddress(insp, "XXLInspectorWantsKeyboard") : nullptr, self,
			    self ? (void*)GetProcAddress(self, "XXLCoopTorchCheat") : nullptr);
			float best = 1e30f;
			for (unsigned i = 0; i < n; i++) {
				char* c = (char*)GetLvlObject(2, 32, i);
				char* node = c ? *(char**)(c + 0x10) : nullptr;
				char* fr = node ? *(char**)(node + 4) : nullptr;
				if (!fr) continue;
				Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
				Log("autotest: torch %u %p at (%.1f %.1f %.1f) +3E=%02X radius %.2f +18 %.2f +1C %.2f", i, c, p.x, p.y, p.z,
				    *(uint8_t*)(c + 0x3E), *(float*)(c + 0x14), *(float*)(c + 0x18), *(float*)(c + 0x1C));
				float d = (p - a.Pos()).len2();
				if (d < best) { best = d; at.watchTorch = c; }
			}
			break;
		}
		char* c = at.watchTorch;
		if (s.a == 4) {   // who writes the trio's offer (+0x4C) and Asterix's +0x464
			const void* w[2] = { (char*)t.raw() + 0x4C, a.raw() + 0x464 };
			WatchBytes(w, 2, 0, 0);
			Log("autotest: watching trio+4C %p and Asterix+464 %p", w[0], w[1]);
			break;
		}
		if (!c) break;
		if (s.a == 6 || s.a == 7) {
			CoopTorchCheat(s.a == 6);
		} else if (s.a == 5) {   // offer it (trio +0x4C) and press "take" (+0x3EC bit 0x2000)
			*(char**)((char*)t.raw() + 0x4C) = c;
			*(uint32_t*)(a.raw() + 0x3EC) |= 0x2000;
			Log("autotest: torch %p offered to Asterix", c);
		} else if (s.a == 1) {
			*(char**)(a.raw() + 0x464) = c;
			*(uint32_t*)(a.raw() + 0x3EC) |= 0x400000;
			Log("autotest: torch %p given to Asterix (fields)", c);
		} else if (s.a == 2) {
			char* node = *(char**)(c + 0x10);
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (fr) {
				Vector3 p = a.Pos();
				((float*)(fr + 0x40))[0] = p.x + 0.5f; ((float*)(fr + 0x40))[1] = p.y; ((float*)(fr + 0x40))[2] = p.z;
			}
			Log("autotest: torch %p moved to Asterix", c);
		} else if (s.a == 3) {
			typedef int(__fastcall* SendFn)(void*, void*, int, void*);
			((SendFn)(*(void***)c)[5])(c, nullptr, 0x513C, nullptr);
			*(char**)(a.raw() + 0x464) = nullptr;
			*(uint32_t*)(a.raw() + 0x3EC) &= ~0x400000u;
			Log("autotest: torch %p dropped (0x513C)", c);
		}
		break;
	}
	case S_WATCHCAM: at.watchCam = true; break;
	case S_WATCHCINE: at.watchCine = true; break;
	case S_WATCHBEACONS: at.watchBeacons = true; at.beaconPoint = Vector3(s.x, s.y, s.z); break;
	case S_WATCHENEMIES: at.watchEnemies = true; at.enemyPoint = Vector3(s.x, s.y, s.z); break;
	case S_CPNTCHECK: {   // a > 0: writes to +0x54 of Roman #b of class a watched as well
		at.cpntCheck = true;
		Log("autotest: checking every Roman's component (+0x54) each frame");
		static bool hooked = false;
		if (!hooked)
			hooked = InstallDetour(0x45C010, (void*)SlotStepHook, (void**)&g_origSlotStep, kSlotStepPrologue,
			                       sizeof(kSlotStepPrologue), "path slot step (autotest)");
		char* e = s.a > 0 ? (char*)GetLvlObject(2, s.a, s.b) : nullptr;
		if (e) {
			const void* w[1] = { e + 0x54 };
			WatchBytes(w, 1, 0, 0);
			Log("autotest: watching writes to Roman %p's component (+0x54 = %p)", e, *(void**)(e + 0x54));
		}
		break;
	}
	case S_NODEDUMP: {   // bonuses (2,114) within 6 units of (x y z): their node trees
		Vector3 c(s.x, s.y, s.z);
		struct D {
			static void Dump(char* node, int depth) {
				for (; node && depth < 6; node = *(char**)(node + 8)) {
					char* fr = *(char**)(node + 4);
					const float* p = fr ? (const float*)(fr + 0x80) : nullptr;
					Log("autotest: %*snode %p vt %08X flags %04X stamp %u at (%.1f %.1f %.1f) +1C %p +24 %p", depth * 2, "",
					    node, *(uint32_t*)node, *(uint16_t*)(node + 0x10), *(uint16_t*)(node + 0x14), p ? p[0] : 0,
					    p ? p[1] : 0, p ? p[2] : 0, *(void**)(node + 0x1C), *(void**)(node + 0x24));
					Dump(*(char**)(node + 0x18), depth + 1);
				}
			}
		};
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 114); i < n; i++) {
			char* b = (char*)GetLvlObject(2, 114, i);
			char* node = b ? *(char**)(b + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!fr) continue;
			Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
			if ((p - c).len2() > 36.0f) continue;
			Log("autotest: bonus %p at (%.1f %.1f %.1f)", b, p.x, p.y, p.z);
			__try {
				Log("autotest: node %p vt %08X flags %04X", node, *(uint32_t*)node, *(uint16_t*)(node + 0x10));
				D::Dump(*(char**)(node + 0x18), 1);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				Log("autotest: (node tree unreadable)");
			}
		}
		break;
	}
	case S_SETPRESET: {   // the game's controller preset a (CKInput +0x64 via FUN_00438950); -1: the saved options (FUN_00503B00)
		CKYellowPages* yp = YellowPages();
		char* in = yp ? *(char**)((char*)yp + 0x14) : nullptr;
		if (!in) break;
		if (s.a == -1) {
			Log("autotest: the saved options (FUN_00503B00), controller preset %d", *(int*)(in + 0x64));
			((void(__fastcall*)(char*, void*))0x503B00)((char*)yp->gameManager, nullptr);
			break;
		}
		typedef void(__fastcall* SetPresetFn)(char* in, void* edx, int preset);
		Log("autotest: controller preset %d -> %d", *(int*)(in + 0x64), s.a);
		((SetPresetFn)0x438950)(in, nullptr, s.a);
		break;
	}
	case S_BOSSSCAN: {   // event groups that send a sidekick to a marker, the zones feeding them, the zones they enable
		CKYellowPages* yp = YellowPages();
		char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		char* det = yp ? *(char**)((char*)yp + 0x54) : nullptr;
		if (!ev || !det || *(uintptr_t*)det != 0x62504C) { Log("autotest: bossscan: no services"); break; }
		unsigned n = (unsigned)*(uint16_t*)(ev + 0x1C) + *(uint16_t*)(ev + 0x1E) + *(uint16_t*)(ev + 0x20);
		uint8_t* tab = *(uint8_t**)(ev + 0xC);
		char** objs = *(char***)(ev + 0x10);
		uint16_t* msgs = *(uint16_t**)(ev + 0x14);
		static unsigned firstOf[4097]; if (n > 4096) n = 4096; firstOf[0] = 0;
		for (unsigned g = 0; g < n; g++) firstOf[g + 1] = firstOf[g] + tab[g * 3];
		const int bases[5] = { 0x2C, 0x34, 0x3C, 0x44, 0x4C };
		const char* names[5] = { "box-enter", "sphere-enter", "plane-cross", "box-level", "sphere-level" };
		auto dumpGroup = [&](unsigned g, const char* indent) {
			for (unsigned k = firstOf[g]; k < firstOf[g + 1]; k++)
				Log("autotest: %s  -> %p vt %08X msg %04X", indent, objs[k], objs[k] ? (unsigned)*(uintptr_t*)objs[k] : 0, msgs[k]);
		};
		auto feeders = [&](unsigned g, const char* indent) {
			for (int li = 0; li < 5; li++) {
				char* e = *(char**)(det + bases[li]);
				unsigned cnt = *(uint16_t*)(det + bases[li] + 4);
				for (unsigned i = 0; e && i < cnt; i++, e += 8)
					if ((*(uint16_t*)(e + 6) >> 3) == g)
						Log("autotest: %s  fed by %s %u (obj %u, flags %02X/%02X, event %04X)", indent, names[li], i,
						    *(uint16_t*)(e + 2), (uint8_t)e[4], (uint8_t)e[5], *(uint16_t*)(e + 6));
			}
		};
		for (unsigned g = 0; g < n; g++) {
			bool walk = false;
			for (unsigned k = firstOf[g]; k < firstOf[g + 1]; k++)
				if (objs[k] && *(uintptr_t*)objs[k] == 0x6256A0 && msgs[k] >= 0x4813 && msgs[k] <= 0x4876) walk = true;
			if (!walk) continue;
			Log("autotest: bossscan: group %03X (#%u), mask %02X", g, g, tab[g * 3 + 1]);
			dumpGroup(g, "");
			feeders(g, "");
			// Zones this group enables (detection service messages 0x14..0x18 + index): what they send.
			for (unsigned k = firstOf[g]; k < firstOf[g + 1]; k++) {
				if (objs[k] != det) continue;
				unsigned op = msgs[k] >> 8, idx = msgs[k] & 0xFF;
				if (op < 0x14 || op > 0x18) continue;
				int li = op - 0x14;
				char* e = *(char**)(det + bases[li]);
				unsigned cnt = *(uint16_t*)(det + bases[li] + 4);
				if (!e || idx >= cnt) continue;
				e += idx * 8;
				unsigned eg = *(uint16_t*)(e + 6) >> 3;
				Log("autotest:     enables %s %u (obj %u, event %04X, group %03X):", names[li], idx, *(uint16_t*)(e + 2), *(uint16_t*)(e + 6), eg);
				if (eg < n) dumpGroup(eg, "    ");
			}
		}
		break;
	}
	case S_DETOFF: {   // detector b of the detection service's list +a (0x2C box enter ...) switched off
		CKYellowPages* yp = YellowPages();
		char* det = yp ? *(char**)((char*)yp + 0x54) : nullptr;
		char* e = det ? *(char**)(det + s.a) : nullptr;
		unsigned n = det ? *(uint16_t*)(det + s.a + 4) : 0;
		if (e && (unsigned)s.b < n) e[8 * s.b + 4] &= ~1;
		Log("autotest: detector %d of list +%02X switched off (%u in it)", s.b, s.a, n);
		break;
	}
	case S_CAMSECTOR: {   // the smallest switched-off camera sector around (x, y, z) switched on (as its 0x701)
		CKYellowPages* yp = YellowPages();
		char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
		char* best = nullptr;
		float bv = 1e30f;
		for (char* sec = cs ? *(char**)(cs + 0x24) : nullptr; sec; sec = *(char**)(sec + 4)) {
			const float* b = (const float*)(sec + 8);
			if ((*(uint8_t*)(sec + 0x29) & 2) || s.x <= b[0] || s.x >= b[3] || s.y <= b[1] || s.y >= b[4] || s.z <= b[2] || s.z >= b[5]) continue;
			float v = (b[3] - b[0]) * (b[4] - b[1]) * (b[5] - b[2]);
			if (v < bv) { bv = v; best = sec; }
		}
		if (best) *(uint8_t*)(best + 0x29) |= 2;
		Log("autotest: camera sector %p around (%.1f %.1f %.1f) switched on (track %p)", best, s.x, s.y, s.z,
		    best ? *(void**)(best + 0x20) : nullptr);
		break;
	}
	case S_POOLHOLD: {   // pool of handler a: b 1 takes its free bonuses away (as if in flight), 0 gives them back, 2 logs
		static char* held[128];
		static int nHeld = 0;
		static char* heldPool = nullptr;
		char* pool = nullptr;
		const int kPoolIds[2] = { 61, 63 };
		for (int k = 0; k < 2 && !pool; k++)
			for (unsigned i = 0, n = GetNumLvlClassInstances(4, kPoolIds[k]); i < n && !pool; i++) {
				char* p = (char*)GetLvlObject(4, kPoolIds[k], i);
				if (p && *(uint8_t*)(p + 0x28) == (uint8_t)s.a) pool = p;
			}
		auto count = [](char* b) { int c = 0; for (; b && c < 999; b = *(char**)(b + 0x14)) c++; return c; };
		if (!pool) { Log("autotest: no bonus pool with handler %d", s.a); break; }
		if (s.b == 1) {
			for (char* b; (b = *(char**)(pool + 0x38)) != nullptr && nHeld < 128;) {
				*(char**)(pool + 0x38) = *(char**)(b + 0x14);
				*(char**)(b + 0x14) = nullptr;
				held[nHeld++] = b;
			}
			heldPool = pool;
		} else if (s.b == 0 && heldPool == pool) {
			while (nHeld > 0) {
				char* b = held[--nHeld];
				*(char**)(b + 0x14) = *(char**)(pool + 0x38);
				*(char**)(pool + 0x38) = b;
			}
		}
		Log("autotest: bonus pool %p (handler %d): %d held; active %d, beacons %d, free %d", pool, s.a, nHeld,
		    count(*(char**)(pool + 0x30)), count(*(char**)(pool + 0x34)), count(*(char**)(pool + 0x38)));
		break;
	}
	case S_AREAHIT: {   // the trio's area hit (event 0x4C14, a mobile tower's shockwave) on hero a, from a stand-in tower
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		int hp[2] = { -1, -1 }, sh[2] = { -1, -1 };
		for (int i = 0; i < 2; i++) IndivStats(i, &hp[i], &sh[i]);
		// x, y, z, radius, the source (answers 0x0C0A with the damage), "a hero hit" (byte)
		struct { float x, y, z, r; void* source; uint8_t hit; } sphere = { h.Pos().x, h.Pos().y + 0.5f, h.Pos().z, 1.5f, &g_standInTower, 0 };
		typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
		((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, 0x4C14, &sphere);
		int hp2[2] = { -1, -1 }, sh2[2] = { -1, -1 };
		for (int i = 0; i < 2; i++) IndivStats(i, &hp2[i], &sh2[i]);
		Log("autotest: area hit on %s (%s leads): health Asterix %d -> %d, Obelix %d -> %d", HeroName(s.a), HeroName(t.Leader()),
		    hp[0], hp2[0], hp[1], hp2[1]);
		break;
	}
	case S_BIND:   // bindings a, b and buttons (> 0) pressed in the game's values for x frames (at least 1; menus)
		at.bind[0] = s.a;
		at.bind[1] = s.b > 0 ? s.b : -1;
		at.bind[2] = s.buttons > 0 ? s.buttons : -1;
		at.bindFrames = s.x >= 1.0f ? (int)s.x : 1;
		Log("autotest: bindings %d %d %d pressed for %d frames", at.bind[0], at.bind[1], at.bind[2], at.bindFrames);
		break;
	case S_STEERCHECK:   // hero a: b = 0 notes its place; b = 1 its walk since, against its player's camera
		SteerCheck(s.a, s.b);
		break;
	case S_CAMTARGET: {   // whom the game's camera and the copy follow (camera service +0xE8: a hero's +0x10)
		CKYellowPages* yp = YellowPages();
		char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
		Trio t = GetTrio();
		if (!cs || !t) break;
		int live = GameCamLiveHero();
		Log("autotest: the game's camera (%s's) follows %s; leader %s", HeroName(live >= 0 ? live : t.Leader()),
		    CamFollows(t, cs), HeroName(t.Leader()));
		for (int k = 0; k < 2; k++)
			if (const char* c = GameCamCopyState(k)) Log("autotest: %s's camera (copy) follows %s", HeroName(k), CamFollows(t, c));
		break;
	}
	case S_CAMDUMP: {   // the camera service (yellow pages +0x68): state, tracks, sectors, input view
		CKYellowPages* yp = YellowPages();
		char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
		if (!cs) break;
		__try {
			Log("autotest: camsrv %p flags %08X next %p cur %p prev %p target %p/+EC %p curSector %p 5D0 %p 5D4 %p rw %p",
			    cs, *(uint32_t*)(cs + 0xC), *(void**)(cs + 0x34), *(void**)(cs + 0x38), *(void**)(cs + 0x3C),
			    *(void**)(cs + 0xE8), *(void**)(cs + 0xEC), *(void**)(cs + 0x4EC), *(void**)(cs + 0x5D0),
			    *(void**)(cs + 0x5D4), *(void**)(cs + 0x1C));
			char* tg = *(char**)(cs + 0xE8);
			if (tg) Log("autotest: target vt %08X +4 %p +8 %p +C %p +10 %p", *(uint32_t*)tg, *(void**)(tg + 4),
			            *(void**)(tg + 8), *(void**)(tg + 0xC), *(void**)(tg + 0x10));
			Trio t = GetTrio();
			for (int h = 0; t && h < 3; h++) {
				Hero hero = t.GetHero(h);
				if (hero) Log("autotest: hero %d hook %p +4 %p +C %p +10 %p", h, hero.raw(), *(void**)(hero.raw() + 4),
				              *(void**)(hero.raw() + 0xC), *(void**)(hero.raw() + 0x10));
			}
			for (int k = 0; k < 3; k++) {
				char* tr = *(char**)(cs + 0x34 + 4 * k);
				if (!tr) continue;
				char lb[512]; LineBuf l(lb);
				l.Add("autotest: track[%d] %p vt %08X:", k, tr, *(uint32_t*)tr);
				for (int o = 4; o < 0x6C; o += 4) l.Add(" %08X", *(uint32_t*)(tr + o));
				Log("%s", l.c_str());
			}
			char* view = *(char**)(cs + 0x580);
			if (view) {
				uint8_t* remap = *(uint8_t**)(view + 8);
				char* inp = *(char**)(view + 0xC);
				InputManager im = GetInputManager();
				Log("autotest: cam view %p input %p (game input %p) remap %u %u %u %u %u %u %u %u", view, inp,
				    im ? im.raw() : nullptr, remap[0], remap[1], remap[2], remap[3], remap[4], remap[5], remap[6], remap[7]);
			}
			int n = 0;
			for (char* sec = *(char**)(cs + 0x24); sec && n < 200; sec = *(char**)(sec + 4), n++) {
				const float* b = (const float*)(sec + 8);
				Log("autotest: camsector %p flags %02X box (%.1f %.1f %.1f)-(%.1f %.1f %.1f) track %p vt %08X time %.2f type %u",
				    sec, *(uint8_t*)(sec + 0x29), b[0], b[1], b[2], b[3], b[4], b[5], *(void**)(sec + 0x20),
				    *(void**)(sec + 0x20) ? *(uint32_t*)*(void**)(sec + 0x20) : 0, *(float*)(sec + 0x24), *(uint8_t*)(sec + 0x28));
			}
			Log("autotest: %d camera sectors (+20 list %p)", n, *(void**)(cs + 0x20));
			n = 0;
			for (char* tr = *(char**)(cs + 0x20); tr && n < 200; tr = *(char**)(tr + 4), n++)
				Log("autotest: track list %p vt %08X", tr, *(uint32_t*)tr);
			Log("autotest: %d tracks in the list", n);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Log("autotest: (camera service unreadable)");
		}
		break;
	}
	case S_P2LOOK:P2CamLookToward(Vector3(s.x, s.y, s.z)); Log("autotest: player 2's camera turned"); break;
	case S_HEROFIELDS: {   // movement parameters of both heroes: +0x300..+0x33C and +0x84 speed, +0x388
		Trio t = GetTrio();
		if (s.b == 1) {   // b = 1: hero a's whole object (dwords) and its collision volumes
			Hero hero = t ? t.GetHero(s.a) : Hero();
			if (!hero) break;
			for (int o = 0; o < 0xD00; o += 0x20) {
				const uint32_t* d = (const uint32_t*)(hero.raw() + o);
				Log("dump %s +%03X: %08X %08X %08X %08X %08X %08X %08X %08X", HeroName(s.a), o, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
			}
			const int vols[] = { 0x228, 0xB84, 0xB88, 0xC18, 0xC1C };
			for (int vo : vols) {
				char* v = *(char**)(hero.raw() + vo);
				if (!v || IsBadReadPtr(v, 0x40)) continue;
				for (int o = 0; o < 0x40; o += 0x20) {
					const uint32_t* d = (const uint32_t*)(v + o);
					Log("dump %s vol+%03X +%02X: %08X %08X %08X %08X %08X %08X %08X %08X", HeroName(s.a), vo, o, d[0], d[1], d[2], d[3], d[4],
					    d[5], d[6], d[7]);
				}
			}
			break;
		}
		for (int h = 0; t && h < 2; h++) {
			Hero hero = t.GetHero(h);
			if (!hero) continue;
			const float* f = (const float*)(hero.raw() + 0x300);
			Log("autotest: %s +300: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f  speed %.2f  388=%08X",
			    HeroName(h), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15],
			    *(float*)(hero.raw() + 0x84), *(uint32_t*)(hero.raw() + 0x388));
		}
		break;
	}
	case S_LEVERS: {   // every lever (CKHkActivator, 2,52): position, events, whom they switch to
		unsigned n = GetNumLvlClassInstances(2, 52);
		for (unsigned i = 0; i < n; i++) {
			char* l = (char*)GetLvlObject(2, 52, i);
			char* node = l ? *(char**)(l + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!l) continue;
			int to = ProximityLeverSwitchTarget(l);
			Log("autotest: lever %u %p at (%.1f %.1f %.1f) events %04X/%04X%s%s", i, l, fr ? ((float*)(fr + 0x80))[0] : 0,
			    fr ? ((float*)(fr + 0x80))[1] : 0, fr ? ((float*)(fr + 0x80))[2] : 0, *(uint16_t*)(l + 0x28),
			    *(uint16_t*)(l + 0x2A), to >= 0 ? " switch to " : "", to >= 0 ? HeroName(to) : "");
		}
		break;
	}
	case S_WATCHCLUE: {
		unsigned n = GetNumLvlClassInstances(2, 161);
		at.watchClue = n ? (char*)GetLvlObject(2, 161, 0) : nullptr;
		Log("autotest: %u clue men; watching %p", n, at.watchClue);
		if (s.a == 1) {   // open the spies' gate (set in play by the first spy's dialogue)
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			char* a = gm ? *(char**)(gm + 0x18) : nullptr;
			char* b = a ? *(char**)(a + 0x24) : nullptr;
			char* c = b ? *(char**)(b + 0x64) : nullptr;
			if (c) *(uint8_t*)(c + 0x10) |= 2;
			Log("autotest: spies' gate opened (%p vt %p, from %p vt %p)", c, c ? *(void**)c : nullptr, b,
			    b ? *(void**)b : nullptr);
			if (c) { const void* w[1] = { c + 0x10 }; WatchBytes(w, 1, 0, 0); }
		}
		for (unsigned i = 0; i < n; i++) {
			char* c = (char*)GetLvlObject(2, 161, i);
			if (c)
				Log("autotest:   spy %u: spot (%.1f %.1f %.1f) appears within %.1f, talks within %.1f", i,
				    *(float*)(c + 0x12C), *(float*)(c + 0x130), *(float*)(c + 0x134), *(float*)(c + 0x68),
				    *(float*)(c + 0x70));
		}
		break;
	}
	case S_WATCHGROUPS: {   // hardware watch on the flag bytes of event groups 0x74, 0x77, 0xC1
		CKYellowPages* yp = YellowPages();
		char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		if (!ev) break;
		uint8_t* tab = *(uint8_t**)(ev + 0xC);
		const void* a[3] = { tab + 0x74 * 3 + 2, tab + 0x77 * 3 + 2, tab + 0xC1 * 3 + 2 };
		WatchBytes(a, 3, 0x46B180, 0x46B331);   // not CKSrvEvent::vf17 clearing them
		Log("autotest: watching group flags 074/077/0C1 at %p %p %p", a[0], a[1], a[2]);
		break;
	}
	case S_MUSICGROUPS: {   // every event group that messages the sound manager, and the detectors feeding it
		CKYellowPages* yp = YellowPages();
		char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		char* det = yp ? *(char**)((char*)yp + 0x54) : nullptr;
		if (!ev) break;
		unsigned n = (unsigned)*(uint16_t*)(ev + 0x1C) + *(uint16_t*)(ev + 0x1E) + *(uint16_t*)(ev + 0x20);
		unsigned n1 = *(uint16_t*)(ev + 0x1C), n2 = *(uint16_t*)(ev + 0x1E);
		uint8_t* tab = *(uint8_t**)(ev + 0xC);
		char** objs = *(char***)(ev + 0x10);
		uint16_t* msgs = *(uint16_t**)(ev + 0x14);
		unsigned first = 0;
		for (unsigned gi = 0; gi < n; first += tab[gi * 3], gi++) {
			for (unsigned k = 0; k < tab[gi * 3]; k++) {
				char* o = objs[first + k];
				if (!o || (*(uintptr_t*)o != 0x624058 && *(uintptr_t*)o != 0x629030)) continue;
				const float* pp = nullptr;
				if (*(uintptr_t*)o == 0x629030) {
					char* nd = *(char**)(o + 0x10);
					char* fr = nd ? *(char**)(nd + 4) : nullptr;
					pp = fr ? (const float*)(fr + 0x80) : nullptr;
					if (!pp || fabsf(pp[0] - 42) > 3 || fabsf(pp[2] + 120) > 3) continue;   // the sector-8 pillar only
				}
				Log("autotest: group %03X (%s, mask %02X, flags %02X) -> %s %p %04X", gi,
				    gi < n1 ? "OR" : gi < n1 + n2 ? "AND acc" : "AND frame", tab[gi * 3 + 1], tab[gi * 3 + 2],
				    *(uintptr_t*)o == 0x629030 ? "light pillar" : "sound manager", o, msgs[first + k]);
				for (unsigned k2 = 0; k2 < tab[gi * 3]; k2++) {
					char* o2 = objs[first + k2];
					Log("autotest:       target %p vt %08X message %04X", o2, o2 ? (unsigned)*(uintptr_t*)o2 : 0,
					    msgs[first + k2]);
				}
				if (false) Log("autotest: group %03X (%s, mask %02X) -> sound manager %04X", gi,
				    gi < n1 ? "OR" : gi < n1 + n2 ? "AND acc" : "AND frame", tab[gi * 3 + 1], msgs[first + k]);
				struct L { const char* name; int base, count; } ls[5] = { { "box-enter", 0x2C, 0x30 },
					{ "sphere-enter", 0x34, 0x38 }, { "plane-cross", 0x3C, 0x40 }, { "box-level", 0x44, 0x48 },
					{ "sphere-level", 0x4C, 0x50 } };
				for (const L& l : ls) {
					char* e = det ? *(char**)(det + l.base) : nullptr;
					unsigned cnt = det ? *(uint16_t*)(det + l.count) : 0;
					for (unsigned i = 0; e && i < cnt; i++, e += 8)
						if ((*(uint16_t*)(e + 6) >> 3) == gi)
							Log("autotest:     fed by %s %u: obj %u flags %02X/%02X event %04X", l.name, i,
							    *(uint16_t*)(e + 2), (uint8_t)e[4], (uint8_t)e[5], *(uint16_t*)(e + 6));
				}
			}
		}
		break;
	}
	case S_REGIONS:
		SoundDumpRegions();
		break;
	case S_DASHFLAG: {
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		if (!gm) break;
		if (s.a) *(uint32_t*)(gm + 0x5C) |= 0x80000u;
		else *(uint32_t*)(gm + 0x5C) &= ~0x80000u;
		Log("autotest: dash flag %s", s.a ? "on" : "off");
		break;
	}
	case S_SETF18: {
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		Log("autotest: %s f18 %02X -> %02X", HeroName(s.a), h.Flags18(), s.b);
		*(uint8_t*)(h.raw() + 0x18) = (uint8_t)s.b;
		break;
	}
	case S_POTION:
		SetPotionGauge(300.0f);
		Log("autotest: potion gauge filled");
		break;
	case S_GLIDE: {
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h || s.a < 0 || s.a >= HERO_COUNT || s.b < -2 || s.b >= kNumSpots) break;
		Hero o = s.b == -2 && s.a <= HERO_OBELIX ? t.GetHero(1 - s.a) : Hero();   // GLIDEBY: (x, z) off the other hero
		if (s.b == -2 && !o) break;
		auto& g = at.glide[s.a];
		g.on = true;
		g.from = h.Pos();
		g.to = s.b == -2 ? Vector3(o.Pos().x + s.x, o.Pos().y + 0.3f, o.Pos().z + s.z)
		     : s.b < 0 ? Vector3(s.x, s.y, s.z) : Vector3(kSpots[s.b][0], kSpots[s.b][1], kSpots[s.b][2]);
		g.start = GetTickCount();
		g.ms = s.b < 0 ? (DWORD)s.buttons : (DWORD)s.x;
		Log("autotest: %s gliding to spot %d over %d ms", HeroName(s.a), s.b, (int)s.x);
		break;
	}
	case S_KEYMAP: {
		static const int kNumpad[CoopConfig::KEY_COUNT] = {
			0x68, 0x65, 0x64, 0x66, 0x6F, 0x62, 0x67, 0x69, 0x60, 0x61, 0x63, 0x6E, 0x2D, 0x2E, 0x24, 0x23 };
		memcpy(g_config.keys, kNumpad, sizeof(kNumpad));
		if (s.a == 2) g_config.keys[CoopConfig::KEY_JUMP] = 0x20;
		Log("autotest: player 2 keyboard layout %d", s.a);
		break;
	}
	case S_VPADR:
		Log("autotest: %s", s.name);
		at.vpad[s.a].rx = s.x;
		at.vpad[s.a].ry = s.y;
		PadsSetVirtual(s.a, &at.vpad[s.a]);
		break;
	case S_VPAD:
		Log("autotest: %s", s.name);
		at.vpad[s.a].buttons = s.buttons;
		at.vpad[s.a].lx = s.x;
		at.vpad[s.a].ly = s.y;
		PadsSetVirtual(s.a, &at.vpad[s.a]);
		CoopLogState(s.name);
		break;
	case S_TELEPORT: {
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		KHook* lifter = (KHook*)GetLvlObject(2, 47, (unsigned)s.b);
		if (!h || !lifter || !lifter->node || !lifter->node->matrix) {
			Log("autotest: teleport %s: no hero or lifter %d", HeroName(s.a), s.b);
			break;
		}
		float* lp = (float*)((char*)lifter->node->matrix + 0x40);
		Vector3 to(lp[0] + s.x, lp[1] + 1.5f, lp[2] + s.y);
		h.Pos() = to;
		if (h.hook->node && h.hook->node->matrix) {
			float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
			hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
		}
		Log("autotest: teleported %s to lifter %d (%.2f %.2f %.2f)", HeroName(s.a), s.b,
			to.x, to.y, to.z);
		break;
	}
	case S_BONUS: {
		// Nearest bonus that a pool currently has active (dropped collectibles),
		// or (s.b == 1) one the pool placed on a beacon last frame, away from the
		// other hero.
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		Hero other = t.GetHero(s.a == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX);
		const size_t listOff = s.b == 1 ? 0x34 : 0x30;
		const bool flaskOnly = s.b >= 2;   // 2: potion pool (type 3) active, 3: potion pool beacons
		char* best = nullptr;
		float bestD = 1e30f;
		Vector3 bestP;
		int pools = 0, active = 0;
		const int kPoolIds[2] = { 61, 63 };
		for (int k = 0; k < 2; k++) {
			unsigned n = GetNumLvlClassInstances(4, kPoolIds[k]);
			for (unsigned i = 0; i < n; i++) {
				char* pool = (char*)GetLvlObject(4, kPoolIds[k], i);
				if (!pool) continue;
				if (flaskOnly && (kPoolIds[k] != 63 || *(int*)(pool + 0x24) != 3)) continue;
				pools++;
				for (char* b = *(char**)(pool + (s.b == 3 ? 0x34 : listOff)); b; b = *(char**)(b + 0x14)) {
					char* node = *(char**)(b + 0x10);
					char* frame = node ? *(char**)(node + 4) : nullptr;
					if (!frame) continue;
					active++;
					Vector3 p(((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1],
					          ((float*)(frame + 0x80))[2]);
					if (s.b == 1 && other && (p - other.Pos()).len2() < 15.0f * 15.0f) continue;
					float d = (p - h.Pos()).len2();
					if (d < bestD) { bestD = d; best = b; bestP = p; }
				}
			}
		}
		Log("autotest: %d pools, %d %s bonuses; nearest to %s at (%.2f %.2f %.2f), %.1f away",
			pools, active, s.b == 1 ? "beacon" : "active", HeroName(s.a), bestP.x, bestP.y, bestP.z,
			best ? sqrtf(bestD) : -1.0f);
		if (!best) break;
		at.watchBonus = best;
		Vector3 to = bestP + Vector3(0, 0.2f, 0);
		h.Pos() = to;
		if (h.hook->node && h.hook->node->matrix) {
			float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
			hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
		}
		break;
	}
	case S_BUMPER: {
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		unsigned n = GetNumLvlClassInstances(2, 160);
		char* best = nullptr;
		float bestD = 1e30f;
		Vector3 bestP;
		for (unsigned i = 0; i < n; i++) {
			char* bmp = (char*)GetLvlObject(2, 160, i);
			char* node = bmp ? *(char**)(bmp + 0x10) : nullptr;
			char* frame = node ? *(char**)(node + 4) : nullptr;
			if (!frame) continue;
			Vector3 p(((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1], ((float*)(frame + 0x80))[2]);
			if (p.len2() == 0.0f) continue;
			float d = (p - h.Pos()).len2();
			if (d < bestD) { bestD = d; best = bmp; bestP = p; }
		}
		if (!best) { Log("autotest: no bumper"); break; }
		// CKHkBumper launches the leader when it hits at speed >= +0x18 with its
		// facing along the bumper's (LTM at) axis: flag 0x40 of +0x3E accepts
		// dot > 0.1, flag 0x80 dot < -0.1. Line the hero up accordingly.
		char* bframe = *(char**)(*(char**)(best + 0x10) + 4);
		Vector3 axis(((float*)(bframe + 0x70))[0], 0, ((float*)(bframe + 0x70))[2]);
		float al = sqrtf(axis.len2());
		axis = al > 0.01f ? axis * (1.0f / al) : Vector3(0, 0, 1);
		uint8_t bflags = *(uint8_t*)(best + 0x3E);
		Vector3 face = (bflags & 0x40) ? axis : axis * -1.0f;
		Log("autotest: %u bumpers; nearest to %s at (%.2f %.2f %.2f), %.1f away; flags %02X speeds "
			"%.2f/%.2f axis (%.2f %.2f) -> facing (%.2f %.2f)",
			n, HeroName(s.a), bestP.x, bestP.y, bestP.z, sqrtf(bestD), bflags, *(float*)(best + 0x14),
			*(float*)(best + 0x18), axis.x, axis.z, face.x, face.z);
		TraceDumpCollisionTests(best);
		// Facing: direction +0xB4 plus the yaw pair +0xCC/+0xD4 the hero turns
		// with (FUN_0048C970 sets all three the same way, yaw = atan2(z, x)).
		h.Dir() = face;
		float yaw = atan2f(face.z, face.x);
		if (yaw < 0) yaw += 6.2831855f;
		*(float*)(h.raw() + 0xCC) = yaw;
		*(float*)(h.raw() + 0xD4) = yaw;
		Vector3 to = bestP - face * 3.0f + Vector3(0, 0.3f, 0);
		h.Pos() = to;
		if (h.hook->node && h.hook->node->matrix) {
			float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
			hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
		}
		break;
	}
	case S_CRATE: {
		if (s.b == -2) {   // live crate objects (a beacon at +0x58) within 30 units of (x, y, z), and the free ones
			unsigned n = GetNumLvlClassInstances(2, 112), live = 0, nearby = 0, intact = 0;
			for (unsigned i = 0; i < n; i++) {
				char* c = (char*)GetLvlObject(2, 112, i);
				if (!c || *(int*)(c + 0x58) == -1) continue;
				live++;
				char* node = *(char**)(c + 0x10);
				char* frame = node ? *(char**)(node + 4) : nullptr;
				if (!frame) continue;
				Vector3 p(((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1], ((float*)(frame + 0x80))[2]);
				if ((p - Vector3(s.x, s.y, s.z)).len2() > 30.0f * 30.0f) continue;
				nearby++;
				if (*(uint32_t*)(c + 8) & 7) intact++;
			}
			Log("autotest: %s: %u crate objects, %u live, %u of them within 30 units (%u intact)", s.name, n, live, nearby, intact);
			break;
		}
		if (s.b == -3) {   // every crate object: where its live crate is, or free
			unsigned n = GetNumLvlClassInstances(2, 112);
			char buf[1500];
			LineBuf d(buf);
			d.Add("autotest: %s: %u crate objects:", s.name, n);
			for (unsigned i = 0; i < n && i < 40; i++) {
				char* c = (char*)GetLvlObject(2, 112, i);
				char* node = c ? *(char**)(c + 0x10) : nullptr;
				char* frame = node ? *(char**)(node + 4) : nullptr;
				if (!c || *(int*)(c + 0x58) == -1 || !frame) { d.Add(" free"); continue; }
				d.Add(" (%.0f %.0f %.0f)%u", ((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1], ((float*)(frame + 0x80))[2],
				      *(uint32_t*)(c + 8) & 7);
			}
			Log("%s", d.c_str());
			break;
		}
		if (s.b < 0) {
			if (at.watchCrate)
				Log("autotest: crate %p has %u pieces left", at.watchCrate, *(uint32_t*)(at.watchCrate + 8) & 7);
			break;
		}
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		const uintptr_t kIronFilter = 0x4B0DF0;   // CKGrpMecaCpntAsterix: Obelix-only crates
		unsigned n = GetNumLvlClassInstances(2, 112), iron = 0, wood = 0;
		char* best = nullptr;
		float bestD = 1e30f;
		Vector3 bestP;
		for (unsigned i = 0; i < n; i++) {
			char* c = (char*)GetLvlObject(2, 112, i);
			if (!c || *(int*)(c + 0x58) == -1 || (*(uint32_t*)(c + 8) & 7) == 0) continue;
			char* cfg = *(char**)(c + 0x14);
			bool isIron = cfg && *(uintptr_t*)(cfg + 0x2E0) == kIronFilter;
			(isIron ? iron : wood)++;
			if (isIron != (s.b == 1)) continue;
			char* node = *(char**)(c + 0x10);
			char* frame = node ? *(char**)(node + 4) : nullptr;
			if (!frame) continue;
			Vector3 p(((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1], ((float*)(frame + 0x80))[2]);
			float d = (p - h.Pos()).len2();
			if (d < bestD) { bestD = d; best = c; bestP = p; }
		}
		Log("autotest: %u crates (%u iron, %u wooden intact); nearest %s one to %s at (%.2f %.2f %.2f), "
			"%.1f away, %u pieces", n, iron, wood, s.b == 1 ? "iron" : "wooden", HeroName(s.a), bestP.x,
			bestP.y, bestP.z, best ? sqrtf(bestD) : -1.0f, best ? *(uint32_t*)(best + 8) & 7 : 0);
		at.watchCrate = best;
		if (!best) break;
		Vector3 face = bestP - h.Pos();
		face.y = 0;
		float fl = sqrtf(face.len2());
		face = fl > 0.01f ? face * (1.0f / fl) : Vector3(0, 0, 1);
		h.Dir() = face;
		float yaw = atan2f(face.z, face.x);
		if (yaw < 0) yaw += 6.2831855f;
		*(float*)(h.raw() + 0xCC) = yaw;
		*(float*)(h.raw() + 0xD4) = yaw;
		Vector3 to = bestP - face * 1.8f;
		to.y = h.Pos().y > bestP.y ? h.Pos().y : bestP.y;
		to.y += 0.3f;
		h.Pos() = to;
		if (h.hook->node && h.hook->node->matrix) {
			float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
			hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
		}
		break;
	}
	case S_ENEMY: {
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		unsigned n = GetNumLvlClassInstances(2, 93);
		char* best = nullptr;
		float bestD = 1e30f;
		Vector3 bestP;
		for (unsigned i = 0; i < n; i++) {
			char* e = (char*)GetLvlObject(2, 93, i);
			char* node = e ? *(char**)(e + 0x10) : nullptr;
			char* frame = node ? *(char**)(node + 4) : nullptr;
			if (!frame || (*(uint8_t*)(node + 0x10) & 2)) continue;   // hidden: not in play
			Vector3 p(((float*)(frame + 0x80))[0], ((float*)(frame + 0x80))[1], ((float*)(frame + 0x80))[2]);
			if (p.len2() == 0.0f) continue;
			float d = (p - h.Pos()).len2();
			if (d < bestD) { bestD = d; best = e; bestP = p; }
		}
		Log("autotest: %u Romans; nearest visible to %s at (%.2f %.2f %.2f), %.1f away", n, HeroName(s.a),
			bestP.x, bestP.y, bestP.z, best ? sqrtf(bestD) : -1.0f);
		if (!best) break;
		at.watchEnemy = best;
		Vector3 face = bestP - h.Pos();
		face.y = 0;
		float fl = sqrtf(face.len2());
		face = fl > 0.01f ? face * (1.0f / fl) : Vector3(0, 0, 1);
		h.Dir() = face;
		float yaw = atan2f(face.z, face.x);
		if (yaw < 0) yaw += 6.2831855f;
		*(float*)(h.raw() + 0xCC) = yaw;
		*(float*)(h.raw() + 0xD4) = yaw;
		if (s.b == 3) break;   // only turn to face it
		if (s.b == 1) {   // the opposite side of the same Roman, facing it
			face = face * -1.0f;
			yaw = atan2f(face.z, face.x);
			if (yaw < 0) yaw += 6.2831855f;
			h.Dir() = face;
			*(float*)(h.raw() + 0xCC) = yaw;
			*(float*)(h.raw() + 0xD4) = yaw;
		}
		Vector3 to = bestP - face * (s.b == 2 ? 6.0f : 1.3f) + Vector3(0, 0.3f, 0);
		h.Pos() = to;
		if (h.hook->node && h.hook->node->matrix) {
			float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
			hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
		}
		break;
	}
	case S_SCATTER: {
		// Move every Roman within 40 units of hero `a` 300 units away (x, z), so
		// "enemies nearby" stops turning the action button into an attack.
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h) break;
		const int kIds[] = { 93, 148, 75, 95, 164, 167 };
		int moved = 0;
		for (int id : kIds) {
			unsigned n = GetNumLvlClassInstances(2, id);
			for (unsigned i = 0; i < n; i++) {
				char* e = (char*)GetLvlObject(2, id, i);
				char* node = e ? *(char**)(e + 0x10) : nullptr;
				char* frame = node ? *(char**)(node + 4) : nullptr;
				if (!frame) continue;
				float* ltm = (float*)(frame + 0x80);
				float dx = ltm[0] - h.Pos().x, dz = ltm[2] - h.Pos().z;
				if (dx * dx + dz * dz > 40.0f * 40.0f) continue;
				float* mp = (float*)(frame + 0x40);
				mp[0] += 300.0f; mp[2] += 300.0f; ltm[0] += 300.0f; ltm[2] += 300.0f;
				*(uint32_t*)(frame + 0x1C) &= ~0x20000u;   // let RW resync the frame
				moved++;
			}
		}
		Log("autotest: moved %d Romans away from %s", moved, HeroName(s.a));
		break;
	}
	case S_PLACE: {
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h || s.b < -1 || s.b >= kNumSpots) break;
		Vector3 to = s.b < 0 ? Vector3(s.x, s.y, s.z) : Vector3(kSpots[s.b][0], kSpots[s.b][1], kSpots[s.b][2]);
		h.Pos() = to;
		if (h.hook->node && h.hook->node->matrix) {
			float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
			hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
			// An AI companion (single player) goes by its world frame (+0x80) too:
			// placed by +0x40 alone, it stayed where it was.
			if (h.IsCompanion()) {
				hp = (float*)((char*)h.hook->node->matrix + 0x80);
				hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
			}
		}
		Log("autotest: %s placed at spot %d", HeroName(s.a), s.b);
		break;
	}
	case S_TRIOEV: {
		Trio t = GetTrio();
		if (!t) break;
		typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
		Log("autotest: trio event %04X -> %d", s.a,
			((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, s.a, nullptr));
		break;
	}
	case S_DETECT: {
		// CKSrvDetection (found by vtable among the yellow pages' services).
		CKYellowPages* yp = YellowPages();
		char* det = nullptr;
		for (int off = 0x10; off < 0xC0 && !det; off += 4) {
			char* o = *(char**)((char*)yp + off);
			if (o && !IsBadReadPtr(o, 4) && *(uintptr_t*)o == 0x62504C) { det = o; Log("autotest: detection service at yp+0x%X", off); }
		}
		if (!det) { Log("autotest: no detection service"); break; }
		unsigned n = *(uint16_t*)(det + 0x28);
		char** nodes = *(char***)(det + 0x18);
		Trio t = GetTrio();
		Log("autotest: %u detected objects; tests %u/%u/%u", n, *(uint16_t*)(det + 0x30), *(uint16_t*)(det + 0x38),
			*(uint16_t*)(det + 0x40));
		for (unsigned i = 0; i < n && i < 64; i++) {
			const char* who = "";
			for (int hI = 0; hI < HERO_COUNT; hI++) {
				Hero h = t ? t.GetHero(hI) : Hero();
				if (h && (char*)h.hook->node == nodes[i]) who = HeroName(hI);
			}
			Log("autotest:   detected %u: node %p vt %p %s", i, nodes[i], nodes[i] ? *(void**)nodes[i] : nullptr, who);
		}
		// Every test: list, shape centre, bound object, flags, event.
		struct L { const char* name; int base, count, shapes, stride; } ls[5] = {
			{ "box-enter", 0x2C, 0x30, 0x0C, 0x18 }, { "sphere-enter", 0x34, 0x38, 0x10, 0x14 },
			{ "plane-cross", 0x3C, 0x40, 0, 0 }, { "box-level", 0x44, 0x48, 0x0C, 0x18 },
			{ "sphere-level", 0x4C, 0x50, 0x10, 0x14 } };
		for (const L& l : ls) {
			char* e = *(char**)(det + l.base);
			unsigned cnt = *(uint16_t*)(det + l.count);
			for (unsigned i = 0; e && i < cnt && i < 200; i++, e += 8) {
				float c[3] = { 0, 0, 0 };
				if (l.shapes) {
					const float* sh = (const float*)(*(char**)(det + l.shapes) + *(uint16_t*)e * l.stride);
					if (l.stride == 0x18) { c[0] = (sh[0] + sh[3]) / 2; c[1] = (sh[1] + sh[4]) / 2; c[2] = (sh[2] + sh[5]) / 2; }
					else { c[0] = sh[0]; c[1] = sh[1]; c[2] = sh[2]; }
				}
				float r = 0;
				if (l.stride == 0x14) r = ((const float*)(*(char**)(det + l.shapes) + *(uint16_t*)e * l.stride))[3];
				Log("autotest:   %s %u: obj %u flags %02X/%02X event %04X at (%.0f %.0f %.0f) r %.1f", l.name, i,
				    *(uint16_t*)(e + 2), (uint8_t)e[4], (uint8_t)e[5], *(uint16_t*)(e + 6), c[0], c[1], c[2], r);
			}
		}
		break;
	}
	case S_CINE: {
		CKYellowPages* yp = YellowPages();
		char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
		char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
		unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
		if (!scenes) { Log("autotest: no cinematic service"); break; }
		if (s.a == -1) {
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			Log("autotest: %u cinematic scenes, service flags %08X, game manager +5C %08X", n, *(uint32_t*)(cin + 0xC),
			    gm ? *(uint32_t*)(gm + 0x5C) : 0);
			for (unsigned i = 0; i < n && i < 128; i++)
				Log("autotest:   scene %u %p: f20=%04X f30=%02X", i, scenes[i],
					*(uint16_t*)(scenes[i] + 0x20), *(uint8_t*)(scenes[i] + 0x30));
			break;
		}
		if (s.a == -5) {   // as Enter skips (CKSrvCinematic::vf17): playing scenes that end on Enter
			typedef void(__fastcall* SkipFn)(void* scene, void* edx);
			for (unsigned i = 0; i < n && i < 512; i++) {
				uint16_t f = *(uint16_t*)(scenes[i] + 0x20);
				if ((f & 1) && (f & 0x1000) && (f & 0x180) != 0x80 && *(int8_t*)(scenes[i] + 0x30) >= 0) {
					Log("autotest: skipping scene %u as Enter does (f20=%04X)", i, f);
					((SkipFn)0x461A80)(scenes[i], nullptr);
				}
			}
			break;
		}
		if (s.a == -4) {
			typedef void(__fastcall* SkipFn)(void* scene, void* edx);
			for (unsigned i = 0; i < n && i < 512; i++)
				if (*(uint16_t*)(scenes[i] + 0x20) & 1) {
					Log("autotest: skipping scene %u (f20=%04X)", i, *(uint16_t*)(scenes[i] + 0x20));
					((SkipFn)0x461A80)(scenes[i], nullptr);
				}
			break;
		}
		typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
		if (s.a == -3) {
			if (at.cineScene < 0) break;
			Log("autotest: stopping scene %d", at.cineScene);
			((SendEventFn)(*(void***)cin)[5])(cin, nullptr, 0x500 + (int)n + at.cineScene, nullptr);
			at.cineScene = -1;
			break;
		}
		int pick = -1;
		for (unsigned i = 0; i < n && pick < 0; i++)
			if ((*(uint8_t*)(scenes[i] + 0x30) & 1) && !(*(uint16_t*)(scenes[i] + 0x20) & 1)) pick = (int)i;
		if (pick < 0) { Log("autotest: no idle scene with bars"); break; }
		Log("autotest: starting scene %d (f20=%04X f30=%02X)", pick, *(uint16_t*)(scenes[pick] + 0x20),
			*(uint8_t*)(scenes[pick] + 0x30));
		((SendEventFn)(*(void***)cin)[5])(cin, nullptr, 0x500 + pick, nullptr);
		at.cineScene = pick;
		break;
	}
	case S_PROBE:
		at.probe = s.a != 0;
		break;
	case S_RELOAD: {   // the current level loaded again (as the Inspector's "go to level" does)
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		if (!gm) break;
		int cur = *(int*)(gm + ax::kGM_currentLevel);
		Log("autotest: level %d loaded again", cur);
		*(int*)(gm + ax::kGM_nextLevel) = cur;
		break;
	}
	case S_GIVE: {   // hero a gets a shield (b = 1) or x health points (b = 0), as a bonus gives them
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		if (!gm) break;
		typedef char(__fastcall* GmFn)(char* gm, void* edx, int arg);
		int prev = IndivEnter(s.a);
		if (s.b == 2 || s.b == 3) {   // the combo bar filled the game's way: the hero's "a hit landed" event (0x4C0E), 40 times (3: x times)
			Trio t = GetTrio();
			Hero h = t ? t.GetHero(s.a) : Hero();
			typedef int(__fastcall* SendFn)(void* self, void* edx, int event, void* param);
			int n = s.b == 3 ? (int)s.x : 40;
			for (int k = 0; h && k < n; k++) ((SendFn)(*(void***)h.raw())[5])(h.raw(), nullptr, 0x4C0E, nullptr);
		} else if (s.b == 1) ((GmFn)0x507210)(gm, nullptr, 5);
		else ((GmFn)0x4BB260)(gm, nullptr, (int)s.x);
		char* cb = *(char**)(gm + 0x40);
		Log("autotest: %s given %s -- health %d, %d shields", HeroName(s.a), s.b == 1 ? "a shield" : "health", *(uint8_t*)(cb + 0x50),
		    *(uint8_t*)(cb + 0x51));
		IndivLeave(prev);
		break;
	}
	case S_HINTTEST: {   // a hint on player a's box (-1 both): b 0 revive, 1 one line, 2 combo buttons (x lit), 3 three lines
		wchar_t text[200];
		int p = s.a < 0 ? 0 : s.a;
		switch (s.b) {
		case 0: swprintf(text, 200, L"%s is down!\nHold %s by him for 10 s to revive", HintHeroName(HERO_OBELIX), HintButtonName(p, HB_B)); break;
		case 1: swprintf(text, 200, L"%s: jump with %s", HintHeroName(HERO_ASTERIX), HintButtonName(p, HB_A)); break;
		case 2: {
			const HintButton seq[4] = { HB_A, HB_X, HB_B, HB_Y };
			wchar_t keys[120] = {};
			for (int i = 0; i < 4; i++) {
				if (i) wcscat(keys, HintNext());
				wcscat(keys, HintButtonName(p, seq[i], i >= (int)s.x));
			}
			wchar_t ic = IconComboGlyph(2);
			swprintf(text, 200, L"%c So many Romans: Twister!\n%s", ic ? ic : L' ', keys);
			break;
		}
		default: swprintf(text, 200, L"Line one of three\nline two is a bit longer\nthree"); break;
		}
		HintShow(HINT_COMBO, s.a, text, (DWORD)(s.y > 0 ? s.y : 6000));
		Log("autotest: hint %d on %s", s.b, s.a < 0 ? "both boxes" : s.a ? "player 2's box" : "player 1's box");
		break;
	}
	case S_HINTSPARK:   // a sparkle on button b of player a's test hint, with the sound
		HintSpark(HINT_COMBO, s.a, s.b, true);
		Log("autotest: sparkle on button %d of player %d's hint", s.b, s.a + 1);
		break;
	case S_COMBOHINT:   // combo a as if just bought, for both players (-1: the crowd's Twister hint again)
		ComboHintsQueue(s.a);
		break;
	case S_SPSWITCH:   // single player: the hero switch button pressed ([single_player] sp_hero_switch)
		Log("autotest: the hero switch button");
		CoopSoloSwitchRequest();
		break;
	case S_CAMFOV: {   // the camera's field of view and shake: the renderer's last fov (camera service +0x18 -> +0x28), the main camera's view window, the shake
		CKYellowPages* yp = YellowPages();
		char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
		char* r = cs ? *(char**)(cs + 0x18) : nullptr;
		const float* w = *(float**)0x662EB8 ? (const float*)(*(char**)0x662EB8 + 0x68) : nullptr;
		Log("autotest: camera: renderer fov %.2f, view window %.3f %.3f, service flags %08X, shake %u (length %.2f, strength %.2f, count %u)",
		    r ? *(float*)(r + 0x28) : -1.0f, w ? w[0] : -1.0f, w ? w[1] : -1.0f, cs ? *(uint32_t*)(cs + 0xC) : 0,
		    cs && (*(uint32_t*)(cs + 0xC) & 0x1000) ? 1 : 0, cs ? *(float*)(cs + 0x668) : 0.0f, cs ? *(float*)(cs + 0x670) : 0.0f,
		    cs ? *(uint32_t*)(cs + 0x680) : 0);
		break;
	}
	case S_SPMOD:   // single player: the mod's buttons a (PAD_* bits) with R3 held ([debug] cheats)
		Log("autotest: R3 + buttons %04X", s.a);
		CoopSoloModRequest(s.a);
		break;
	case S_HUDDIFF:
		HudDiffStart();
		break;
	case S_END:
		HudDiffReport();
		Finish("timeline complete");
		break;
	}
}

// Player 1's synthetic input: written over CKInput's freshly updated values, so
// both the real device path and the hero code see it as ordinary input.
void InjectP1() {
	if (!at.p1Active) return;
	InputManager im = GetInputManager();
	uint8_t* values = im.Values();
	int n = im.NumActions();
	if (!values || n <= 0) return;
	if (n > (int)kMaxActions) n = (int)kMaxActions;
	at.p1.pressed = (uint16_t)(at.p1.buttons & ~at.p1Prev);
	at.p1Prev = at.p1.buttons;
	uint8_t tmp[kMaxActions];
	BuildActionsFromPad(at.p1, tmp, n);
	for (int i = 0; i < n; i++)
		if (tmp[i]) values[i] = tmp[i];
	// B held: binding 6's hold time (+0x18, seconds), which CKInput times from a
	// real device only -- the revive's grab (CoopHoldTime) reads it.
	static DWORD s_bSince = 0;
	if (at.p1.buttons & PAD_B) {
		if (!s_bSince) s_bSince = GetTickCount();
		if (n > 6) *(float*)(im.Binding(6) + 0x18) = (GetTickCount() - s_bSince) / 1000.0f;
	} else {
		s_bSince = 0;
	}
	// The right stick: the game camera's input view (service +0x580) maps its
	// actions 0-3 (up, down, right, left) to bindings, as CoopP2CameraValues does.
	CKYellowPages* yp = YellowPages();
	char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	char* view = cs ? *(char**)(cs + 0x580) : nullptr;
	const uint8_t* remap = view ? *(const uint8_t**)(view + 8) : nullptr;
	auto put = [&](int action, float v) {
		if (remap && v > 0.0f && remap[action] < n) values[remap[action]] = (uint8_t)(fminf(v, 1.0f) * 255.0f + 0.5f);
	};
	put(0, at.p1.ry); put(1, -at.p1.ry);
	put(2, at.p1.rx); put(3, -at.p1.rx);
	if (at.p1Combo >= 0) {   // a combo's action (value numActions + k) for one frame
		values[im.NumActions() + at.p1Combo] = 0xFF;
		at.p1Combo = -1;
	}
}

// S_BIND: one binding of the game's values for one frame -- the menus'
// (interface left 18, right 19, confirm 20, cancel 21), which player 1's
// scripted pad does not fill.
void InjectBind() {
	if (at.bindFrames <= 0) return;
	at.bindFrames--;
	InputManager im = GetInputManager();
	uint8_t* values = im.Values();
	for (int i = 0; i < 3 && values; i++)
		if (at.bind[i] >= 0 && at.bind[i] < im.NumActions()) values[at.bind[i]] = 0xFF;
}

} // namespace

void AutotestInit() {
	char name[64] = {};
	if (!GetEnvironmentVariableA("XXL_AUTOTEST", name, sizeof(name)) || !name[0]) return;
	for (const Scenario& s : kScenarios) {
		if (_stricmp(s.name, name) == 0) {
			at.sc = &s;
			at.phase = AutotestState::WAIT;
			at.bootTick = GetTickCount();
			Log("autotest: scenario '%s' armed (%d steps)", s.name, s.count);
			return;
		}
	}
	Log("autotest: unknown scenario '%s' -- not running", name);
}

bool AutotestActive() { return at.phase != AutotestState::OFF; }

// In a remote desktop session the game's window stays white (screenshots too)
// until it is minimised and restored once (the user's finding): done 1.5 s and
// 5 s after the window is known.
void RemoteDesktopFlip(DWORD now) {
	static DWORD first = 0;
	static int done = 0;
	// (XXL_FLIPWINDOW=1: the same without a remote session -- white shots otherwise)
	if (!at.hwnd || done >= 2 || !(GetSystemMetrics(SM_REMOTESESSION) || getenv("XXL_FLIPWINDOW"))) return;
	if (!first) first = now;
	if (now - first < (done == 0 ? 1500u : 5000u)) return;
	done++;
	ShowWindow(at.hwnd, SW_MINIMIZE);
	ShowWindow(at.hwnd, SW_RESTORE);
	SetForegroundWindow(at.hwnd);
	Log("autotest: remote desktop session -- the window minimised and restored (%d)", done);
}

void AutotestFrame() {
	if (at.phase == AutotestState::OFF || at.phase == AutotestState::DONE) return;
	DWORD now = GetTickCount();
	SubclassWindow();
	RemoteDesktopFlip(now);

	if (at.phase == AutotestState::WAIT && _strnicmp(at.sc->name, "boot", 4) == 0) {
		// Boot scenarios (run with XXL_LEVEL=-1): the game's own start -- the run
		// starts with level 0's first cutscene, and nothing is pressed for it.
		CKYellowPages* yp = YellowPages();
		int level = (yp && yp->gameManager) ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
		char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
		char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
		unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
		bool playing = false;
		for (unsigned i = 0; scenes && i < n && i < 512; i++) playing |= (*(uint16_t*)(scenes[i] + 0x20) & 1) != 0;
		if (level != 0 || !playing) {
			if ((int)(now - at.bootTick) > 120000) Finish("level 0's cutscene never played");
			return;
		}
		at.phase = AutotestState::RUN;
		at.runStart = now;
		at.next = 0;
		Log("autotest: === run '%s' === (level 0's first cutscene, %.1f s after boot)", at.sc->name, (now - at.bootTick) / 1000.0f);
	}
	if (at.phase == AutotestState::WAIT) {
		bool live = CoopGameplayLive() && GetNumLvlClassInstances(ax::kCKHkObelix) > 0;
		// Only the forced level counts: the menu's attract-mode demo also has
		// live heroes, and a test run inside it would measure nothing useful.
		CKYellowPages* yp = YellowPages();
		int level = (yp && yp->gameManager) ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
		if (g_config.forceLevel >= 0 && level != g_config.forceLevel) live = false;
		if (!live) {
			at.liveSince = 0;
			// Without a game controller the menu opens on "No controller detected
			// ... continue with default keyboard control", which holds the forced
			// level back until somebody presses Enter.
			static DWORD lastEnter = 0;
			if (level <= 0 && (int)(now - at.bootTick) > 6000 && (int)(now - lastEnter) > 5000 &&
			    GetForegroundWindow() == at.hwnd) {
				lastEnter = now;
				Log("autotest: pressing Enter (no-controller notice?)");
				keybd_event(VK_RETURN, 0x1C, 0, 0);
				Sleep(60);
				keybd_event(VK_RETURN, 0x1C, KEYEVENTF_KEYUP, 0);
			}
			if ((int)(now - at.bootTick) > 120000) Finish("gameplay never became live");
			return;
		}
		if (!at.liveSince) {
			at.liveSince = now;
			Log("autotest: gameplay live, settling");
		}
		if ((int)(now - at.liveSince) < (at.sc->steps == kCine ? 0 : 4000)) return;
		at.phase = AutotestState::RUN;
		at.runStart = now;
		at.next = 0;
		Log("autotest: === run '%s' ===", at.sc->name);
	}

	int t = (int)(now - at.runStart);
	if (at.watchCine) {
		static uint16_t f20[64];
		static uint8_t  f30[64];
		static bool     seen[64];
		static int      lines = 0;
		CKYellowPages* yp = YellowPages();
		char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
		char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
		unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
		int level = (yp && yp->gameManager) ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
		for (unsigned i = 0; scenes && i < n && i < 64 && lines < 400; i++) {
			uint16_t a = *(uint16_t*)(scenes[i] + 0x20);
			uint8_t b = *(uint8_t*)(scenes[i] + 0x30);
			if (seen[i] && a == f20[i] && b == f30[i]) continue;
			seen[i] = true; f20[i] = a; f30[i] = b; lines++;
			Log("autotest: cine %.2f s: level %d scene %u %p f20=%04X f30=%02X running %p", t / 1000.0f, level, i, scenes[i], a, b,
			    *(void**)(scenes[i] + 0xC));
		}
	}
	while (at.phase == AutotestState::RUN && at.next < at.sc->count && at.sc->steps[at.next].t <= t)
		RunStep(at.sc->steps[at.next++]);
	if (getenv("XXL_CATTRACE")) {
		// XXL_CATTRACE: Roman catapults (CKHkCatapult 2,130) -- its fields once, its
		// state (+0x14 flags, +0x1A, charge timer +0x70 of +0x74) as it changes, the
		// leader's distance (x / z) and each hero's health drop.
		static bool s_dumped[8];
		static uint32_t s_key[8];
		static int s_hp[2] = { -1, -1 }, s_lines = 0;
		Trio tr = GetTrio();
		Hero lead = tr ? tr.GetHero(tr.Leader()) : Hero();
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, 130); i < n && i < 8 && s_lines < 600; i++) {
			char* c = (char*)GetLvlObject(2, 130, i);
			char* node = c ? *(char**)(c + 0x10) : nullptr;
			char* fr = node ? *(char**)(node + 4) : nullptr;
			if (!fr) continue;
			const float* at = (const float*)(fr + 0x80);
			if (!s_dumped[i]) {
				s_dumped[i] = true;
				s_lines++;
				Log("autotest: catapult #%u at (%.1f %.1f %.1f): +2C %.2f +48 %.2f +4C %.2f +50 %.2f +54 %.2f +58 %.2f +5C %.2f "
				    "+60 %.2f +64 %.2f +68 %.2f +6C %.2f +74 %.2f, +18 %02X +19 %d +1B %02X +1C %02X +1D %02X", i, at[0], at[1],
				    at[2], *(float*)(c + 0x2C), *(float*)(c + 0x48), *(float*)(c + 0x4C), *(float*)(c + 0x50),
				    *(float*)(c + 0x54), *(float*)(c + 0x58), *(float*)(c + 0x5C), *(float*)(c + 0x60), *(float*)(c + 0x64),
				    *(float*)(c + 0x68), *(float*)(c + 0x6C), *(float*)(c + 0x74), *(uint8_t*)(c + 0x18),
				    *(int8_t*)(c + 0x19), *(uint8_t*)(c + 0x1B), *(uint8_t*)(c + 0x1C), *(uint8_t*)(c + 0x1D));
			}
			uint32_t key = *(uint8_t*)(c + 0x14) | *(uint8_t*)(c + 0x1A) << 8 | (*(float*)(c + 0x70) < 0.05f) << 16;
			if (key != s_key[i] && lead) {
				s_key[i] = key;
				s_lines++;
				float dx = lead.Pos().x - at[0], dz = lead.Pos().z - at[2];
				Log("autotest: catapult #%u %.2f s: flags %02X state %u charge %.2f / %.2f, leader %.1f off", i, t / 1000.0f,
				    *(uint8_t*)(c + 0x14), *(uint8_t*)(c + 0x1A), *(float*)(c + 0x70), *(float*)(c + 0x74),
				    sqrtf(dx * dx + dz * dz));
			}
		}
		for (int k = 0; k < 2; k++) {
			int hp, sh;
			if (!IndivStats(k, &hp, &sh)) continue;
			if (s_hp[k] >= 0 && hp < s_hp[k] && s_lines < 600) {
				s_lines++;
				Log("autotest: catapult trace %.2f s: %s hit, health %d -> %d", t / 1000.0f, HeroName(k), s_hp[k], hp);
			}
			s_hp[k] = hp;
		}
	}
	{
		// XXL_DOWNAT=<hero>:<ms>: that hero (0 Asterix, 1 Obelix) goes down that long
		// into the run (individual heroes): any scenario with a fallen ally.
		// XXL_DOWNAT2 likewise for a second one (both down: the game over).
		static int s_downHero[2] = { -2, -2 }, s_downAt[2] = {};
		for (int k = 0; k < 2; k++) {
			if (s_downHero[k] == -2) {
				const char* e = getenv(k ? "XXL_DOWNAT2" : "XXL_DOWNAT");
				s_downHero[k] = -1;
				if (e && sscanf(e, "%d:%d", &s_downHero[k], &s_downAt[k]) != 2) s_downHero[k] = -1;
			}
			if (s_downHero[k] >= 0 && t >= s_downAt[k]) {
				int hero = s_downHero[k];
				s_downHero[k] = -1;
				int prev = IndivEnter(hero);
				CKYellowPages* yp = YellowPages();
				char* gm = yp ? (char*)yp->gameManager : nullptr;
				char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
				if (cb) *(uint8_t*)(cb + 0x50) = 0;
				IndivLeave(prev);
				bool down = IndivHeroDown(hero, "autotest XXL_DOWNAT");
				Log("autotest: %s put down (%s)", HeroName(hero), down ? "fallen" : "not fallen");
			}
		}
		// XXL_HPAT=<hero>:<ms>:<hp>: that hero's health set then (low health: the HUD's pulse).
		static int s_hpHero = -2, s_hpAt = 0, s_hp = 0;
		if (s_hpHero == -2) {
			const char* e = getenv("XXL_HPAT");
			s_hpHero = -1;
			if (e && sscanf(e, "%d:%d:%d", &s_hpHero, &s_hpAt, &s_hp) != 3) s_hpHero = -1;
		}
		if (s_hpHero >= 0 && t >= s_hpAt) {
			int hero = s_hpHero;
			s_hpHero = -1;
			int prev = IndivEnter(hero);
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
			if (cb) *(uint8_t*)(cb + 0x50) = (uint8_t)s_hp;
			if (gm) *(uint32_t*)(gm + 0x5C) |= 0x4000;   // the HUD shows it
			IndivLeave(prev);
			Log("autotest: %s's health set to %d", HeroName(hero), s_hp);
		}
		// XXL_UPAT=<ms>: the fallen hero revived beside the other one then.
		static int s_upAt = -2;
		if (s_upAt == -2) s_upAt = getenv("XXL_UPAT") ? atoi(getenv("XXL_UPAT")) : -1;
		if (s_upAt >= 0 && t >= s_upAt) {
			s_upAt = -1;
			int down = IndivFallenHero();
			if (down >= 0) IndivReviveAtLauncher(down ^ 1);
			Log("autotest: revive at XXL_UPAT (%s was down)", down >= 0 ? HeroName(down) : "nobody");
		}
	}
	if (at.phase != AutotestState::RUN) return;

	RunGlides(now);
	InjectP1();
	InjectBind();
	HudDiffFrame();
	// XXL_GATESHOTS: shots as the fight gate's counter (interface +0x24 bits
	// 17-18) slides in and when it goes (state 3, its sparkle); the run ends 3 s
	// after it has gone.
	static const bool gateShots = getenv("XXL_GATESHOTS") != nullptr;
	if (gateShots) {
		static unsigned lastState = 0, shotN = 0;
		static int since = -1, endAt = -1;
		char* itf = (char*)GetLvlObject(2, 132, 0);
		unsigned st = itf ? (*(uint32_t*)(itf + 0x24) >> 17) & 3 : 0;
		if (st != lastState) {
			Log("autotest: gate counter state %u -> %u", lastState, st);
			if (st == 1 || st == 3) since = t;
			if (st == 0 && lastState == 3) endAt = t + 3000;
			lastState = st;
		}
		static const int kAt[] = { 0, 150, 300, 500, 800, 1200, 1700 };
		if (since >= 0 && shotN < 7 && t - since >= kAt[shotN]) {
			char name[40];
			_snprintf_s(name, sizeof(name), _TRUNCATE, "gate_s%u_%d", st, kAt[shotN]);
			Shot(name);
			if (++shotN == 7) { shotN = 0; since = -1; }
		}
		if (endAt >= 0 && t >= endAt) { Finish("gate counter gone"); return; }
	}
	if ((int)(now - at.lastLog) >= 100) {
		at.lastLog = now;
		LogTick(t / 1000.0f);
	}
}

void AutotestBeforeLifeTick(int heroIndex, Hero hero) {
	if (at.phase != AutotestState::RUN || heroIndex != at.forceHero) return;
	uint8_t f = hero.Flags18();
	if (!HeroModePlayable(f) || HeroIsPlayerVariant(f)) return;
	if (at.forceReverts++ < 20)
		Log("autotest: %s f18=%02X before its tick -> forcing player variant",
			HeroName(heroIndex), f);
	*(uint8_t*)(hero.raw() + 0x18) = HeroPlayerVariant(f);
}

void AutotestAfterLifeTick(int heroIndex, Hero hero) {
	(void)heroIndex; (void)hero;
}
