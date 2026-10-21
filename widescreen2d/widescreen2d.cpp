// xxl_widescreen2d.asi: true letter shapes for Asterix & Obelix XXL's 2D text on
// any screen shape, independent of the co-op mod.
//
// Text (CText2d) is laid out (0x42DF40, 0x42E840, 0x42EF80) and drawn (0x42F430,
// Rt2dCTMScale per glyph) with its widths times aspect * 0.75 -- the aspect being
// the 2D manager's (yellow pages +0x34 -> +4, the screen's width / height), 0.75 the
// constant at 0x623C64, which only those four places read. That is 1 at 4:3 and
// 1.33 at 16:9: letters and the font's button icons come out a third too wide.
// Keeping the constant at 1 / aspect makes the product 1 at any aspect. Positions
// (x * aspect) are not touched; lines just hold more words.
//
// The HUD's 3D models (health shields, helmet counter, ...) are drawn with the HUD
// camera (orthographic, view window 0.5 x 0.5 over the whole viewport) from three
// node lists (view +0x4C/+0x50/+0x54 = the statics below; list: vtable, first node
// +4; entry: RpAtomic +8 (its frame at +4), next +0x24; vtable slot 3 draws and
// empties the list). Over a 16:9 viewport that view is a third too wide. The draw
// slot of those lists is wrapped: each atomic's frame matrix (LTM, frame +0x50) is
// squeezed along the camera's right axis about its own position for the draw and
// put back after, so the models keep their places and lose the stretch. The squeeze
// comes from the D3D viewport and the camera's view window, so it is right in any
// layout (a split screen's halves too).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdarg>

