#include "potioncombos.h"
#include "config.h"
#include "coop.h"
#include "detour.h"
#include "trio.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// Roman contacts (Class_2_84::sendEvent 0x4CBA60, thiscall(roman, event, list),
// ret 8; BasicEnemy / JumpingRoman / Archer through FUN_004D12F0): event 0x300
// brings a list of {next, pair}, a pair {volume a, volume b, ..., +0x1A contact
// type ('G' = none)}; a volume's +0x19 is its kind and +0x20 its owner. The
// trio's shockwave volume is kind 4: the Roman is pushed, hit (+0x24 |= 0x80)
// and, with its component's flag 0x08, disarmed (|= 0x8000).
// A catapult's stone (event 0x301 in the same function, 0x4CBF48..0x4CC064):
// velocity +0x58 = (away x, 0.33, away z) normalised x component +0xFC, the
// hitter +0x1A0 = 0, +0x24 |= 0x402 -- bit 0x2 is state 1 (knocked out: the
// death sound, "a Roman beaten" to its squad) whatever else is set (vf26).
const uintptr_t kRomanSend        = 0x4CBA60;
const uint8_t   kRomanSendPrologue[6] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };   // mov eax, fs:[0]
typedef int(__fastcall* RomanSendFn)(char*, void*, uint32_t, void*);
RomanSendFn g_origRomanSend = nullptr;

// The trio's shockwave (FUN_0047CE70, thiscall(trio, pos, params)) is started by
// the Club (0x493477, each impact) and the Power Hammer (0x493B8C), esi = the hero.
const uintptr_t kShockwave        = 0x47CE70;
const uintptr_t kClubShockCall    = 0x493477;
const uintptr_t kHammerShockCall  = 0x493B8C;
const uint8_t   kClubShockBytes[5]   = { 0xE8, 0xF4, 0x99, 0xFE, 0xFF };
const uint8_t   kHammerShockBytes[5] = { 0xE8, 0xDF, 0x92, 0xFE, 0xFF };
const DWORD     kShockMs          = 2000;   // a shockwave's contacts come for about a second after it starts

// The Mole (hero body handler 0x494160, steps +0x38E & 0x3F 0x1C..0x20): its
// catch (FUN_004966E0) sends 0x4C0A to the Romans over it until Asterix's +0xB60
// (CKHkAsterix asteUnk0, the level's) are taken.
const int       kMoleFactor       = 3;
// The Rage (Idefix's bite state 0x49E4C0): bites left +0xB91 = +0xB90 with the
// bar full and the Rage learned (one for a plain RB bite).

struct Shock { DWORD tick; float pos[3]; bool potion, club; int beaten; };
Shock g_shock = {};
int  g_logs = 0, g_beatLogs = 0, g_moleLogs = 0;
bool g_rage = false;
char* g_moleHero = nullptr;
int  g_moleOrig = 0;
bool g_inMole = false;
int  g_moleCaught = 0;

bool On() { return g_config.potionCombos && (CoopP2Joined() || g_config.SoloMod()); }
bool HasPotion(char* hero) { return hero && (*(uint32_t*)(hero + 0x388) & 4) != 0; }

const char* WhoIs(char* p) {
	Trio t = GetTrio();
	if (!t || !p) return "?";
	if (p == t.raw()) return "trio";
	for (int i = 0; i < HERO_COUNT; i++)
		if (t.GetHero(i).raw() == p) return HeroName(i);
	return "other";
}

void __stdcall OnShock(char* hero, const float* pos, int club) {
	if (g_shock.tick && g_shock.potion && g_logs < 60) {
		g_logs++;
		Log("potioncombos: the last shockwave beat %d Romans", g_shock.beaten);
	}
	g_shock.tick = GetTickCount();
	memcpy(g_shock.pos, pos, sizeof(g_shock.pos));
	g_shock.potion = HasPotion(hero) && On();
	g_shock.club = club != 0;
	g_shock.beaten = 0;
	if (g_logs < 60) {
		g_logs++;
		Log("potioncombos: %s shockwave by %s at %.1f %.1f %.1f, %s", club ? "club" : "power hammer", WhoIs(hero), pos[0],
		    pos[1], pos[2], g_shock.potion ? "under the potion: it beats the Romans" : "no potion: the game's disarm");
	}
}

void* g_shockFn = (void*)kShockwave;

__declspec(naked) void ClubShockThunk() {
	__asm {
		pushad
		push 1
		push dword ptr [esp + 40]   // pos: 8 saved registers + return address + the push above
		push esi                    // the hero
		call OnShock
		popad
		jmp  dword ptr [g_shockFn]
	}
}

__declspec(naked) void HammerShockThunk() {
	__asm {
		pushad
		push 0
		push dword ptr [esp + 40]
		push esi
		call OnShock
		popad
		jmp  dword ptr [g_shockFn]
	}
}

// As a catapult's stone: knocked out, flying off away from `from`.
bool Beat(char* roman, const float* from) {
	if (*(uint8_t*)(roman + 0x21) == 1 || (*(uint32_t*)(roman + 0x24) & 2)) return false;
	char* cpnt = *(char**)(roman + 0x54);
	if (!cpnt) return false;
	float* pos = (float*)(roman + 0x2C);
	float dx = pos[0] - from[0], dz = pos[2] - from[2];
	float l = sqrtf(dx * dx + dz * dz);
	if (l > 0.0f) { dx /= l; dz /= l; } else { dx = 0.0f; dz = 1.0f; }
	float dy = 0.33f;
	l = sqrtf(dx * dx + dy * dy + dz * dz);
	float speed = *(float*)(cpnt + 0xFC) / l;
	float* vel = (float*)(roman + 0x58);
	vel[0] = dx * speed; vel[1] = dy * speed; vel[2] = dz * speed;
	*(uint32_t*)(roman + 0x1A0) = 0;
	*(uint32_t*)(roman + 0x24) |= 0x402;
	return true;
}

