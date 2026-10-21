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

//
// The text font (Rt2d bitmap font "arial24-windows", 28 px glyphs on a 256 x 256
// page, from the language pack xxGLOC.KWN) is blurred when stretched to a modern
// screen. Its first page is swapped for a 4x one (xxl_widescreen2d\
// arial24-windows0_LL.png next to the game, LL the pack: 00 French, 01 English,
// 02 German, 03 Spanish, 04 Italian), drawn from Arial Bold on the same layout:
// the glyphs' texture coordinates are shares of the page, so they stay as they are.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <cstdlib>

#pragma comment(lib, "gdiplus.lib")

namespace {

const uintptr_t kYellowPages = 0x6621F4;   // CKYellowPages*
const uintptr_t kGlyphScale  = 0x623C64;   // float 0.75 in .rdata
const uintptr_t kHudCamera   = 0x662EBC;   // RwCamera* (orthographic)
const uintptr_t kD3DDevice   = 0x668978;   // IDirect3DDevice9*
const uintptr_t kHudLists[3] = { 0x662DDC, 0x662BC0, 0x662E00 };
const uintptr_t kFrameId     = 0x6464C4;   // bumped every frame

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

// --- Font pages -----------------------------------------------------------------
// CManager2d (yellow pages +0x34) keeps its fonts at +8 + id * 4 (ids 0-9). Font:
// +0x10 flags (2 = wide characters), +0x24 128 shorts (glyph of each ASCII
// character), +0x124 first wide character, +0x128 count, +0x12C their glyphs,
// +0x134 glyph list (+0 array; glyph 0x20: +4 texture, +8 u0 v0 u1 v1, +0x1C page).
// RwTexture: +0 raster, +0x50 filter (byte). RwRaster: +0xC width, +0x10 height,
// +0x18 stride. RenderWare (cdecl): RwRasterCreate(w, h, depth, flags) -- 4
// texture, 0x500 8888, 0x9000 mipmaps made on unlock (as the game's own texture
// reader asks for them) --, RwRasterLock(raster, level, 1 = write),
// RwRasterUnlock, RwRasterDestroy, RwRasterGetNumLevels.
typedef char*    (__cdecl* RasterCreateFn)(int w, int h, int depth, int flags);
typedef uint8_t* (__cdecl* RasterLockFn)(char* raster, int level, int mode);
typedef char*    (__cdecl* RasterFn)(char* raster);
typedef int      (__cdecl* RasterLevelsFn)(char* raster);
const uintptr_t kRwRasterCreate = 0x591160, kRwRasterLock = 0x591200, kRwRasterUnlock = 0x590F60;
const uintptr_t kRwRasterDestroy = 0x590FB0, kRwRasterLevels = 0x5910F0;
const int kPageSize = 1024;

// The packs' fonts differ in their extra characters and layout: one character
// only that pack has, and where its "A" starts on the page (texels).
const struct { int mark; float aX; } kPacks[5] = {
	{ 0x153, 173.5f }, { 0x2013, 182.5f }, { 0xDF, 173.5f }, { 0xA1, 162.5f }, { 0xC8, 182.5f } };

ULONG_PTR g_gdiplus = 0;
uint32_t* g_pagePx[5];     // decoded pages, A8R8G8B8
bool      g_pageTried[5];
char*     g_retired = nullptr;   // the last swapped-out raster: destroyed at the next swap, frames later
uint32_t  g_fontsFrame = 0;

const uint32_t* PagePixels(int pack) {
	if (g_pageTried[pack]) return g_pagePx[pack];
	g_pageTried[pack] = true;
	if (!g_gdiplus) {
		Gdiplus::GdiplusStartupInput in;
		if (Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr) != Gdiplus::Ok) { g_gdiplus = 0; return nullptr; }
	}
	wchar_t path[MAX_PATH];
	DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
	while (len && path[len - 1] != L'\\') len--;
	wsprintfW(path + len, L"xxl_widescreen2d\\arial24-windows0_%02d.png", pack);
	Gdiplus::Bitmap bmp(path);
	if (bmp.GetLastStatus() != Gdiplus::Ok || bmp.GetWidth() != kPageSize || bmp.GetHeight() != kPageSize) {
		Log("font: pack %02d has no %dx%d page in xxl_widescreen2d -- its font stays as it is", pack, kPageSize, kPageSize);
		return nullptr;
	}
	Gdiplus::Rect r(0, 0, kPageSize, kPageSize);
	Gdiplus::BitmapData d;
	if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &d) != Gdiplus::Ok) return nullptr;
	uint32_t* px = (uint32_t*)malloc(kPageSize * kPageSize * 4);
	if (px)
		for (int y = 0; y < kPageSize; y++) memcpy(px + y * kPageSize, (char*)d.Scan0 + y * d.Stride, kPageSize * 4);
	bmp.UnlockBits(&d);
	g_pagePx[pack] = px;
	return px;
}

