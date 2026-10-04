#include "autotest.h"
#include "coop.h"
#include "config.h"
#include "inputmgr.h"
#include "life.h"
#include "log.h"
#include "p2input.h"
#include "pads.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum StepKind { S_MARK, S_SHOT, S_FORCE, S_P1, S_VPAD, S_VPADR, S_TELEPORT, S_BONUS, S_PROBE, S_BUMPER,
                S_CRATE, S_ENEMY, S_CINE, S_DETECT, S_TRIOEV, S_PLACE, S_SCATTER, S_END };

struct Step {
	int         t;        // ms after the scenario starts
	StepKind    kind;
	int         a;        // S_FORCE: hero index; S_VPAD: XInput slot
	int         b;        // S_FORCE: 1 = hold in player state, 0 = release
	uint16_t    buttons;  // S_P1 / S_VPAD
	float       x, y;     // S_P1 / S_VPAD left stick
	const char* name;
};

#define MARK(t, n)                  { t, S_MARK,  0, 0, 0, 0, 0, n }
#define SHOT(t, n)                  { t, S_SHOT,  0, 0, 0, 0, 0, n }
#define FORCE(t, hero, on)          { t, S_FORCE, hero, on, 0, 0, 0, on ? "force player" : "release force" }
#define P1(t, btn, x, y, n)         { t, S_P1,    0, 0, btn, x, y, n }
#define VPAD(t, slot, btn, x, y, n) { t, S_VPAD,  slot, 0, btn, x, y, n }
#define END(t)                      { t, S_END,   0, 0, 0, 0, 0, "end" }
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
// Turn the cutscene probe (extra state every log tick) on or off.
#define PROBE(t, on)                { t, S_PROBE, on, 0, 0, 0, 0, "probe" }
// Right stick of virtual pad `slot` (camera look).
#define VPADR(t, slot, x, y, n)     { t, S_VPADR, slot, 0, 0, x, y, n }
// Puts hero `hero` next to lifter `lifter` (offset dx/dz, 1.5 units up).
#define TELEPORT(t, hero, lifter, dx, dz) { t, S_TELEPORT, hero, lifter, 0, dx, dz, "teleport" }

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
const Step kSurvey[] = {
	MARK(0, "survey"), { 0, S_DETECT, 0, 0, 0, 0, 0, "detect" }, SHOT(100, "survey"), END(300),
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

struct Scenario { const char* name; const Step* steps; int count; };
#define SCENARIO(n, arr) { n, arr, (int)(sizeof(arr) / sizeof(arr[0])) }
const Scenario kScenarios[] = {
	SCENARIO("idle", kIdle),
	SCENARIO("survey", kSurvey),
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
	SCENARIO("swingsp", kSwingSp),
	SCENARIO("cutscene", kCutscene),
	SCENARIO("exp1", kExp1),
	SCENARIO("p2basic", kP2Basic),
	SCENARIO("p2mirror", kP2Mirror),
	SCENARIO("p2busy", kP2Busy),
};

struct AutotestState {
	const Scenario* sc = nullptr;
	enum Phase { OFF, WAIT, RUN, DONE } phase = OFF;
	DWORD bootTick = 0, liveSince = 0, runStart = 0, lastLog = 0;
	int   next = 0;
	int   forceHero = -1;
	int   forceReverts = 0;

	bool     p1Active = false;
	PadState p1;
	uint16_t p1Prev = 0;
	PadState vpad[kMaxPads];

	HWND    hwnd = nullptr;
	WNDPROC prevProc = nullptr;
	char*   watchBonus = nullptr;   // CKHkBasicBonus hook being watched
	char*   watchCrate = nullptr;   // CKHkCrate chosen by NEAR_CRATE
	char*   watchEnemy = nullptr;   // CKHkBasicEnemy chosen by NEAR_ENEMY
	int     cineScene = -1;         // scene started by CINE_START
	bool    probe = false;          // log cutscene-related engine state each tick
} at;

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

void LogTick(float t) {
	Trio tr = GetTrio();
	if (!tr) return;
	char buf[512];
	LineBuf line(buf);
	line.Add("at %6.2f", t);
	for (int i = HERO_ASTERIX; i <= HERO_OBELIX; i++) {
		Hero h = tr.GetHero(i);
		if (!h) continue;
		line.Add(" | %c f18=%02X L=%02X sit=%02X body=%02X ai=%02X pos=(%7.2f %6.2f %7.2f)",
			i == HERO_ASTERIX ? 'A' : 'O', h.Flags18(), LifeLastState(i),
			*(uint8_t*)(h.raw() + 0x3E9), *(uint8_t*)(h.raw() + 0xE5),
			*(uint8_t*)(h.raw() + 0x561), h.Pos().x, h.Pos().y, h.Pos().z);
	}
	line.Add(" | lead=%d p2=%s", tr.Leader(), CoopP2Joined() ? HeroName(CoopP2Hero()) : "-");
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
	if (at.watchBonus) {
		char* node = *(char**)(at.watchBonus + 0x10);
		char* life = *(char**)(at.watchBonus + 0x0C);
		typedef char(__fastcall* BoolFn)(void* self, void* edx);
		char following = ((BoolFn)(*(void***)at.watchBonus)[19])(at.watchBonus, nullptr);
		line.Add(" | bonus hidden=%d following=%d target=%p", node ? (*(uint8_t*)(node + 0x10) & 2) != 0 : -1,
			following, *(void**)(at.watchBonus + 0x20));
		(void)life;
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
				pools++;
				for (char* b = *(char**)(pool + listOff); b; b = *(char**)(b + 0x14)) {
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
		if (s.b == 1) {   // the opposite side of the same Roman, facing it
			face = face * -1.0f;
			yaw = atan2f(face.z, face.x);
			if (yaw < 0) yaw += 6.2831855f;
			h.Dir() = face;
			*(float*)(h.raw() + 0xCC) = yaw;
			*(float*)(h.raw() + 0xD4) = yaw;
		}
		Vector3 to = bestP - face * 1.3f + Vector3(0, 0.3f, 0);
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
		// Fixed spots (level 2), from players' logs.
		static const float kSpots[][3] = {
			{ -793.89f, 512.30f, -1060.49f },   // 0: the cable car (swing) seat
			{ -794.40f, 508.40f, -1060.40f },   // 1: next to the cable car's rope
			{ -790.45f, 510.60f, -1059.94f },   // 2: just before the cable car
		};
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(s.a) : Hero();
		if (!h || s.b < 0 || s.b >= 3) break;
		Vector3 to(kSpots[s.b][0], kSpots[s.b][1], kSpots[s.b][2]);
		h.Pos() = to;
		if (h.hook->node && h.hook->node->matrix) {
			float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
			hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
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
		break;
	}
	case S_CINE: {
		CKYellowPages* yp = YellowPages();
		char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
		char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
		unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
		if (!scenes) { Log("autotest: no cinematic service"); break; }
		if (s.a == -1) {
			Log("autotest: %u cinematic scenes, service flags %08X", n, *(uint32_t*)(cin + 0xC));
			for (unsigned i = 0; i < n && i < 128; i++)
				Log("autotest:   scene %u %p: f20=%04X f30=%02X", i, scenes[i],
					*(uint16_t*)(scenes[i] + 0x20), *(uint8_t*)(scenes[i] + 0x30));
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
	case S_END:
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

void AutotestFrame() {
	if (at.phase == AutotestState::OFF || at.phase == AutotestState::DONE) return;
	DWORD now = GetTickCount();
	SubclassWindow();

	if (at.phase == AutotestState::WAIT) {
		bool live = CoopGameplayLive() && GetNumLvlClassInstances(ax::kCKHkObelix) > 0;
		// Only the forced level counts: the menu's attract-mode demo also has
		// live heroes, and a test run inside it would measure nothing useful.
		CKYellowPages* yp = YellowPages();
		int level = (yp && yp->gameManager) ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
		if (g_config.forceLevel >= 0 && level != g_config.forceLevel) live = false;
		if (!live) {
			at.liveSince = 0;
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
	while (at.phase == AutotestState::RUN && at.next < at.sc->count && at.sc->steps[at.next].t <= t)
		RunStep(at.sc->steps[at.next++]);
	if (at.phase != AutotestState::RUN) return;

	InjectP1();
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
