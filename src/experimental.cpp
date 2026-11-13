#include "experimental.h"
#include "levelforge.h"
#include "nolock.h"
#include "config.h"
#include "coop.h"
#include "individual.h"
#include "kal.h"
#include "detour.h"
#include "log.h"
#include "pathfind.h"
#include "trio.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// When the squads' and the combo bar's rules count. Co-op on (player 2 in),
// the heroes are apart while one is held by the level (a cable car ride, a
// wait), down waiting to be revived, or where the other can't walk to it (the
// level's pathfinding graph; difficulty_pathfinding): the one left fights with
// the game's numbers -- Romans at once and the combo bar as the level has
// them, and a fight that starts then has the level's Romans to defeat until
// the heroes are together again (UpdateSquads; difficulty_to_defeat_stays: for
// good). difficulty_static: never apart, the rules on all through co-op.
// Single player: the rules on, or with coop_only the game's own numbers.
enum Mode { MODE_GAME, MODE_APART, MODE_ON };
const char* const kModeName[] = { "single player, the game's own", "the heroes apart", "on" };

// Can the heroes walk to each other (either way, pathfind.cpp)? Asked every
// 0.5 s while player 2 plays; apart after three noes in a row (not a hero in
// mid-jump over a gap's cells), together again on the first yes. Not known (a
// hero on a lifter, in the air): as it was. Not asked with the rules static,
// with difficulty_pathfinding off, or with no squad or combo bar factor (the
// game's own all through). The first answer after player 2 joins counts at
// once, in the frame of the join: the join doubles a fight under way, and
// LV3-6's had been doubled (200) a second before "apart" (Obelix left at the
// level's start) -- too late to keep it at the level's 100.
bool g_pathCheck = false;
struct Apart { bool apart = false, fresh = true; int noes = 0; DWORD at = 0; int told = 0, timed = 0; } g_apart;

void ApartFrame() {
	// (The co-op options can change the factors and rules while playing.)
	bool squads = g_config.squadAliveFactor != 1.0f || g_config.squadCountFactor != 1.0f;
	bool combo = g_config.comboFactor != 1.0f || g_config.comboDrainFactor != 1.0f || g_config.comboFullDrainFactor != 1.0f;
	g_pathCheck = (squads || combo) && !g_config.rulesStatic && g_config.apartByPath;
	if (!g_pathCheck || !CoopP2Joined() || !KalLevelIsLoaded()) {
		g_apart.apart = false;
		g_apart.fresh = true;
		g_apart.noes = 0;
		return;
	}
	DWORD now = GetTickCount();
	if (CoopSuspended() || (!g_apart.fresh && now - g_apart.at < 500)) return;
	Trio t = GetTrio();
	Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero(), o = t ? t.GetHero(HERO_OBELIX) : Hero();
	if (!a || !o) return;
	g_apart.at = now;
	LARGE_INTEGER t0, t1, f;
	QueryPerformanceCounter(&t0);
	int r = PfConnected(a.Pos(), o.Pos());
	QueryPerformanceCounter(&t1);
	if (g_apart.timed < 3 && QueryPerformanceFrequency(&f)) {
		g_apart.timed++;
		Log("experimental: the heroes' path looked up in %.2f ms", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
	}
	if (r == PF_YES) g_apart.noes = 0;
	else if (r == PF_NO) g_apart.noes = g_apart.fresh ? 3 : g_apart.noes + 1;
	if (r != PF_UNKNOWN) g_apart.fresh = false;
	bool apart = r == PF_YES ? false : r == PF_NO && g_apart.noes >= 3 ? true : g_apart.apart;
	if (apart == g_apart.apart) return;
	g_apart.apart = apart;
	if (g_apart.told++ < 60)
		Log(apart ? "experimental: the heroes are apart: neither can walk to the other (Asterix %.1f %.1f %.1f, Obelix %.1f %.1f %.1f)"
		          : "experimental: the heroes can walk to each other again (Asterix %.1f %.1f %.1f, Obelix %.1f %.1f %.1f)",
		    a.Pos().x, a.Pos().y, a.Pos().z, o.Pos().x, o.Pos().y, o.Pos().z);
}

Mode ModeNow() {
	if (!CoopP2Joined()) return g_config.coopOnly ? MODE_GAME : MODE_ON;
	if (g_config.rulesStatic) return MODE_ON;
	if (CoopHeroHeld(HERO_ASTERIX) || CoopHeroHeld(HERO_OBELIX) || IndivFallen(HERO_ASTERIX) ||
	    IndivFallen(HERO_OBELIX) || (g_apart.apart && KalLevelIsLoaded()))
		return MODE_APART;
	return MODE_ON;
}

// What a squad's pools held in the level file, and how the mod treats them.
struct SquadOrig {
	unsigned n = 0;
	bool     scaled[8] = {};     // its Romans to defeat scaled
	bool     atOnce[8] = {};     // its numbers at once scaled (a pool its script sends Romans in for)
	bool     shared[8] = {};     // ... but not its starting Romans: its pool also feeds another squad here
	int16_t  origCount[8] = {};  // Romans to defeat as the level has them
	uint8_t  origU1[8] = {}, origU2[8] = {};
	bool     shareChecked = false;
	// Romans to defeat now
	bool     full = false;       // scaled
	bool     begun = false;      // its fight has begun (GateStartFrame, Beaten)
	bool     startApart = false; // ... with the heroes apart (difficulty_to_defeat_stays: not scaled)
	int      own[8] = {};        // added to the pool's count: its own doubling
	int      extra[8] = {};      // ... the share of other pools (beaten ones, kept ones)
	char*    gaveTo[8] = {};     // a kept pool: the squad its doubling went to (GateFrame)
	DWORD    idleSince[8] = {};  // Romans of the mod's left to send and none coming since (TrimStuck)
};
std::unordered_map<char*, SquadOrig> g_squads;
int g_gatePending = 0;   // beaten pools' share for the pools of a fight gate (Raise -> GateFrame)

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

int Scaled(int v, float f, int maxV) {
	int r = (int)(v * f + 0.5f);
	if (v > 0 && r < 1) r = 1;
	return r > maxV ? maxV : r;
}

// Pools: +0x10 U1 (at once after a restart), +0x11 alive (loaded as U2),
// +0x14 queued, +0x15 fallen to replace, +0x16 sent in so far (the ones alive
// too), +0x18 to defeat. The level's numbers are kept here; what the rules
// make of them is put in each frame (UpdateSquads).
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
		uint8_t u1 = *(uint8_t*)(rec + 0x10);
		uint8_t u2 = *(uint8_t*)(rec + 0x11);
		int16_t count = *(int16_t*)(rec + 0x18);
		o.origCount[i] = count;
		o.origU1[i] = u1;
		o.origU2[i] = u2;
		// A set piece: all its Romans placed at the start, none sent in after (LV4-1's
		// hiding Romans) -- as the level has it.
		bool placedOnly = count > 0 && count <= (u2 > u1 ? u2 : u1);
		bool kept = KeptAsIs(*(char**)(rec + 4));
		o.scaled[i] = count > 0 && !placedOnly && !kept;
		o.atOnce[i] = count != 0 && !placedOnly && !kept;
		int a1 = o.atOnce[i] ? Scaled(u1, g_config.squadAliveFactor, 255) : u1;
		int a2 = o.atOnce[i] ? Scaled(u2, g_config.squadAliveFactor, 255) : u2;
		int c1 = o.scaled[i] ? Scaled(count, g_config.squadCountFactor, 32767) : count;
		if (count > 0) { before += count; after += c1; }
		if (len > 0 && len < (int)sizeof(line))
			len += _snprintf(line + len, sizeof(line) - len, " [%sat once %d/%d -> %d/%d, %d -> %d]",
			                 kept ? "kept (type), " : placedOnly ? "kept (placed), " : "", u1, u2, a1, a2, count, c1);
	}
	if (len > 0 && len < (int)sizeof(line))
		_snprintf(line + len, sizeof(line) - len, " total %d -> %d (while the rules are on)", before, after);
	line[sizeof(line) - 1] = 0;
	Log("%s", line);
	g_squads[sq] = o;
}

