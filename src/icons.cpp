#include "icons.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "inputmgr.h"
#include "log.h"
#include "pads.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {

// RenderWare (cdecl). RwRasterCreate(w, h, depth, flags): flags 4 = texture,
// 0x500 = 8888. RwRasterLock(raster, level, mode 1 = write) -> pixels, stride
// at raster +0x18. RwTextureCreate(raster): filter / addressing +0x50 / +0x51,
// reference count +0x54. RwSListGetNewEntries(list, n, hint): list +0 array,
// +4 count, +8 capacity, +0xC entry size.
typedef char*    (__cdecl* RasterCreateFn)(int w, int h, int depth, int flags);
typedef uint8_t* (__cdecl* RasterLockFn)(char* raster, int level, int mode);
typedef char*    (__cdecl* RasterUnlockFn)(char* raster);
typedef char*    (__cdecl* TextureCreateFn)(char* raster);
typedef void*    (__cdecl* SListNewFn)(void* list, int n, uint32_t hint);
typedef void*    (__cdecl* RwMallocFn)(size_t size, uint32_t hint);
typedef void     (__cdecl* RwFreeFn)(void* p);
const uintptr_t kRwRasterCreate = 0x591160;
const uintptr_t kRwRasterLock = 0x591200;
const uintptr_t kRwRasterUnlock = 0x590F60;
const uintptr_t kRwTextureCreate = 0x5889B0;
const uintptr_t kRwSListNew = 0x617AF0;
const uintptr_t kRwEngine = 0x66885C;   // -> +0x134 malloc, +0x138 free

// Rt2dFontStreamRead (FUN_005FC970, cdecl(stream) -> font), from
// CManager2d::deserialize. Font: +0x10 flags (2 = wide characters), +0x14
// four page textures (destroyed with the font), +0x24 128 shorts (glyph of
// each ASCII character), +0x124 first wide character, +0x128 count, +0x12C
// their glyphs (shorts, -1 none), +0x130 glyph count, +0x134 glyph list. Glyph
// (0x20): +0 width (to the height), +4 texture, +8 u0 v0 u1 v1, +0x18 0, +0x1C
// page. The renderer (FUN_005FCF20) draws each glyph from its own texture.
typedef char*(__cdecl* FontReadFn)(void* stream);
const uintptr_t kFontRead = 0x5FC970;
const uint8_t   kFontReadPrologue[7] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57 };
FontReadFn g_origFontRead = nullptr;

const wchar_t kFirstCode = 0xE100, kLastCode = 0xE1FF;
const int kAtlasW = 1024, kAtlasH = 512, kCell = 64, kGap = 4;

struct Glyph { wchar_t code; int x, y, w; };
struct Font  { char* f; size_t glyphs; bool ok, tried; int page; };

uint32_t* g_atlas = nullptr;          // kAtlasW x kAtlasH, A8R8G8B8
int g_penX = kGap, g_penY = kGap;
std::vector<Glyph> g_glyphs;
std::unordered_map<std::string, wchar_t> g_byName;
std::unordered_map<int, wchar_t> g_byVk;
std::vector<Font> g_fonts;
char* g_raster = nullptr;
char* g_texture = nullptr;
bool  g_loaded = false;
ULONG_PTR g_gdiplus = 0;
int   g_logs = 0;

char* __cdecl FontReadHook(void* stream) {
	char* f = g_origFontRead(stream);
	if (f) g_fonts.push_back({ f, 0, false, false, -1 });
	return f;
}

// A place for a glyph `w` wide on the page.
bool Place(int w, int* x, int* y) {
	if (g_penX + w + kGap > kAtlasW) { g_penX = kGap; g_penY += kCell + kGap; }
	if (g_penY + kCell + kGap > kAtlasH || w + 2 * kGap > kAtlasW) return false;
	*x = g_penX; *y = g_penY;
	g_penX += w + kGap;
	return true;
}

// Copies a 32bpp ARGB GDI+ bitmap, kCell high, to the page as a new glyph.
wchar_t AddBitmap(Gdiplus::Bitmap& bmp) {
	int w = (int)bmp.GetWidth();
	if ((int)bmp.GetHeight() != kCell || kFirstCode + g_glyphs.size() > kLastCode) return 0;
	int x, y;
	if (!Place(w, &x, &y)) return 0;
	Gdiplus::Rect r(0, 0, w, kCell);
	Gdiplus::BitmapData d;
	if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &d) != Gdiplus::Ok) return 0;
	for (int row = 0; row < kCell; row++)
		memcpy(g_atlas + (y + row) * kAtlasW + x, (char*)d.Scan0 + row * d.Stride, w * 4);
	bmp.UnlockBits(&d);
	Glyph g = { (wchar_t)(kFirstCode + g_glyphs.size()), x, y, w };
	g_glyphs.push_back(g);
	return g.code;
}

// A bitmap of any size, scaled to kCell high (`alpha`: its opacity times this).
wchar_t AddScaled(Gdiplus::Bitmap& src, float alpha = 1.0f) {
	if (src.GetLastStatus() != Gdiplus::Ok || !src.GetHeight()) return 0;
	int w = (int)((src.GetWidth() * kCell + src.GetHeight() / 2) / src.GetHeight());
	if (w < 1 || w > 4 * kCell) return 0;
	Gdiplus::Bitmap dst(w, kCell, PixelFormat32bppARGB);
	{
		Gdiplus::Graphics g(&dst);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
		Gdiplus::ImageAttributes at;
		at.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
		Gdiplus::ColorMatrix cm = { { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, alpha, 0 }, { 0, 0, 0, 0, 1 } } };
		if (alpha < 1.0f) at.SetColorMatrix(&cm);
		g.DrawImage(&src, Gdiplus::Rect(0, 0, w, kCell), 0, 0, src.GetWidth(), src.GetHeight(), Gdiplus::UnitPixel, &at);
	}
	return AddBitmap(dst);
}

