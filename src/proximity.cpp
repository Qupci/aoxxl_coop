#include "proximity.h"
#include "coop.h"
#include "detour.h"
#include "log.h"
#include "trio.h"

#include <cmath>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

const uintptr_t kBundleVtbl     = 0x6240C4;   // CKBundle (12,3)
const size_t    kBundleTickSlot = 17;
const uintptr_t kBundleTick     = 0x43DFE0;

// Lives that must keep the real arrangement: the heroes' own (their ticks do
// their own slicing in coop.cpp) and the in-game HUD.
const uintptr_t kAsterixLifeVtbl = 0x625C90;
const uintptr_t kObelixLifeVtbl  = 0x625D18;
const uintptr_t kIdefixLifeVtbl  = 0x625DA0;
const uintptr_t kHudHookVtbl     = 0x6285F0;   // CKHkInterfaceInGame (2,132)
const uintptr_t kBonusGroupLife  = 0x6296FC;   // CKGrpBonusLife: handled per bonus below

// Bonus pools (helmets and the like). CKGrpBonusPool::vf27 updates every active
// bonus, then attracts those within the pool's radius of the leader's position
// to the leader (bonus->vf48(hero, 0)); a bonus then flies to the hero it was
// given and is collected by it. After each pool update the same attraction pass
// is run for player 2's hero, with the pool's own "may this hero collect"
// check (vf39). CKGrpAsterixBonusPool::vf27 calls the base function directly,
// so both vtables are hooked.
const uintptr_t kBonusPoolVtbl        = 0x629658;   // CKGrpBonusPool (4,61)
const uintptr_t kAsterixBonusPoolVtbl = 0x62808C;   // CKGrpAsterixBonusPool (4,63)
const size_t    kPoolUpdateSlot       = 27;
const uintptr_t kBonusPoolUpdate        = 0x54C380;
const uintptr_t kAsterixBonusPoolUpdate = 0x5072C0;

// Objects further than this from both players keep the real leader.
const float kMaxRange2 = 90.0f * 90.0f;

typedef void(__fastcall* TickFn)(void* self, void* edx, void* dt);
TickFn g_origBundleTick = nullptr;
// __thiscall with one stack argument (dt): CKGrpBonusLife::vf17 passes it and
// the function ends in `ret 4` (the decompiler shows it as taking none).
typedef void(__fastcall* PoolFn)(void* self, void* edx, void* dt);
PoolFn g_origPoolUpdate = nullptr;
PoolFn g_origAsterixPoolUpdate = nullptr;
volatile long g_slices = 0;
volatile long g_p2Bonuses = 0;
bool g_loggedOverride = false;

bool NodePos(void* node, Vector3* out) {
	if (!node) return false;
	char* frame = *(char**)((char*)node + 4);
	if (!frame) return false;
	const float* p = (const float*)(frame + 0x80);   // LTM position
	if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
	if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) return false;
	*out = Vector3(p[0], p[1], p[2]);
	return true;
}

// Where in the world is this Life's object? Hook lives point at their hook
// (life+0x10); group lives at their group (life+0x0C, as CKGrpTrioLife and
// CKGrpBonusLife read it), located by its first child hook with a node.
bool LifePos(int* life, bool isGroup, Vector3* out) {
	char* owner = *(char**)((char*)life + (isGroup ? 0x0C : 0x10));
	if (!owner) return false;
	if (!isGroup) {
		if (*(uintptr_t*)owner == kHudHookVtbl) return false;
		return NodePos(*(void**)(owner + 0x10), out);
	}
	for (char* hook = *(char**)(owner + 0x20); hook; hook = *(char**)(hook + 4))
		if (NodePos(*(void**)(hook + 0x10), out)) return true;
	return false;
}

