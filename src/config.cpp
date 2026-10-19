#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cctype>
#include <cstdlib>
#include <cstring>

CoopConfig g_config;

static bool ReadBool(const char* section, const char* key, bool def, const char* path) {
	char buf[64] = {};
	GetPrivateProfileStringA(section, key, def ? "on" : "off", buf, sizeof(buf), path);
	for (char* p = buf; *p; ++p) *p = (char)tolower((unsigned char)*p);
	return !strcmp(buf, "on") || !strcmp(buf, "1") ||
	       !strcmp(buf, "yes") || !strcmp(buf, "true");
}

// "W", "SPACE", "ENTER", "UP", "F5", "NUM4", "0x26"... -> virtual-key code (0 = none).
static int ParseKey(const char* t) {
	char k[32] = {};
	size_t n = 0;
	for (; t[n] && n < sizeof(k) - 1; n++) k[n] = (char)toupper((unsigned char)t[n]);
	while (n && (k[n - 1] == ' ' || k[n - 1] == '	')) k[--n] = 0;
	const char* s = k;
	while (*s == ' ') s++;
	if (!*s) return 0;
	if (s[0] == '0' && s[1] == 'X') return (int)strtol(s, nullptr, 16);
	if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
	static const struct { const char* name; int vk; } kNames[] = {
		{ "SPACE", 0x20 }, { "ENTER", 0x0D }, { "RETURN", 0x0D }, { "BACKSPACE", 0x08 }, { "TAB", 0x09 },
		{ "ESC", 0x1B }, { "SHIFT", 0x10 }, { "LSHIFT", 0xA0 }, { "RSHIFT", 0xA1 }, { "CTRL", 0x11 },
		{ "LCTRL", 0xA2 }, { "RCTRL", 0xA3 }, { "ALT", 0x12 }, { "UP", 0x26 }, { "DOWN", 0x28 },
		{ "LEFT", 0x25 }, { "RIGHT", 0x27 }, { "INSERT", 0x2D }, { "DELETE", 0x2E }, { "HOME", 0x24 },
		{ "END", 0x23 }, { "PAGEUP", 0x21 }, { "PAGEDOWN", 0x22 }, { "COMMA", 0xBC }, { "PERIOD", 0xBE },
		{ "SEMICOLON", 0xBA }, { "SLASH", 0xBF }, { "NUMDIV", 0x6F }, { "NUMMUL", 0x6A },
		{ "NUMMINUS", 0x6D }, { "NUMPLUS", 0x6B }, { "NUMDOT", 0x6E }, { "NONE", 0 },
	};
	for (const auto& e : kNames) if (!strcmp(s, e.name)) return e.vk;
	if (s[0] == 'F' && s[1] >= '1' && s[1] <= '9') { int f = atoi(s + 1); if (f >= 1 && f <= 12) return 0x70 + f - 1; }
	if (!strncmp(s, "NUM", 3) && s[3] >= '0' && s[3] <= '9' && !s[4]) return 0x60 + (s[3] - '0');
	return 0;
}