// A PNG of any size, scaled to kCell high.
wchar_t AddPng(const wchar_t* path) {
	Gdiplus::Bitmap src(path);
	if (src.GetLastStatus() != Gdiplus::Ok || !src.GetHeight()) return 0;
	int w = (int)((src.GetWidth() * kCell + src.GetHeight() / 2) / src.GetHeight());
	if (w < 1 || w > 4 * kCell) return 0;
	Gdiplus::Bitmap dst(w, kCell, PixelFormat32bppARGB);
	{
		Gdiplus::Graphics g(&dst);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
		Gdiplus::ImageAttributes at;
		at.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
		g.DrawImage(&src, Gdiplus::Rect(0, 0, w, kCell), 0, 0, src.GetWidth(), src.GetHeight(), Gdiplus::UnitPixel, &at);
	}
	return AddBitmap(dst);
}

void IconsDir(wchar_t* out, size_t n, const char* file) {
	char a[MAX_PATH];
	char rel[MAX_PATH];
	snprintf(rel, sizeof(rel), "coop_icons\\%s", file);
	GameDirPath(a, sizeof(a), rel);
	MultiByteToWideChar(CP_ACP, 0, a, -1, out, (int)n);
}

// The merchant's combo icons (CKHkAsterixShop's item pictures), read from a
// level file's texture dictionary as kwn_textures.py does: an entry is its
// name (32 bytes), filtering, u and v addressing (4 bytes each), then an RW
// image chunk (0x18) whose struct chunk (1, 16 bytes) holds width, height,
// bits per pixel and pitch; the pixels follow (RGBA, or indices into a palette
// of 1 << bpp RGBA entries after them). Their opaque pinkish-grey backing (R:G:B
// about 140:100:111, darker and lighter) is keyed out.
const char* const kComboTex[6] = { "o_ico_massue_g01", "o_ico_marteaupilon_g01", "o_ico_twister_g01",
                                   "o_ico_twisterfusion_g01", "o_ico_taupe_g01", "o_ico_idefix_g01" };

bool ComboImage(const std::vector<uint8_t>& d, const char* name, std::vector<uint32_t>& argb, int* w, int* h) {
	size_t len = strlen(name);
	for (size_t at = 0; at + len + 1 < d.size(); at++) {
		const uint8_t* p = (const uint8_t*)memchr(d.data() + at, name[0], d.size() - at);
		if (!p) return false;
		at = p - d.data();
		if (at + 44 + 40 > d.size() || memcmp(p, name, len) != 0 || p[len] != 0) continue;
		size_t i = at + 44;
		uint32_t type, t1, s1, iw, ih, bpp, pitch;
		memcpy(&type, &d[i], 4); memcpy(&t1, &d[i + 12], 4); memcpy(&s1, &d[i + 16], 4);
		memcpy(&iw, &d[i + 24], 4); memcpy(&ih, &d[i + 28], 4); memcpy(&bpp, &d[i + 32], 4); memcpy(&pitch, &d[i + 36], 4);
		if (type != 0x18 || t1 != 1 || s1 != 16 || !iw || !ih || iw > 256 || ih > 256 || (bpp != 8 && bpp != 32 && bpp != 4) ||
		    pitch < iw * bpp / 8)
			continue;
		size_t px = i + 40, pal = px + (size_t)pitch * ih;
		if (pal + (bpp <= 8 ? 4u << bpp : 0) > d.size()) continue;
		argb.resize(iw * ih);
		for (uint32_t y = 0; y < ih; y++)
			for (uint32_t x = 0; x < iw; x++) {
				const uint8_t* c;
				if (bpp == 32) c = &d[px + y * pitch + x * 4];
				else {
					unsigned k = bpp == 8 ? d[px + y * pitch + x] : (d[px + y * pitch + x / 2] >> (x & 1 ? 4 : 0)) & 15;
					c = &d[pal + k * 4];
				}
				unsigned a = c[3];
				if (a && c[1] > 25) {   // the backing: R / G about 1.40, B / G about 1.11
					float dist = fabsf(c[0] / (float)c[1] - 1.40f) + fabsf(c[2] / (float)c[1] - 1.11f);
					if (dist < 0.16f) a = 0;
					else if (dist < 0.30f) a = (unsigned)(a * (dist - 0.16f) / 0.14f);
				}
				argb[y * iw + x] = a << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
			}
		*w = (int)iw;
		*h = (int)ih;
		return true;
	}
	return false;
}

void LoadComboIcons() {
	int found = 0;
	for (int lvl : { 2, 1, 3, 4, 5, 6 }) {
		char rel[64], path[MAX_PATH];
		snprintf(rel, sizeof(rel), "LVL%03d\\LVL%02d.KWN", lvl, lvl);
		GameDirPath(path, sizeof(path), rel);
		FILE* f = fopen(path, "rb");
		if (!f) continue;
		std::vector<uint8_t> d;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (size > 0 && size < 256 * 1024 * 1024) {
			d.resize(size);
			if (fread(d.data(), 1, size, f) != (size_t)size) d.clear();
		}
		fclose(f);
		for (int k = 0; k < 6; k++) {
			char name[16];
			snprintf(name, sizeof(name), "combo%d", k);
			if (g_byName.count(name)) continue;
			std::vector<uint32_t> argb;
			int w = 0, h = 0;
			if (!ComboImage(d, kComboTex[k], argb, &w, &h)) continue;
			Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)argb.data());
			wchar_t code = AddScaled(bmp);
			if (code) { g_byName[name] = code; found++; }
		}
		if (found == 6) break;
	}
	Log("icons: %d of the merchant's 6 combo icons from the level files", found);
}

