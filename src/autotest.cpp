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

enum StepKind { S_MARK, S_SHOT, S_FORCE, S_P1, S_VPAD, S_END };

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
	SHOT(16000, "p2_04_end"),
	END(16500),
};

// idle: just boot to gameplay, log and screenshot. Smoke test for the harness.
const Step kIdle[] = {
	MARK(0, "baseline"), SHOT(100, "idle_00"), SHOT(3000, "idle_01"), END(3500),
};

struct Scenario { const char* name; const Step* steps; int count; };
#define SCENARIO(n, arr) { n, arr, (int)(sizeof(arr) / sizeof(arr[0])) }
const Scenario kScenarios[] = {
	SCENARIO("idle", kIdle),
	SCENARIO("exp1", kExp1),
	SCENARIO("p2basic", kP2Basic),
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
	CreateDirectoryA("shots", nullptr);
	char path[MAX_PATH];
	_snprintf_s(path, sizeof(path), _TRUNCATE, "shots\\%s.png", name);
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
	case S_MARK:
		Log("autotest: --- %s ---", s.name);
		CoopLogState(s.name);
		break;
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
