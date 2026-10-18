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

#include <cmath>
#include <cstdarg>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum StepKind { S_MARK, S_SHOT, S_FORCE, S_P1, S_VPAD, S_VPADR, S_TELEPORT, S_BONUS, S_PROBE, S_BUMPER,
                S_CRATE, S_ENEMY, S_CINE, S_DETECT, S_TRIOEV, S_PLACE, S_SCATTER, S_KEY, S_KEYMAP, S_SECTORS, S_GLIDE, S_LOADSECTOR, S_POTION, S_PLATES, S_DASHFLAG, S_SETF18, S_REGIONS, S_POKE8, S_EVGROUP, S_MUSICGROUPS, S_WATCHGROUPS, S_WATCHCLUE, S_TORCH, S_LEVERS, S_WATCHCAM, S_HEROFIELDS, S_WATCHRW, S_NEAROBJ, S_INPUTDUMP, S_WATCHBEACONS, S_P2LOOK, S_FINDGROUPS, S_WATCHGROUP2, S_P1COMBO, S_WATCHENEMIES, S_NODEDUMP, S_CAMDUMP, S_BOSSSCAN, S_SETPRESET, S_NEXTTO, S_HUDDIFF, S_GIVE, S_RELOAD, S_OBJEV, S_END };

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
// Turn the cutscene probe (extra state every log tick) on or off.
#define PROBE(t, on)                { t, S_PROBE, on, 0, 0, 0, 0, "probe" }
// Right stick of virtual pad `slot` (camera look).
#define VPADR(t, slot, x, y, n)     { t, S_VPADR, slot, 0, 0, x, y, n }
// Puts hero `hero` next to lifter `lifter` (offset dx/dz, 1.5 units up).
#define TELEPORT(t, hero, lifter, dx, dz) { t, S_TELEPORT, hero, lifter, 0, dx, dz, "teleport" }
// Hero `hero` put at / moved frame by frame to a point given by its coordinates.
#define PLACEAT(t, hero, X, Y, Z)       { t, S_PLACE, hero, -1, 0, X, Y, "place", Z }
#define GLIDEAT(t, hero, X, Y, Z, ms)   { t, S_GLIDE, hero, -1, ms, X, Y, "glide", Z }

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
// has to suspend as for the leader, and Asterix lead again after the landing.
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
	SCENARIO("spya", kSpyA),
	SCENARIO("spy2o", kSpy2O),
	SCENARIO("spywalk", kSpyWalk),
	SCENARIO("sas", kSas),
	SCENARIO("sectorzone", kSectorZone),
	SCENARIO("torch", kTorch),
	SCENARIO("torchgrab", kTorchGrab),
	SCENARIO("lever", kLever),
	SCENARIO("launchp2", kLaunchP2),
	SCENARIO("launcha", kLaunchA),
	SCENARIO("lvl1", kLvl1),
	SCENARIO("lvl1obelix", kLvl1Obelix),
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
	SCENARIO("boss36", kBoss36),
	SCENARIO("sky2", kSky2),
	SCENARIO("shadow33", kShadow33),
	SCENARIO("shadow3s", kShadow3S),
	SCENARIO("shadow29", kShadow29),
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
	SCENARIO("fog21", kFog21),
	SCENARIO("spy21o", kSpy21O),
	SCENARIO("spy21a", kSpy21A),
	SCENARIO("mgun3a", kMgun3A),
	SCENARIO("mgun3n", kMgun3N),
	SCENARIO("mgun3o", kMgun3O),
	SCENARIO("mgun33o", kMgun33O),
	SCENARIO("mgun33a", kMgun33A),
	SCENARIO("mgun33j", kMgun33J),
	SCENARIO("boat49o", kBoat49O),
	SCENARIO("boat49a", kBoat49A),
	SCENARIO("hintjoin", kHintJoin),
	SCENARIO("spy3n", kSpy3N),
	SCENARIO("spy3p", kSpy3P),
	SCENARIO("catapultp", kCatapultP),
	SCENARIO("catapulta", kCatapultA),
	SCENARIO("shop", kShop),
	SCENARIO("shopo", kShopO),
	SCENARIO("shopox", kShopOX),
	SCENARIO("shopsplit", kShopSplit),
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
	SCENARIO("shields6", kShields6),
	SCENARIO("shieldfull", kShieldFull),
	SCENARIO("hudorder", kHudOrder),
	SCENARIO("shopspy", kShopSpy),
	SCENARIO("spyonly", kSpyOnly),
	SCENARIO("shopspyw", kShopSpyW),
	SCENARIO("fight4h", kFight4H),
	SCENARIO("lock4", kLock4),
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
	SCENARIO("shopboss", kShopBoss),
	SCENARIO("combodrain", kComboDrain),
	SCENARIO("shopzone", kShopZone),
	SCENARIO("shopsave", kShopSave),
	SCENARIO("cabled33", kCableD33),
	SCENARIO("corkdown", kCorkDown),
	SCENARIO("beamundo", kBeamUndo),
	SCENARIO("liftd58", kLiftD58),
	SCENARIO("reload2", kReload2),
	SCENARIO("fight64", kFight64),
	SCENARIO("twistroman", kTwistRoman),
	SCENARIO("twistromana", kTwistRomanA),
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
	SCENARIO("cam32s", kCam32S),
	SCENARIO("fight4o", kFight4O),
	SCENARIO("camfight", kCamFight),
	SCENARIO("fight4a", kFight4A),
	SCENARIO("fight36", kFight36),
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
	bool    watchBeacons = false;   // beacon bonuses near beaconPoint, WATCHBEACONS
	bool    watchEnemies = false;   // Romans and squads near enemyPoint, WATCHENEMIES
	bool    enemiesPrimed = false;
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
	}
	line.Add(" | lead=%d p2=%s", tr.Leader(), CoopP2Joined() ? HeroName(CoopP2Hero()) : "-");
	{   // shared health (game manager +0x40 -> +0x50 points, +0x51 shields) and each hero's dead bit (+0x390 bit 2)
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
		Hero a = tr.GetHero(HERO_ASTERIX), o = tr.GetHero(HERO_OBELIX);
		if (cb) line.Add(" | hp=%d/%d dead=%d%d", *(uint8_t*)(cb + 0x50), *(uint8_t*)(cb + 0x51),
		                 a ? (*(uint8_t*)(a.raw() + 0x390) >> 2) & 1 : 9, o ? (*(uint8_t*)(o.raw() + 0x390) >> 2) & 1 : 9);
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
	if (at.watchEnemies) {   // live Romans near the point; squads near it (flags, member groups)
		int live = 0;
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
		line.Add(" | romans near %d", live);
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
				__try {
					typedef char** (__fastcall* ListFn)(char* self, void* edx);
					char** head = ((ListFn)(*(void***)pool)[0x30 / 4])(pool, nullptr);
					for (char* e = head ? *head : nullptr; e && nFree < 500; e = *(char**)(e + 4)) nFree++;
					for (char* e = *(char**)(pool + 0xC); e && nHeld < 500; e = *(char**)(e + 4)) nHeld++;
				} __except (EXCEPTION_EXECUTE_HANDLER) { nFree = -1; }
				Log("autotest: Roman pool %u %p: %d free, %d in its +0C list", i, pool, nFree, nHeld);
			}
		for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
			char* sq = (char*)GetLvlObject(4, 26, i);
			if (!sq) continue;
			const float* f = (const float*)sq;
			Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
			if ((c - at.enemyPoint).len2() > 60.0f * 60.0f) continue;
			line.Add(" sq%u %p f%04X", i, sq, *(uint16_t*)(sq + 0x152));
			if (!primed)
				Log("autotest: squad %u %p box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f", i, sq, f[0x4A], f[0x47], f[0x4B],
				    f[0x48], f[0x4C], f[0x49]);
			unsigned ng = *(unsigned*)(sq + 0x14C);
			char* gr = *(char**)(sq + 0x144);
			for (unsigned g2 = 0; gr && g2 < ng && g2 < 4; g2++) {
				char* e = gr + g2 * 0x1C;
				line.Add(" [%u %u %u %u %d]", *(uint8_t*)(e + 0x11), *(uint8_t*)(e + 0x14), *(uint8_t*)(e + 0x15),
				         *(uint16_t*)(e + 0x16), *(int16_t*)(e + 0x18));
			}
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
	case S_NEAROBJ: {   // hero a put s.x units from the nearest object of class (2, b), facing it
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
			if (!fr) continue;
			Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
			Log("autotest:   class (2,%d) #%u %p at (%.1f %.1f %.1f)", s.b, i, o, p.x, p.y, p.z);
			float d = (p - h.Pos()).len2();
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
	case S_WATCHBEACONS: at.watchBeacons = true; at.beaconPoint = Vector3(s.x, s.y, s.z); break;
	case S_WATCHENEMIES: at.watchEnemies = true; at.enemyPoint = Vector3(s.x, s.y, s.z); break;
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
	case S_SETPRESET: {   // the game's controller preset a (CKInput +0x64 via FUN_00438950)
		CKYellowPages* yp = YellowPages();
		char* in = yp ? *(char**)((char*)yp + 0x14) : nullptr;
		if (!in) break;
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
		if (!h || s.a < 0 || s.a >= HERO_COUNT || s.b < -1 || s.b >= kNumSpots) break;
		auto& g = at.glide[s.a];
		g.on = true;
		g.from = h.Pos();
		g.to = s.b < 0 ? Vector3(s.x, s.y, s.z) : Vector3(kSpots[s.b][0], kSpots[s.b][1], kSpots[s.b][2]);
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
		if (s.b == 2) {   // the combo bar filled the game's way: the hero's "a hit landed" event (0x4C0E), 40 times
			Trio t = GetTrio();
			Hero h = t ? t.GetHero(s.a) : Hero();
			typedef int(__fastcall* SendFn)(void* self, void* edx, int event, void* param);
			for (int k = 0; h && k < 40; k++) ((SendFn)(*(void***)h.raw())[5])(h.raw(), nullptr, 0x4C0E, nullptr);
		} else if (s.b == 1) ((GmFn)0x507210)(gm, nullptr, 5);
		else ((GmFn)0x4BB260)(gm, nullptr, (int)s.x);
		char* cb = *(char**)(gm + 0x40);
		Log("autotest: %s given %s -- health %d, %d shields", HeroName(s.a), s.b == 1 ? "a shield" : "health", *(uint8_t*)(cb + 0x50),
		    *(uint8_t*)(cb + 0x51));
		IndivLeave(prev);
		break;
	}
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
	if (at.p1Combo >= 0) {   // a combo's action (value numActions + k) for one frame
		values[im.NumActions() + at.p1Combo] = 0xFF;
		at.p1Combo = -1;
	}
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
	while (at.phase == AutotestState::RUN && at.next < at.sc->count && at.sc->steps[at.next].t <= t)
		RunStep(at.sc->steps[at.next++]);
	{
		// XXL_DOWNAT=<hero>:<ms>: that hero (0 Asterix, 1 Obelix) goes down that long
		// into the run (individual heroes): any scenario with a fallen ally.
		static int s_downHero = -2, s_downAt = 0;
		if (s_downHero == -2) {
			const char* e = getenv("XXL_DOWNAT");
			s_downHero = -1;
			if (e && sscanf(e, "%d:%d", &s_downHero, &s_downAt) != 2) s_downHero = -1;
		}
		if (s_downHero >= 0 && t >= s_downAt) {
			int hero = s_downHero;
			s_downHero = -1;
			int prev = IndivEnter(hero);
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
			if (cb) *(uint8_t*)(cb + 0x50) = 0;
			IndivLeave(prev);
			bool down = IndivHeroDown(hero, "autotest XXL_DOWNAT");
			Log("autotest: %s put down (%s)", HeroName(hero), down ? "fallen" : "not fallen");
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
	HudDiffFrame();
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