// A sparkle (a four-pointed star) and a glow, gold and white like the combo
// bar's level sparkles, each in four strengths (spark0 .. spark3, glow0 ..):
// spark.png / glow.png in coop_icons are drawn instead when there.
void LoadSparkles() {
	for (int kind = 0; kind < 2; kind++) {
		const char* base = kind ? "glow" : "spark";
		wchar_t path[MAX_PATH];
		IconsDir(path, MAX_PATH, kind ? "glow.png" : "spark.png");
		Gdiplus::Bitmap file(path);
		Gdiplus::Bitmap drawn(kCell, kCell, PixelFormat32bppARGB);
		Gdiplus::Bitmap* src = &file;
		if (file.GetLastStatus() != Gdiplus::Ok || !file.GetHeight()) {
			src = &drawn;
			Gdiplus::Graphics g(&drawn);
			g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
			g.Clear(Gdiplus::Color(0, 0, 0, 0));
			const float c = kCell / 2.0f;
			if (kind) {
				Gdiplus::GraphicsPath disc;
				disc.AddEllipse(2.0f, 2.0f, kCell - 4.0f, kCell - 4.0f);
				Gdiplus::PathGradientBrush b(&disc);
				b.SetCenterColor(Gdiplus::Color(230, 255, 244, 190));
				Gdiplus::Color edge(0, 255, 170, 40);
				int n = 1;
				b.SetSurroundColors(&edge, &n);
				g.FillPath(&b, &disc);
			} else {
				Gdiplus::PointF pts[8];
				for (int i = 0; i < 8; i++) {
					float a = 3.14159265f * i / 4.0f, r = i & 1 ? 6.0f : c - 2.0f;
					pts[i] = Gdiplus::PointF(c + r * cosf(a), c + r * sinf(a));
				}
				Gdiplus::GraphicsPath star;
				star.AddPolygon(pts, 8);
				Gdiplus::PathGradientBrush b(&star);
				b.SetCenterPoint(Gdiplus::PointF(c, c));
				b.SetCenterColor(Gdiplus::Color(255, 255, 255, 255));
				Gdiplus::Color edge(160, 255, 200, 60);
				int n = 1;
				b.SetSurroundColors(&edge, &n);
				g.FillPath(&b, &star);
			}
		}
		static const float kFade[4] = { 1.0f, 0.7f, 0.45f, 0.2f };
		for (int f = 0; f < 4; f++) {
			wchar_t code = AddScaled(*src, kFade[f]);
			char name[16];
			snprintf(name, sizeof(name), "%s%d", base, f);
			if (code) g_byName[name] = code;
		}
	}
	// "Then" between a combo's buttons (the game's font has no '>'): a chevron,
	// white with a dark edge like the text (next.png replaces it).
	if (!g_byName.count("next")) {
		Gdiplus::Bitmap chev(kCell / 2, kCell, PixelFormat32bppARGB);
		{
			Gdiplus::Graphics g(&chev);
			g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
			g.Clear(Gdiplus::Color(0, 0, 0, 0));
			Gdiplus::PointF pts[3] = { Gdiplus::PointF(9.0f, 20.0f), Gdiplus::PointF(23.0f, 32.0f), Gdiplus::PointF(9.0f, 44.0f) };
			Gdiplus::Pen edge(Gdiplus::Color(200, 0, 0, 0), 9.0f);
			edge.SetLineJoin(Gdiplus::LineJoinRound);
			edge.SetStartCap(Gdiplus::LineCapRound);
			edge.SetEndCap(Gdiplus::LineCapRound);
			g.DrawLines(&edge, pts, 3);
			Gdiplus::Pen white(Gdiplus::Color(255, 244, 246, 244), 5.0f);
			white.SetLineJoin(Gdiplus::LineJoinRound);
			white.SetStartCap(Gdiplus::LineCapRound);
			white.SetEndCap(Gdiplus::LineCapRound);
			g.DrawLines(&white, pts, 3);
		}
		wchar_t code = AddBitmap(chev);
		if (code) g_byName["next"] = code;
	}
	// Each hero's blank (hero_slot_asterix...): as wide as its icon's ink (its
	// transparent sides left out) drawn half as big again -- hints.cpp draws the
	// icon over it, so it stands as far from the words as they do from each other.
	for (const char* hero : { "asterix", "obelix" }) {
		auto it = g_byName.find(hero);
		if (it == g_byName.end()) continue;
		const Glyph* gl = nullptr;
		for (const Glyph& g : g_glyphs) if (g.code == it->second) gl = &g;
		if (!gl) continue;
		int x0 = gl->w, x1 = -1;
		for (int y = 0; y < kCell; y++)
			for (int x = 0; x < gl->w; x++)
				if ((g_atlas[(gl->y + y) * kAtlasW + gl->x + x] >> 24) > 40) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
		int ink = x1 >= x0 ? x1 - x0 + 1 : gl->w;
		Gdiplus::Bitmap blank(ink * 3 / 2, kCell, PixelFormat32bppARGB);
		{
			Gdiplus::Graphics g(&blank);
			g.Clear(Gdiplus::Color(0, 0, 0, 0));
		}
		wchar_t code = AddBitmap(blank);
		if (code) g_byName[std::string("hero_slot_") + hero] = code;
		Log("icons: %s's icon: ink %d of %d px wide (columns %d-%d)", hero, ink, gl->w, x0, x1);
	}
}

// Every PNG of coop_icons but the keycap templates (key, key_wide and their
// unpressed variants) and the sparkles' images.
void LoadIcons() {
	g_loaded = true;
	if (!g_gdiplus) {
		Gdiplus::GdiplusStartupInput in;
		if (Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr) != Gdiplus::Ok) { g_gdiplus = 0; return; }
	}
	if (!g_atlas) g_atlas = (uint32_t*)calloc(kAtlasW * kAtlasH, 4);
	if (!g_atlas) return;
	wchar_t pattern[MAX_PATH];
	IconsDir(pattern, MAX_PATH, "*.png");
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE) { Log("icons: no coop_icons folder -- buttons stay names"); return; }
	do {
		char name[MAX_PATH];
		WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, name, sizeof(name), nullptr, nullptr);
		char* dot = strrchr(name, '.');
		if (dot) *dot = 0;
		for (char* c = name; *c; c++) *c = (char)tolower(*c);
		if (!strcmp(name, "key") || !strcmp(name, "key_wide") || !strcmp(name, "key_unpressed") ||
		    !strcmp(name, "key_wide_unpressed") || !strcmp(name, "spark") || !strcmp(name, "glow"))
			continue;
		wchar_t path[MAX_PATH];
		IconsDir(path, MAX_PATH, (std::string(name) + ".png").c_str());
		wchar_t code = AddPng(path);
		if (code) g_byName[name] = code;
		else Log("icons: %s.png not loaded", name);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	Log("icons: %u icons from coop_icons", (unsigned)g_byName.size());
	LoadComboIcons();
	LoadSparkles();
}