int __fastcall RomanSendHook(char* roman, void* edx, uint32_t ev, void* param) {
	uint32_t e = ev & 0xFFFF;
	if (e == 0x4C0A && g_inMole) g_moleCaught++;
	// Idefix's bite (0x4C0C: hit, a bite kind; the answer byte 1 if it took): in
	// the Rage under the potion the bitten Roman is beaten.
	if (e == 0x4C0C) {
		int r = g_origRomanSend(roman, edx, ev, param);
		Trio t = GetTrio();
		char* idefix = t ? t.GetHero(HERO_IDEFIX).raw() : nullptr;
		if (g_rage && idefix && param && *(uint8_t*)param == 1 && On() && PotionGauge() > 0.0f) {
			uint32_t before = *(uint32_t*)(roman + 0x24);
			if (Beat(roman, (const float*)(idefix + 0x6C)) && g_beatLogs < 40) {
				g_beatLogs++;
				Log("potioncombos: roman %p beaten by Idefix's rage (+0x24 %08X -> %08X)", roman, before,
				    *(uint32_t*)(roman + 0x24));
			}
		}
		return r;
	}
	if (e != 0x300 || !param) return g_origRomanSend(roman, edx, ev, param);
	bool shock = false;
	if (g_shock.potion && GetTickCount() - g_shock.tick < kShockMs && On()) {
		Trio t = GetTrio();
		char* trio = t ? t.raw() : nullptr;
		for (char** node = (char**)param; node && trio && !shock; node = *(char***)node) {
			char* pair = node[1];
			if (!pair || *(uint8_t*)(pair + 0x1A) == 'G') continue;
			char* a = *(char**)pair;
			char* b = *(char**)(pair + 4);
			if (!a || !b) continue;
			char* other = *(char**)(b + 0x20) != roman ? b : a;
			shock = *(char**)(other + 0x20) == trio && *(uint8_t*)(other + 0x19) == 4;
		}
	}
	uint32_t before = *(uint32_t*)(roman + 0x24);
	int r = g_origRomanSend(roman, edx, ev, param);
	if (shock && Beat(roman, g_shock.pos)) {
		g_shock.beaten++;
		if (g_beatLogs < 40) {
			g_beatLogs++;
			Log("potioncombos: roman %p beaten by the %s's shockwave (+0x24 %08X -> %08X)", roman,
			    g_shock.club ? "club" : "power hammer", before, *(uint32_t*)(roman + 0x24));
		}
	}
	return r;
}

} // namespace

void PotionCombosFrame() {
	Trio t = GetTrio();
	Hero ast = t ? t.GetHero(HERO_ASTERIX) : Hero();
	Hero idx = t ? t.GetHero(HERO_IDEFIX) : Hero();
	// The Rage: from Idefix's bites left going past one until they run out.
	bool rage = g_rage;
	if (idx) {
		uint8_t left = *(uint8_t*)(idx.raw() + 0xB91);
		if (left > 1) rage = true;
		else if (left == 0) rage = false;
	} else rage = false;
	if (rage != g_rage) {
		g_rage = rage;
		if (g_logs < 60) {
			g_logs++;
			Log("potioncombos: Idefix's rage %s (bites %u)", rage ? "begins" : "ends",
			    idx ? *(uint8_t*)(idx.raw() + 0xB90) : 0);
		}
	}
	// The Mole: Asterix's cap x3 under the potion, changed only between Moles.
	if (!ast) { g_moleHero = nullptr; g_inMole = false; return; }
	char* h = ast.raw();
	int* cap = (int*)(h + 0xB60);
	if (h != g_moleHero || (*cap != g_moleOrig && *cap != g_moleOrig * kMoleFactor)) {
		g_moleHero = h;
		g_moleOrig = *cap;   // a new level (or one loaded again)
	}
	uint8_t step = *(uint8_t*)(h + 0x38E) & 0x3F;
	bool inMole = step >= 0x1C && step <= 0x20;
	if (inMole != g_inMole) {
		if (inMole) g_moleCaught = 0;
		else if (g_moleLogs < 40) {
			g_moleLogs++;
			Log("potioncombos: the mole took %d Romans (cap %d)", g_moleCaught, *cap);
		}
		g_inMole = inMole;
	}
	if (inMole || g_moleOrig <= 0 || g_moleOrig > 1000) return;
	int want = On() && HasPotion(h) ? g_moleOrig * kMoleFactor : g_moleOrig;
	if (*cap != want) {
		*cap = want;
		if (g_moleLogs < 40) {
			g_moleLogs++;
			Log("potioncombos: the mole takes %d Romans now (the level's %d)", want, g_moleOrig);
		}
	}
}

bool PotionCombosInstallHooks() {
	bool ok = InstallDetour(kRomanSend, (void*)RomanSendHook, (void**)&g_origRomanSend, kRomanSendPrologue,
	                        sizeof(kRomanSendPrologue), "Roman sendEvent (potion combos)");
	ok &= PatchCallSite(kClubShockCall, kClubShockBytes, sizeof(kClubShockBytes), (void*)ClubShockThunk,
	                    "club shockwave (potion combos)");
	ok &= PatchCallSite(kHammerShockCall, kHammerShockBytes, sizeof(kHammerShockBytes), (void*)HammerShockThunk,
	                    "power hammer shockwave (potion combos)");
	return ok;
}
