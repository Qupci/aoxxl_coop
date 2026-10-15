#include "experimental.h"
#include "config.h"
#include "coop.h"
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
// player's hero is held by the level (a cable car ride...).
bool AtOnceNow() {
	return !(CoopP2Joined() && (CoopHeroHeld(0) || CoopHeroHeld(1)));
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

// The combo bar fills with the hits at game manager +0xB0 (+1 a hit, +3 with
// the potion, from four places); FUN_0048C860 sets its level from the
// thresholds +0xB8..+0xBA (bytes) and fills it at +0xC8 (byte: "full" once the
// hits reach it); the HUD draws hits / +0xC8, a full bar's refill per hit is
// +0xA0 / +0xC8. Those thresholds are divided by the factor: the bar fills
// that much slower, whoever hits.
char*   g_comboGm = nullptr;
uint8_t g_comboSet[4] = {};   // the thresholds as last written

} // namespace

void ExperimentalFrame() {
	static int s_atOnce = -1;
	int atOnce = AtOnceNow() ? 1 : 0;
	if (atOnce != s_atOnce && g_config.squadAliveFactor != 1.0f) {
		if (s_atOnce >= 0)
			Log("experimental: Romans at once %s", atOnce ? "x%.2f again (both players play)" : "as the level has them (a player's hero is held)", g_config.squadAliveFactor);
		s_atOnce = atOnce;
	}
	if (g_config.comboFactor == 1.0f) return;
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	if (!gm) return;
	uint8_t* th[4] = { (uint8_t*)(gm + 0xB8), (uint8_t*)(gm + 0xB9), (uint8_t*)(gm + 0xBA), (uint8_t*)(gm + 0xC8) };
	bool same = gm == g_comboGm;
	for (int i = 0; i < 4 && same; i++) same = *th[i] == g_comboSet[i];
	if (same) return;
	uint8_t orig[4];
	for (int i = 0; i < 4; i++) {
		orig[i] = *th[i];
		int v = (int)(orig[i] / g_config.comboFactor + 0.5f);
		*th[i] = (uint8_t)(v > 255 ? 255 : v);
		g_comboSet[i] = *th[i];
	}
	g_comboGm = gm;
	Log("experimental: combo bar levels %u/%u/%u, full at %u -> %u/%u/%u, full at %u", orig[0], orig[1], orig[2], orig[3],
	    *th[0], *th[1], *th[2], *th[3]);
}

namespace {
} // namespace

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