void Upload() {
	if (!g_atlas) return;
	if (!g_raster) {
		g_raster = ((RasterCreateFn)kRwRasterCreate)(kAtlasW, kAtlasH, 32, 0x504);
		if (!g_raster) { Log("icons: no raster"); return; }
		g_texture = ((TextureCreateFn)kRwTextureCreate)(g_raster);
		if (!g_texture) { Log("icons: no texture"); return; }
		*(uint8_t*)(g_texture + 0x50) = 2;      // linear filtering
		*(uint8_t*)(g_texture + 0x51) = 0x33;   // clamped
		*(int*)(g_texture + 0x54) += 1;         // the mod's own reference: fonts come and go
	}
	uint8_t* px = ((RasterLockFn)kRwRasterLock)(g_raster, 0, 1);
	if (!px) { Log("icons: the page did not lock"); return; }
	int stride = *(int*)(g_raster + 0x18);
	for (int y = 0; y < kAtlasH; y++) memcpy(px + y * stride, g_atlas + y * kAtlasW, kAtlasW * 4);
	((RasterUnlockFn)kRwRasterUnlock)(g_raster);
}

// The font with the game's action icons gets the page and room for the mod's
// glyphs (its wide characters run to kLastCode).
bool PrepareFont(Font& ft) {
	char* f = ft.f;
	ft.tried = true;
	if (!(*(uint32_t*)(f + 0x10) & 2)) return false;
	int first = *(int*)(f + 0x124), n = *(int*)(f + 0x128);
	int16_t* wide = *(int16_t**)(f + 0x12C);
	if (!wide || first > 0xE000 || 0xE000 >= first + n || wide[0xE000 - first] < 0) return false;
	char* list = *(char**)(f + 0x134);
	if (!list || *(int*)(list + 4) != *(int*)(f + 0x130) || *(int*)(list + 0xC) != 0x20) return false;
	int page = -1;
	for (int i = 0; i < 4 && page < 0; i++) if (!*(char**)(f + 0x14 + 4 * i)) page = i;
	if (page < 0) return false;
	int need = kLastCode + 1 - first;
	if (n < need) {
		char* rw = *(char**)kRwEngine;
		int16_t* nw = (int16_t*)(*(RwMallocFn*)(rw + 0x134))(need * 2, 0x30190);
		if (!nw) return false;
		memcpy(nw, wide, n * 2);
		for (int i = n; i < need; i++) nw[i] = -1;
		(*(RwFreeFn*)(rw + 0x138))(wide);
		*(int16_t**)(f + 0x12C) = nw;
		*(int*)(f + 0x128) = need;
	}
	*(char**)(f + 0x14 + 4 * page) = g_texture;
	*(int*)(g_texture + 0x54) += 1;
	ft.page = page;
	ft.ok = true;
	return true;
}

void AddGlyphs(Font& ft) {
	char* f = ft.f;
	int first = *(int*)(f + 0x124);
	for (; ft.glyphs < g_glyphs.size(); ft.glyphs++) {
		const Glyph& g = g_glyphs[ft.glyphs];
		char* list = *(char**)(f + 0x134);
		((SListNewFn)kRwSListNew)(list, 1, 0x30190);
		int count = *(int*)(list + 4);
		char* e = *(char**)list + (count - 1) * 0x20;
		float* fl = (float*)e;
		fl[0] = g.w / (float)kCell;
		*(char**)(e + 4) = g_texture;
		fl[2] = (g.x + 0.5f) / kAtlasW;
		fl[3] = (g.y + 0.5f) / kAtlasH;
		fl[4] = (g.x + g.w - 0.5f) / kAtlasW;
		fl[5] = (g.y + kCell - 0.5f) / kAtlasH;
		*(uint32_t*)(e + 0x18) = 0;
		*(uint32_t*)(e + 0x1C) = (uint32_t)ft.page;
		*(int*)(f + 0x130) = count;
		(*(int16_t**)(f + 0x12C))[g.code - first] = (int16_t)(count - 1);
	}
}

void AddToFonts() {
	for (Font& ft : g_fonts) if (ft.ok) AddGlyphs(ft);
}

void KeyLabel(int vk, wchar_t* out, int n) {
	switch (vk) {
	case VK_UP:    wcscpy(out, L"\x2191"); return;
	case VK_DOWN:  wcscpy(out, L"\x2193"); return;
	case VK_LEFT:  wcscpy(out, L"\x2190"); return;
	case VK_RIGHT: wcscpy(out, L"\x2192"); return;
	case VK_BACK:  wcscpy(out, L"Bksp"); return;
	case VK_RETURN: wcscpy(out, L"Enter"); return;
	case VK_LSHIFT: wcscpy(out, L"L Shift"); return;
	case VK_RSHIFT: wcscpy(out, L"R Shift"); return;
	case VK_LCONTROL: wcscpy(out, L"L Ctrl"); return;
	case VK_RCONTROL: wcscpy(out, L"R Ctrl"); return;
	case VK_LMENU: wcscpy(out, L"L Alt"); return;
	case VK_RMENU: wcscpy(out, L"R Alt"); return;
	}
	IconsKeyName(vk, out, n);
}

