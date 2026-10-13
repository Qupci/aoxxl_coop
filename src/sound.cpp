// Sound heard from both players' cameras.
//
// The engine has one listener: CKSoundManager (+4 -> wrapper -> object) copies
// player 1's camera frame into it every frame (CKSoundManager::vf17, 0x43ADD0:
// position +0x28, up +0x40, at +0x4C, right +0x58, from the frame's LTM). Two
// things use it:
//
//  * a range cut: a sound starts (FUN_0043B730) or keeps its voice (vf17) only
//    while it is within the sound's range of the listener -- four sqrt calls,
//    FUN_005917C0(dx*dx + dy*dy + dz*dz), at 0x43B0D5, 0x43B512 (vf17: playing
//    voices, deferred sounds) and 0x43B9B9, 0x43BCBF (FUN_0043B730);
//  * the software 3D mix of every voice, FUN_005DE840(&listener, n, in, out,
//    ...), which turns the voice's world position into volume and pan.
//
// While the split screen is up, the four range checks measure the distance to
// the nearer of the two cameras (each call site goes through a thunk that finds
// the sound and listener positions in the caller's stack frame), and a voice
// nearer to player 2's camera is mixed against a copy of the listener moved to
// player 2's camera. Player 1 hears what happens around player 2 as if standing
// there; the two halves are mixed into one output.

#include "coop.h"
#include "crash.h"
#include "detour.h"
#include "log.h"
#include "p2cam.h"
#include "splitscreen.h"
#include "trio.h"

#include <cmath>
#include <cstring>
#include <intrin.h>

