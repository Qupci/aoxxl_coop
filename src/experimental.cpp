#include "experimental.h"
#include "config.h"
#include "coop.h"
#include "individual.h"
#include "kal.h"
#include "detour.h"
#include "log.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {

typedef int(__fastcall* DeserializeFn)(char* self, void* edx, void* file, int pass);

const uintptr_t kSquadDeserialize = 0x52DE80;
const uint8_t   kSquadDeserializePrologue[7] = { 0x6A, 0xFF, 0x68, 0x0E, 0xFE, 0x61, 0x00 };
DeserializeFn   g_origSquadDeserialize = nullptr;

const uintptr_t kShopDeserialize = 0x4C0C50;
const uint8_t   kShopDeserializePrologue[5] = { 0x53, 0x8B, 0x5C, 0x24, 0x0C };
DeserializeFn   g_origShopDeserialize = nullptr;

// What a squad's pools held in the level file, and how the mod treats them.
struct SquadOrig {
	unsigned n = 0;
	bool     scaled[8] = {};     // its Romans to defeat scaled
	bool     atOnce[8] = {};     // its numbers at once scaled (a pool its script sends Romans in for)
	int16_t  origCount[8] = {};  // Romans to defeat as the level has them
	bool     settled[8] = {};    // kept as is: its share of the doubling given to another pool of its gate
	uint8_t  origU1[8] = {}, origU2[8] = {};
	bool     shareChecked = false;
};
std::unordered_map<char*, SquadOrig> g_squads;

// Enemies left as the level has them (not Romans in numbers): the turtles,
// the mobile tower (a mini-boss), the jetpack Romans -- by their component's
// vtable (pool record +4).
bool KeptAsIs(char* cpnt) {
	if (!cpnt) return false;
	switch (*(uintptr_t*)cpnt) {
	case 0x627C90:   // CKJetPackRomanCpnt
	case 0x627D20:   // CKMobileTowerCpnt
	case 0x627D50:   // CKTriangularTurtleCpnt
	case 0x627DB8:   // CKSquareTurtleCpnt
	case 0x627DEC:   // CKDonutTurtleCpnt
	case 0x627E20:   // CKPyramidalTurtleCpnt
		return true;
	}
	return false;
}

// Numbers at once doubled only while both players play: not while one
// player's hero is held by the level (a cable car ride...) or is down waiting
// to be revived (individual heroes) -- the one left fights the level's numbers.
// Romans to defeat stay scaled.
bool AtOnceNow() {
	static const bool noDownFix = getenv("XXL_NOATONCEDOWNFIX") != nullptr;   // diagnostics: as before
	return !(CoopP2Joined() && (CoopHeroHeld(0) || CoopHeroHeld(1) || (!noDownFix && (IndivFallen(0) || IndivFallen(1)))));
}

int Scaled(int v, float f, int maxV) {
	int r = (int)(v * f + 0.5f);
	if (v > 0 && r < 1) r = 1;
	return r > maxV ? maxV : r;
}

// Pools: +0x10 U1 (at once after a restart), +0x11 alive (loaded as U2),
// +0x18 to defeat.
void ScaleSquad(char* sq) {
	unsigned n = *(unsigned*)(sq + 0x14C);
	char* rec0 = *(char**)(sq + 0x144);
	if (!rec0 || n == 0 || n > 8) return;
	SquadOrig o;
	o.n = n;
	char line[700];
	int len = _snprintf(line, sizeof(line), "experimental: squad %p:", sq);
	int before = 0, after = 0;
	for (unsigned i = 0; i < n; i++) {
		char* rec = rec0 + i * 0x1C;
		uint8_t& u1 = *(uint8_t*)(rec + 0x10);
		uint8_t& u2 = *(uint8_t*)(rec + 0x11);
		int16_t& count = *(int16_t*)(rec + 0x18);
		int a0 = u1, s0 = u2, c0 = count;
		o.origCount[i] = count;
		o.origU1[i] = u1;
		o.origU2[i] = u2;
		// A set piece: all its Romans placed at the start, none sent in after (LV4-1's
		// hiding Romans) -- as the level has it.
		bool placedOnly = count > 0 && count <= (u2 > u1 ? u2 : u1);
		bool kept = KeptAsIs(*(char**)(rec + 4));
		o.scaled[i] = count > 0 && !placedOnly && !kept;
		o.atOnce[i] = count != 0 && !placedOnly && !kept;
		if (o.atOnce[i] && g_config.squadAliveFactor != 1.0f) {
			u1 = (uint8_t)Scaled(u1, g_config.squadAliveFactor, 255);
			u2 = (uint8_t)Scaled(u2, g_config.squadAliveFactor, 255);
		}
		if (o.scaled[i] && g_config.squadCountFactor != 1.0f)
			count = (int16_t)Scaled(count, g_config.squadCountFactor, 32767);
		if (c0 > 0) { before += c0; after += count; }
		if (len > 0 && len < (int)sizeof(line))
			len += _snprintf(line + len, sizeof(line) - len, " [%sat once %d/%d -> %d/%d, %d -> %d]",
			                 kept ? "kept (type), " : placedOnly ? "kept (placed), " : "", a0, s0, u1, u2, c0, count);
	}
	if (len > 0 && len < (int)sizeof(line))
		_snprintf(line + len, sizeof(line) - len, " total %d -> %d", before, after);
	line[sizeof(line) - 1] = 0;
	Log("%s", line);
	g_squads[sq] = o;
}

