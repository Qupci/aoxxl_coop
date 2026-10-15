#include "experimental.h"
#include "config.h"
#include "coop.h"
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

// What a squad's pools held in the level file, and how the mod treats it.
struct SquadOrig {
	unsigned n = 0;
	int16_t  count[8] = {};      // Romans to defeat (as loaded)
	uint8_t  placed[8] = {};     // placed when the squad first wakes (U2)
	bool     placedOnly[8] = {}; // all its Romans placed at the start, none sent in after: a set piece
	bool     atOnce = false;     // numbers at once scaled (a pool its script sends Romans in for)
	DWORD    stalledSince[8] = {};
};
std::unordered_map<char*, SquadOrig> g_squads;

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
	for (unsigned i = 0; i < n; i++) {
		char* rec = rec0 + i * 0x1C;
		o.count[i] = *(int16_t*)(rec + 0x18);
		o.placed[i] = *(uint8_t*)(rec + 0x11);
		uint8_t u1 = *(uint8_t*)(rec + 0x10);
		o.placedOnly[i] = o.count[i] > 0 && o.count[i] <= (o.placed[i] > u1 ? o.placed[i] : u1);
		if (!o.placedOnly[i] && o.count[i] != 0) o.atOnce = true;
	}
	char line[600];
	int len = _snprintf(line, sizeof(line), "experimental: squad %p:", sq);
	int before = 0, after = 0;
	for (unsigned i = 0; i < n; i++) {
		char* rec = rec0 + i * 0x1C;
		uint8_t& u1 = *(uint8_t*)(rec + 0x10);
		uint8_t& u2 = *(uint8_t*)(rec + 0x11);
		int16_t& count = *(int16_t*)(rec + 0x18);
		int a0 = u1, s0 = u2, c0 = count;
		if (!o.placedOnly[i] && g_config.squadAliveFactor != 1.0f) {
			u1 = (uint8_t)Scaled(u1, g_config.squadAliveFactor, 255);
			u2 = (uint8_t)Scaled(u2, g_config.squadAliveFactor, 255);
		}
		if (count > 0 && g_config.squadCountFactor != 1.0f)
			count = (int16_t)Scaled(count, g_config.squadCountFactor, 32767);
		if (c0 > 0) { before += c0; after += count; }
		if (len > 0 && len < (int)sizeof(line))
			len += _snprintf(line + len, sizeof(line) - len, " [%sat once %d/%d -> %d/%d, %d -> %d]",
			                 o.placedOnly[i] ? "placed, " : "", a0, s0, u1, u2, c0, count);
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
		bool atOnce = it != g_squads.end() && it->second.atOnce;
		int n = (int)a[0];
		switch (cmd) {
		case 5:  if (atOnce && n >= 2) f = g_config.squadAliveFactor; break;
		case 8: case 12: case 19: case 20: if (atOnce && n >= 1) f = g_config.squadAliveFactor; break;
		case 16: if (n >= 2) f = g_config.squadCountFactor; break;
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

// A placed-only pool (a set piece: Romans in their spots, no script sending
// more) whose count was raised gets the rest as reinforcements: once its
// Romans have all fallen, the squad's own reinforcement tick (vf37 0x52F930,
// run while the squad is active) sends them in from out of the players'
// sight, as many at a time as the level placed.
typedef void(__fastcall* SquadTickFn)(char* self, void* edx);
SquadTickFn g_origReinforce[3] = {};

void TopUp(char* sq) {
	auto it = g_squads.find(sq);
	if (it == g_squads.end()) return;
	SquadOrig& o = it->second;
	unsigned n = *(unsigned*)(sq + 0x14C);
	char* rec0 = *(char**)(sq + 0x144);
	if (!rec0 || n != o.n) return;
	DWORD now = GetTickCount();
	for (unsigned i = 0; i < n; i++) {
		if (!o.placedOnly[i]) continue;
		char* rec = rec0 + i * 0x1C;
		int count = *(int16_t*)(rec + 0x18), sent = *(uint16_t*)(rec + 0x16);
		bool idle = count > o.count[i] && sent >= o.count[i] && sent < count &&
		            *(uint8_t*)(rec + 0x11) == 0 && *(uint8_t*)(rec + 0x14) == 0 && *(uint8_t*)(rec + 0x15) == 0;
		if (!idle) { o.stalledSince[i] = 0; continue; }
		if (!o.stalledSince[i]) { o.stalledSince[i] = now; continue; }
		if (now - o.stalledSince[i] < 1500) continue;
		int wave = count - sent;
		int most = o.placed[i] ? o.placed[i] : o.count[i];
		if (wave > most) wave = most;
		*(uint8_t*)(rec + 0x15) = (uint8_t)wave;
		o.stalledSince[i] = 0;
		Log("experimental: squad %p pool %u: its %d placed Romans have fallen -- %d more sent in (%d of %d)",
		    sq, i, o.count[i], wave, sent, count);
	}
}

void __fastcall ReinforceHook0(char* self, void* edx) { TopUp(self); g_origReinforce[0](self, edx); }
void __fastcall ReinforceHook1(char* self, void* edx) { TopUp(self); g_origReinforce[1](self, edx); }
void __fastcall ReinforceHook2(char* self, void* edx) { TopUp(self); g_origReinforce[2](self, edx); }

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
		// CKGrpSquadEnemy, CKGrpSquad, CKGrpSquadJetPack: reinforcements (vf37)
		const uintptr_t vts[3] = { 0x6266B0, 0x62682C, 0x626A6C };
		void* hooks[3] = { (void*)ReinforceHook0, (void*)ReinforceHook1, (void*)ReinforceHook2 };
		for (int i = 0; i < 3; i++)
			ok &= PatchVtableSlot(vts[i], 37, 0x52F930, hooks[i], (void**)&g_origReinforce[i],
			                      "squad vf37 (experimental: set pieces' extra Romans)");
	}
	if (g_config.priceFactor != 1.0f)
		ok &= InstallDetour(kShopDeserialize, (void*)ShopDeserializeHook, (void**)&g_origShopDeserialize,
		                    kShopDeserializePrologue, sizeof(kShopDeserializePrologue),
		                    "merchant load (experimental: prices)");
	return ok;
}