void TickLife(int* life, void* dt, bool isGroup, Trio t, int p1, int p2, const Vector3& pos1,
              const Vector3& pos2) {
	TickFn tick = (TickFn)(*(void***)life)[17];
	uintptr_t vt = *(uintptr_t*)life;
	Vector3 at;
	if (vt == kAsterixLifeVtbl || vt == kObelixLifeVtbl || vt == kIdefixLifeVtbl ||
	    vt == kBonusGroupLife || !LifePos(life, isGroup, &at)) {
		tick(life, nullptr, dt);
		return;
	}
	float d1 = (at - pos1).len2(), d2 = (at - pos2).len2();
	int nearest = d2 < d1 ? p2 : p1;
	uint8_t* tr = (uint8_t*)t.raw();
	if ((d1 > kMaxRange2 && d2 > kMaxRange2) || tr[0x2C] == nearest) {
		tick(life, nullptr, dt);
		return;
	}
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	int other = nearest == p1 ? p2 : p1;
	tr[0x2C] = (uint8_t)nearest;
	tr[0x2D] = (uint8_t)other;
	InterlockedIncrement(&g_slices);
	tick(life, nullptr, dt);
	if (tr[0x2C] == (uint8_t)nearest && tr[0x2D] == (uint8_t)other) {
		tr[0x2C] = leader;
		tr[0x2D] = companion;
	} else if (!g_loggedOverride) {
		g_loggedOverride = true;   // e.g. a blue circle switched leaders: keep it
		Log("proximity: an object changed the trio's leader during its tick (now %s); kept",
			HeroName(tr[0x2C]));
	}
}

// Mirrors CKBundle::vf17 exactly, with the per-object leader around each tick.
void __fastcall BundleTickHook(char* bundle, void* edx, void* dt) {
	Trio t = GetTrio();
	int p1 = CoopP1Hero(), p2 = CoopP2Hero();
	Hero h1 = t ? t.GetHero(p1) : Hero(), h2 = t ? t.GetHero(p2) : Hero();
	if (!CoopP2Joined() || CoopSuspended() || !h1 || !h2) {
		g_origBundleTick(bundle, edx, dt);
		return;
	}
	Vector3 pos1 = h1.Pos(), pos2 = h2.Pos();

	int* groupLife = *(int**)(bundle + 0x0C);
	if (groupLife && (groupLife[1] & 3) == 0)
		TickLife(groupLife, dt, true, t, p1, p2, pos1, pos2);
	for (int* life = *(int**)(bundle + 0x10); life; life = (int*)life[2]) {
		if ((*(uint8_t*)(life + 1) & 2) == 0)
			TickLife(life, dt, false, t, p1, p2, pos1, pos2);
	}
}

void AttractToPlayer2(char* pool) {
	Trio t = GetTrio();
	Hero h2 = t ? t.GetHero(CoopP2Hero()) : Hero();
	if (!h2 || !CoopP2Joined() || CoopSuspended()) return;
	typedef char(__fastcall* CanFn)(void* pool, void* edx, void* hero);
	typedef char(__fastcall* BoolFn)(void* self, void* edx);
	typedef void(__fastcall* AttractFn)(void* self, void* edx, void* hero, int zero);
	if (!((CanFn)(*(void***)pool)[39])(pool, nullptr, h2.hook)) return;
	char* radiusObj = *(char**)(pool + 0x3C);
	if (!radiusObj) return;
	float r2 = *(float*)(radiusObj + 4);
	Vector3 p2 = h2.Pos();
	for (char* b = *(char**)(pool + 0x30); b; b = *(char**)(b + 0x14)) {
		if (((BoolFn)(*(void***)b)[19])(b, nullptr)) continue;   // already following someone
		Vector3 at;
		if (!NodePos(*(void**)(b + 0x10), &at)) continue;
		if ((at - p2).len2() < r2) {
			((AttractFn)(*(void***)b)[18])(b, nullptr, h2.hook, 0);
			InterlockedIncrement(&g_p2Bonuses);
		}
	}
}