// Which pack's font this is (-1: not the text font, or already swapped), and the
// texture of its first page.
int FontPack(char* f, char** texture) {
	if (!(*(uint32_t*)(f + 0x10) & 2)) return -1;
	int first = *(int*)(f + 0x124), n = *(int*)(f + 0x128);
	const int16_t* wide = *(int16_t**)(f + 0x12C);
	int16_t a = ((int16_t*)(f + 0x24))['A'];
	char* list = *(char**)(f + 0x134);
	if (!wide || a < 0 || !list || a >= *(int*)(list + 4)) return -1;
	char* glyph = *(char**)list + a * 0x20;
	char* tex = *(char**)(glyph + 4);
	char* raster = tex ? *(char**)tex : nullptr;
	if (*(uint32_t*)(glyph + 0x1C) != 0 || !raster || *(int*)(raster + 0x0C) != 256 || *(int*)(raster + 0x10) != 256)
		return -1;
	float ax = *(float*)(glyph + 8) * 256.0f;
	for (int p = 0; p < 5; p++) {
		int c = kPacks[p].mark;
		if (c >= first && c < first + n && wide[c - first] >= 0 && fabsf(ax - kPacks[p].aX) < 0.01f) {
			*texture = tex;
			return p;
		}
	}
	return -1;
}

void SwapPage(char* tex, int pack) {
	const uint32_t* px = PagePixels(pack);
	if (!px) return;
	char* r = ((RasterCreateFn)kRwRasterCreate)(kPageSize, kPageSize, 32, 0x9504);
	if (!r) r = ((RasterCreateFn)kRwRasterCreate)(kPageSize, kPageSize, 32, 0x504);
	if (!r) { Log("font: no raster for pack %02d", pack); return; }
	bool mips = (*(uint8_t*)(r + 0x23) & 0x80) != 0;   // the driver may drop the mipmap flags
	uint8_t* dst = ((RasterLockFn)kRwRasterLock)(r, 0, 1);
	if (!dst) { ((RasterFn)kRwRasterDestroy)(r); Log("font: the page did not lock"); return; }
	int stride = *(int*)(r + 0x18);
	for (int y = 0; y < kPageSize; y++) memcpy(dst + y * stride, px + y * kPageSize, kPageSize * 4);
	((RasterFn)kRwRasterUnlock)(r);
	if (g_retired) ((RasterFn)kRwRasterDestroy)(g_retired);
	g_retired = *(char**)tex;
	*(char**)tex = r;
	*(uint8_t*)(tex + 0x50) = mips ? 6 : 2;   // trilinear / linear
	Log("font: pack %02d's page now %dx%d, format %02X, %d levels", pack, kPageSize, kPageSize, *(uint8_t*)(r + 0x23),
	    ((RasterLevelsFn)kRwRasterLevels)(r));
}

