#include "tint.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>

// Second chance's two health rows (hud2.cpp): each hero's shields in its own
// colours -- Asterix's red (his trousers), Obelix's blue (his stripes). The
// shields' textures are a wood's orange-brown with next to no blue (a material
// colour only darkens a channel: no blue row that way), so they are recoloured
// once per hero into copies of their own -- every mip level; DXT1/3/5 by each
// block's two end colours (the texels in between follow), 8888 / 565 / 1555 /
// 4444 by pixel; other formats are left as they are -- and the copy is bound
// in the original's place while that hero's row is drawn: the device's
// SetTexture (IDirect3DDevice9 vtable slot 65) is wrapped. The shields'
// textures are learned in the other hero's row pass (only its shields and their
// chips are drawn then); in the game's own HUD pass (the leading hero's row,
// with the rest of the HUD) only those are swapped.
// RenderWare binds a texture only when it is not the one it bound last (its
// stage cache: RwRaster* at 0x668600 + 24 * stage, FUN_0058BE30), so the cache
// of stage 0 is cleared -- and the device's stage 0 with it -- when a tinted
// pass begins and when it ends: the copies are bound through it, and nothing
// of ours stays bound behind its back.
namespace {

const uintptr_t kD3DDevicePtr = 0x668978;   // IDirect3DDevice9* (RenderWare's)
const uintptr_t kStage0Cache  = 0x668600;   // RenderWare's raster bound on stage 0
const size_t    kSetTextureSlot = 65;

typedef HRESULT(__stdcall* SetTextureFn)(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex);
SetTextureFn g_origSetTexture = nullptr;
void**       g_hookedVtbl = nullptr;

int  g_hero = -1;      // the row being drawn, -1 none
bool g_learn = false;

IDirect3DBaseTexture9* g_known[16];   // the shields' textures
int g_nKnown = 0;
struct Copy { IDirect3DBaseTexture9* src; int hero; IDirect3DTexture9* dst; };   // dst null: could not be made
Copy g_copies[32];
int  g_nCopies = 0;

// Each hero's colour, and how much of it (the rest is the texture's own).
// Out = (1 - kMix) * in + kMix * luma(in) * colour * kGain.
const float kColour[2][3] = { { 1.0f, 0.32f, 0.26f }, { 0.30f, 0.52f, 1.0f } };
const float kGain = 1.75f, kMix = 0.8f;

void Recolour(int hero, float& r, float& g, float& b) {
	float l = 0.30f * r + 0.59f * g + 0.11f * b;
	float* c[3] = { &r, &g, &b };
	for (int k = 0; k < 3; k++) {
		float v = l * kColour[hero][k] * kGain;
		if (v > 1.0f) v = 1.0f;
		*c[k] = (1.0f - kMix) * *c[k] + kMix * v;
	}
}

uint16_t Recolour565(int hero, uint16_t c) {
	float r = ((c >> 11) & 31) / 31.0f, g = ((c >> 5) & 63) / 63.0f, b = (c & 31) / 31.0f;
	Recolour(hero, r, g, b);
	return (uint16_t)((int)(r * 31.0f + 0.5f) << 11 | (int)(g * 63.0f + 0.5f) << 5 | (int)(b * 31.0f + 0.5f));
}

// One 8-byte colour block. DXT1 reads it in 4 colours when end colour 0 > 1,
// else in 3 and transparent: kept so (the ends swapped, and the texels' indices
// with them, where the new colours came out the other way round).
void RecolourBlock(int hero, uint8_t* blk, bool dxt1) {
	uint16_t c0 = (uint16_t)(blk[0] | blk[1] << 8), c1 = (uint16_t)(blk[2] | blk[3] << 8);
	uint32_t idx = (uint32_t)(blk[4] | blk[5] << 8 | blk[6] << 16 | (uint32_t)blk[7] << 24);
	uint16_t n0 = Recolour565(hero, c0), n1 = Recolour565(hero, c1);
	if (dxt1) {
		bool four = c0 > c1;
		if (four) {
			if (n0 < n1) { uint16_t t = n0; n0 = n1; n1 = t; idx ^= 0x55555555u; }   // 0 <-> 1, 2 <-> 3
			else if (n0 == n1) { if (n0) n1 = (uint16_t)(n0 - 1); else n0 = 1; }
		} else if (n0 > n1) {
			uint16_t t = n0; n0 = n1; n1 = t;
			uint32_t out = 0;
			for (int i = 0; i < 16; i++) {   // 0 <-> 1; 2 (the middle) and 3 (transparent) stay
				uint32_t v = (idx >> (2 * i)) & 3;
				if (v < 2) v ^= 1;
				out |= v << (2 * i);
			}
			idx = out;
		}
	}
	blk[0] = (uint8_t)n0; blk[1] = (uint8_t)(n0 >> 8);
	blk[2] = (uint8_t)n1; blk[3] = (uint8_t)(n1 >> 8);
	blk[4] = (uint8_t)idx; blk[5] = (uint8_t)(idx >> 8); blk[6] = (uint8_t)(idx >> 16); blk[7] = (uint8_t)(idx >> 24);
}

// One locked level, `h` rows of `rowBytes` (blocks: rows of 4 x 4 texels).
bool RecolourLevel(int hero, D3DFORMAT f, uint8_t* p, int pitch, unsigned w, unsigned h) {
	if (f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5) {
		unsigned bw = (w + 3) / 4, bh = (h + 3) / 4, size = f == D3DFMT_DXT1 ? 8 : 16;
		for (unsigned y = 0; y < bh; y++)
			for (unsigned x = 0; x < bw; x++) {
				uint8_t* blk = p + y * pitch + x * size;
				RecolourBlock(hero, f == D3DFMT_DXT1 ? blk : blk + 8, f == D3DFMT_DXT1);
			}
		return true;
	}
	for (unsigned y = 0; y < h; y++) {
		uint8_t* row = p + y * pitch;
		for (unsigned x = 0; x < w; x++) {
			float r, g, b;
			if (f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8) {
				uint8_t* px = row + 4 * x;   // B G R A
				r = px[2] / 255.0f; g = px[1] / 255.0f; b = px[0] / 255.0f;
				Recolour(hero, r, g, b);
				px[2] = (uint8_t)(r * 255.0f + 0.5f); px[1] = (uint8_t)(g * 255.0f + 0.5f); px[0] = (uint8_t)(b * 255.0f + 0.5f);
				continue;
			}
			uint16_t* px = (uint16_t*)(row + 2 * x);
			if (f == D3DFMT_R5G6B5) { *px = Recolour565(hero, *px); continue; }
			if (f == D3DFMT_A1R5G5B5 || f == D3DFMT_X1R5G5B5) {
				r = ((*px >> 10) & 31) / 31.0f; g = ((*px >> 5) & 31) / 31.0f; b = (*px & 31) / 31.0f;
				Recolour(hero, r, g, b);
				*px = (uint16_t)((*px & 0x8000) | (int)(r * 31.0f + 0.5f) << 10 | (int)(g * 31.0f + 0.5f) << 5 | (int)(b * 31.0f + 0.5f));
				continue;
			}
			if (f == D3DFMT_A4R4G4B4) {
				r = ((*px >> 8) & 15) / 15.0f; g = ((*px >> 4) & 15) / 15.0f; b = (*px & 15) / 15.0f;
				Recolour(hero, r, g, b);
				*px = (uint16_t)((*px & 0xF000) | (int)(r * 15.0f + 0.5f) << 8 | (int)(g * 15.0f + 0.5f) << 4 | (int)(b * 15.0f + 0.5f));
				continue;
			}
			return false;
		}
	}
	return true;
}

IDirect3DTexture9* MakeCopy(IDirect3DDevice9* dev, IDirect3DBaseTexture9* base, int hero) {
	static int s_told = 0;
	if (base->GetType() != D3DRTYPE_TEXTURE) return nullptr;
	IDirect3DTexture9* src = static_cast<IDirect3DTexture9*>(base);
	D3DSURFACE_DESC d;
	if (FAILED(src->GetLevelDesc(0, &d))) return nullptr;
	DWORD levels = src->GetLevelCount();
	if (d.Pool != D3DPOOL_MANAGED && d.Pool != D3DPOOL_SYSTEMMEM) {
		if (s_told++ < 8) Log("tint: texture %p not lockable (pool %d) -- as it is", base, (int)d.Pool);
		return nullptr;
	}
	IDirect3DTexture9* dst = nullptr;
	if (FAILED(dev->CreateTexture(d.Width, d.Height, levels, 0, d.Format, D3DPOOL_MANAGED, &dst, nullptr)) || !dst) return nullptr;
	bool ok = true;
	for (DWORD l = 0; l < levels && ok; l++) {
		D3DSURFACE_DESC ld;
		D3DLOCKED_RECT rs, rd;
		if (FAILED(src->GetLevelDesc(l, &ld))) { ok = false; break; }
		if (FAILED(src->LockRect(l, &rs, nullptr, D3DLOCK_READONLY))) { ok = false; break; }
		if (FAILED(dst->LockRect(l, &rd, nullptr, 0))) { src->UnlockRect(l); ok = false; break; }
		bool blocks = d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5;
		unsigned rows = blocks ? (ld.Height + 3) / 4 : ld.Height;
		unsigned rowBytes = (unsigned)(rs.Pitch < rd.Pitch ? rs.Pitch : rd.Pitch);
		for (unsigned y = 0; y < rows; y++) memcpy((uint8_t*)rd.pBits + y * rd.Pitch, (uint8_t*)rs.pBits + y * rs.Pitch, rowBytes);
		ok = RecolourLevel(hero, d.Format, (uint8_t*)rd.pBits, rd.Pitch, ld.Width, ld.Height);
		dst->UnlockRect(l);
		src->UnlockRect(l);
	}
	if (s_told++ < 8)
		Log("tint: %s's copy of texture %p (%ux%u, format %08X, %lu levels) %s", hero ? "Obelix" : "Asterix", base, d.Width,
		    d.Height, (unsigned)d.Format, levels, ok ? "made" : "not made (its format) -- as it is");
	if (!ok) { dst->Release(); return nullptr; }
	return dst;
}

bool Known(IDirect3DBaseTexture9* t) {
	for (int i = 0; i < g_nKnown; i++) if (g_known[i] == t) return true;
	return false;
}

HRESULT __stdcall SetTextureHook(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex) {
	if (g_hero < 0 || stage != 0 || !tex) return g_origSetTexture(dev, stage, tex);
	if (g_learn && !Known(tex) && g_nKnown < 16) g_known[g_nKnown++] = tex;
	if (!Known(tex)) return g_origSetTexture(dev, stage, tex);
	Copy* c = nullptr;
	for (int i = 0; i < g_nCopies && !c; i++) if (g_copies[i].src == tex && g_copies[i].hero == g_hero) c = &g_copies[i];
	if (!c && g_nCopies < 32) {
		c = &g_copies[g_nCopies++];
		c->src = tex;
		c->hero = g_hero;
		c->dst = MakeCopy(dev, tex, g_hero);
	}
	return g_origSetTexture(dev, stage, c && c->dst ? c->dst : tex);
}

IDirect3DDevice9* Device() { return *(IDirect3DDevice9**)kD3DDevicePtr; }

// RenderWare's stage 0 forgotten (and the device's): its next draw binds again.
void ForgetStage0() {
	IDirect3DDevice9* dev = Device();
	*(void**)kStage0Cache = nullptr;
	if (dev && g_origSetTexture) g_origSetTexture(dev, 0, nullptr);
}

bool Hook() {
	IDirect3DDevice9* dev = Device();
	if (!dev) return false;
	void** vt = *(void***)dev;
	if (vt == g_hookedVtbl && vt[kSetTextureSlot] == (void*)SetTextureHook) return true;
	DWORD old;
	if (!VirtualProtect(&vt[kSetTextureSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
	if (vt[kSetTextureSlot] != (void*)SetTextureHook) {
		g_origSetTexture = (SetTextureFn)vt[kSetTextureSlot];
		vt[kSetTextureSlot] = (void*)SetTextureHook;
	}
	VirtualProtect(&vt[kSetTextureSlot], sizeof(void*), old, &old);
	g_hookedVtbl = vt;
	Log("tint: the device's SetTexture wrapped (vtable %p, was %p) -- the health rows' colours", vt, (void*)g_origSetTexture);
	return true;
}

} // namespace

void TintBegin(int hero, bool learn) {
	if ((hero != 0 && hero != 1) || !Hook()) return;
	g_hero = hero;
	g_learn = learn;
	ForgetStage0();
}

void TintEnd() {
	if (g_hero < 0) return;
	g_hero = -1;
	g_learn = false;
	ForgetStage0();
}

void TintForget() {
	for (int i = 0; i < g_nCopies; i++) if (g_copies[i].dst) g_copies[i].dst->Release();
	g_nCopies = 0;
	g_nKnown = 0;
}