int __fastcall SquadDeserializeHook(char* self, void* edx, void* file, int pass) {
	int r = g_origSquadDeserialize(self, edx, file, pass);
	if (pass == 0) ScaleSquad(self);
	return r;
}

// The avoidance service (CKSrvAvoidance, Romans stepping aside from each
// other) keeps 50 entries at +0x10 and their count right after them (+0xD8);
// its add (0x4600A0, thiscall(entry), ret 4) has no bound. With more Romans
// awake than any level had (pools filled up, levelforge.cpp) a 51st wrote over
// the count and the service's tick (vf17) read garbage (crash at 0x41CB56, LV4
// at x3). Past 50 a Roman is left out (it does not step aside; its push is
// cleared as the add would); the remove (0x4600E0) skips one it can't find.
const uintptr_t kAvoidAdd = 0x4600A0;
const uint8_t   kAvoidAddPrologue[10] = { 0x8B, 0x91, 0xD8, 0x00, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x04 };
typedef void(__fastcall* AvoidAddFn)(char* self, void* edx, char* entry);
AvoidAddFn g_origAvoidAdd = nullptr;

void __fastcall AvoidAddHook(char* self, void* edx, char* entry) {
	if (*(uint32_t*)(self + 0xD8) < 50) {
		g_origAvoidAdd(self, edx, entry);
		return;
	}
	memcpy(entry + 0x14, (const void*)0x6621D8, 12);   // the zero vector the add writes
	*(uint32_t*)(entry + 0x20) = 0;
	static int s_told = 0;
	if (s_told++ < 10) Log("experimental: avoidance service full (50): Roman %p left out", entry);
}

// A squad's formation (its current choreography key: FUN_0052C780(squad); key
// +4 spots of 0x1C -- +0 position, +0xC facing, +0x18 pool or -1 -- +0xC their
// count) has a spot per Roman the level placed. The squad's tick (FUN_0052B860)
// hands the spots out; a Roman left over gets FUN_0052B670(squad; Roman, key,
// -1) (thiscall, ret 0xC): the formation's centre (node +0x30), and the squad's
// wake (vf33) places such Romans on the centre too (FUN_0052EC30) -- with more
// Romans at once than spots, the extras stood inside each other for good
// (LV2-1). Extra n now gets spot n % spots shifted sideways by a row: row 1 on
// one side, 2 on the other, 3 further out on the first... (ExtraSpot); its
// target (Roman component 0x46: +0 spot, +0xC facing, +0x18 step = (spot -
// Roman) x/z / max(distance, squad +0xC0)) set after the game's.
typedef char*(__fastcall* KeyFn)(char* squad, void* edx);
typedef void(__cdecl* XformFn)(float* out, const float* in, int count, char* node);
typedef void(__fastcall* SlotFn)(char* squad, void* edx, char* roman, char* key, int spot);
typedef void(__fastcall* FormationFn)(char* squad, void* edx);
const uintptr_t kSlot = 0x52B670, kFormation = 0x52B860;
const uint8_t   kSlotPrologue[6] = { 0x83, 0xEC, 0x24, 0x53, 0x56, 0x57 };
const uint8_t   kFormationPrologue[6] = { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56 };
SlotFn g_origSlot = nullptr;
FormationFn g_origFormation = nullptr;
char* g_extraSquad = nullptr;   // the squad whose formation is being handed out
int   g_extraN = 0;             // ... its extras so far