// On the render thread (from the text draw), once a frame: fonts come with the
// language pack and may be read again.
void FontPagesFrame() {
	uint32_t frame = *(uint32_t*)kFrameId;
	if (frame == g_fontsFrame) return;
	g_fontsFrame = frame;
	__try {
		char* yp = *(char**)kYellowPages;
		char* m2d = yp ? *(char**)(yp + 0x34) : nullptr;
		if (!m2d || *(uintptr_t*)m2d != 0x623A28) return;
		for (int i = 0; i < 10; i++) {
			char* f = *(char**)(m2d + 8 + 4 * i);
			char* tex = nullptr;
			int pack = f ? FontPack(f, &tex) : -1;
			if (pack >= 0) SwapPage(tex, pack);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// --- 2D plates ------------------------------------------------------------------
// A CBillboard2d (draw 0x42D6F0, thiscall(bb, float pos[2], ?, char alignX, char
// alignY, ?), ret 0x14; width +0x14, height +0x18) with +0x1C set takes its width
// as a share of the screen's: the HUD's see-through plates (the Romans to beat,
// the helmets, the laurels) come out a third too wide at 16:9. They are drawn
// (4/3) / aspect as wide, about the edge or middle they are placed by (the game's
// own sums keep it). What sits on a plate -- its text (CText2d draw 0x42F430,
// same arguments), small billboards, the HUD models -- was placed by shares of the
// screen too, so it is drawn that much nearer the plate's anchor. Plates spanning
// (nearly) the whole screen -- fades, bars, backgrounds -- are left as they are.
struct Plate { float anchor, left, right, cy, halfH; };
Plate    g_plates[2][64];
int      g_nPlates[2];
int      g_platesNow = 0;
uint32_t g_platesFrame = 0;

float PlateSqueeze() {
	float aspect = 0.0f;
	__try {
		char* yp = *(char**)kYellowPages;
		char* m2d = yp ? *(char**)(yp + 0x34) : nullptr;
		aspect = m2d ? *(float*)(m2d + 4) : 0.0f;
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
	return aspect > 0.5f && aspect < 8.0f ? (4.0f / 3.0f) / aspect : 1.0f;
}

void PlatesNextFrame() {
	uint32_t f = *(uint32_t*)kFrameId;
	if (f == g_platesFrame) return;
	g_platesFrame = f;
	g_platesNow ^= 1;
	g_nPlates[g_platesNow] = 0;
}

// The smallest plate (this frame's or the last one's) holding 2D point (x, y).
const Plate* PlateAt(float x, float y) {
	const Plate* best = nullptr;
	for (int b = 0; b < 2; b++)
		for (int i = 0; i < g_nPlates[b]; i++) {
			const Plate& p = g_plates[b][i];
			if (x >= p.left && x <= p.right && fabsf(y - p.cy) <= p.halfH &&
			    (!best || p.right - p.left < best->right - best->left))
				best = &p;
		}
	return best;
}

typedef int(__fastcall* Draw2dFn)(char* self, void* edx, float* pos, int a, char alignX, char alignY, int b);
Draw2dFn g_origBillboard = nullptr, g_origText = nullptr;

int __fastcall BillboardDrawHook(char* self, void* edx, float* pos, int a, char alignX, char alignY, int b) {
	PlatesNextFrame();
	float s = PlateSqueeze();
	if (s > 0.995f || !pos) return g_origBillboard(self, edx, pos, a, alignX, alignY, b);
	float* w = (float*)(self + 0x14);
	float h = *(float*)(self + 0x18);
	float p[2] = { pos[0], pos[1] };
	const Plate* on = PlateAt(pos[0], pos[1]);
	bool stretched = *(char*)(self + 0x1C) != 0 && *w > 0.0f && *w < 0.95f;
	if (on && !(stretched && on->right - on->left <= *w * 1.01f))   // not itself
		p[0] = on->anchor + (p[0] - on->anchor) * s;
	if (!stretched) return g_origBillboard(self, edx, p, a, alignX, alignY, b);
	float width = *w;
	Plate& rec = g_plates[g_platesNow][g_nPlates[g_platesNow] < 64 ? g_nPlates[g_platesNow]++ : 63];
	rec.anchor = pos[0];
	rec.left = alignX == 2 ? pos[0] - width : alignX == 1 ? pos[0] - width * 0.5f : pos[0];
	rec.right = rec.left + width;
	rec.cy = alignY == 0 ? pos[1] - h * 0.5f : alignY == 2 ? pos[1] + h * 0.5f : pos[1];
	rec.halfH = h * 0.5f;
	*w = width * s;
	int r = g_origBillboard(self, edx, p, a, alignX, alignY, b);
	*w = width;
	return r;
}

int __fastcall TextDrawHook(char* self, void* edx, float* pos, int a, char alignX, char alignY, int b) {
	FontPagesFrame();
	PlatesNextFrame();
	const Plate* on = pos ? PlateAt(pos[0], pos[1]) : nullptr;
	float s = on ? PlateSqueeze() : 1.0f;
	if (!on || s > 0.995f) return g_origText(self, edx, pos, a, alignX, alignY, b);
	float p[2] = { on->anchor + (pos[0] - on->anchor) * s, pos[1] };
	return g_origText(self, edx, p, a, alignX, alignY, b);
}

bool Detour(uintptr_t target, const uint8_t* expected, size_t len, void* hook, void** orig) {
	uint8_t* code = (uint8_t*)target;
	if (memcmp(code, expected, len) != 0) return false;
	uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) return false;
	memcpy(tramp, code, len);
	tramp[len] = 0xE9;
	*(uint32_t*)(tramp + len + 1) = (uint32_t)((target + len) - ((uintptr_t)tramp + len + 5));
	*orig = tramp;
	DWORD old;
	if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old)) return false;
	code[0] = 0xE9;
	*(uint32_t*)(code + 1) = (uint32_t)((uintptr_t)hook - (target + 5));
	for (size_t i = 5; i < len; i++) code[i] = 0x90;
	VirtualProtect(code, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), code, len);
	return true;
}