void CoopConfig::Load(const char* iniPath) {
	// GetPrivateProfileString wants a full path or it searches the Windows dir.
	char full[MAX_PATH];
	GetFullPathNameA(iniPath, MAX_PATH, full, nullptr);

	enabled     = ReadBool("coop", "enabled", enabled, full);
	splitScreen = ReadBool("coop", "split_screen", splitScreen, full);
	{
		char cb[32] = {};
		GetPrivateProfileStringA("coop", "camera", "game", cb, sizeof(cb), full);
		gameCamera = !(cb[0] == 'm' || cb[0] == 'M');
		GetPrivateProfileStringA("coop", "split_fov", "15", cb, sizeof(cb), full);
		splitFov = (float)atof(cb);
		if (splitFov < -30.0f) splitFov = -30.0f;
		if (splitFov > 60.0f) splitFov = 60.0f;
	}
	{
		char pb[32] = {};
		GetPrivateProfileStringA("coop", "p1_pad", "", pb, sizeof(pb), full);
		if (pb[0] == 'k' || pb[0] == 'K') p1Pad = 4;   // the keyboard (pads.h kKeyboardPad)
		else if (pb[0]) p1Pad = atoi(pb);
	}
	dropHoldMs  = GetPrivateProfileIntA("coop", "drop_hold_ms", dropHoldMs, full);
	{
		char hb[32] = {};
		GetPrivateProfileStringA("coop", "p2_hero", "obelix", hb, sizeof(hb), full);
		p2Hero = (hb[0] == 'a' || hb[0] == 'A') ? 0 : (hb[0] == 'o' && hb[1] == 'b') || (hb[0] == 'O' && hb[1] == 'b') ? 1 : -1;
	}

	safeSequence = ReadBool("coop", "safe_sequence", safeSequence, full);
	individual = ReadBool("coop", "individual_heroes", individual, full);
	obelixPotion = ReadBool("coop", "obelix_potion", obelixPotion, full);
	sectorCatchUpMs = GetPrivateProfileIntA("coop", "sector_catch_up_ms", sectorCatchUpMs, full);
	if (sectorCatchUpMs < 0) sectorCatchUpMs = 0;

	verbose     = ReadBool("debug", "verbose", verbose, full);
	forceLevel  = GetPrivateProfileIntA("debug", "force_level", forceLevel, full);
	watchdog    = ReadBool("debug", "watchdog", watchdog, full);
	vtProfile   = ReadBool("debug", "vtable_profile", vtProfile, full);
	renderProbe = ReadBool("debug", "render_probe", renderProbe, full);
	splitTest   = ReadBool("debug", "split_test", splitTest, full);
	cheats      = ReadBool("debug", "cheats", cheats, full);
	skipCutscenes = ReadBool("debug", "skip_cutscenes", skipCutscenes, full);

	char buf[64] = {};
	GetPrivateProfileStringA("coop", "split_mode", "vertical", buf, sizeof(buf), full);
	if (const char* env = getenv("XXL_SPLITMODE")) lstrcpynA(buf, env, sizeof(buf));   // autotests: either split
	splitMode = (buf[0] == 'v' || buf[0] == 'V') ? SPLIT_VERTICAL : SPLIT_HORIZONTAL;

	keyboard = ReadBool("keyboard", "enabled", keyboard, full);
	static const char* const kKeyNames[KEY_COUNT] = {
		"move_up", "move_down", "move_left", "move_right", "look_up", "look_down", "look_left",
		"look_right", "jump", "attack", "grab", "dash", "lb", "rb", "start", "back", "center_camera" };
	for (int i = 0; i < KEY_COUNT; i++) {
		char kb[32] = {};
		GetPrivateProfileStringA("keyboard", kKeyNames[i], "", kb, sizeof(kb), full);
		if (kb[0]) keys[i] = ParseKey(kb);
	}

	auto readFactor = [&](const char* key, float def) {
		char fb[32] = {};
		GetPrivateProfileStringA("experimental", key, "", fb, sizeof(fb), full);
		if (!fb[0]) return def;
		if (!_stricmp(fb, "off")) return 1.0f;
		float f = (float)atof(fb);
		return f < 0.1f ? 1.0f : f > 10.0f ? 10.0f : f;
	};
	squadAliveFactor = readFactor("squad_romans_at_once", squadAliveFactor);
	squadCountFactor = readFactor("squad_romans_to_defeat", squadCountFactor);
	priceFactor      = readFactor("merchant_prices", priceFactor);
	comboFactor      = readFactor("combo_bar_fill", comboFactor);
	comboDrainFactor = readFactor("combo_bar_drain", comboDrainFactor);
	comboFullDrainFactor = readFactor("combo_bar_full_drain", comboFullDrainFactor);

	GetPrivateProfileStringA("coop", "deadzone", "0.24", buf, sizeof(buf), full);
	deadzone = (float)atof(buf);
	if (deadzone < 0.0f || deadzone > 0.9f) deadzone = 0.24f;

	Log("config: enabled=%d p1Pad=%d split=%d mode=%s deadzone=%.2f forceLevel=%d watchdog=%d "
		"splitTest=%d verbose=%d keyboard=%d cheats=%d skipCutscenes=%d safeSequence=%d sectorCatchUpMs=%d individual=%d "
		"experimental: squad at once x%.2f, to defeat x%.2f, prices x%.2f, combo bar x%.2f drain x%.2f / x%.2f",
		enabled, p1Pad, splitScreen, splitMode == SPLIT_VERTICAL ? "vertical" : "horizontal",
		deadzone, forceLevel, watchdog, splitTest, verbose, keyboard, cheats, skipCutscenes, safeSequence, sectorCatchUpMs, individual,
		squadAliveFactor, squadCountFactor, priceFactor, comboFactor, comboDrainFactor, comboFullDrainFactor);
}