int __fastcall SquadDeserializeHook(char* self, void* edx, void* file, int pass) {
	int r = g_origSquadDeserialize(self, edx, file, pass);
	if (pass == 0) ScaleSquad(self);
	return r;
}

int __fastcall ShopDeserializeHook(char* self, void* edx, void* file, int pass) {
	int r = g_origShopDeserialize(self, edx, file, pass);
	if (pass == 0) {
		uint32_t* price = (uint32_t*)(self + 0xE4);
		char line[256];
		int len = _snprintf(line, sizeof(line), "experimental: merchant %p prices", self);
		for (int i = 0; i < 12; i++) {
			uint32_t p0 = price[i];
			if (p0 && p0 < 100000000) price[i] = (uint32_t)(p0 * g_config.priceFactor + 0.5f);
			if (len > 0 && len < (int)sizeof(line))
				len += _snprintf(line + len, sizeof(line) - len, " %u->%u", p0, price[i]);
		}
		line[sizeof(line) - 1] = 0;
		Log("%s", line);
	}
	return r;
}

// A squad's script (its message actions) runs commands through the squad's
// dispatcher (vf25 0x4C7F80, thiscall(cmd, args) ret 8). Roman counts in them:
//   5  condition: Romans of pool args[2] alive (vf43) compared (args[1]) with args[0]
//   8  queue args[0] Romans of pool args[1] (vf53)
//   12 send args[0] Romans of pool args[2] in at a point (vf55)
//   16 condition: Romans of pool args[2] sent in so far (+0x16) compared with args[0]
//   19 replace args[0] fallen Romans of pool args[1] (vf65)
//   20 keep args[0] Romans of pool args[1] alive (vf54)
// (LV4-1's first squad: "if pool 0 alive <= 6: replace 1", over and over.)
// The numbers are scaled on a copy; a 0 or 1 in a condition is left alone
// ("all fallen").
typedef uint32_t(__fastcall* SquadCmdFn)(char* self, void* edx, uint32_t cmd, uint32_t* args);
SquadCmdFn g_origSquadCmd = nullptr, g_origJetCmd = nullptr;
bool g_logCmds = false;

void LogSquadCmd(char* self, uint32_t cmd, const uint32_t* a, const uint32_t* b, uint32_t r) {
	static struct { char* sq; uint32_t cmd, a0, a1, a2, r; unsigned n; } seen[256];
	static int nSeen = 0;
	for (int i = 0; i < nSeen; i++)
		if (seen[i].sq == self && seen[i].cmd == cmd && seen[i].a0 == a[0] && seen[i].a1 == a[1] &&
		    seen[i].a2 == a[2] && seen[i].r == (r & 0xFF)) {
			unsigned n = ++seen[i].n;
			if (n == 10 || n == 100 || n == 1000)
				Log("experimental: squad %p cmd %u (%d %d %d) -> %u: %u times", self, cmd, a[0], a[1], a[2], r & 0xFF, n);
			return;
		}
	if (nSeen < 256) seen[nSeen++] = { self, cmd, a[0], a[1], a[2], r & 0xFF, 1 };
	Log("experimental: squad %p cmd %u (%d %d %d) [as %d] -> %u", self, cmd, a[0], a[1], a[2], b[0], r & 0xFF);
}