// The designer's keycap of a key in coop_icons\kb (the Figma export's names:
// "/" came out as key_.png, "\" in a folder key_, "*" as key__.png), plain and
// pressed; null when it has none.
struct KbCap { int vk; const char* plain; const char* pressed; };
const KbCap kKbOdd[] = {
	{ VK_OEM_2, "key_.png", "key_\\_press-1.png" },  { VK_DIVIDE, "key_.png", "key_\\_press-1.png" },
	{ VK_OEM_5, "key_\\_.png", "key_\\_press.png" }, { VK_OEM_102, "key_\\_.png", "key_\\_press.png" },
	{ VK_MULTIPLY, "key__.png", "key___press.png" },
};
const char* KbName(int vk) {
	static char buf[16];
	if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(buf, sizeof(buf), "key_%c", tolower(vk)); return buf; }
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(buf, sizeof(buf), "key_%d", vk - VK_NUMPAD0); return buf; }
	if (vk >= VK_F1 && vk <= VK_F12) { snprintf(buf, sizeof(buf), "key_F%d", vk - VK_F1 + 1); return buf; }
	switch (vk) {
	case VK_UP: return "arrow_up";          case VK_DOWN: return "arrow_down";
	case VK_LEFT: return "arrow_left";      case VK_RIGHT: return "arrow_right";
	case VK_SPACE: return "key_space";      case VK_RETURN: return "key_enter";
	case VK_ESCAPE: return "key_esc";       case VK_TAB: return "key_tab";
	case VK_BACK: return "key_bksp";        case VK_CAPITAL: return "key_caps";
	case VK_DELETE: return "key_del";       case VK_INSERT: return "key_insert";
	case VK_HOME: return "key_home";        case VK_END: return "key_end";
	case VK_PRIOR: return "key_pgup";       case VK_NEXT: return "key_pgdn";
	case VK_PAUSE: return "key_pausebreak"; case VK_SNAPSHOT: return "key_printscreen";
	case VK_SCROLL: return "key_scrolllock";
	case VK_LSHIFT: return "key_lshift";    case VK_RSHIFT: return "key_rshift";
	case VK_LCONTROL: return "key_lctrl";   case VK_RCONTROL: return "key_rctrl";
	case VK_LMENU: return "key_lalt";       case VK_RMENU: return "key_ralt";
	case VK_LWIN: case VK_RWIN: return "key_win";
	case VK_OEM_7: return "key_'";          case VK_OEM_COMMA: return "key_,";
	case VK_OEM_MINUS: case VK_SUBTRACT: return "key_-";
	case VK_OEM_PERIOD: case VK_DECIMAL: return "key_.";
	case VK_OEM_1: return "key_;";          case VK_OEM_PLUS: return "key_=";
	case VK_ADD: return "key_+";            case VK_OEM_4: return "key_[";
	case VK_OEM_6: return "key_]";          case VK_OEM_3: return "key_`";
	}
	return nullptr;
}
wchar_t AddKbPng(int vk, bool pressed) {
	char file[64];
	file[0] = 0;
	for (const KbCap& k : kKbOdd)
		if (k.vk == vk) snprintf(file, sizeof(file), "kb\\%s", pressed ? k.pressed : k.plain);
	const char* name = file[0] ? nullptr : KbName(vk);
	if (name) snprintf(file, sizeof(file), "kb\\%s%s.png", name, pressed ? "_press" : "");
	if (!file[0]) return 0;
	wchar_t path[MAX_PATH];
	IconsDir(path, MAX_PATH, file);
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;
	return AddPng(path);
}

// A keycap: coop_icons\kb's, else key.png (one character) or key_wide.png with
// the key's name on it. Pressed (a combo hint's button done, a held key):
// kb's *_press.png; drawn, key_unpressed.png / key_wide_unpressed.png, else the
// plain template darkened, and a grey name.
wchar_t AddKeycap(int vk, int look) {
	bool unpressed = look == ICON_PRESSED;
	if (wchar_t g = AddKbPng(vk, unpressed)) return g;
	wchar_t label[32];
	KeyLabel(vk, label, 32);
	bool wide = wcslen(label) > 1 && !(label[0] >= 0x2190 && label[0] <= 0x2193);
	wchar_t path[MAX_PATH];
	IconsDir(path, MAX_PATH, unpressed ? (wide ? "key_wide_unpressed.png" : "key_unpressed.png") : wide ? "key_wide.png" : "key.png");
	Gdiplus::Bitmap own(path);
	bool darken = false;
	Gdiplus::Bitmap* tplp = &own;
	wchar_t tplPath[MAX_PATH];
	IconsDir(tplPath, MAX_PATH, wide ? "key_wide.png" : "key.png");
	Gdiplus::Bitmap pressedTpl(tplPath);
	if (unpressed && (own.GetLastStatus() != Gdiplus::Ok || !own.GetHeight())) { tplp = &pressedTpl; darken = true; }
	Gdiplus::Bitmap& tpl = *tplp;
	if (tpl.GetLastStatus() != Gdiplus::Ok || !tpl.GetHeight()) return 0;
	int w = (int)((tpl.GetWidth() * kCell + tpl.GetHeight() / 2) / tpl.GetHeight());
	Gdiplus::Bitmap cap(w, kCell, PixelFormat32bppARGB);
	Gdiplus::Graphics g(&cap);
	g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
	g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	Gdiplus::ImageAttributes dim;
	Gdiplus::ColorMatrix cm = { { { 0.45f, 0, 0, 0, 0 }, { 0, 0.45f, 0, 0, 0 }, { 0, 0, 0.45f, 0, 0 }, { 0, 0, 0, 1, 0 }, { 0, 0, 0, 0, 1 } } };
	dim.SetColorMatrix(&cm);
	g.DrawImage(&tpl, Gdiplus::Rect(0, 0, w, kCell), 0, 0, tpl.GetWidth(), tpl.GetHeight(), Gdiplus::UnitPixel,
	            darken ? &dim : nullptr);
	// The name, as big as fits on the cap's face.
	Gdiplus::FontFamily fam(L"Arial");
	Gdiplus::StringFormat sf;
	sf.SetAlignment(Gdiplus::StringAlignmentCenter);
	sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	float size = wide ? 32.0f : 46.0f;
	Gdiplus::RectF bounds;
	for (; size > 10.0f; size -= 1.0f) {
		Gdiplus::GraphicsPath probe;
		probe.AddString(label, -1, &fam, Gdiplus::FontStyleBold, size, Gdiplus::PointF(0, 0), nullptr);
		probe.GetBounds(&bounds);
		if (bounds.Width <= w - 20 && bounds.Height <= kCell - 28) break;
	}
	Gdiplus::GraphicsPath path2;
	path2.AddString(label, -1, &fam, Gdiplus::FontStyleBold, size, Gdiplus::RectF(0, 0, (float)w, kCell - 8.0f), &sf);
	Gdiplus::Pen outline(Gdiplus::Color(200, 0, 0, 0), 3.0f);
	outline.SetLineJoin(Gdiplus::LineJoinRound);
	g.DrawPath(&outline, &path2);
	Gdiplus::SolidBrush white(unpressed ? Gdiplus::Color(255, 150, 152, 150) : Gdiplus::Color(255, 244, 246, 244));
	g.FillPath(&white, &path2);
	return AddBitmap(cap);
}

