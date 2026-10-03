// Window capture to PNG for autotest. Captures the game window's own content via
// PrintWindow(PW_RENDERFULLCONTENT), so whatever else is on the desktop never ends
// up in a screenshot.

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
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
	BOOL ok = PrintWindow(hwnd, full, PW_CLIENTONLY | PW_RENDERFULLCONTENT);

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