namespace {

const uintptr_t kSqrt  = 0x5917C0;


// Player 2's camera, written by the game thread once per frame, read by the
// game thread (range checks) and the mixer (whatever thread it runs on).
struct P2Listener { float pos[3], right[3], up[3], at[3]; };
P2Listener g_l2[2];
volatile long g_l2Index = -1;    // which half of g_l2 is current, -1 = none
volatile long g_nearP2 = 0, g_mixedAsP2 = 0;

bool CurrentP2(P2Listener& out) {
	long i = g_l2Index;
	if (i < 0) return false;
	out = g_l2[i & 1];
	return true;
}

float Dist2(const float* a, const float* b) {
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// Replacement for FUN_005917C0 at the range checks: the distance from the sound
// `s` to the nearer of the game's listener `l` and player 2's camera.
// Region sounds (music and ambience zones: sound definition +0xC bit 0) also
// need the listener inside the sound instance's box (+0x20/+0x24/+0x28 max
// x/y/z, +0x2C/+0x30/+0x34 min) to start (vf17's deferred list) and to keep
// playing (vf17's voice loop, right after the range check at 0x43B0D5, on the
// same listener copy). The listener is the game camera, which follows the
// leader: a music zone only the non-leading player stood in stayed silent. In
// the voice loop the copy is pointed at player 2's camera for a region voice
// whose box holds it but not the listener; it is put back at the next check.
char* g_sm = nullptr;   // set by the vf17 wrapper
const float* TrueListener() {
	char* wrap = g_sm ? *(char**)((char*)g_sm + 4) : nullptr;
	char* obj = wrap ? *(char**)wrap : nullptr;
	return obj ? (const float*)(obj + 0x28) : nullptr;
}

bool InRegionBox(const char* inst, const float* p) {
	const float* b = (const float*)inst;
	return b[0x2C / 4] < p[0] && p[0] <= b[0x20 / 4] && b[0x34 / 4] < p[2] && p[2] <= b[0x28 / 4] &&
	       b[0x30 / 4] < p[1] && p[1] <= b[0x24 / 4];
}

volatile long g_checked = 0, g_mismatch = 0;

float __cdecl RangeDist(float sq, const float* s, const float* l) {
	// Self-check: the caller's squared distance must be |s - l|^2, or the thunk
	// read the wrong stack slots.
	if (s && l && g_checked < 2000) {
		InterlockedIncrement(&g_checked);
		float d = Dist2(s, l);
		if (fabsf(d - sq) > 0.001f * sq + 0.01f && InterlockedIncrement(&g_mismatch) <= 5)
			Log("sound: range check mismatch: caller %.3f, recomputed %.3f", sq, d);
		if (g_checked == 2000) Log("sound: range checks verified (%ld mismatches in 2000)", g_mismatch);
	}
	P2Listener l2;
	if (s && l && CurrentP2(l2)) {
		float d2 = Dist2(s, l2.pos);
		if (d2 < sq) {
			InterlockedIncrement(&g_nearP2);
			sq = d2;
		}
	}
	return sqrtf(sq > 0.0f ? sq : 0.0f);
}

// Deferred-sound records (vf17, 0x43B512): ESI = the 0x1C-byte record, +4 the
// sound; its position is the sound's own (+4..+0xC) or, with +0x38 bit 4, its
// owner's frame (+4 -> +4 -> LTM position +0x80).
float __cdecl RangeDistRecord(float sq, const char* rec, float* l) {
	const float* tl = TrueListener();
	if (tl && l) {   // the voice loop may have left player 2's camera there
		l[0] = tl[0]; l[1] = tl[1]; l[2] = tl[2];
		const float* s0 = nullptr;
		(void)s0;
	}
	const float* s = nullptr;
	const char* snd = rec ? *(const char* const*)(rec + 4) : nullptr;
	if (snd) {
		if (*(const uint8_t*)(snd + 0x38) & 0x10) {
			const char* owner = *(const char* const*)(snd + 4);
			const char* frame = owner ? *(const char* const*)(owner + 4) : nullptr;
			if (frame) s = (const float*)(frame + 0x80);
		} else {
			s = (const float*)(snd + 4);
		}
	}
	if (s && l) sq = Dist2(s, l);
	return RangeDist(sq, s, l);
}

// The thunks are entered with [esp] = return address, [esp+4] = the squared
// distance the caller pushed; the caller's own frame starts at esp+4.
//   0x43B0D5 (vf17, playing voices): sound at +0x2C, listener at +0x20.
//   0x43B512 (vf17, deferred sounds): sound via ESI, listener at +0x20.
//   0x43B9B9 / 0x43BCBF (FUN_0043B730): sound at +0x18, listener at +0x24.
volatile long g_regionKept = 0, g_regionStarted = 0;

float __cdecl RangeDistVoice(float sq, const float* s, float* l, const char* rec) {
	const float* tl = TrueListener();
	if (tl && l) { l[0] = tl[0]; l[1] = tl[1]; l[2] = tl[2]; }
	P2Listener l2;
	if (!s || !l || !CurrentP2(l2)) return RangeDist(sq, s, l);
	float d1 = Dist2(s, l), d2 = Dist2(s, l2.pos);
	const char* def = rec ? *(const char* const*)(rec + 4) : nullptr;
	const char* inst = rec ? *(const char* const*)(rec + 8) : nullptr;
	if (def && inst && (*(const uint8_t*)(def + 0xC) & 1) && InRegionBox(inst, l2.pos) && !InRegionBox(inst, l)) {
		l[0] = l2.pos[0]; l[1] = l2.pos[1]; l[2] = l2.pos[2];
		InterlockedIncrement(&g_regionKept);
		return sqrtf(d2);
	}
	return sqrtf(d1 < d2 ? d1 : d2);
}

__declspec(naked) void ThunkVoices() {
	__asm {
		lea  eax, [esp + 4 + 0x20]
		lea  ecx, [esp + 4 + 0x2C]
		push ebx
		push eax
		push ecx
		push dword ptr [esp + 16]
		call RangeDistVoice
		add  esp, 16
		ret
	}
}

__declspec(naked) void ThunkDeferred() {
	__asm {
		lea  eax, [esp + 4 + 0x20]
		push eax
		push esi
		push dword ptr [esp + 12]
		call RangeDistRecord
		add  esp, 12
		ret
	}
}

__declspec(naked) void ThunkStart() {
	__asm {
		lea  eax, [esp + 4 + 0x24]
		lea  ecx, [esp + 4 + 0x18]
		push eax
		push ecx
		push dword ptr [esp + 12]
		call RangeDist
		add  esp, 12
		ret
	}
}

// Voice update (FUN_005F06B0, cdecl(voice), the RwA 3D voice's update callback
// at voice+0x30): mixes the voice from its world position (+0xA0 x negated,
// +0xA4, +0xA8; velocity +0xAC..+0xB4) -- volume through FUN_005DE840 against
// the listener (voice+0x94 -> +0x14 -> object), pan through the DirectSound 3D
// buffer (SetAllParameters with voice+0x9C) against the DirectSound listener,
// both player 1's camera. A voice nearer to player 2's camera is moved, for the
// length of the update, to where it sits relative to player 2's camera but
// around player 1's: distance and left/right then follow player 2's view.
// +0x28 bit 2 = position changed (the 3D buffer is only updated then); it is
// set on every update while the split screen is up, so the pan follows the
// cameras turning, not just sounds moving.
typedef char*(__cdecl* VoiceUpdateFn)(char* voice);
const uintptr_t kVoiceUpdate = 0x5F06B0;
const uint8_t   kVoiceUpdatePrologue[5] = { 0x83, 0xEC, 0x60, 0x53, 0x55 };
VoiceUpdateFn g_origVoiceUpdate = nullptr;
volatile DWORD g_refreshUntil = 0;

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// World vector `d` seen from frame (r2, u2, a2), re-expressed around (r1, u1, a1).
void Remap(const float* d, const P2Listener& l2, const char* l1, float* out) {
	const float* r1 = (const float*)(l1 + 0x58);
	const float* u1 = (const float*)(l1 + 0x40);
	const float* a1 = (const float*)(l1 + 0x4C);
	float cr = Dot(d, l2.right), cu = Dot(d, l2.up), ca = Dot(d, l2.at);
	for (int k = 0; k < 3; k++) out[k] = cr * r1[k] + cu * u1[k] + ca * a1[k];
}

char* __cdecl VoiceUpdateHook(char* v) {
	P2Listener l2;
	bool active = CurrentP2(l2);
	DWORD now = GetTickCount();
	if (active) g_refreshUntil = now + 1000;
	char* dev = v ? *(char**)(v + 0x94) : nullptr;
	char* l1 = dev ? *(char**)(dev + 0x14) : nullptr;
	char* sample = v ? *(char**)(v + 0x84) : nullptr;
	bool is3d = l1 && sample && (*(uint8_t*)(sample + 0x54) & 2);
	if (!is3d) return g_origVoiceUpdate(v);
	if ((int)(g_refreshUntil - now) > 0) *(uint32_t*)(v + 0x28) |= 4;
	if (!active) return g_origVoiceUpdate(v);

	float* vp = (float*)(v + 0xA0);
	float saved[6];
	memcpy(saved, vp, sizeof(saved));
	float p[3] = { -vp[0], vp[1], vp[2] };
	const float* l1pos = (const float*)(l1 + 0x28);
	if (Dist2(p, l2.pos) >= Dist2(p, l1pos)) return g_origVoiceUpdate(v);

	float d[3] = { p[0] - l2.pos[0], p[1] - l2.pos[1], p[2] - l2.pos[2] }, rd[3];
	Remap(d, l2, l1, rd);
	float vel[3] = { -vp[3], vp[4], vp[5] }, rv[3];
	Remap(vel, l2, l1, rv);
	vp[0] = -(l1pos[0] + rd[0]); vp[1] = l1pos[1] + rd[1]; vp[2] = l1pos[2] + rd[2];
	vp[3] = -rv[0]; vp[4] = rv[1]; vp[5] = rv[2];
	InterlockedIncrement(&g_mixedAsP2);
	char* r = g_origVoiceUpdate(v);
	memcpy(vp, saved, sizeof(saved));
	return r;
}

typedef void(__fastcall* SmTickFn)(char* self, void* edx, char* dt);
const uintptr_t kSoundMgrVtbl = 0x624058, kSoundMgrTick = 0x43ADD0;
const size_t    kSoundMgrTickSlot = 17;
SmTickFn g_origSmTick = nullptr;
typedef int(__fastcall* StartFn)(char* self, void* edx, void* inst, int a, int b, int c, int d, int e);
const uintptr_t kSoundStart = 0x43B730;

void __fastcall SoundTickHook(char* sm, void* edx, char* dt) {
	g_sm = sm;
	g_origSmTick(sm, edx, dt);
	{   // diagnostics: region sounds (music/ambience zones) leaving or joining the waiting list
		static int lastRegions = -1;
		char* a = *(char**)(sm + 0x18);
		unsigned c = *(unsigned*)(sm + 0x20);
		int n = 0;
		for (unsigned i = 0; a && i < c && i < 512; i++) {
			const char* def = *(const char* const*)(a + i * 0x1C);
			if (def && (*(const uint8_t*)(def + 0xC) & 1)) n++;
		}
		if (n != lastRegions) {
			if (lastRegions >= 0) Log("sound: region sounds waiting %d -> %d", lastRegions, n);
			lastRegions = n;
		}
	}
	P2Listener l2;
	char* wrap = *(char**)((char*)sm + 4);
	char* obj = wrap ? *(char**)wrap : nullptr;
	if (!obj || !CurrentP2(l2)) return;
	float* lp = (float*)(obj + 0x28);
	// Deferred sounds: +0x18 array of 0x1C-byte records (+0 definition, +4 the
	// instance, +8/+0xC/+0x10/+0x18 the start arguments), +0x20 count.
	char* arr = *(char**)((char*)sm + 0x18);
	unsigned& count = *(unsigned*)((char*)sm + 0x20);
	for (unsigned i = 0; arr && i < count; i++) {
		int* r = (int*)(arr + i * 0x1C);
		const char* def = (const char*)r[0];
		char* inst = (char*)r[1];
		if (!def || !inst || !(*(const uint8_t*)(def + 0xC) & 1)) continue;
		if (!InRegionBox(inst, l2.pos) || InRegionBox(inst, lp)) continue;
		const float* sp = (*(uint8_t*)(inst + 0x38) & 0x10) ? nullptr : (const float*)(inst + 4);
		if (sp && Dist2(sp, l2.pos) >= *(const float*)(def + 8) * *(const float*)(def + 8)) continue;
		float saved[3] = { lp[0], lp[1], lp[2] };
		lp[0] = l2.pos[0]; lp[1] = l2.pos[1]; lp[2] = l2.pos[2];
		*(uint32_t*)(inst + 0x38) &= ~4u;
		((StartFn)kSoundStart)(sm, nullptr, inst, r[2], r[3], (char)r[6], r[4], 0);
		lp[0] = saved[0]; lp[1] = saved[1]; lp[2] = saved[2];
		InterlockedIncrement(&g_regionStarted);
		Log("sound: region sound %p started for player 2's camera", inst);
		// Taken off the deferred list, as vf17 does after starting one.
		unsigned last = --count;
		if (i != last) memcpy(arr + i * 0x1C, arr + last * 0x1C, 0x1C);
		i--;
	}
}

// Diagnostics: streams (music, voices) start through FUN_0043CC20 (sound
// manager, slot = 3 channels of 0x134 bytes): called by cinematic stream blocks
// (FUN_0046A070, CKStreamAloneCinematicBloc::vf12) and CKSekens (FUN_00439110).
typedef int(__fastcall* StreamStartFn)(char* self, void* edx, unsigned id, void* b, int c, char d, char e);
const uintptr_t kStreamStart = 0x43CC20;
const uint8_t   kStreamStartPrologue[5] = { 0x83, 0xEC, 0x28, 0x53, 0x55 };
StreamStartFn g_origStreamStart = nullptr;

int __fastcall StreamStartHook(char* self, void* edx, unsigned id, void* b, int c, char d, char e) {
	Trio t = GetTrio();
	Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero(), o = t ? t.GetHero(HERO_OBELIX) : Hero();
	int r = g_origStreamStart(self, edx, id, b, c, d, e);
	Log("sound: stream %u started (%p, %d, %d, %d) -> channel %d, from %p; leader %s; Asterix (%.1f %.1f %.1f) "
	    "Obelix (%.1f %.1f %.1f)", id & 0xFF, b, c, d, e, r, _ReturnAddress(), t ? HeroName(t.Leader()) : "?",
	    a ? a.Pos().x : 0, a ? a.Pos().y : 0, a ? a.Pos().z : 0, o ? o.Pos().x : 0, o ? o.Pos().y : 0,
	    o ? o.Pos().z : 0);
	LogBacktrace("sound: stream start", (uintptr_t)_AddressOfReturnAddress());
	return r;
}

// Diagnostics: every distinct (sound, calling code) pair is logged the first
// time it starts (FUN_0043B730), with both heroes' positions -- a music loop
// that only starts for the leader shows up as a new pair at that moment.
typedef int(__fastcall* SoundStartFn)(char* self, void* edx, uint32_t* inst, float vol, float pitch, int p4,
                                      float p5, int p6);
const uint8_t kSoundStartPrologue[10] = { 0xD9, 0x44, 0x24, 0x08, 0xD8, 0x1D, 0xD4, 0x22, 0x62, 0x00 };
SoundStartFn g_origSoundStart = nullptr;
struct SeenStart { uint32_t id; uintptr_t from; };
SeenStart g_seen[1024];
volatile long g_nSeen = 0;

int __fastcall SoundStartHook(char* self, void* edx, uint32_t* inst, float vol, float pitch, int p4, float p5,
                              int p6) {
	uintptr_t from = (uintptr_t)_ReturnAddress();
	uint32_t id = inst ? inst[0] : 0;
	bool seen = false;
	for (long i = 0; i < g_nSeen && !seen; i++) seen = g_seen[i].id == id && g_seen[i].from == from;
	static const bool all = getenv("XXL_LOGSOUNDS") != nullptr;   // diagnostics: every start from hero code
	if (all && (getenv("XXL_LOGSOUNDS")[0] == '2' || (from >= 0x490000 && from < 0x4B0000)))
		Log("sound: start %08X from %p", id, (void*)from);
	if (!seen && g_nSeen < 1024) {
		g_seen[g_nSeen].id = id;
		g_seen[g_nSeen].from = from;
		g_nSeen++;
		Trio t = GetTrio();
		Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero(), o = t ? t.GetHero(HERO_OBELIX) : Hero();
		const float* sp = inst && !(inst[0xE] & 0x10) ? (const float*)(inst + 1) : nullptr;
		Log("sound: first start of sound %08X (instance %p at %.0f %.0f %.0f) from %p; leader %s; Asterix (%.1f %.1f "
		    "%.1f) Obelix (%.1f %.1f %.1f)", id, inst, sp ? sp[0] : 0, sp ? sp[1] : 0, sp ? sp[2] : 0, (void*)from,
		    t ? HeroName(t.Leader()) : "?", a ? a.Pos().x : 0, a ? a.Pos().y : 0, a ? a.Pos().z : 0,
		    o ? o.Pos().x : 0, o ? o.Pos().y : 0, o ? o.Pos().z : 0);
	}
	return g_origSoundStart(self, edx, inst, vol, pitch, p4, p5, p6);
}

} // namespace

void SoundSetP2Listener() {
	float m[16];
	if (!SplitActive() || !OtherViewMatrix(m)) {
		g_l2Index = -1;
		return;
	}
	long next = (g_l2Index < 0) ? 0 : ((g_l2Index + 1) & 1);
	P2Listener& d = g_l2[next];
	memcpy(d.right, m + 0, 12);
	memcpy(d.up, m + 4, 12);
	memcpy(d.at, m + 8, 12);
	memcpy(d.pos, m + 12, 12);
	g_l2Index = next;
}

long SoundNearP2() { return g_nearP2; }
long SoundRegionKept() { return g_regionKept; }
long SoundRegionStarted() { return g_regionStarted; }

void SoundDumpRegions() {
	if (!g_sm) { Log("sound: no sound manager seen yet"); return; }
	char* arr = *(char**)(g_sm + 0x18);
	unsigned count = *(unsigned*)(g_sm + 0x20);
	int n = 0;
	for (unsigned i = 0; arr && i < count; i++) {
		int* r = (int*)(arr + i * 0x1C);
		const char* def = (const char*)r[0];
		const float* b = (const float*)r[1];
		if (!def || !b || !(*(const uint8_t*)(def + 0xC) & 1)) continue;
		n++;
		Log("sound:   region (waiting) %p box x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f range %.0f", b,
		    b[0x2C / 4], b[0x20 / 4], b[0x30 / 4], b[0x24 / 4], b[0x34 / 4], b[0x28 / 4], *(const float*)(def + 8));
	}
	Log("sound: %u waiting sounds, %d of them region sounds", count, n);
}
long SoundMixedAsP2() { return g_mixedAsP2; }

bool SoundInstallHooks() {
	static const uint8_t kCall[4][5] = {
		{ 0xE8, 0xE6, 0x66, 0x15, 0x00 },   // 0x43B0D5
		{ 0xE8, 0xA9, 0x62, 0x15, 0x00 },   // 0x43B512
		{ 0xE8, 0x02, 0x5E, 0x15, 0x00 },   // 0x43B9B9
		{ 0xE8, 0xFC, 0x5A, 0x15, 0x00 },   // 0x43BCBF
	};
	InstallDetour(kSoundStart, (void*)SoundStartHook, (void**)&g_origSoundStart, kSoundStartPrologue,
	              sizeof(kSoundStartPrologue), "sound start (diagnostics)");
	InstallDetour(kStreamStart, (void*)StreamStartHook, (void**)&g_origStreamStart, kStreamStartPrologue,
	              sizeof(kStreamStartPrologue), "stream start (diagnostics)");
	bool ok = PatchVtableSlot(kSoundMgrVtbl, kSoundMgrTickSlot, kSoundMgrTick, (void*)SoundTickHook,
	                          (void**)&g_origSmTick, "CKSoundManager::tick");
	ok &= PatchCallSite(0x43B0D5, kCall[0], 5, (void*)ThunkVoices, "sound range, playing voices");
	ok &= PatchCallSite(0x43B512, kCall[1], 5, (void*)ThunkDeferred, "sound range, deferred sounds");
	ok &= PatchCallSite(0x43B9B9, kCall[2], 5, (void*)ThunkStart, "sound range, start (1)");
	ok &= PatchCallSite(0x43BCBF, kCall[3], 5, (void*)ThunkStart, "sound range, start (2)");
	ok &= InstallDetour(kVoiceUpdate, (void*)VoiceUpdateHook, (void**)&g_origVoiceUpdate, kVoiceUpdatePrologue,
	                    sizeof(kVoiceUpdatePrologue), "3D voice update");
	(void)kSqrt;
	return ok;
}

