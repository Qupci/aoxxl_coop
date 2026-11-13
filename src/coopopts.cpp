#include "coopopts.h"
#include "config.h"
#include "coop.h"
#include "log.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

// Kinds: a YES / NO, a number (left / right by `step` within lo..hi), a choice
// of names (left / right go round), and the few with rules of their own.
enum Kind { K_BOOL, K_NUM, K_CHOICE, K_SP, K_LIMIT, K_P2HERO, K_SPLIT, K_CAMERA };
struct Opt {
	const char* section;
	const char* key;
	Kind kind;
	void* p;          // bool* / float* / int* (the K_SP: index into g_sp)
	float lo, hi, step;
	const char* fmt;  // a number's value ("%.1f")
};

// The sp_ options as the ini has them (g_config's are off with sp_isolation on).
enum { SP_COMBO_HINTS, SP_HERO_SWITCH, SP_SWITCH_LOCK, SP_SECOND_CHANCE, SP_SIDEKICK_VIEW, SP_COUNT };
bool g_sp[SP_COUNT];
const char* const kSpKeys[SP_COUNT] = { "sp_combo_hints", "sp_hero_switch", "sp_hero_switch_lock", "sp_second_chance",
                                        "sp_sidekick_view" };
float g_lastLimit = 60.0f;   // revive_time_limit's seconds, for a switch back from auto

#define B(sec, key, field) { sec, key, K_BOOL, &g_config.field, 0, 0, 0, nullptr }
#define N(sec, key, field, lo, hi, step, fmt) { sec, key, K_NUM, &g_config.field, lo, hi, step, fmt }
#define SP(key, idx) { "single_player", key, K_SP, (void*)(intptr_t)(idx), 0, 0, 0, nullptr }
const Opt kOpts[] = {
	{ "coop", "p2_hero", K_P2HERO, &g_config.p2Hero, 0, 0, 0, nullptr },
	B("coop", "safe_sequence", safeSequence),
	B("coop", "individual_heroes", individual),
	N("coop", "revive_hold_seconds", reviveHold, 0.5f, 30.0f, 0.5f, "%.1f"),
	N("coop", "revive_hold_catapult_window", reviveHoldCatapultWindow, 0.05f, 1.0f, 0.05f, "%.2f"),
	{ "coop", "revive_time_limit", K_LIMIT, &g_config.reviveLimit, 0, 600, 5, nullptr },
	N("coop", "revive_time_limit_grace", reviveGrace, 5.0f, 300.0f, 5.0f, "%.0f"),
	B("coop", "romans_guard_downed", romansGuardDowned),
	B("coop", "nolock", noLock),
	B("coop", "nolock_except_corkscrews", noLockExceptCorkscrews),
	B("coop", "nolock_except_rotarybeams", noLockExceptBeams),
	B("coop", "walk_to_marker_teleport", markerTeleport),
	B("coop", "romans_spawn_in_view", romansInView),
	B("coop", "obelix_potion", obelixPotion),
	{ "coop", "split_mode", K_SPLIT, &g_config.splitMode, 0, 0, 0, nullptr },
	{ "coop", "camera", K_CAMERA, &g_config.gameCamera, 0, 0, 0, nullptr },
	N("coop", "split_fov", splitFov, -30.0f, 60.0f, 1.0f, "%.0f"),
	B("coop_difficulty", "difficulty_coop_only", coopOnly),
	B("coop_difficulty", "difficulty_static", rulesStatic),
	B("coop_difficulty", "difficulty_pathfinding", apartByPath),
	B("coop_difficulty", "difficulty_to_defeat_stays", toDefeatStays),
	// squad_romans_at_once: not here -- the levels' Roman pools are rebuilt for it at the start.
	N("coop_difficulty", "squad_romans_to_defeat", squadCountFactor, 1.0f, 10.0f, 0.5f, "%.2f"),
	N("coop_difficulty", "merchant_prices", priceFactor, 0.5f, 10.0f, 0.25f, "%.2f"),
	N("coop_difficulty", "combo_bar_fill", comboFactor, 0.1f, 3.0f, 0.1f, "%.2f"),
	N("coop_difficulty", "combo_bar_drain", comboDrainFactor, 0.1f, 3.0f, 0.1f, "%.2f"),
	N("coop_difficulty", "combo_bar_full_drain", comboFullDrainFactor, 0.1f, 3.0f, 0.1f, "%.2f"),
	B("coop_difficulty.experimental", "nolock_romans", noLockRomans),
	B("coop_difficulty.experimental", "nolock_romans_any_markers", noLockRomansAnyMarkers),
	B("hints", "hint_join", hintJoin),
	B("hints", "hint_pass_roman", hintPass),
	B("hints", "hint_combos", hintCombos),
	B("hints", "hint_machine_gun", hintMachineGun),
	SP("sp_combo_hints", SP_COMBO_HINTS),
	SP("sp_hero_switch", SP_HERO_SWITCH),
	SP("sp_hero_switch_lock", SP_SWITCH_LOCK),
	SP("sp_second_chance", SP_SECOND_CHANCE),
	SP("sp_sidekick_view", SP_SIDEKICK_VIEW),
	B("debug", "cheats", cheats),
	B("debug", "show_leader", showLeader),
	B("debug", "sidekick_view_always", sidekickViewAlways),
	B("debug", "skip_cutscenes", skipCutscenes),
};
#undef B
#undef N
#undef SP
const int kCount = sizeof(kOpts) / sizeof(kOpts[0]);

