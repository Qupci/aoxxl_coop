#include "shared.h"
#include "coop.h"
#include "detour.h"
#include "hints.h"
#include "inputmgr.h"
#include "life.h"
#include "log.h"
#include "trio.h"

#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

typedef uint8_t(__fastcall* GetActionFn)(char* view, void* edx, uint32_t action);
const uintptr_t kGetAction = 0x4358B0;
const uint8_t   kGetActionPrologue[7] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x51, 0x08 };
GetActionFn g_origGetAction = nullptr;

// The input maps by name (strings in the game's data).
enum MapId { MAP_TRIO, MAP_GUN, MAP_BOAT, MAP_CAMERA, MAP_SEKENSOR, MAP_MENU, MAP_COUNT, MAP_OTHER = MAP_COUNT };
const uintptr_t kMapName[MAP_COUNT] = { 0x64D05C, 0x64F550, 0x650F7C, 0x64713C, 0x64743C, 0x64F28C };
const char* const kMapLabel[MAP_COUNT + 1] = { "trio", "gun", "boat", "camera", "sekensor", "menu", "other" };

struct State {
	char*    input = nullptr;               // CKInput the bases were read from
	char*    base[MAP_COUNT] = {};          // first view of each map
	char*    end[MAP_COUNT] = {};           // the next map's first view
	uint8_t* p1 = nullptr;                  // the game's own values (player 1's pad)
	const uint8_t* p2 = nullptr;            // player 2's hero values
	const uint8_t* p2cam = nullptr;         // player 2's values for the camera map
	SharedMode mode = SHARED_NONE;
	int      owner[2] = { -1, -1 };         // player (0 = P1, 1 = P2) driving Asterix / Obelix
	bool     joined = false;
	int      logActions = -1;               // XXL_ACTIONLOG
} g;

int MapOf(const char* view) {
	for (int m = 0; m < MAP_COUNT; m++)
		if (g.base[m] && view >= g.base[m] && view < g.end[m]) return m;
	return MAP_OTHER;
}

void ReadMaps(char* in) {
	g.input = in;
	memset(g.base, 0, sizeof(g.base));
	if (!in) return;
	typedef uint32_t(__fastcall* FindMapFn)(char* in, void* edx, const void* name);
	char** maps = *(char***)(in + 0x40);
	unsigned n = *(uint8_t*)(in + 0x38);
	for (int m = 0; m < MAP_COUNT && maps; m++) {
		uint32_t i = ((FindMapFn)0x437FF0)(in, nullptr, (const void*)kMapName[m]) & 0xFF;
		if (i < n) g.base[m] = maps[i];
	}
	// A map's views run up to the next map's (at most 8).
	for (int m = 0; m < MAP_COUNT; m++) {
		if (!g.base[m]) continue;
		g.end[m] = g.base[m] + 8 * 0x10;
		for (unsigned i = 0; i < n; i++)
			if (maps[i] > g.base[m] && maps[i] < g.end[m]) g.end[m] = maps[i];
	}
}

// Diagnostics (XXL_ACTIONLOG=1): each (caller, map, action) once, and each
// (map, action) the first time it reads non-zero.
void LogAction(const char* view, uint32_t action, uint8_t v, void* caller) {
	struct Seen { void* caller; int map; uint32_t action; };
	static Seen seen[512];
	static int nSeen = 0;
	static uint8_t lit[MAP_COUNT + 1][64];
	int map = MapOf(view);
	uint8_t idx = (*(uint8_t**)(view + 8))[action & 0xFF];
	bool known = false;
	for (int i = 0; i < nSeen && !known; i++)
		known = seen[i].caller == caller && seen[i].map == map && seen[i].action == action;
	if (!known && nSeen < 512) {
		seen[nSeen++] = { caller, map, action };
		Log("shared: GetAction %s[%d] (view %p +%d) -> value %d from %p", kMapLabel[map], (int)action, view,
		    map < MAP_COUNT ? (int)(view - g.base[map]) / 0x10 : -1, idx, caller);
	}
	if (v && (action & 0xFF) < 64 && !lit[map][action & 0x3F]) {
		lit[map][action & 0x3F] = 1;
		Log("shared: %s[%d] (value %d) first read as %d from %p", kMapLabel[map], (int)action, idx, v, caller);
	}
}