void InstallPlateHooks() {
	static const uint8_t kBillboard[7] = { 0x83, 0xEC, 0x10, 0x8B, 0x44, 0x24, 0x14 };
	static const uint8_t kText[7] = { 0x83, 0xEC, 0x28, 0x53, 0x56, 0x8B, 0xF1 };
	bool bb = Detour(0x42D6F0, kBillboard, sizeof(kBillboard), (void*)BillboardDrawHook, (void**)&g_origBillboard);
	bool tx = Detour(0x42F430, kText, sizeof(kText), (void*)TextDrawHook, (void**)&g_origText);
	Log("plates: billboard draw %s, text draw %s", bb ? "hooked" : "NOT hooked", tx ? "hooked" : "NOT hooked");
}

// A HUD model's place as a 2D point (x 0..1 from the left, y 0..1 from the
// bottom of the screen), from its camera-space x / y: the camera's view window
// maps onto the D3D viewport, the 2D layer onto the whole back buffer.
struct HudMap { float vpX, vpY, vpW, vpH, winX, winY, fullW, fullH; };

bool GetHudMap(const char* cam, HudMap& m) {
	struct { DWORD x, y, w, h; float minZ, maxZ; } vp = {};
	void* dev = *(void**)kD3DDevice;
	typedef long(__stdcall* GetViewportFn)(void* dev, void* vp);
	if (!dev || ((GetViewportFn)(*(void***)dev)[48])(dev, &vp) < 0 || !vp.w || !vp.h) return false;
	const char* raster = *(char**)(cam + 0x60);
	if (!raster) return false;
	m = { (float)vp.x, (float)vp.y, (float)vp.w, (float)vp.h, ((const float*)(cam + 0x68))[0],
	      ((const float*)(cam + 0x68))[1], (float)*(const int*)(raster + 0x0C), (float)*(const int*)(raster + 0x10) };
	return m.winX > 0.0f && m.winY > 0.0f && m.fullW > 0.0f && m.fullH > 0.0f;
}

// Rows of models (the health shields): models on one line, each within kRowGap
// of the next, are drawn as a group, so their spacing shrinks with them --
// towards the row's outer end, which stays put. A model on its own keeps its
// centre (it often sits in a 2D plate, which keeps its place). HUD camera
// units: the view is 1 across.
const float kRowGap = 0.15f, kRowLine = 0.03f;

void CloseRows(const char* cam, const float* camLtm, float s, int n) {
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
	// models on a plate go nearer its anchor with it, and are no row's
	HudMap hm;
	float sp = PlateSqueeze();
	if (sp < 0.995f && GetHudMap(cam, hm))
		for (int k = 0; k < n; k++) {
			float px = (hm.vpX + hm.vpW * (0.5f - x[k] / (2.0f * hm.winX))) / hm.fullW;
			float py = 1.0f - (hm.vpY + hm.vpH * (0.5f - y[k] / (2.0f * hm.winY))) / hm.fullH;
			const Plate* on = PlateAt(px, py);
			if (!on) continue;
			float dx = -(on->anchor + (px - on->anchor) * sp - px) * hm.fullW * 2.0f * hm.winX / hm.vpW;
			float* p = g_saved[k].ltm + 12;
			for (int c = 0; c < 3; c++) p[c] += dx * ru[c];
			group[k] = -1;
		}
	// neighbours join a group (merging until nothing changes)
	for (bool changed = true; changed;) {
		changed = false;
		for (int a = 0; a < n; a++)
			for (int b = a + 1; b < n; b++)
				if (group[a] >= 0 && group[b] >= 0 && group[a] != group[b] && fabsf(y[a] - y[b]) < kRowLine && fabsf(x[a] - x[b]) < kRowGap) {
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
			if (len2 > 0.0f) CloseRows(cam, camLtm, s, n);
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
	InstallPlateHooks();
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