void __fastcall FormationHook(char* squad, void* edx) {
	char* prevSquad = g_extraSquad;
	int prevN = g_extraN;
	g_extraSquad = squad;
	g_extraN = 0;
	g_origFormation(squad, edx);
	g_extraSquad = prevSquad;
	g_extraN = prevN;
}

void __fastcall SlotHook(char* squad, void* edx, char* roman, char* key, int spot) {
	g_origSlot(squad, edx, roman, key, spot);
	if (spot >= 0 || squad != g_extraSquad) return;
	float pos[3], dir[3];
	if (!ExperimentalExtraSpot(squad, g_extraN++, pos, dir)) return;
	typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
	char* c = ((CpntFn)(*(void***)roman)[12])(roman, nullptr, 0x46);
	char* node = *(char**)(roman + 0x10);
	char* fr = node ? *(char**)(node + 4) : nullptr;
	if (!c || !fr) return;
	float* t = (float*)c;
	memcpy(t, pos, 12);
	memcpy(t + 3, dir, 12);
	const float* rp = (const float*)(fr + 0x80);
	float dx = pos[0] - rp[0], dz = pos[2] - rp[2], len = sqrtf(dx * dx + dz * dz);
	float k = len > 0.01f ? 1.0f / (len > *(float*)(squad + 0xC0) ? len : *(float*)(squad + 0xC0)) : 0.0f;
	t[6] = dx * k; t[7] = 0.0f; t[8] = dz * k;
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
// The numbers are scaled on a copy -- at once while the rules are on, sent in
// so far as the pool's count is (the level's and the mod's own doubling); a 0
// or 1 in a condition is left alone ("all fallen").
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
	auto it = g_squads.find(self);
	const SquadOrig* o = it != g_squads.end() ? &it->second : nullptr;
	if (read && o && (int)a[0] <= 10000) {
		int n = (int)a[0];
		// the pool the command is about (-1: any)
		int pool = (cmd == 5 || cmd == 12 || cmd == 16) ? (int)a[2] : (int)a[1];
		auto all = [&](const bool* flags) {
			if (pool >= 0 && pool < (int)o->n) return flags[pool];
			for (unsigned i = 0; i < o->n; i++) if (!flags[i]) return false;
			return o->n > 0;
		};
		bool atOnce = ModeNow() == MODE_ON;
		switch (cmd) {
		case 5:  if (n >= 2 && atOnce && all(o->atOnce)) f = g_config.squadAliveFactor; break;
		case 8: case 12: case 19: case 20: if (n >= 1 && atOnce && all(o->atOnce)) f = g_config.squadAliveFactor; break;
		case 16:   // as the pool's count is now: scaled where it stood (Raise)
			if (n >= 2 && all(o->scaled)) {
				int was = 0, is = 0;
				for (unsigned i = 0; i < o->n; i++)
					if (pool < 0 || pool == (int)i) { was += o->origCount[i]; is += o->origCount[i] + o->own[i]; }
				if (was > 0) f = (float)is / was;
			}
			break;
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

// Romans of pool `pool` alive (squad vf42 0x4C81E0, thiscall(pool) ret 4; 0
// while the squad is not active).
int Alive(char* sq, unsigned pool) {
	typedef int(__fastcall* AliveFn)(char* self, void* edx, int pool);
	return ((AliveFn)(*(void***)sq)[42])(sq, nullptr, (int)pool);
}

int Left(const char* rec) { return *(const int16_t*)(rec + 0x18) - *(const uint16_t*)(rec + 0x16); }

// Romans of a pool still to defeat: to send in, and alive (what the gate shows).
int Remaining(char* sq, const char* rec, unsigned pool) {
	if (*(const int16_t*)(rec + 0x18) < 0) return 0;
	int r = Left(rec) + ((*(uint8_t*)(sq + 0x152) & 2) ? Alive(sq, pool) : 0);
	return r > 0 ? r : 0;
}

// A pool still in the fight: Romans left to send in, queued, to replace or
// alive. One with none (beaten) is done with in its squad's script.
bool Live(char* sq, const char* rec, unsigned pool) {
	if (*(const int16_t*)(rec + 0x18) <= 0) return false;
	return Left(rec) > 0 || *(const uint8_t*)(rec + 0x14) || *(const uint8_t*)(rec + 0x15) || Alive(sq, pool) > 0;
}

void Give(int* to, char* rec, int n) {
	int16_t& count = *(int16_t*)(rec + 0x18);
	int c = count + n > 32767 ? 32767 : count + n;
	*to += c - count;
	count = (int16_t)c;
}

// Takes up to `n` of the Romans the mod gave pool `i` and that are not sent in
// yet (the share of others first); how many it took.
int Take(SquadOrig& o, char* rec, unsigned i, int n) {
	int16_t& count = *(int16_t*)(rec + 0x18);
	int room = Left(rec);
	if (n > room) n = room;
	if (n <= 0) return 0;
	int fromExtra = n < o.extra[i] ? n : o.extra[i];
	o.extra[i] -= fromExtra;
	o.own[i] -= n - fromExtra;
	count = (int16_t)(count - n);
	return n;
}

// The scaled pool still in the fight with the most left to send in of squad
// `sq` (only pool `sel` when its fight gate counts that one), else -1.
int BestPool(char* sq, const SquadOrig& o, int sel) {
	char* rec0 = *(char**)(sq + 0x144);
	int best = -1, bestLeft = -1;
	for (unsigned i = 0; i < o.n; i++) {
		if (!o.scaled[i] || (sel >= 0 && sel != (int)i)) continue;
		const char* rec = rec0 + i * 0x1C;
		if (!Live(sq, rec, i)) continue;
		int left = Left(rec);
		if (left > bestLeft) { best = (int)i; bestLeft = left; }
	}
	return best;
}

int PoolsLine(char* sq, const SquadOrig& o, char* buf, int size) {
	char* rec0 = *(char**)(sq + 0x144);
	int len = 0;
	for (unsigned i = 0; i < o.n && len >= 0 && len < size; i++) {
		const char* rec = rec0 + i * 0x1C;
		len += _snprintf(buf + len, size - len, " [%u: %d to defeat (level %d), sent %u, alive %d]", i,
		                 *(const int16_t*)(rec + 0x18), o.origCount[i], *(const uint16_t*)(rec + 0x16),
		                 (*(uint8_t*)(sq + 0x152) & 2) ? Alive(sq, i) : 0);
	}
	buf[size - 1] = 0;
	return len;
}

// Romans to defeat scaled for squad `sq`: each scaled pool's Romans still to
// defeat are scaled where they stand (a fight under way at 85 goes to 170; the
// ones beaten are not made up for). The share of a pool with none left to send
// in -- its part of the script may be over: Romans added there might never
// come, and a gate would wait for them -- goes to the pool with the most left
// to send in, of the squad, else of the squads its fight gate counts
// (GateFrame). Never to the kept pools (Romans placed on their spots,
// turtles). Returns the Romans added.
int Raise(char* sq, SquadOrig& o) {
	char* rec0 = *(char**)(sq + 0x144);
	int pending = 0, added = 0;
	for (unsigned i = 0; i < o.n; i++) {
		if (!o.scaled[i]) continue;
		char* rec = rec0 + i * 0x1C;
		int rem = Remaining(sq, rec, i);
		int share = Scaled(rem, g_config.squadCountFactor, 32767) - rem;
		if (share <= 0) continue;
		if (Left(rec) > 0) {
			int before = o.own[i];
			Give(&o.own[i], rec, share);
			added += o.own[i] - before;
		} else {
			pending += share;
		}
	}
	o.full = true;
	if (pending > 0) {
		int sel = *(int8_t*)(sq + 0x154);
		int best = BestPool(sq, o, sel);
		if (best >= 0) {
			int before = o.extra[best];
			Give(&o.extra[best], rec0 + best * 0x1C, pending);
			added += o.extra[best] - before;
			Log("experimental: squad %p: the share of its beaten pools (%d) goes to pool %d", sq, pending, best);
		} else if (sel >= -1) {
			g_gatePending += pending;
		} else {
			Log("experimental: squad %p: the share of its beaten pools (%d) has no pool left to go to", sq, pending);
		}
	}
	return added;
}

// Back to the game's Romans to defeat, where they stand: the other pools'
// shares taken away (given again with the rules), each pool's own Romans
// still to defeat scaled back (170 -> 85) -- of what the mod gave it and is
// not sent in yet. Returns the Romans taken away.
int Lower(char* sq, SquadOrig& o) {
	char* rec0 = *(char**)(sq + 0x144);
	int removed = 0;
	for (unsigned i = 0; i < o.n; i++) {
		if (!o.own[i] && !o.extra[i]) continue;
		char* rec = rec0 + i * 0x1C;
		removed += Take(o, rec, i, o.extra[i]);
		int rem = Remaining(sq, rec, i);
		int n = rem - Scaled(rem, 1.0f / g_config.squadCountFactor, 32767);
		removed += Take(o, rec, i, n < o.own[i] ? n : o.own[i]);
	}
	o.full = false;
	for (auto& e : g_squads)   // kept pools whose share was here: theirs to give again
		for (unsigned j = 0; j < e.second.n; j++)
			if (e.second.gaveTo[j] == sq) e.second.gaveTo[j] = nullptr;
	return removed;
}

// The safety net of a share given in a fight under way: a pool of a squad a
// fight gate counts, with Romans of the mod's left to send in and none queued,
// to replace or alive for 6 s while the squad is active, has had its part of
// the script end before them (a script counting what it has sent, "sent in so
// far >= n") -- they are taken away, or the gate would wait for them.
void TrimStuck(char* sq, SquadOrig& o, DWORD now) {
	char* rec0 = *(char**)(sq + 0x144);
	for (unsigned i = 0; i < o.n; i++) {
		char* rec = rec0 + i * 0x1C;
		bool idle = (o.own[i] || o.extra[i]) && *(uint16_t*)(rec + 0x16) && Left(rec) > 0 &&
		            !*(uint8_t*)(rec + 0x14) && !*(uint8_t*)(rec + 0x15) && Alive(sq, i) == 0;
		if (!idle) { o.idleSince[i] = 0; continue; }
		if (!o.idleSince[i]) { o.idleSince[i] = now; continue; }
		if (now - o.idleSince[i] < 6000) continue;
		o.idleSince[i] = 0;
		int taken = Take(o, rec, i, o.own[i] + o.extra[i]);
		Log("experimental: squad %p pool %u: none of its %d Romans left to send came in 6 s -- %d of the mod's taken away",
		    sq, i, Left(rec) + taken, taken);
	}
}

// When a fight begins. A squad wakes as its place comes in (the sector loaded,
// the heroes near), its starting Romans placed -- often long before its fight:
// LV3-4's courtyard squad places its two guards as the sector loads, and its
// fight only begins as a hero walks into the lock's sphere. So a fight behind
// a gate begins as the gate takes its squads (the lock's "add all pools to the
// lock": squad +0x154 >= -1), and is set kGateSettleMs later -- time for the
// path look-up to see doors the lock shuts between the heroes. The gate's
// squads go as one. A fight without a gate begins at its first Roman beaten
// (of a scaled pool: not a guard placed on its spot).
const DWORD kGateSettleMs = 2500;
struct GateStart { bool on = false, begun = false, apart = false; DWORD since = 0; } g_gateStart;

void GateStartFrame(Mode m, DWORD now) {
	bool any = false;
	const int classes[2] = { 26, 64 };   // CKGrpSquadEnemy, CKGrpSquadJetPack
	for (int c : classes)
		for (unsigned k = 0, n = GetNumLvlClassInstances(4, c); k < n && !any; k++) {
			char* sq = (char*)GetLvlObject(4, c, k);
			any = sq && *(int8_t*)(sq + 0x154) >= -1;
		}
	if (!any) {
		g_gateStart = GateStart();
		return;
	}
	if (!g_gateStart.on) {
		g_gateStart.on = true;
		g_gateStart.since = now;
	}
	if (!g_gateStart.begun && now - g_gateStart.since >= kGateSettleMs) {
		g_gateStart.begun = true;
		g_gateStart.apart = m == MODE_APART;
		static int s_told = 0;
		if (s_told++ < 60) Log("experimental: fight gate: its fight begins (rules %s)", kModeName[m]);
	}
	if (m == MODE_GAME) g_gateStart.apart = false;   // player 2 gone: scaled like any fight when they come back
}

bool Beaten(char* sq, const SquadOrig& o) {
	if (!(*(uint8_t*)(sq + 0x152) & 2)) return false;
	char* rec0 = *(char**)(sq + 0x144);
	for (unsigned i = 0; i < o.n; i++) {
		const char* rec = rec0 + i * 0x1C;
		if (o.scaled[i] && (*(const uint8_t*)(rec + 0x15) || *(const uint16_t*)(rec + 0x16) > Alive(sq, i))) return true;
	}
	return false;
}

// Each frame: the squads' numbers for the mode. A squad whose fight has not
// begun takes the mode as it is. Once it has, its Romans to defeat stay as
// they are when the heroes come apart; a fight begun with the heroes apart is
// scaled as soon as the rules are on again (Raise; with
// difficulty_to_defeat_stays never, while player 2 stays in), and goes back
// to the game's when player 2 leaves with coop_only. Starting Romans: U1
// (+0x10, read at a restart) follows the mode, U2 (+0x11) too while the squad
// has not woken.
void UpdateSquads(Mode m, DWORD now) {
	bool on = m == MODE_ON;
	int freshUp = 0, freshDown = 0;
	const int classes[2] = { 26, 64 };   // CKGrpSquadEnemy, CKGrpSquadJetPack
	for (int c : classes)
		for (unsigned k = 0, n = GetNumLvlClassInstances(4, c); k < n; k++) {
			char* sq = (char*)GetLvlObject(4, c, k);
			auto it = sq ? g_squads.find(sq) : g_squads.end();
			if (it == g_squads.end() || it->second.n != *(unsigned*)(sq + 0x14C)) continue;
			SquadOrig& o = it->second;
			char* rec0 = *(char**)(sq + 0x144);
			if (!rec0) continue;
			bool fresh = true;
			for (unsigned i = 0; i < o.n && fresh; i++) {
				const char* rec = rec0 + i * 0x1C;
				fresh = !*(const uint16_t*)(rec + 0x16) && !*(const uint8_t*)(rec + 0x14) && !*(const uint8_t*)(rec + 0x15);
			}
			bool active = (*(uint8_t*)(sq + 0x152) & 2) != 0;
			if (g_config.squadAliveFactor != 1.0f)
				for (unsigned i = 0; i < o.n; i++) {
					if (!o.atOnce[i]) continue;
					char* rec = rec0 + i * 0x1C;
					bool up = on && !o.shared[i];
					int s1 = Scaled(o.origU1[i], g_config.squadAliveFactor, 255);
					int s2 = Scaled(o.origU2[i], g_config.squadAliveFactor, 255);
					*(uint8_t*)(rec + 0x10) = (uint8_t)(up ? s1 : o.origU1[i]);
					uint8_t& u2 = *(uint8_t*)(rec + 0x11);
					if (fresh && !active && (u2 == s2 || u2 == o.origU2[i])) u2 = (uint8_t)(up ? s2 : o.origU2[i]);
				}
			if (g_config.squadCountFactor == 1.0f) continue;
			// A fight no-lock mode ended with Romans of it out for a waiting hero: those
			// are not Romans left to defeat, to double when the heroes meet again.
			if (NoLockEndedFight(sq)) continue;
			bool gated = *(int8_t*)(sq + 0x154) >= -1;
			if (gated) {
				o.begun = g_gateStart.begun;
				o.startApart = g_gateStart.apart;
			} else if (fresh) {
				o.begun = false;
			} else if (!o.begun && Beaten(sq, o)) {
				o.begun = true;
				o.startApart = m == MODE_APART;
			} else if (m == MODE_GAME) {
				o.startApart = false;   // player 2 gone: scaled like any fight when they come back
			}
			bool begun = o.begun;
			bool raise = on && !o.full && !(g_config.toDefeatStays && begun && o.startApart);
			bool lower = begun ? m == MODE_GAME && o.full : !on && o.full;
			if (raise || lower) {
				char before[400];
				if (begun) PoolsLine(sq, o, before, sizeof(before));
				int d = raise ? Raise(sq, o) : -Lower(sq, o);
				if (!begun) {
					(raise ? freshUp : freshDown)++;
				} else {
					static int s_told = 0;
					char after[400];
					PoolsLine(sq, o, after, sizeof(after));
					if (d && s_told++ < 60)
						Log("experimental: squad %p, its fight under way: Romans to defeat %s (%+d; rules %s):%s ->%s", sq,
						    raise ? "scaled" : "as the level has them", d, kModeName[m], before, after);
				}
			}
			if (!fresh && active && gated) TrimStuck(sq, o, now);
		}
	static int s_toldFresh = 0;
	if ((freshUp || freshDown) && s_toldFresh++ < 60)
		Log("experimental: Romans to defeat %s for %d squad(s) whose fight has not begun (rules %s)",
		    freshUp ? "scaled" : "as the level has them", freshUp + freshDown, kModeName[m]);
}

// A fight gate (fight lock) counts the Romans left in the squads given to it
// (squad +0x154: -1 all its pools, n pool n, 0xFE none): every frame each
// such squad adds count - sent in + alive (FUN_0052D340) to game manager
// +0x64, the number shown. A pool kept as the level has it (set piece, turtle)
// gives what doubling its Romans still to defeat would add to the pool with
// the most left to send in of a squad the same gate counts with its Romans to
// defeat scaled, so the gate shows the whole number doubled; so does a squad
// scaled mid-fight whose pools with Romans to send take none of the others'
// share (Raise). While no squad of the gate is scaled (its fight began with
// the heroes apart) the kept pools' share waits.
void GateFrame(Mode m) {
	int pending = g_gatePending;
	g_gatePending = 0;
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
	// The Romans left behind the gate, logged as they jump (a share given or taken).
	static int s_left = -1, s_told = 0;
	int left = 0;
	for (int k = 0; k < nLocked; k++) {
		const Locked& L = locked[k];
		char* rec0 = *(char**)(L.sq + 0x144);
		bool active = (*(uint8_t*)(L.sq + 0x152) & 2) != 0;
		for (unsigned i = 0; i < L.o->n; i++) {
			if (L.sel >= 0 && L.sel != (int)i) continue;
			const char* rec = rec0 + i * 0x1C;
			if (*(const int16_t*)(rec + 0x18) >= 0) left += Left(rec) + (active ? Alive(L.sq, i) : 0);
		}
	}
	if (!nLocked) left = -1;
	if (left != s_left && (s_left < 0 || left < 0 || left > s_left || left < s_left - 1) && s_told++ < 80)
		Log("experimental: fight gate: %d squad(s), %d Romans left (rules %s)", nLocked, left, kModeName[m]);
	// Pool by pool: as the count goes up (LV3-6's went from 197 back to 200 with
	// nothing of the mod's logged); every change with XXL_GATELOG (autotests).
	static const bool gateLog = getenv("XXL_GATELOG") != nullptr;
	static int s_upTold = 0;
	if ((gateLog && left != s_left) || (left > s_left && s_left >= 0 && s_upTold++ < 20))
		for (int k = 0; k < nLocked; k++) {
			char line[400];
			PoolsLine(locked[k].sq, *locked[k].o, line, sizeof(line));
			Log("experimental: gate %d -> %d, squad %p (active %d):%s", s_left, left, locked[k].sq,
			    (*(uint8_t*)(locked[k].sq + 0x152) >> 1) & 1, line);
		}
	s_left = left;
	if (!nLocked) {
		if (pending) Log("experimental: %d Romans of beaten pools have no gate to go to", pending);
		return;
	}
	int16_t* target = nullptr;
	int targetLeft = -1, targetPool = -1;
	Locked* targetL = nullptr;
	for (int k = 0; k < nLocked; k++) {
		Locked& L = locked[k];
		if (!L.o->full) continue;
		int best = BestPool(L.sq, *L.o, L.sel);
		if (best < 0) continue;
		char* rec = *(char**)(L.sq + 0x144) + best * 0x1C;
		if (Left(rec) > targetLeft) { targetLeft = Left(rec); target = (int16_t*)(rec + 0x18); targetPool = best; targetL = &L; }
	}
	if (!target) {
		if (pending) Log("experimental: %d Romans of beaten pools: no pool of the gate's left to go to", pending);
		return;
	}
	int deficit = 0;
	for (int k = 0; k < nLocked; k++) {
		const Locked& L = locked[k];
		for (unsigned i = 0; i < L.o->n; i++)
			if ((L.sel < 0 || L.sel == (int)i) && !L.o->scaled[i] && !L.o->gaveTo[i] && L.o->origCount[i] > 0) {
				int rem = Remaining(L.sq, *(char**)(L.sq + 0x144) + i * 0x1C, i);
				deficit += Scaled(rem, g_config.squadCountFactor, 32767) - rem;
				L.o->gaveTo[i] = targetL->sq;
			}
	}
	if (deficit + pending <= 0) return;
	int before = *target;
	Give(&targetL->o->extra[targetPool], (char*)target - 0x18, deficit + pending);
	Log("experimental: a fight gate counts %d squad(s): %d Romans (kept pools %d, beaten pools %d) go to squad %p pool %d (%d -> %d)",
	    nLocked, deficit + pending, deficit, pending, targetL->sq, targetPool, before, *target);
}

// Romans come from pools (CKGrpPoolSquad, pool record +0) that squads of the
// same place share, and a pool holds what the level needs at once there
// (LV5-6: pool of 7, squad 40 places 3, squad 4's script keeps 4 of its 40
// alive). Doubled starting Romans of one squad (6) are taken for the whole
// fight and left squad 4 one at a time. So a squad's starting Romans stay as
// the level has them (shared) when its pool also feeds another squad whose box
// overlaps its own and whose numbers are scaled. Checked once the level is in.
void CheckSharedPools() {
	if (g_config.squadAliveFactor == 1.0f) return;
	// Pools filled up to the factor (levelforge.cpp): enough Romans for both squads.
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	if (gm && LevelForgePoolsFilled(*(int*)(gm + ax::kGM_currentLevel))) return;
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
			if (!o.atOnce[i] || (Scaled(o.origU1[i], g_config.squadAliveFactor, 255) == o.origU1[i] &&
			                     Scaled(o.origU2[i], g_config.squadAliveFactor, 255) == o.origU2[i]))
				continue;
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
			o.shared[i] = true;
			Log("experimental: squad %p pool %u: starting Romans %u/%u as the level has them "
			    "(their pool %p also feeds squad %p pool %u here)", all[s].sq, i, o.origU1[i], o.origU2[i],
			    pool, other, otherRec);
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
// While the rules are on; the game's own otherwise. A bar under way when that
// changes keeps its share: each hero's hits, full bar's time and decay timer
// are rescaled with the levels (the hero not leading: individual heroes' copy).
char*   g_comboGm = nullptr;
uint8_t g_comboOrig[4] = {}, g_comboSet[4] = {};   // the thresholds: the game's, as last written
float   g_drainOrig[2] = {}, g_drainSet[2] = {};   // +0x9C, +0xA0
float   g_hitsK = 1.0f, g_gaugeK = 1.0f, g_decayK = 1.0f;
unsigned g_fullAt = 0;

void RescaleBar(uint32_t* hits, float* gauge, float* decay, bool full) {
	uint32_t h = (uint32_t)(*hits * g_hitsK + 0.5f);
	if (!full && g_fullAt && h >= g_fullAt) h = g_fullAt - 1;
	*hits = h;
	*gauge *= g_gaugeK;
	*decay *= g_decayK;
}

void ComboFrame(Mode m) {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
	if (!gm || !cb) return;
	uint8_t* th[4] = { (uint8_t*)(gm + 0xB8), (uint8_t*)(gm + 0xB9), (uint8_t*)(gm + 0xBA), (uint8_t*)(gm + 0xC8) };
	float* dr[2] = { (float*)(gm + 0x9C), (float*)(gm + 0xA0) };
	bool rewritten = gm != g_comboGm;   // the game's own values put in again (a level loaded)
	for (int i = 0; i < 4; i++)
		if (gm != g_comboGm || *th[i] != g_comboSet[i]) { g_comboOrig[i] = *th[i]; rewritten = true; }
	for (int i = 0; i < 2; i++)
		if (gm != g_comboGm || *dr[i] != g_drainSet[i]) { g_drainOrig[i] = *dr[i]; rewritten = true; }
	bool on = m == MODE_ON;
	float fill = on ? g_config.comboFactor : 1.0f;
	uint8_t want[4];
	for (int i = 0; i < 4; i++) {
		int v = (int)(g_comboOrig[i] / fill + 0.5f);
		want[i] = (uint8_t)(v > 255 ? 255 : v);
	}
	float wantDr[2] = { g_drainOrig[0] / (on ? g_config.comboDrainFactor : 1.0f),
	                    g_drainOrig[1] / (on ? g_config.comboFullDrainFactor : 1.0f) };
	bool same = !rewritten && wantDr[0] == g_drainSet[0] && wantDr[1] == g_drainSet[1];
	for (int i = 0; i < 4 && same; i++) same = want[i] == g_comboSet[i];
	if (same) return;
	if (!rewritten) {
		g_hitsK = g_comboSet[3] ? (float)want[3] / g_comboSet[3] : 1.0f;
		g_fullAt = want[3];
		g_gaugeK = g_drainSet[1] > 0.0f ? wantDr[1] / g_drainSet[1] : 1.0f;
		g_decayK = g_drainSet[0] > 0.0f ? wantDr[0] / g_drainSet[0] : 1.0f;
		uint32_t* hits = (uint32_t*)(gm + 0xB0);
		uint32_t h0 = *hits;
		RescaleBar(hits, (float*)(gm + 0xA4), (float*)(gm + 0xA8), (*(uint32_t*)(cb + 0x10) & 0x4000) != 0);
		IndivEachComboCopy(RescaleBar);
		static int s_told = 0;
		if (s_told++ < 60)
			Log("experimental: combo bar full at %u -> %u, a hit lost every %.2f -> %.2f s, full for %.2f -> %.2f s "
			    "(rules %s); the leader's hits %u -> %u", g_comboSet[3], want[3], g_drainSet[0], wantDr[0], g_drainSet[1],
			    wantDr[1], kModeName[m], h0, *hits);
	} else {
		Log("experimental: combo bar levels %u/%u/%u, full at %u -> %u/%u/%u, full at %u; a hit lost every %.2f -> %.2f s, "
		    "full for %.2f -> %.2f s (rules %s)", g_comboOrig[0], g_comboOrig[1], g_comboOrig[2], g_comboOrig[3], want[0],
		    want[1], want[2], want[3], g_drainOrig[0], wantDr[0], g_drainOrig[1], wantDr[1], kModeName[m]);
	}
	for (int i = 0; i < 4; i++) *th[i] = g_comboSet[i] = want[i];
	for (int i = 0; i < 2; i++) *dr[i] = g_drainSet[i] = wantDr[i];
	g_comboGm = gm;
}

} // namespace

bool EnemyKindKeptAsIs(char* cpnt) { return KeptAsIs(cpnt); }

bool ExperimentalExtraSpot(char* squad, int n, float* pos, float* dir) {
	char* node = squad ? *(char**)(squad + 0x30) : nullptr;
	if (!node || n < 0) return false;
	char* key = ((KeyFn)0x52C780)(squad, nullptr);
	unsigned spots = key ? *(unsigned*)(key + 0xC) : 0;
	char* list = key ? *(char**)(key + 4) : nullptr;
	float p[3], d[3];
	int row;
	if (spots && list && spots < 256) {
		const float* s = (const float*)(list + (n % spots) * 0x1C);
		((XformFn)0x5918B0)(p, s, 1, node);
		((XformFn)0x591910)(d, s + 3, 1, node);
		row = n / (int)spots + 1;
	} else {   // no formation: round the centre
		memcpy(p, node + 0x30, 12);
		memcpy(d, node + 0x20, 12);
		row = n + 1;
	}
	float len = sqrtf(d[0] * d[0] + d[2] * d[2]);
	float sx = len > 0.001f ? -d[2] / len : 1.0f, sz = len > 0.001f ? d[0] / len : 0.0f;   // sideways
	float off = 1.5f * ((row + 1) / 2) * ((row & 1) ? 1.0f : -1.0f);
	pos[0] = p[0] + sx * off;
	pos[1] = p[1];
	pos[2] = p[2] + sz * off;
	memcpy(dir, d, 12);
	return true;
}

void ExperimentalFrame() {
	ApartFrame();
	Mode m = ModeNow();
	static int s_mode = -1, s_told = 0;
	if (m != s_mode) {
		if (s_mode >= 0 && s_told++ < 80)
			Log("experimental: squad and combo bar rules: %s -> %s", kModeName[s_mode], kModeName[m]);
		s_mode = m;
	}
	CheckSharedPools();
	DWORD now = GetTickCount();
	GateStartFrame(m, now);
	UpdateSquads(m, now);
	GateFrame(m);
	// (Once on, it goes on: factors put back to 1 in the co-op options give the game's own values back.)
	if (g_comboGm || g_config.comboFactor != 1.0f || g_config.comboDrainFactor != 1.0f || g_config.comboFullDrainFactor != 1.0f)
		ComboFrame(m);
}


bool ExperimentalInstallHooks() {
	bool ok = true;
	bool squads = g_config.squadAliveFactor != 1.0f || g_config.squadCountFactor != 1.0f;
	bool combo = g_config.comboFactor != 1.0f || g_config.comboDrainFactor != 1.0f || g_config.comboFullDrainFactor != 1.0f;
	g_pathCheck = (squads || combo) && !g_config.rulesStatic && g_config.apartByPath;
	Log("experimental: squad and combo bar rules %s; the heroes' path %s", !squads && !combo ? "none (the game's own)" :
	    g_config.rulesStatic ? "static (on all through co-op)" : "dynamic (the game's own while the heroes are apart)",
	    g_pathCheck ? "looked up every 0.5 s in co-op" : "not looked up");
	g_logCmds = getenv("XXL_SQUADCMDS") != nullptr;
	if (true) {   // (always: the co-op options can turn the factors on while playing)
		ok &= InstallDetour(kSquadDeserialize, (void*)SquadDeserializeHook, (void**)&g_origSquadDeserialize,
		                    kSquadDeserializePrologue, sizeof(kSquadDeserializePrologue),
		                    "squad load (experimental: Romans at once / to defeat)");
		ok &= PatchVtableSlot(0x6266B0, 25, 0x4C7F80, (void*)SquadCmdHook, (void**)&g_origSquadCmd,
		                      "CKGrpSquadEnemy::vf25 (squad script numbers)");
		ok &= PatchVtableSlot(0x626A6C, 25, 0x4C7F80, (void*)JetCmdHook, (void**)&g_origJetCmd,
		                      "CKGrpSquadJetPack::vf25 (squad script numbers)");
	}
	ok &= InstallDetour(kFormation, (void*)FormationHook, (void**)&g_origFormation, kFormationPrologue,
	                    sizeof(kFormationPrologue), "squad formation tick (extra Romans' spots)");
	ok &= InstallDetour(kSlot, (void*)SlotHook, (void**)&g_origSlot, kSlotPrologue, sizeof(kSlotPrologue),
	                    "squad formation spot (extra Romans beside the spots, not on the centre)");
	ok &= InstallDetour(kAvoidAdd, (void*)AvoidAddHook, (void**)&g_origAvoidAdd, kAvoidAddPrologue,
	                    sizeof(kAvoidAddPrologue), "avoidance add (at most its 50 entries)");
	ok &= InstallDetour(kShopDeserialize, (void*)ShopDeserializeHook, (void**)&g_origShopDeserialize,
		                    kShopDeserializePrologue, sizeof(kShopDeserializePrologue),
		                    "merchant load (experimental: prices)");
	return ok;
}