// Which player answers (map, action) in the current mode: 0 = player 1,
// 1 = player 2, 2 = either (the larger value), -1 = leave it to the game.
int Route(int map, uint32_t action) {
	int ast = g.owner[HERO_ASTERIX], obe = g.owner[HERO_OBELIX];
	// The merchant's words and menu: the shopper's alone (the other player plays
	// on meanwhile -- an X of theirs must not click through it).
	int shopper = CoopShopper();
	if (shopper >= 0 && (map == MAP_SEKENSOR || map == MAP_MENU)) return g.owner[shopper];
	if (map == MAP_SEKENSOR) return 2;   // dialogues: either player clicks on
	switch (g.mode) {
	case SHARED_GUN:
		// Obelix turns the gun, Asterix fires it.
		if (map == MAP_GUN) return action == 0 || action == 1 ? obe : ast;
		if (map == MAP_TRIO) return ast;
		return -1;
	case SHARED_GUN_SOLO:
		// Asterix alone on the gun (Obelix's player free, split screen on): all
		// of it his -- the gun's own tick reads the game's values, which are
		// player 1's even when Asterix is player 2's.
		return map == MAP_GUN ? ast : -1;
	case SHARED_BOAT:
	case SHARED_SLIDE:
		// Obelix steers (and the camera), Asterix jumps.
		if (map == MAP_TRIO || map == MAP_BOAT) return action == 4 ? ast : obe;
		if (map == MAP_CAMERA) return obe;
		return -1;
	case SHARED_FUSION:
		// Asterix forward / back, Obelix left / right and the camera.
		if (map == MAP_TRIO) return action == 0 || action == 1 ? ast : action == 2 || action == 3 ? obe : -1;
		if (map == MAP_CAMERA) return obe;
		return -1;
	default:
		return -1;
	}
}

uint8_t __fastcall GetActionHook(char* view, void* edx, uint32_t action) {
	uint8_t v = g_origGetAction(view, edx, action);
	if (g.logActions > 0) LogAction(view, action, v, _ReturnAddress());
	if (!g.joined || !g.p1 || !g.p2) return v;
	int map = MapOf(view);
	if (map == MAP_OTHER) return v;
	int who = Route(map, action);
	if (who < 0) return v;
	uint8_t idx = (*(uint8_t**)(view + 8))[action & 0xFF];
	uint8_t a = g.p1[idx];
	uint8_t b = (map == MAP_CAMERA && g.p2cam ? g.p2cam : g.p2)[idx];
	uint8_t r = who == 0 ? a : who == 1 ? b : (a > b ? a : b);
	if (g.logActions > 0 && r && r != v) {   // diagnostics: answered from the other player's pad
		static uint8_t seen[MAP_COUNT][64];
		if ((action & 0xFF) < 64 && !seen[map][action & 0x3F]) {
			seen[map][action & 0x3F] = 1;
			Log("shared: %s %s[%d] answered by player %d (%d, the game had %d)", SharedModeName(g.mode), kMapLabel[map],
			    (int)action, who == 2 ? (a > b ? 1 : 2) : who + 1, r, v);
		}
	}
	return r;
}

SharedMode DetectMode(Trio t) {
	if (CoopOneScreen()) return SHARED_FUSION;
	Hero a = t.GetHero(HERO_ASTERIX), o = t.GetHero(HERO_OBELIX);
	if (!a || !o) return SHARED_NONE;
	uint8_t ma = HeroMode(a.Flags18()), mo = HeroMode(o.Flags18());
	if (ma == 2) return mo == 2 ? SHARED_GUN : SHARED_GUN_SOLO;
	if (ma == 1) return SHARED_BOAT;
	if (mo == 3 && t.Leader() == HERO_OBELIX && HeroIsPlayerVariant(o.Flags18())) return SHARED_SLIDE;
	return SHARED_NONE;
}

