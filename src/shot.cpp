// Window capture to PNG for autotest. Captures the game window's own content via
// PrintWindow(PW_RENDERFULLCONTENT), so whatever else is on the desktop never ends
// up in a screenshot.

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <d3d9.h>
#include <cstdio>
#include <cwchar>

#include "log.h"

#pragma comment(lib, "gdiplus.lib")

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

static ULONG_PTR g_gdiplusToken = 0;

static bool PngEncoder(CLSID* out) {
	UINT num = 0, size = 0;
	Gdiplus::GetImageEncodersSize(&num, &size);
	if (!size) return false;
	Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)malloc(size);
	if (!info) return false;
	Gdiplus::GetImageEncoders(num, size, info);
	bool found = false;
	for (UINT i = 0; i < num; i++) {
		if (wcscmp(info[i].MimeType, L"image/png") == 0) { *out = info[i].Clsid; found = true; break; }
	}
	free(info);
	return found;
}

// Fullscreen (exclusive) D3D9 shows nothing to PrintWindow -- the shots came out
// black. There the back buffer (the last frame, before the next one is drawn) is
// copied instead: resolved into a plain render target (the anti-aliased buffer is
// multisampled), read back to system memory and put into the bitmap. Called on
// the game's thread (the autotest steps), the one that drives the device.
static const uintptr_t kD3DDevicePtr = 0x668978;   // IDirect3DDevice9* (RenderWare's)

static bool DeviceFullscreen(IDirect3DDevice9* dev) {
	IDirect3DSwapChain9* sc = nullptr;
	if (FAILED(dev->GetSwapChain(0, &sc)) || !sc) return false;
	D3DPRESENT_PARAMETERS pp = {};
	bool full = SUCCEEDED(sc->GetPresentParameters(&pp)) && !pp.Windowed;
	sc->Release();
	return full;
}

static bool BackBufferToDc(IDirect3DDevice9* dev, HDC dc, int w, int h) {
	IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
	bool ok = false;
	D3DSURFACE_DESC d = {};
	if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && SUCCEEDED(bb->GetDesc(&d)) &&
	    (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
	    SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
	    SUCCEEDED(dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
	    SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
	    SUCCEEDED(dev->GetRenderTargetData(rt, sys))) {
		D3DLOCKED_RECT lr;
		if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
			BITMAPINFO bi = {};
			bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
			bi.bmiHeader.biWidth = lr.Pitch / 4;
			bi.bmiHeader.biHeight = -(int)d.Height;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			SetStretchBltMode(dc, HALFTONE);
			ok = StretchDIBits(dc, 0, 0, w, h, 0, 0, d.Width, d.Height, lr.pBits, &bi, DIB_RGB_COLORS, SRCCOPY) != 0;
			sys->UnlockRect();
		}
	}
	if (sys) sys->Release();
	if (rt) rt->Release();
	if (bb) bb->Release();
	return ok;
}

bool CaptureWindowPng(void* hwndPtr, const char* path, int scaleDiv) {
	HWND hwnd = (HWND)hwndPtr;
	if (!IsWindow(hwnd)) return false;
	if (!g_gdiplusToken) {
		Gdiplus::GdiplusStartupInput in;
		if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &in, nullptr) != Gdiplus::Ok) return false;
	}
	RECT rc;
	GetClientRect(hwnd, &rc);
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (w <= 0 || h <= 0) return false;
	if (scaleDiv < 1) scaleDiv = 1;
	int ow = w / scaleDiv, oh = h / scaleDiv;

	HDC wnd = GetDC(hwnd);
	HDC full = CreateCompatibleDC(wnd);
	HBITMAP fullBmp = CreateCompatibleBitmap(wnd, w, h);
	HGDIOBJ oldFull = SelectObject(full, fullBmp);
	IDirect3DDevice9* dev = *(IDirect3DDevice9**)kD3DDevicePtr;
	BOOL ok = FALSE;
	if (dev && DeviceFullscreen(dev)) {
		ok = BackBufferToDc(dev, full, w, h);
		static int told = 0;
		if (told++ < 1) Log("autotest: fullscreen -- screenshots from the back buffer (%s)", ok ? "ok" : "FAILED");
	}
	if (!ok) ok = PrintWindow(hwnd, full, PW_CLIENTONLY | PW_RENDERFULLCONTENT);

	HDC smallDc = CreateCompatibleDC(wnd);
	HBITMAP smallBmp = CreateCompatibleBitmap(wnd, ow, oh);
	HGDIOBJ oldSmall = SelectObject(smallDc, smallBmp);
	SetStretchBltMode(smallDc, HALFTONE);
	StretchBlt(smallDc, 0, 0, ow, oh, full, 0, 0, w, h, SRCCOPY);
	SelectObject(smallDc, oldSmall);
	SelectObject(full, oldFull);

	bool saved = false;
	CLSID png;
	if (ok && PngEncoder(&png)) {
		wchar_t wpath[MAX_PATH];
		MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
		Gdiplus::Bitmap bmp(smallBmp, nullptr);
		saved = bmp.Save(wpath, &png, nullptr) == Gdiplus::Ok;
	}
	DeleteObject(smallBmp);
	DeleteDC(smallDc);
	DeleteObject(fullBmp);
	DeleteDC(full);
	ReleaseDC(hwnd, wnd);
	Log("autotest: screenshot %s %s (%dx%d)", path, saved ? "saved" : "FAILED", ow, oh);
	return saved;
}