// Placed bonuses (the helmets hanging in the air) are beacons, not pool
// members: every frame CKGrpBonusPool::vf27 walks the beacons of the live
// sectors, gives each uncollected one a bonus from the free list (vf21 places it,
// vf15 updates it) and, when the leader is within the pool's radius, calls
// bonus->vf18(leader, &beacon), which sends it flying and marks the beacon
// collected. Bonuses nobody claimed go back to the free list next frame. That
// distance check (0x54C6C6..0x54C719) is replaced: same test for the leader,
// then the same for player 2's hero.
const uintptr_t kBeaconCheck     = 0x54C6C6;   // mov al,[esp+13h]; test al,al; jz ...
const uintptr_t kBeaconCheckDone = 0x54C71A;
const uint8_t   kBeaconCheckBytes[10] = { 0x8A, 0x44, 0x24, 0x13, 0x84, 0xC0, 0x74, 0x4C, 0xD9, 0x44 };
volatile long g_p2Beacons = 0;

// `frame` is vf27's ESP inside the beacon loop: +0x13 the leader may collect,
// +0x1C the beacon descriptor, +0x20 the leader's hook, +0x3C leader position,
// +0x48 beacon position.
void __cdecl BeaconCheck(char* pool, char* bonus, char* frame) {
	typedef void(__fastcall* AttractFn)(void* self, void* edx, void* hero, void* beacon);
	typedef char(__fastcall* CanFn)(void* pool, void* edx, void* hero);
	AttractFn attract = (AttractFn)(*(void***)bonus)[18];
	float r2 = *(float*)(*(char**)(pool + 0x3C) + 4);
	const float* b = (const float*)(frame + 0x48);
	void* beacon = frame + 0x1C;
	if (*(uint8_t*)(frame + 0x13)) {
		const float* l = (const float*)(frame + 0x3C);
		float dx = b[0] - l[0], dy = b[1] - l[1], dz = b[2] - l[2];
		if (dx * dx + dy * dy + dz * dz <= r2) {
			attract(bonus, nullptr, *(void**)(frame + 0x20), beacon);
			return;
		}
	}
	if (!CoopP2Joined() || CoopSuspended()) return;
	Trio t = GetTrio();
	Hero h2 = t ? t.GetHero(CoopP2Hero()) : Hero();
	if (!h2 || !((CanFn)(*(void***)pool)[39])(pool, nullptr, h2.hook)) return;
	Vector3 p = h2.Pos();
	float dx = b[0] - p.x, dy = b[1] - p.y, dz = b[2] - p.z;
	if (dx * dx + dy * dy + dz * dz <= r2) {
		attract(bonus, nullptr, h2.hook, beacon);
		InterlockedIncrement(&g_p2Beacons);
	}
}

__declspec(naked) void BeaconCheckThunk() {
	__asm {
		pushad
		lea  eax, [esp + 36]   // vf27's ESP: 8 saved registers + our return address
		push eax
		push edi               // the bonus just placed on the beacon
		push esi               // the pool
		call BeaconCheck
		add  esp, 12
		popad
		ret
	}
}