uint32_t SquadCmd(SquadCmdFn orig, char* self, void* edx, uint32_t cmd, uint32_t* args) {
	uint32_t a[4] = {};
	bool read = false;
	if (args) {
		__try { a[0] = args[0]; a[1] = args[1]; a[2] = args[2]; a[3] = args[3]; read = true; }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	float f = 1.0f;
	if (read && (int)a[0] <= 10000) {
		auto it = g_squads.find(self);
		int n = (int)a[0];
		// the pool the command is about (-1: any)
		int pool = (cmd == 5 || cmd == 12 || cmd == 16) ? (int)a[2] : (int)a[1];
		auto all = [&](const bool* flags) {
			if (it == g_squads.end()) return false;
			const SquadOrig& o = it->second;
			if (pool >= 0 && pool < (int)o.n) return flags[pool];
			for (unsigned i = 0; i < o.n; i++) if (!flags[i]) return false;
			return o.n > 0;
		};
		switch (cmd) {
		case 5:  if (n >= 2 && AtOnceNow() && all(it->second.atOnce)) f = g_config.squadAliveFactor; break;
		case 8: case 12: case 19: case 20: if (n >= 1 && AtOnceNow() && all(it->second.atOnce)) f = g_config.squadAliveFactor; break;
		case 16: if (n >= 2 && all(it->second.scaled)) f = g_config.squadCountFactor; break;
		}
	}
	uint32_t b[4] = { a[0], a[1], a[2], a[3] };
	uint32_t r;
	if (f != 1.0f) {
		b[0] = (uint32_t)Scaled((int)a[0], f, 10000);
		r = orig(self, edx, cmd, b);
	} else {
		r = orig(self, edx, cmd, args);
	}
	if (g_logCmds && read) LogSquadCmd(self, cmd, a, b, r);
	return r;
}
uint32_t __fastcall SquadCmdHook(char* self, void* edx, uint32_t cmd, uint32_t* args) {
	return SquadCmd(g_origSquadCmd, self, edx, cmd, args);
}
uint32_t __fastcall JetCmdHook(char* self, void* edx, uint32_t cmd, uint32_t* args) {
	return SquadCmd(g_origJetCmd, self, edx, cmd, args);
}

// A fight gate (fight lock) counts the Romans left in the squads given to it
// (squad +0x154: -1 all its pools, n pool n, 0xFE none): every frame each
// such squad adds count - sent in + alive (FUN_0052D340) to game manager
// +0x64, the number shown. A pool kept as the level has it (set piece, turtle)
// gives what its doubling would have added to the biggest doubled pool counted
// by the same gate, so the gate shows the whole total doubled.
void BalanceGates() {
	if (g_config.squadCountFactor == 1.0f) return;
	struct Locked { char* sq; SquadOrig* o; int sel; };
	Locked locked[64];
	int nLocked = 0;
	const int classes[2] = { 26, 64 };   // CKGrpSquadEnemy, CKGrpSquadJetPack
	for (int c : classes)
		for (unsigned i = 0, n = GetNumLvlClassInstances(4, c); i < n && nLocked < 64; i++) {
			char* sq = (char*)GetLvlObject(4, c, i);
			if (!sq) continue;
			int sel = *(int8_t*)(sq + 0x154);
			if (sel < -1) continue;
			auto it = g_squads.find(sq);
			if (it == g_squads.end() || it->second.n != *(unsigned*)(sq + 0x14C)) continue;
			locked[nLocked++] = { sq, &it->second, sel };
		}
	if (!nLocked) return;
	int deficit = 0;
	int16_t* target = nullptr;
	char* targetSq = nullptr;
	for (int k = 0; k < nLocked; k++) {
		const Locked& L = locked[k];
		char* rec0 = *(char**)(L.sq + 0x144);
		for (unsigned i = 0; i < L.o->n; i++) {
			if (L.sel >= 0 && L.sel != (int)i) continue;
			int16_t* count = (int16_t*)(rec0 + i * 0x1C + 0x18);
			if (L.o->scaled[i]) {
				if (*count > 0 && (!target || *count > *target)) { target = count; targetSq = L.sq; }
			} else if (!L.o->settled[i] && L.o->origCount[i] > 0) {
				deficit += Scaled(L.o->origCount[i], g_config.squadCountFactor, 32767) - L.o->origCount[i];
			}
		}
	}
	if (deficit <= 0 || !target) return;
	for (int k = 0; k < nLocked; k++)
		for (unsigned i = 0; i < locked[k].o->n; i++)
			if ((locked[k].sel < 0 || locked[k].sel == (int)i) && !locked[k].o->scaled[i]) locked[k].o->settled[i] = true;
	int before = *target;
	*target = (int16_t)(before + deficit > 32767 ? 32767 : before + deficit);
	Log("experimental: a fight gate counts %d squad(s): %d Romans of the kept pools go to squad %p (%d -> %d)",
	    nLocked, deficit, targetSq, before, *target);
}

// Romans come from pools (CKGrpPoolSquad, pool record +0) that squads of the
// same place share, and a pool holds what the level needs at once there
// (LV5-6: pool of 7, squad 40 places 3, squad 4's script keeps 4 of its 40
// alive). Doubled starting Romans of one squad (6) are taken for the whole
// fight and left squad 4 one at a time. So a squad's starting Romans stay as
// the level has them when its pool also feeds another squad whose box
// overlaps its own and whose numbers are scaled. Done once the level is in,
// before the squads wake (nothing sent in yet).
void CheckSharedPools() {
	static const bool noShareFix = getenv("XXL_NOSQUADSHAREFIX") != nullptr;   // diagnostics: as before
	if (noShareFix || g_config.squadAliveFactor == 1.0f) return;
	struct Sq { char* sq; SquadOrig* o; };
	Sq all[128];
	int nAll = 0;
	bool fresh = false;
	const int classes[2] = { 26, 64 };   // CKGrpSquadEnemy, CKGrpSquadJetPack
	for (int c : classes)
		for (unsigned i = 0, n = GetNumLvlClassInstances(4, c); i < n && nAll < 128; i++) {
			char* sq = (char*)GetLvlObject(4, c, i);
			auto it = sq ? g_squads.find(sq) : g_squads.end();
			if (it == g_squads.end() || it->second.n != *(unsigned*)(sq + 0x14C)) continue;
			all[nAll++] = { sq, &it->second };
			fresh = fresh || !it->second.shareChecked;
		}
	if (!fresh) return;
	auto overlap = [](char* a, char* b) {   // boxes: max +0x11C, min +0x128
		const float* fa = (const float*)a;
		const float* fb = (const float*)b;
		for (int k = 0; k < 3; k++)
			if (fa[0x47 + k] < fb[0x4A + k] || fb[0x47 + k] < fa[0x4A + k]) return false;
		return true;
	};
	for (int s = 0; s < nAll; s++) {
		SquadOrig& o = *all[s].o;
		if (o.shareChecked) continue;
		o.shareChecked = true;
		char* rec0 = *(char**)(all[s].sq + 0x144);
		for (unsigned i = 0; i < o.n; i++) {
			char* rec = rec0 + i * 0x1C;
			uint8_t& u1 = *(uint8_t*)(rec + 0x10);
			uint8_t& u2 = *(uint8_t*)(rec + 0x11);
			if (!o.atOnce[i] || (u1 == o.origU1[i] && u2 == o.origU2[i]) || *(int16_t*)(rec + 0x16) != 0) continue;
			char* pool = *(char**)rec;
			char* other = nullptr;
			unsigned otherRec = 0;
			for (int t = 0; t < nAll && !other; t++) {
				if (t == s || !overlap(all[s].sq, all[t].sq)) continue;
				char* r0 = *(char**)(all[t].sq + 0x144);
				for (unsigned j = 0; j < all[t].o->n && !other; j++)
					if (all[t].o->atOnce[j] && *(char**)(r0 + j * 0x1C) == pool) { other = all[t].sq; otherRec = j; }
			}
			if (!other) continue;
			Log("experimental: squad %p pool %u: starting Romans %u/%u -> %u/%u as the level has them "
			    "(their pool %p also feeds squad %p pool %u here)", all[s].sq, i, u1, u2, o.origU1[i], o.origU2[i],
			    pool, other, otherRec);
			u1 = o.origU1[i];
			u2 = o.origU2[i];
		}
	}
}

// The combo bar fills with the hits at game manager +0xB0 (+1 a hit, +3 with
// the potion, from four places); FUN_0048C860 sets its level from the
// thresholds +0xB8..+0xBA (bytes) and fills it at +0xC8 (byte: "full" once the
// hits reach it); the HUD draws hits / +0xC8, a full bar's refill per hit is
// +0xA0 / +0xC8. Those thresholds are divided by the factor: the bar fills
// that much slower, whoever hits.
// Draining: before it is full the bar loses a hit every +0x9C seconds (+0xA8
// counts down); a full bar lasts +0xA0 seconds (+0xA4 counts down, a hit puts
// back +0xA0 / +0xC8). Divided by their factors.
char*   g_comboGm = nullptr;
uint8_t g_comboSet[4] = {};   // the thresholds as last written
float   g_drainSet[2] = {};   // +0x9C, +0xA0 as last written

void ScaleCombo() {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	if (!gm) return;
	uint8_t* th[4] = { (uint8_t*)(gm + 0xB8), (uint8_t*)(gm + 0xB9), (uint8_t*)(gm + 0xBA), (uint8_t*)(gm + 0xC8) };
	float* dr[2] = { (float*)(gm + 0x9C), (float*)(gm + 0xA0) };
	bool same = gm == g_comboGm;
	for (int i = 0; i < 4 && same; i++) same = *th[i] == g_comboSet[i];
	for (int i = 0; i < 2 && same; i++) same = *dr[i] == g_drainSet[i];
	if (same) return;
	uint8_t orig[4];
	for (int i = 0; i < 4; i++) {
		orig[i] = *th[i];
		int v = (int)(orig[i] / g_config.comboFactor + 0.5f);
		*th[i] = (uint8_t)(v > 255 ? 255 : v);
		g_comboSet[i] = *th[i];
	}
	float origDr[2] = { *dr[0], *dr[1] };
	*dr[0] = origDr[0] / g_config.comboDrainFactor;
	*dr[1] = origDr[1] / g_config.comboFullDrainFactor;
	g_drainSet[0] = *dr[0];
	g_drainSet[1] = *dr[1];
	g_comboGm = gm;
	Log("experimental: combo bar levels %u/%u/%u, full at %u -> %u/%u/%u, full at %u; a hit lost every %.2f -> %.2f s, "
	    "full for %.2f -> %.2f s", orig[0], orig[1], orig[2], orig[3], *th[0], *th[1], *th[2], *th[3], origDr[0], *dr[0],
	    origDr[1], *dr[1]);
}

} // namespace