void Write(const Opt& o, const char* value) {
	IniWrite(o.section, o.key, value);
	Log("coopopts: %s = %s", o.key, value);
}

// The sp_ options into g_config as Load does: off with sp_isolation, the lock only with the switch.
void ApplySp() {
	bool iso = g_config.spIsolation;
	g_config.spComboHints = g_sp[SP_COMBO_HINTS] && !iso;
	g_config.spHeroSwitch = g_sp[SP_HERO_SWITCH] && !iso;
	g_config.spSwitchLock = g_sp[SP_SWITCH_LOCK] && g_config.spHeroSwitch;
	g_config.spSecondChance = g_sp[SP_SECOND_CHANCE] && !iso;
	g_config.spSidekickView = g_sp[SP_SIDEKICK_VIEW] && !iso;
}

bool IniBool(const char* section, const char* key, bool def) {
	char b[32] = {};
	GetPrivateProfileStringA(section, key, def ? "on" : "off", b, sizeof(b), g_config.iniPath);
	return !_stricmp(b, "on") || !strcmp(b, "1") || !_stricmp(b, "yes") || !_stricmp(b, "true");
}

} // namespace

void CoopOptsInit() {
	static const bool kDefaults[SP_COUNT] = { true, true, true, false, true };
	for (int i = 0; i < SP_COUNT; i++) g_sp[i] = IniBool("single_player", kSpKeys[i], kDefaults[i]);
	if (g_config.reviveLimit > 0.0f) g_lastLimit = g_config.reviveLimit;
}

int CoopOptCount() { return kCount; }
const char* CoopOptKey(int i) { return i >= 0 && i < kCount ? kOpts[i].key : ""; }

bool CoopOptToggle(int i, bool* on) {
	if (i < 0 || i >= kCount) return false;
	const Opt& o = kOpts[i];
	if (o.kind == K_BOOL) { *on = *(bool*)o.p; return true; }
	if (o.kind == K_SP) { *on = g_sp[(intptr_t)o.p]; return true; }
	return false;
}