bool PatchBeaconCheck() {
	uint8_t* code = (uint8_t*)kBeaconCheck;
	if (memcmp(code, kBeaconCheckBytes, sizeof(kBeaconCheckBytes)) != 0) {
		Log("proximity: beacon check at %08X does not match -- not patching", (unsigned)kBeaconCheck);
		return false;
	}
	DWORD old = 0;
	if (!VirtualProtect(code, sizeof(kBeaconCheckBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
	code[0] = 0xE8;   // call BeaconCheckThunk
	*(uint32_t*)(code + 1) = (uint32_t)((uintptr_t)BeaconCheckThunk - (kBeaconCheck + 5));
	code[5] = 0xE9;   // jmp past the original check
	*(uint32_t*)(code + 6) = (uint32_t)(kBeaconCheckDone - (kBeaconCheck + 10));
	VirtualProtect(code, sizeof(kBeaconCheckBytes), old, &old);
	FlushInstructionCache(GetCurrentProcess(), code, sizeof(kBeaconCheckBytes));
	Log("proximity: beacon bonus check %08X -> %p", (unsigned)kBeaconCheck, (void*)BeaconCheckThunk);
	return true;
}

void __fastcall PoolUpdateHook(void* self, void* edx, void* dt) {
	g_origPoolUpdate(self, edx, dt);
	AttractToPlayer2((char*)self);
}

void __fastcall AsterixPoolUpdateHook(void* self, void* edx, void* dt) {
	g_origAsterixPoolUpdate(self, edx, dt);
	AttractToPlayer2((char*)self);
}

// Collision contacts. CKSrvCollision's tick hands out what its tests found:
//  * FUN_00447E30, shape-pair tests: receiver->sendEvent(0x300, list), list
//    entries {next, test}; a test starts with its two shapes.
//  * FUN_00447FF0, projectiles (punches, thrown things) against targets:
//    sendEvent(0x301, list) to each target's owner and to each projectile's
//    owner; entries {next, u8 record, u8 projectile, u16 target, u8 sub}, the
//    projectile's shape being at service+0x24 + 8*projectile.
// A shape's owner hook is at +0x20. Interactive objects only accept what comes
// from the trio's leader: CKHkBumper launches the leader if the touching shape
// is the leader's; an iron crate (filter 0x4B0DF0) breaks only if the punch is
// the leader's and the leader is not Asterix. Those calls are redirected here,
// and when the contact comes from player 2's hero alone (or the receiver is that
// hero), the leader is player 2's hero for the length of the call.
typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
const uintptr_t kPairCallSite = 0x447FD1;          // mov edx,[ecx]; call [edx+14h]
const uint8_t   kPairCallBytes[5] = { 0x8B, 0x11, 0xFF, 0x52, 0x14 };
const uintptr_t kTargetCallSite = 0x4483C5;        // mov edx,[ecx]; call [edx+14h]
const uint8_t   kTargetCallBytes[5] = { 0x8B, 0x11, 0xFF, 0x52, 0x14 };
const uintptr_t kProjectileCallSite = 0x448405;    // mov edx,[ecx]; push eax; push 301h; call [edx+14h]
const uint8_t   kProjectileCallBytes[11] = { 0x8B, 0x11, 0x50, 0x68, 0x01, 0x03, 0x00, 0x00,
                                             0xFF, 0x52, 0x14 };
volatile long g_p2Contacts = 0;

// Which player hero does this contact list come from? -1: none or both.
template <class OwnerAt>
int ContactActor(char* receiver, Hero h1, Hero h2, int p1, int p2, OwnerAt ownersOf) {
	if (receiver == h1.raw()) return p1;
	if (receiver == h2.raw()) return p2;
	bool by1 = false, by2 = false;
	ownersOf([&](char* owner) {
		if (owner == h1.raw()) by1 = true;
		else if (owner == h2.raw()) by2 = true;
	});
	if (by2 && !by1) return p2;
	if (by1 && !by2) return p1;
	return -1;
}

template <class OwnerAt>
int DispatchAs(char* receiver, int event, void* list, OwnerAt ownersOf) {
	SendEventFn send = (SendEventFn)(*(void***)receiver)[5];
	Trio t = GetTrio();
	if (!t || !CoopP2Joined() || CoopSuspended()) return send(receiver, nullptr, event, list);
	int p1 = CoopP1Hero(), p2 = CoopP2Hero();
	Hero h1 = t.GetHero(p1), h2 = t.GetHero(p2);
	if (!h1 || !h2) return send(receiver, nullptr, event, list);
	int actor = ContactActor(receiver, h1, h2, p1, p2, ownersOf);
	uint8_t* tr = (uint8_t*)t.raw();
	if (actor < 0 || tr[0x2C] == actor) return send(receiver, nullptr, event, list);

	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	int other = actor == p1 ? p2 : p1;
	tr[0x2C] = (uint8_t)actor;
	tr[0x2D] = (uint8_t)other;
	if (actor == p2) InterlockedIncrement(&g_p2Contacts);
	int r = send(receiver, nullptr, event, list);
	if (tr[0x2C] == (uint8_t)actor && tr[0x2D] == (uint8_t)other) {
		tr[0x2C] = leader;
		tr[0x2D] = companion;
	}
	return r;
}

int __fastcall PairContactDispatch(char* receiver, void*, int event, int* list) {
	return DispatchAs(receiver, event, list, [&](auto&& note) {
		for (int* e = list; e; e = (int*)e[0]) {
			char** test = (char**)e[1];
			if (!test) continue;
			for (int k = 0; k < 2; k++)
				if (test[k]) note(*(char**)(test[k] + 0x20));
		}
	});
}

int __fastcall ProjectileContactDispatch(char* receiver, void*, int event, uint8_t* list) {
	return DispatchAs(receiver, event, list, [&](auto&& note) {
		CKYellowPages* yp = YellowPages();
		char* srv = yp ? *(char**)((char*)yp + 0x50) : nullptr;
		if (!srv) return;
		unsigned count = *(uint8_t*)(srv + 0x1B6);
		for (uint8_t* e = list; e; e = *(uint8_t**)e) {
			unsigned idx = e[5];
			if (idx >= count) continue;
			char* shape = *(char**)(srv + 0x24 + 8 * idx);
			if (shape) note(*(char**)(shape + 0x20));
		}
	});
}

// The projectile-owner loop loads the vtable before its pushes, so the patch
// there rewrites the whole sequence: push eax; push 301h; call dispatcher.
bool PatchProjectileOwnerCall() {
	uint8_t* code = (uint8_t*)kProjectileCallSite;
	if (memcmp(code, kProjectileCallBytes, sizeof(kProjectileCallBytes)) != 0) {
		Log("proximity: projectile owner call site %08X does not match -- not patching",
			(unsigned)kProjectileCallSite);
		return false;
	}
	DWORD old = 0;
	if (!VirtualProtect(code, sizeof(kProjectileCallBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
	code[0] = 0x50;                                   // push eax (the list)
	code[1] = 0x68; *(uint32_t*)(code + 2) = 0x301;   // push 301h
	code[6] = 0xE8;                                   // call ProjectileContactDispatch
	*(uint32_t*)(code + 7) = (uint32_t)((uintptr_t)ProjectileContactDispatch - (kProjectileCallSite + 11));
	VirtualProtect(code, sizeof(kProjectileCallBytes), old, &old);
	FlushInstructionCache(GetCurrentProcess(), code, sizeof(kProjectileCallBytes));
	Log("proximity: projectile hits, owners (0x301) call site %08X -> %p", (unsigned)kProjectileCallSite,
		(void*)ProjectileContactDispatch);
	return true;
}

} // namespace

long ProximityP2Contacts() { return g_p2Contacts; }
long ProximityP2Beacons() { return g_p2Beacons; }
long ProximitySlices() { return g_slices; }
long ProximityP2Bonuses() { return g_p2Bonuses; }

bool ProximityInstallHooks() {
	bool ok = PatchVtableSlot(kBundleVtbl, kBundleTickSlot, kBundleTick,
	                          (void*)BundleTickHook, (void**)&g_origBundleTick, "CKBundle::tick");
	ok &= PatchVtableSlot(kBonusPoolVtbl, kPoolUpdateSlot, kBonusPoolUpdate,
	                      (void*)PoolUpdateHook, (void**)&g_origPoolUpdate, "CKGrpBonusPool::update");
	ok &= PatchVtableSlot(kAsterixBonusPoolVtbl, kPoolUpdateSlot, kAsterixBonusPoolUpdate,
	                      (void*)AsterixPoolUpdateHook, (void**)&g_origAsterixPoolUpdate,
	                      "CKGrpAsterixBonusPool::update");
	ok &= PatchCallSite(kPairCallSite, kPairCallBytes, sizeof(kPairCallBytes),
	                    (void*)PairContactDispatch, "collision contacts (0x300)");
	ok &= PatchCallSite(kTargetCallSite, kTargetCallBytes, sizeof(kTargetCallBytes),
	                    (void*)ProjectileContactDispatch, "projectile hits, targets (0x301)");
	ok &= PatchProjectileOwnerCall();
	ok &= PatchBeaconCheck();
	return ok;
}