namespace {

const uintptr_t kYellowPages = 0x6621F4;   // CKYellowPages*
const uintptr_t kGlyphScale  = 0x623C64;   // float 0.75 in .rdata
const uintptr_t kHudCamera   = 0x662EBC;   // RwCamera* (orthographic)
const uintptr_t kD3DDevice   = 0x668978;   // IDirect3DDevice9*
const uintptr_t kHudLists[3] = { 0x662DDC, 0x662BC0, 0x662E00 };

typedef void(__fastcall* ListDrawFn)(char* list, void* edx);
struct Wrapped { uintptr_t vtbl; ListDrawFn orig; };
Wrapped g_wrapped[3];
int     g_nWrapped = 0;

struct Saved { float* ltm; float m[16]; };
Saved g_saved[256];

// Diagnostics: xxl_widescreen2d.log next to the game, a few lines at most.
void Log(const char* fmt, ...) {
	static int lines = 0;
	if (++lines > 40) return;
	char buf[256];
	va_list a;
	va_start(a, fmt);
	int n = wvsprintfA(buf, fmt, a);
	va_end(a);
	char path[MAX_PATH];
	DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
	while (len && path[len - 1] != '\\') len--;
	lstrcpyA(path + len, "xxl_widescreen2d.log");
	HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, lines == 1 ? CREATE_ALWAYS : OPEN_ALWAYS, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD w;
	WriteFile(f, buf, n, &w, nullptr);
	WriteFile(f, "\r\n", 2, &w, nullptr);
	CloseHandle(f);
}

bool IsHudList(const char* list) {
	for (uintptr_t l : kHudLists)
		if ((uintptr_t)list == l) return true;
	return false;
}

// How much narrower the HUD camera's view must be drawn: 1 when its units are
// shaped as on a 4:3 screen (the HUD's design), 0.75 on a 16:9 one.
float HudSqueeze(const char* cam) {
	struct { DWORD x, y, w, h; float minZ, maxZ; } vp = {};
	void* dev = *(void**)kD3DDevice;
	typedef long(__stdcall* GetViewportFn)(void* dev, void* vp);
	if (!dev || ((GetViewportFn)(*(void***)dev)[48])(dev, &vp) < 0 || !vp.w || !vp.h) return 1.0f;
	const float* window = (const float*)(cam + 0x68);
	if (window[0] <= 0.0f || window[1] <= 0.0f) return 1.0f;
	return window[0] / window[1] * (float)vp.h / (float)vp.w * (4.0f / 3.0f);
}

// Rows of models (the health shields): models on one line, each within kRowGap
// of the next, are drawn as a group, so their spacing shrinks with them --
// towards the row's outer end, which stays put. A model on its own keeps its
// centre (it often sits in a 2D plate, which keeps its place). HUD camera
// units: the view is 1 across.
const float kRowGap = 0.15f, kRowLine = 0.03f;

void CloseRows(const float* camLtm, float s, int n) {
	float ru[3], uu[3];
	float rl = sqrtf(camLtm[0] * camLtm[0] + camLtm[1] * camLtm[1] + camLtm[2] * camLtm[2]);
	float ul = sqrtf(camLtm[4] * camLtm[4] + camLtm[5] * camLtm[5] + camLtm[6] * camLtm[6]);
	if (rl <= 0.0f || ul <= 0.0f) return;
	for (int c = 0; c < 3; c++) { ru[c] = camLtm[c] / rl; uu[c] = camLtm[4 + c] / ul; }
	static float x[256], y[256];
	static int group[256];
	for (int k = 0; k < n; k++) {
		const float* p = g_saved[k].ltm + 12;
		float d[3] = { p[0] - camLtm[12], p[1] - camLtm[13], p[2] - camLtm[14] };
		x[k] = d[0] * ru[0] + d[1] * ru[1] + d[2] * ru[2];
		y[k] = d[0] * uu[0] + d[1] * uu[1] + d[2] * uu[2];
		group[k] = k;
	}
	// neighbours join a group (merging until nothing changes)
	for (bool changed = true; changed;) {
		changed = false;
		for (int a = 0; a < n; a++)
			for (int b = a + 1; b < n; b++)
				if (group[a] != group[b] && fabsf(y[a] - y[b]) < kRowLine && fabsf(x[a] - x[b]) < kRowGap) {
					int to = group[a] < group[b] ? group[a] : group[b];
					int from = group[a] < group[b] ? group[b] : group[a];
					for (int k = 0; k < n; k++) if (group[k] == from) group[k] = to;
					changed = true;
				}
	}
	for (int g = 0; g < n; g++) {
		float lo = 1e9f, hi = -1e9f, sum = 0.0f;
		int count = 0;
		for (int k = 0; k < n; k++)
			if (group[k] == g) { lo = x[k] < lo ? x[k] : lo; hi = x[k] > hi ? x[k] : hi; sum += x[k]; count++; }
		if (count < 2 || hi - lo < 0.001f) continue;
		float anchor = sum / count < 0.0f ? lo : hi;   // the end nearer the edge
		for (int k = 0; k < n; k++)
			if (group[k] == g) {
				float delta = (anchor + (x[k] - anchor) * s) - x[k];
				float* p = g_saved[k].ltm + 12;
				for (int c = 0; c < 3; c++) p[c] += delta * ru[c];
			}
	}
}

void __fastcall ListDrawHook(char* list, void* edx) {
	ListDrawFn orig = nullptr;
	for (int i = 0; i < g_nWrapped; i++)
		if (g_wrapped[i].vtbl == *(uintptr_t*)list) orig = g_wrapped[i].orig;
	if (!orig) return;
	int n = 0;
	__try {
		char* cam = IsHudList(list) ? *(char**)kHudCamera : nullptr;
		float s = cam ? HudSqueeze(cam) : 1.0f;
		if (s > 0.2f && s < 0.995f) {
			// the camera's right axis in the world: its frame's LTM, first row
			const float* camLtm = (const float*)(*(char**)(cam + 4) + 0x50);
			float r[3] = { camLtm[0], camLtm[1], camLtm[2] };
			float len2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
			for (char* node = *(char**)(list + 4); node && len2 > 0.0f && n < 256; node = *(char**)(node + 0x24)) {
				char* atomic = *(char**)(node + 8);
				char* frame = atomic ? *(char**)(atomic + 4) : nullptr;
				if (!frame) continue;
				float* m = (float*)(frame + 0x50);
				bool seen = false;
				for (int k = 0; k < n && !seen; k++) seen = g_saved[k].ltm == m;
				if (seen) continue;
				g_saved[n].ltm = m;
				memcpy(g_saved[n].m, m, sizeof(g_saved[n].m));
				n++;
				for (int axis = 0; axis < 3; axis++) {   // right, up, at; not the position
					float* v = m + axis * 4;
					float d = (v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) / len2 * (1.0f - s);
					for (int c = 0; c < 3; c++) v[c] -= d * r[c];
				}
			}
			if (len2 > 0.0f) CloseRows(camLtm, s, n);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	static int logged = 0;
	if (IsHudList(list) && *(char**)(list + 4) && logged < 1) {
		logged++;
		char* cam = *(char**)kHudCamera;
		Log("hud list %08X: %d nodes squeezed, squeeze x1000 = %d", (unsigned)(uintptr_t)list, n,
		    cam ? (int)(HudSqueeze(cam) * 1000) : -1);
	}
	orig(list, edx);
	for (int k = n - 1; k >= 0; k--) memcpy(g_saved[k].ltm, g_saved[k].m, sizeof(g_saved[k].m));
}

// The lists get their vtables from the game's start-up code.
bool WrapHudLists() {
	for (uintptr_t l : kHudLists)
		if (!*(uintptr_t*)l) return false;
	for (uintptr_t l : kHudLists) {
		uintptr_t vt = *(uintptr_t*)l;
		bool done = false;
		for (int i = 0; i < g_nWrapped; i++) done |= g_wrapped[i].vtbl == vt;
		if (done) continue;
		void** slot = (void**)vt + 3;
		DWORD old;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) continue;
		g_wrapped[g_nWrapped].vtbl = vt;
		g_wrapped[g_nWrapped].orig = (ListDrawFn)*slot;
		g_nWrapped++;
		*slot = (void*)ListDrawHook;
		VirtualProtect(slot, sizeof(void*), old, &old);
		Log("hud list vtable %08X: draw %08X wrapped", (unsigned)vt, (unsigned)(uintptr_t)g_wrapped[g_nWrapped - 1].orig);
	}
	return true;
}

float ReadAspect() {
	__try {
		char* yp = *(char**)kYellowPages;
		char* m2d = yp ? *(char**)(yp + 0x34) : nullptr;
		return m2d ? *(float*)(m2d + 4) : 0.0f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0.0f;
	}
}

// The aspect follows the resolution, so it is watched rather than read once.
DWORD WINAPI WatchThread(LPVOID) {
	float* scale = (float*)kGlyphScale;
	DWORD old;
	if (*scale != 0.75f || !VirtualProtect(scale, sizeof(float), PAGE_READWRITE, &old)) return 0;
	bool listsWrapped = false;
	for (;;) {
		if (!listsWrapped) listsWrapped = WrapHudLists();
		float aspect = ReadAspect();
		if (aspect > 0.5f && aspect < 8.0f && *scale != 1.0f / aspect) *scale = 1.0f / aspect;
		Sleep(10);
	}
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hinst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hinst);
		CloseHandle(CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr));
	}
	return TRUE;
}
