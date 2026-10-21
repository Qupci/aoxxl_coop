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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace {

const uintptr_t kYellowPages = 0x6621F4;   // CKYellowPages*
const uintptr_t kGlyphScale  = 0x623C64;   // float 0.75 in .rdata

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
	for (;;) {
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
