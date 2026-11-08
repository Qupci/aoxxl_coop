#include "cutskip.h"
#include "coop.h"
#include "hints.h"
#include "icons.h"
#include "kal.h"
#include "log.h"
#include "pads.h"

#include <windows.h>
#include <cwchar>

namespace {

const DWORD kHoldMs = 3000;      // held this long, the scene is skipped
const DWORD kShowMs = 5000;      // the prompt stays this long after the last press
const DWORD kDoneMs = 400;       // the full bar stays this long (its fade-out: hints.cpp)

struct State {
	char*  scene = nullptr;      // the playing skippable scene (the first one)
	bool   armed = false;        // the button let go since the scene began (a jump's A held into it does not count)
	bool   held = false;
	DWORD  heldMs = 0;           // how long held
	DWORD  lastAt = 0;           // the last frame
	DWORD  inputAt = 0;          // the last press or hold (the prompt shows for kShowMs after it)
	bool   fire = false;         // the next read of the skip action is answered 1
	DWORD  doneAt = 0;           // the hold completed
	int    player = 0;           // whose box shows the prompt
	bool   keyboard = false;     // the prompt shows Enter, else pad A
	bool   shown = false;
	int    logs = 0;
} g;

bool GameHasFocus() {
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg) GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

int LevelNow() {
	CKYellowPages* yp = YellowPages();
	return (yp && yp->gameManager) ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
}

// The first playing scene the cinematic service would skip on its button
// (flag 0x1000, not of kind 0x80, +0x30 not negative) that is skipped by a
// hold: every one, but in level 0 only its intro (scene 0, made skippable by
// skip_cutscenes) -- its film (the game's own skip) and the title (skipped as
// "Press Validate" is pressed) on a press, as before.
char* SkippableScene() {
	int level = LevelNow();
	if (level < 0) return nullptr;
	CKYellowPages* yp = YellowPages();
	char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
	char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
	unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
	for (unsigned i = 0; scenes && i < n && i < 512; i++) {
		char* sc = scenes[i];
		if (!sc) continue;
		uint16_t f = *(uint16_t*)(sc + 0x20);
		if (!((f & 1) && (f & 0x1000) && (f & 0x180) != 0x80 && *(int8_t*)(sc + 0x30) >= 0)) continue;
		if (level > 0 || (i == 0 && CoopSkipAdded(sc))) return sc;
	}
	return nullptr;
}

void ShowPrompt(DWORD ms, float progress) {
	wchar_t g1 = 0;
	bool pressed = g.held || g.doneAt;
	if (g.keyboard) g1 = IconKeyGlyph(VK_RETURN, !pressed);
	else g1 = IconGlyph(pressed ? "pad_a" : "pad_a_unpressed");
	if (!g1 && !g.keyboard && !pressed) g1 = IconGlyph("pad_a");
	wchar_t text[64];
	if (g1) swprintf(text, 64, L"Hold %c to skip", g1);
	else swprintf(text, 64, L"Hold %s to skip", g.keyboard ? L"Enter" : L"A");
	HintShow(HINT_SKIP, g.player, text, ms);
	HintProgress(HINT_SKIP, g.player, progress);
	g.shown = true;
}

void Hide() {
	if (g.shown) HintHide(HINT_SKIP);
	g.shown = false;
}

} // namespace

uint8_t CutSkipRead(uint8_t v) {
	if (!SkippableScene()) return v;   // none skipped by a hold: the game's own (level 0's film on a press)
	if (g.fire) {
		g.fire = false;
		Log("cutskip: scene %p skipped (held %.1f s)", g.scene, g.heldMs / 1000.0f);
		return 1;
	}
	return 0;
}

void CutSkipFrame() {
	DWORD now = GetTickCount();
	DWORD dtMs = g.lastAt && now - g.lastAt < 200 ? now - g.lastAt : 0;
	g.lastAt = now;
	char* sc = SkippableScene();
	if (sc != g.scene) {
		if (g.logs < 40 && (sc || g.scene)) { g.logs++; Log("cutskip: skippable scene %p -> %p", g.scene, sc); }
		g.scene = sc;
		g.armed = false;
		g.held = false;
		g.heldMs = 0;
		g.fire = false;
		g.inputAt = 0;
		if (!g.doneAt) Hide();   // (a skip's full bar stays for its fade-out)
	}
	if (g.doneAt) {
		if (now - g.doneAt < kDoneMs) ShowPrompt(kDoneMs - (now - g.doneAt), 1.0f);
		else { g.doneAt = 0; Hide(); }
		return;
	}
	if (!sc) return;
	// The skip button held: Enter, any pad's A (player 2's: its box). (The
	// game's own value is a press's edge, from a combined action (value 44) with
	// no hold time of its own.)
	bool enter = GameHasFocus() && (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
	int p2pad = CoopP2Joined() ? CoopP2Pad() : -1;
	bool p2 = false, pad = false;
	for (int i = 0; i < kXInputPads; i++) {
		if (!(PadGet(i).buttons & PAD_A)) continue;
		if (i == p2pad) p2 = true;
		else pad = true;
	}
	bool held = enter || pad || p2;
	if (!g.armed) {
		if (!held) g.armed = true;
		return;
	}
	if (held && !g.held) {
		g.keyboard = enter && !pad && !p2;
		g.player = p2 && !pad && !enter ? 1 : 0;
		if (g.logs < 40) {
			g.logs++;
			Log("cutskip: skip button down (%s, player %d's box)", g.keyboard ? "Enter" : "pad A", g.player + 1);
		}
	}
	g.held = held;
	if (held) {
		g.inputAt = now;
		g.heldMs += dtMs;
		if (g.heldMs >= kHoldMs) {
			g.doneAt = now;
			g.fire = true;
			ShowPrompt(kDoneMs, 1.0f);
			return;
		}
	} else {
		g.heldMs = 0;
	}
	if (g.inputAt && now - g.inputAt < kShowMs) ShowPrompt(kShowMs - (now - g.inputAt), g.heldMs / (float)kHoldMs);
	else Hide();
}
