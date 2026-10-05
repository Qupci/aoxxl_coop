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
#include "detour.h"
#include "log.h"
#include "p2cam.h"
#include "splitscreen.h"

#include <cmath>
#include <cstring>

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
float __cdecl RangeDistRecord(float sq, const char* rec, const float* l) {
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
	return RangeDist(sq, s, l);
}

// The thunks are entered with [esp] = return address, [esp+4] = the squared
// distance the caller pushed; the caller's own frame starts at esp+4.
//   0x43B0D5 (vf17, playing voices): sound at +0x2C, listener at +0x20.
//   0x43B512 (vf17, deferred sounds): sound via ESI, listener at +0x20.
//   0x43B9B9 / 0x43BCBF (FUN_0043B730): sound at +0x18, listener at +0x24.
__declspec(naked) void ThunkVoices() {
	__asm {
		lea  eax, [esp + 4 + 0x20]
		lea  ecx, [esp + 4 + 0x2C]
		push eax
		push ecx
		push dword ptr [esp + 12]
		call RangeDist
		add  esp, 12
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

} // namespace

void SoundSetP2Listener() {
	float m[16];
	if (!SplitActive() || !P2CamMatrix(m)) {
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
long SoundMixedAsP2() { return g_mixedAsP2; }

bool SoundInstallHooks() {
	static const uint8_t kCall[4][5] = {
		{ 0xE8, 0xE6, 0x66, 0x15, 0x00 },   // 0x43B0D5
		{ 0xE8, 0xA9, 0x62, 0x15, 0x00 },   // 0x43B512
		{ 0xE8, 0x02, 0x5E, 0x15, 0x00 },   // 0x43B9B9
		{ 0xE8, 0xFC, 0x5A, 0x15, 0x00 },   // 0x43BCBF
	};
	bool ok = PatchCallSite(0x43B0D5, kCall[0], 5, (void*)ThunkVoices, "sound range, playing voices");
	ok &= PatchCallSite(0x43B512, kCall[1], 5, (void*)ThunkDeferred, "sound range, deferred sounds");
	ok &= PatchCallSite(0x43B9B9, kCall[2], 5, (void*)ThunkStart, "sound range, start (1)");
	ok &= PatchCallSite(0x43BCBF, kCall[3], 5, (void*)ThunkStart, "sound range, start (2)");
	ok &= InstallDetour(kVoiceUpdate, (void*)VoiceUpdateHook, (void**)&g_origVoiceUpdate, kVoiceUpdatePrologue,
	                    sizeof(kVoiceUpdatePrologue), "3D voice update");
	(void)kSqrt;
	return ok;
}