// Player 1's binding of `action`: the byte it reads (binding +0xC source, +0x14
// the current pointer) in the pad (CKInput +0x04, 0xCC a pad) or the keyboard
// (+0x0C, 0x3FC). Returns 1 pad (`off` within it), 2 keyboard (`off` = byte), 0.
int P1Source(int action, int* off) {
	InputManager im = GetInputManager();
	char* in = im ? im.raw() : nullptr;
	if (!in || action < 0 || action >= im.NumActions()) return 0;
	char* src = *(char**)(im.Binding(action) + 0xC);
	char* cur = src ? *(char**)(src + 0x14) : nullptr;
	if (!cur) return 0;
	char* pad = *(char**)(in + 0x04);
	unsigned npad = *(uint8_t*)(in + 0x10);
	if (pad && npad && cur >= pad && cur < pad + npad * 0xCC) { *off = (int)(cur - pad) % 0xCC; return 1; }
	char* kb = *(char**)(in + 0x0C);
	unsigned nkb = *(uint8_t*)(in + 0x12);
	if (kb && nkb && cur >= kb && cur < kb + 0x3FC) { *off = (int)(cur - kb); return 2; }
	return 0;
}

// The game's pad bytes: +6..9 left stick up/down/left/right, +10..13 right
// stick, +14 + n button n (A B X Y LB RB Back Start L3 R3), +26..29 D-pad.
const char* PadByteName(int off) {
	static const char* const kButtons[10] = { "pad_a", "pad_b", "pad_x", "pad_y", "pad_lb", "pad_rb",
	                                          "pad_back", "pad_start", "pad_l3", "pad_r3" };
	static const char* const kLs[4] = { "ls_up", "ls_down", "ls_left", "ls_right" };
	static const char* const kRs[4] = { "rs_up", "rs_down", "rs_left", "rs_right" };
	static const char* const kDpad[4] = { "dpad_up", "dpad_down", "dpad_left", "dpad_right" };
	if (off >= 6 && off <= 9) return kLs[off - 6];
	if (off >= 10 && off <= 13) return kRs[off - 10];
	if (off >= 14 && off <= 23) return kButtons[off - 14];
	if (off >= 26 && off <= 29) return kDpad[off - 26];
	return nullptr;
}

// Keyboard keys: the game's keyboard device (CKInput +0x0C, 0x3FC bytes) keeps
// one byte per key, written by its window-message handler (vtable slot 5,
// thiscall(vk, lparam, down), ret 0xC). Which byte a virtual key lands on is
// found by pressing each key once on the device with its bytes saved and put
// back.
uint8_t g_vkOfByte[0x400];
bool    g_vkMapped = false;

void MapKeyboard(char* kb) {
	g_vkMapped = true;
	static uint8_t saved[0x3FC];
	typedef void(__fastcall* KeyFn)(char* dev, void* edx, uint32_t vk, uint32_t lparam, uint32_t down);
	KeyFn key = (KeyFn)(*(void***)kb)[5];
	for (uint32_t vk = 1; vk < 255; vk++) {
		// The device files a key by the scan code in the message's lparam (bits
		// 16-23, 24 extended): F at 0xB5 = DIK_F 0x21 + 0x94. Without it every
		// key landed on one byte and no key had a cap.
		UINT sc = MapVirtualKeyW(vk, 0 /* MAPVK_VK_TO_VSC */);
		if (!sc) continue;
		uint32_t lp = 1 | sc << 16;
		if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
		    vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE || vk == VK_RCONTROL ||
		    vk == VK_RMENU || vk == VK_NUMLOCK || vk == VK_LWIN || vk == VK_RWIN)
			lp |= 1u << 24;
		memcpy(saved, kb, sizeof(saved));
		key(kb, nullptr, vk, lp, 1);
		for (int i = 4; i < 0x3FC; i++)   // past the vtable
			if (kb[i] != (char)saved[i] && !g_vkOfByte[i]) { g_vkOfByte[i] = (uint8_t)vk; break; }
		memcpy(kb, saved, sizeof(saved));
	}
	int n = 0;
	for (int i = 0; i < 0x400; i++) n += g_vkOfByte[i] != 0;
	Log("icons: keyboard mapped, %d keys (Space at byte %X, F at %X, Up at %X)", n,
	    [] { for (int i = 0; i < 0x400; i++) if (g_vkOfByte[i] == VK_SPACE) return i; return -1; }(),
	    [] { for (int i = 0; i < 0x400; i++) if (g_vkOfByte[i] == 'F') return i; return -1; }(),
	    [] { for (int i = 0; i < 0x400; i++) if (g_vkOfByte[i] == VK_UP) return i; return -1; }());
}

// The game's action icons and the CKInput actions they stand for.
int ActionOfIcon(wchar_t c) {
	switch (c) {
	case 0xE000: return 5;    // action
	case 0xE001: return 10;   // launch Dogmatix
	case 0xE002: return 4;    // jump
	case 0xE003: return 12;   // pause
	case 0xE005: return 0;    // up
	case 0xE006: return 1;    // down
	case 0xE007: return 3;    // right
	case 0xE008: return 2;    // left
	case 0xE009: return 7;    // targeted attack
	case 0xE00A: return 6;    // grab
	case 0xE00B: return 8;    // launch Obelix
	case 0xE00E: return 20;   // confirm
	case 0xE00F: return 21;   // cancel
	}
	return -1;
}
bool IsDirIcon(wchar_t c) { return c >= 0xE005 && c <= 0xE008; }

// The CKInput actions of a kind of directions, up / down / left / right.
const int kDirActions[3][4] = { { 0, 1, 2, 3 }, { 16, 17, 18, 19 }, { 47, 46, 48, 49 } };

// Where a player's `dirs` directions are: "ls", "rs", "dpad" (all four on that
// stick or the D-pad, `bound` the direction 0-3 each is on there), "kb" (all on
// keys), else "is" (a mix: the general icons). Player 2 moves with the left
// stick or its keys; its menus and camera are not its own (the D-pad's, the
// right stick's on a pad, the general icons on the keyboard).
const char* DirDevice(int player, int dirs, int bound[4]) {
	for (int d = 0; d < 4; d++) bound[d] = d;
	if (player == 1) {
		bool kb = CoopP2Pad() == kKeyboardPad || getenv("XXL_ICONP2KB");
		if (dirs == DIRS_MOVE) return kb ? "kb" : "ls";
		return kb ? "is" : dirs == DIRS_CAMERA ? "rs" : "dpad";
	}
	static const char* const kPads[3] = { "ls", "rs", "dpad" };
	static const int kFirst[3] = { 6, 10, 26 };   // each one's up byte (up, down, left, right)
	int dev = -1, keys = 0;
	for (int d = 0; d < 4; d++) {
		int off, src = P1Source(kDirActions[dirs][d], &off);
		if (src == 2) { keys++; continue; }
		int k = -1;
		for (int i = 0; i < 3; i++) if (src == 1 && off >= kFirst[i] && off < kFirst[i] + 4) k = i;
		if (k < 0 || (dev >= 0 && k != dev)) return "is";
		dev = k;
		bound[d] = off - kFirst[k];
	}
	if (keys == 4) return "kb";
	return keys || dev < 0 ? "is" : kPads[dev];
}

