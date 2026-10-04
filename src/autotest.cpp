#include "autotest.h"
#include "coop.h"
#include "config.h"
#include "inputmgr.h"
#include "life.h"
#include "log.h"
#include "p2input.h"
#include "pads.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum StepKind { S_MARK, S_SHOT, S_FORCE, S_P1, S_VPAD, S_TELEPORT, S_END };

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

// survey: log which group-action objects the level has, then leave.
const Step kSurvey[] = {
	MARK(0, "survey"), SHOT(100, "survey"), END(300),
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
		if ((int)(now - at.liveSince) < 4000) return;
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