void ExperimentalFrame() {
	static int s_atOnce = -1;
	int atOnce = AtOnceNow() ? 1 : 0;
	if (atOnce != s_atOnce && g_config.squadAliveFactor != 1.0f) {
		if (s_atOnce >= 0)
			Log(atOnce ? "experimental: Romans at once x%.2f again (both players play)"
			           : "experimental: Romans at once as the level has them (a player's hero is held or down; x%.2f off)",
			    g_config.squadAliveFactor);
		s_atOnce = atOnce;
	}
	CheckSharedPools();
	BalanceGates();
	if (g_config.comboFactor != 1.0f || g_config.comboDrainFactor != 1.0f || g_config.comboFullDrainFactor != 1.0f)
		ScaleCombo();
}


bool ExperimentalInstallHooks() {
	bool ok = true;
	bool squads = g_config.squadAliveFactor != 1.0f || g_config.squadCountFactor != 1.0f;
	g_logCmds = getenv("XXL_SQUADCMDS") != nullptr;
	if (squads || g_logCmds) {
		ok &= InstallDetour(kSquadDeserialize, (void*)SquadDeserializeHook, (void**)&g_origSquadDeserialize,
		                    kSquadDeserializePrologue, sizeof(kSquadDeserializePrologue),
		                    "squad load (experimental: Romans at once / to defeat)");
		ok &= PatchVtableSlot(0x6266B0, 25, 0x4C7F80, (void*)SquadCmdHook, (void**)&g_origSquadCmd,
		                      "CKGrpSquadEnemy::vf25 (squad script numbers)");
		ok &= PatchVtableSlot(0x626A6C, 25, 0x4C7F80, (void*)JetCmdHook, (void**)&g_origJetCmd,
		                      "CKGrpSquadJetPack::vf25 (squad script numbers)");
	}
	if (g_config.priceFactor != 1.0f)
		ok &= InstallDetour(kShopDeserialize, (void*)ShopDeserializeHook, (void**)&g_origShopDeserialize,
		                    kShopDeserializePrologue, sizeof(kShopDeserializePrologue),
		                    "merchant load (experimental: prices)");
	return ok;
}