// One player's icons for a run of direction icons ("[Left] [Right]",
// "[Up] - [Down]"): one glyph for the pair or all four on a stick, else one
// per direction.
std::wstring DirRun(int player, int dirs, const wchar_t* run, size_t n) {
	static const char* const kOne[4] = { "up", "down", "left", "right" };
	int bound[4];
	const char* dev = DirDevice(player, dirs, bound);
	if (!strcmp(dev, "kb")) {   // each key's cap, side by side
		std::wstring out;
		for (size_t i = 0; i < n; i++) {
			if (!IsDirIcon(run[i])) continue;
			wchar_t g = IconActionGlyph(player, kDirActions[dirs][ActionOfIcon(run[i])]);
			out += g ? g : run[i];
		}
		return out;
	}
	// The directions shown, as the stick's own (the binding's: "Camera up" may
	// be the right stick's down).
	unsigned mask = 0;
	for (size_t i = 0; i < n; i++) if (IsDirIcon(run[i])) mask |= 1u << bound[ActionOfIcon(run[i])];
	const char* set = mask == 3 ? "updown" : mask == 12 ? "leftright" : mask == 15 ? "all" : nullptr;
	for (int d = 0; d < 4 && !set; d++) if (mask == 1u << d) set = kOne[d];
	if (set) {
		wchar_t g = IconGlyph((std::string(dev) + "_" + set).c_str());
		if (g) return std::wstring(1, g);
	}
	std::wstring out;
	for (size_t i = 0; i < n; i++) {
		if (!IsDirIcon(run[i])) { out += run[i]; continue; }
		wchar_t g = IconGlyph((std::string(dev) + "_" + kOne[bound[ActionOfIcon(run[i])]]).c_str());
		out += g ? g : run[i];
	}
	return out;
}

// `s` for the log: the mod's glyphs by name ("<pad_a>"), the game's icons '#'.
void GlyphText(const wchar_t* s, char* out, size_t n) {
	size_t o = 0;
	for (; *s && o + 1 < n; s++) {
		const char* name = nullptr;
		for (const auto& kv : g_byName) if (kv.second == *s) name = kv.first.c_str();
		for (const auto& kv : g_byVk)
			if (kv.second == *s && !name) {
				static char key[16];
				snprintf(key, sizeof(key), "vk%02X%s", kv.first & 0xFF, kv.first & 0x100 ? "_press" : "");
				name = key;
			}
		if (name) o += snprintf(out + o, n - o, "<%s>", name);
		else out[o++] = *s < 128 ? (char)*s : *s >= 0xE000 && *s < 0xE200 ? '#' : '?';
		if (o >= n) o = n - 1;
	}
	out[o] = 0;
}

std::wstring One(int player, wchar_t c) {
	wchar_t g = IconActionGlyph(player, ActionOfIcon(c));
	return std::wstring(1, g ? g : c);
}

} // namespace

void IconsKeyName(int vk, wchar_t* out, int n) {
	UINT sc = MapVirtualKeyW(vk, 0 /* MAPVK_VK_TO_VSC */);
	LONG lp = (LONG)(sc << 16);
	if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
	    vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE)
		lp |= 1 << 24;
	if (!sc || !GetKeyNameTextW(lp, out, n)) swprintf(out, n, L"key %02X", vk);
}

int IconsP1Vk(int action) {
	int off;
	if (P1Source(action, &off) != 2) return 0;
	InputManager im = GetInputManager();
	char* kb = *(char**)(im.raw() + 0x0C);
	if (!g_vkMapped) MapKeyboard(kb);
	// The bindings read the device's second block (+0x90: Space at 0xCD), the
	// message handler writes the first (Space at 0x3D).
	return off >= 0x90 && g_vkOfByte[off - 0x90] ? g_vkOfByte[off - 0x90] : g_vkOfByte[off];
}

wchar_t IconGlyph(const char* name) {
	auto it = g_byName.find(name);
	return it == g_byName.end() || !g_texture ? 0 : it->second;
}

wchar_t IconKeyGlyph(int vk, int look) {
	if (vk <= 0 || vk > 255 || !g_texture || !g_atlas) return 0;
	int key = vk | (look == ICON_PRESSED ? 0x100 : 0);   // a key waiting is the plain one
	auto it = g_byVk.find(key);
	if (it != g_byVk.end()) return it->second;
	wchar_t g = AddKeycap(vk, look);
	g_byVk[key] = g;
	if (!g) return 0;
	AddToFonts();
	Upload();
	return g;
}

wchar_t IconComboGlyph(int kind) {
	char name[16];
	snprintf(name, sizeof(name), "combo%d", kind);
	return kind >= 0 && kind < 6 ? IconGlyph(name) : 0;
}

wchar_t IconSparkGlyph(int fade) {
	char name[16];
	snprintf(name, sizeof(name), "spark%d", fade < 0 ? 0 : fade > 3 ? 3 : fade);
	return IconGlyph(name);
}

wchar_t IconGlowGlyph(int fade) {
	char name[16];
	snprintf(name, sizeof(name), "glow%d", fade < 0 ? 0 : fade > 3 ? 3 : fade);
	return IconGlyph(name);
}

namespace {
// A pad icon's waiting variant ("pad_a" -> "pad_a_unpressed"), else itself.
wchar_t PadGlyph(const char* name, int look) {
	if (look == ICON_WAITING) {
		wchar_t g = IconGlyph((std::string(name) + "_unpressed").c_str());
		if (g) return g;
	}
	return IconGlyph(name);
}
} // namespace