// Hint: who does what, for 10 s (5 s for the short-lived fused tornado).
void ShowModeHint(SharedMode m) {
	if (m == SHARED_NONE) { HintHide(HINT_MODE); return; }
	int a = g.owner[HERO_ASTERIX], o = g.owner[HERO_OBELIX];
	wchar_t text[96];
	switch (m) {
	case SHARED_GUN:
		swprintf(text, 96, L"Obelix (P%d): turn the gun\nAsterix (P%d): fire with %s", o + 1, a + 1, HintButtonName(a, HB_X));
		break;
	case SHARED_GUN_SOLO:
		swprintf(text, 96, L"Obelix (P%d): %s at the gun's handles\nto turn it for Asterix", o + 1, HintButtonName(o, HB_X));
		HintShow(HINT_MODE, text, 6000);
		return;
	case SHARED_BOAT:
	case SHARED_SLIDE:
		swprintf(text, 96, L"Obelix (P%d): steer and camera\nAsterix (P%d): jump with %s", o + 1, a + 1,
		         HintButtonName(a, HB_A));
		break;
	default:
		swprintf(text, 96, L"Asterix (P%d): forward and back\nObelix (P%d): left, right and camera", a + 1, o + 1);
		break;
	}
	HintShow(HINT_MODE, text, m == SHARED_FUSION ? 5000 : 10000);
}

} // namespace

const char* SharedModeName(SharedMode m) {
	switch (m) {
	case SHARED_GUN: return "machine gun";
	case SHARED_GUN_SOLO: return "machine gun (Asterix alone)";
	case SHARED_BOAT: return "boat";
	case SHARED_SLIDE: return "belly slide";
	case SHARED_FUSION: return "Twister Fusion";
	default: return "none";
	}
}

SharedMode SharedCurrentMode() { return g.mode; }

void SharedFrame() {
	if (g.logActions < 0) g.logActions = getenv("XXL_ACTIONLOG") ? atoi(getenv("XXL_ACTIONLOG")) : 0;
	InputManager im = GetInputManager();
	char* in = im ? im.raw() : nullptr;
	if (in != g.input || (in && g.base[MAP_TRIO] == nullptr)) ReadMaps(in);
	g.p1 = in ? *(uint8_t**)(in + 0x1C) : nullptr;
	g.joined = CoopP2Joined();
	g.p2 = g.joined ? CoopP2Values() : nullptr;
	g.p2cam = g.joined ? CoopP2CameraValues() : nullptr;
	g.owner[HERO_ASTERIX] = CoopP1Hero() == HERO_ASTERIX ? 0 : 1;
	g.owner[HERO_OBELIX] = CoopP1Hero() == HERO_OBELIX ? 0 : 1;
	Trio t = GetTrio();
	SharedMode m = g.joined && t ? DetectMode(t) : SHARED_NONE;
	if (g.logActions > 0 && t) {   // the machine on offer (trio +0x48, +0x3F4 bits 3, 10-11)
		static uint32_t lastOffer = 0;
		static void* lastMachine = nullptr;
		char* tr = (char*)t.raw();
		uint32_t offer = *(uint32_t*)(tr + 0x3F4) & 0xC08;
		void* machine = *(void**)(tr + 0x48);
		if (offer != lastOffer || machine != lastMachine)
			Log("shared: trio offer %03X machine %p (vtable %p), leader %s", offer, machine,
			    machine ? *(void**)machine : nullptr, HeroName(t.Leader()));
		lastOffer = offer;
		lastMachine = machine;
	}
	if (m != g.mode) {
		Log("shared: %s -> %s (Asterix: player %d, Obelix: player %d)", SharedModeName(g.mode), SharedModeName(m),
		    g.owner[HERO_ASTERIX] + 1, g.owner[HERO_OBELIX] + 1);
		g.mode = m;
		ShowModeHint(m);
	}
}

bool SharedInstallHooks() {
	return InstallDetour(kGetAction, (void*)GetActionHook, (void**)&g_origGetAction, kGetActionPrologue,
	                     sizeof(kGetActionPrologue), "InputView_GetAction (shared modes, dialogues)");
}