void CoopOptValue(int i, wchar_t* out, int n) {
	out[0] = 0;
	if (i < 0 || i >= kCount) return;
	const Opt& o = kOpts[i];
	switch (o.kind) {
	case K_NUM: {
		char b[32];
		snprintf(b, sizeof(b), o.fmt, *(float*)o.p);
		swprintf(out, n, L"%hs", b);
		break;
	}
	case K_LIMIT: {
		float v = *(float*)o.p;
		if (v < 0.0f) wcsncpy(out, L"auto", n - 1);
		else if (v == 0.0f) wcsncpy(out, L"off", n - 1);
		else swprintf(out, n, L"%.0f", v);
		break;
	}
	case K_P2HERO: {
		int v = *(int*)o.p;
		wcsncpy(out, v == 0 ? L"asterix" : v == 1 ? L"obelix" : L"other", n - 1);
		break;
	}
	case K_SPLIT: wcsncpy(out, *(int*)o.p == SPLIT_VERTICAL ? L"vertical" : L"horizontal", n - 1); break;
	case K_CAMERA: wcsncpy(out, *(bool*)o.p ? L"game" : L"mod", n - 1); break;
	default: break;
	}
}

bool CoopOptChange(int i, int dir) {
	if (i < 0 || i >= kCount) return false;
	const Opt& o = kOpts[i];
	char b[32];
	switch (o.kind) {
	case K_BOOL: {
		bool& v = *(bool*)o.p;
		v = !v;
		Write(o, v ? "on" : "off");
		return true;
	}
	case K_SP: {
		bool& v = g_sp[(intptr_t)o.p];
		v = !v;
		ApplySp();
		Write(o, v ? "on" : "off");
		return true;
	}
	case K_NUM: {
		if (!dir) return false;
		float& v = *(float*)o.p;
		float w = v + o.step * dir;
		w = w < o.lo ? o.lo : w > o.hi ? o.hi : w;
		w = floorf(w / o.step + 0.5f) * o.step;   // on the steps
		if (fabsf(w - v) < 1e-4f) return false;
		v = w;
		snprintf(b, sizeof(b), "%g", v);
		Write(o, b);
		return true;
	}
	case K_LIMIT: {
		// Confirm: auto <-> seconds; left / right: the seconds (auto: back to them), 0 = off.
		float& v = *(float*)o.p;
		if (!dir) v = v < 0.0f ? g_lastLimit : -1.0f;
		else if (v < 0.0f) v = g_lastLimit;
		else {
			float w = v + o.step * dir;
			w = w < 0.0f ? 0.0f : w > o.hi ? o.hi : w;
			if (w == v) return false;
			v = w;
		}
		if (v > 0.0f) g_lastLimit = v;
		if (v < 0.0f) strcpy(b, "auto");
		else if (v == 0.0f) strcpy(b, "off");
		else snprintf(b, sizeof(b), "%g", v);
		Write(o, b);
		return true;
	}
	case K_P2HERO: {
		// obelix -> asterix -> other (rightwards). In co-op a hero named for
		// player 2 that is not its own swaps the heroes now.
		static const int kOrder[3] = { 1, 0, -1 };
		int& v = *(int*)o.p;
		int k = 0;
		while (k < 3 && kOrder[k] != v) k++;
		k = ((k < 3 ? k : 0) + (dir < 0 ? 2 : 1)) % 3;
		v = kOrder[k];
		Write(o, v == 0 ? "asterix" : v == 1 ? "obelix" : "other");
		if (CoopP2Joined() && v >= 0 && v != CoopP2Hero()) CoopSwapHeroes("co-op options: p2_hero");
		return true;
	}
	case K_SPLIT: {
		int& v = *(int*)o.p;
		v = v == SPLIT_VERTICAL ? SPLIT_HORIZONTAL : SPLIT_VERTICAL;
		Write(o, v == SPLIT_VERTICAL ? "vertical" : "horizontal");
		return true;
	}
	case K_CAMERA: {
		bool& v = *(bool*)o.p;
		v = !v;
		Write(o, v ? "game" : "mod");
		return true;
	}
	}
	return false;
}