wchar_t IconActionGlyph(int player, int action, int look) {
	static bool kbTest = getenv("XXL_ICONP2KB") != nullptr;   // autotest: player 2's icons as on the keyboard
	if (player == 1) {
		if (CoopP2Pad() == kKeyboardPad || kbTest) {
			int key;
			switch (action) {
			case 0: key = CoopConfig::KEY_UP; break;      case 1: key = CoopConfig::KEY_DOWN; break;
			case 2: key = CoopConfig::KEY_LEFT; break;    case 3: key = CoopConfig::KEY_RIGHT; break;
			case 4: case 20: key = CoopConfig::KEY_JUMP; break;
			case 5: key = CoopConfig::KEY_ATTACK; break;
			case 6: case 21: key = CoopConfig::KEY_GRAB; break;
			case 7: key = CoopConfig::KEY_DASH; break;    case 8: key = CoopConfig::KEY_LB; break;
			case 10: key = CoopConfig::KEY_RB; break;     case 15: key = CoopConfig::KEY_START; break;
			default: return 0;
			}
			return IconKeyGlyph(g_config.keys[key], look);
		}
		const char* name;
		switch (action) {
		case 0: name = "ls_up"; break;    case 1: name = "ls_down"; break;
		case 2: name = "ls_left"; break;  case 3: name = "ls_right"; break;
		case 4: case 20: name = "pad_a"; break;
		case 5: name = "pad_x"; break;
		case 6: case 21: name = "pad_b"; break;
		case 7: name = "pad_y"; break;    case 8: name = "pad_lb"; break;
		case 10: name = "pad_rb"; break;  case 15: name = "pad_start"; break;
		default: return 0;
		}
		return PadGlyph(name, look);
	}
	int off;
	switch (P1Source(action, &off)) {
	case 1: { const char* n = PadByteName(off); return n ? PadGlyph(n, look) : 0; }
	case 2: return IconKeyGlyph(IconsP1Vk(action), look);
	}
	return 0;
}

const wchar_t* IconsRewrite(const wchar_t* s, int who, int dirs) {
	if (!s || !g_texture) return s;
	const wchar_t* c = s;
	while (*c && !(*c >= 0xE000 && *c <= 0xE00F)) c++;
	if (!*c) return s;
	std::wstring out;
	for (c = s; *c;) {
		if (ActionOfIcon(*c) < 0) { out += *c++; continue; }
		std::wstring u[2];
		if (IsDirIcon(*c)) {
			// The run: direction icons with only ' ', '-', ',' between them.
			const wchar_t* e = c + 1;
			for (const wchar_t* k = e;; ) {
				while (*k == L' ' || *k == L'-' || *k == L',') k++;
				if (!IsDirIcon(*k)) break;
				e = ++k;
			}
			for (int p = 0; p < 2; p++) u[p] = DirRun(p, dirs, c, e - c);
			c = e;
		} else {
			for (int p = 0; p < 2; p++) u[p] = One(p, *c);
			c++;
		}
		if (who >= 0) out += u[who];
		else if (u[0] == u[1]) out += u[0];
		else out += u[0] + L"/" + u[1];
	}
	if (out == s) return s;
	static std::unordered_map<std::wstring, std::wstring*> kept;
	auto it = kept.find(out);
	if (it == kept.end()) {
		it = kept.emplace(out, new std::wstring(out)).first;
		if (g_logs < 40) {
			g_logs++;
			char a[160] = {}, b[160] = {};
			for (int i = 0; s[i] && i < 159; i++) a[i] = s[i] < 128 ? (char)s[i] : s[i] >= 0xE000 && s[i] < 0xE200 ? '#' : '?';
			GlyphText(out.c_str(), b, sizeof(b));
			Log("icons: \"%s\" -> \"%s\" (for %s)", a, b, who < 0 ? "both" : who ? "player 2" : "player 1");
		}
	}
	return it->second->c_str();
}

const wchar_t* IconsAppendAction(const wchar_t* s, int action) {
	wchar_t g = s && g_texture ? IconActionGlyph(0, action) : 0;
	if (!g) return s;
	std::wstring out = std::wstring(s) + L" " + g;
	static std::unordered_map<std::wstring, std::wstring*> kept;
	auto it = kept.find(out);
	if (it == kept.end()) {
		it = kept.emplace(out, new std::wstring(out)).first;
		if (g_logs < 40) {
			g_logs++;
			char b[160];
			GlyphText(out.c_str(), b, sizeof(b));
			Log("icons: action %d's icon after its row: \"%s\"", action, b);
		}
	}
	return it->second->c_str();
}

namespace {

// Fonts read before the hook was in (the mod's hooks go in with the title
// screen up): CManager2d (13/16, vtable 0x623A28) keeps them at +8 + id * 4
// (ids 0-9).
void FindFonts() {
	static DWORD last = 0;
	if (GetTickCount() - last < 1000) return;
	last = GetTickCount();
	char* m = (char*)GetLvlObject(13, 16, 0);
	if (!m || *(uintptr_t*)m != 0x623A28) return;
	for (int i = 0; i < 10; i++) {
		char* f = *(char**)(m + 8 + 4 * i);
		if (!f) continue;
		bool known = false;
		for (const Font& ft : g_fonts) if (ft.f == f) known = true;
		if (!known) g_fonts.push_back({ f, 0, false, false, -1 });
	}
}

} // namespace

void IconsFrame() {
	FindFonts();
	bool pending = false;
	for (const Font& ft : g_fonts) if (!ft.tried) pending = true;
	if (!pending) return;
	if (!g_loaded) LoadIcons();
	if (g_byName.empty()) { for (Font& ft : g_fonts) ft.tried = true; return; }
	if (!g_texture) Upload();
	if (!g_texture) { for (Font& ft : g_fonts) ft.tried = true; return; }
	for (Font& ft : g_fonts) {
		if (ft.tried) continue;
		if (PrepareFont(ft)) {
			AddGlyphs(ft);
			Log("icons: font %p takes the icons (page %d, %u glyphs)", ft.f, ft.page, (unsigned)ft.glyphs);
		}
	}
	Upload();
}

bool IconsInstallHooks() {
	return InstallDetour(kFontRead, (void*)FontReadHook, (void**)&g_origFontRead, kFontReadPrologue,
	                     sizeof(kFontReadPrologue), "Rt2dFontStreamRead (co-op icons)");
}
