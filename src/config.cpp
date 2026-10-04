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

void CoopConfig::Load(const char* iniPath) {
	// GetPrivateProfileString wants a full path or it searches the Windows dir.
	char full[MAX_PATH];
	GetFullPathNameA(iniPath, MAX_PATH, full, nullptr);

	enabled     = ReadBool("coop", "enabled", enabled, full);
	splitScreen = ReadBool("coop", "split_screen", splitScreen, full);
	p1Pad       = GetPrivateProfileIntA("coop", "p1_pad", p1Pad, full);
	dropHoldMs  = GetPrivateProfileIntA("coop", "drop_hold_ms", dropHoldMs, full);

	verbose     = ReadBool("debug", "verbose", verbose, full);
	forceLevel  = GetPrivateProfileIntA("debug", "force_level", forceLevel, full);
	watchdog    = ReadBool("debug", "watchdog", watchdog, full);
	vtProfile   = ReadBool("debug", "vtable_profile", vtProfile, full);
	renderProbe = ReadBool("debug", "render_probe", renderProbe, full);
	splitTest   = ReadBool("debug", "split_test", splitTest, full);

	char buf[64] = {};
	GetPrivateProfileStringA("coop", "split_mode", "vertical", buf, sizeof(buf), full);
	splitMode = (buf[0] == 'v' || buf[0] == 'V') ? SPLIT_VERTICAL : SPLIT_HORIZONTAL;

	GetPrivateProfileStringA("coop", "deadzone", "0.24", buf, sizeof(buf), full);
	deadzone = (float)atof(buf);
	if (deadzone < 0.0f || deadzone > 0.9f) deadzone = 0.24f;

	Log("config: enabled=%d p1Pad=%d split=%d mode=%s deadzone=%.2f forceLevel=%d watchdog=%d "
		"splitTest=%d verbose=%d",
		enabled, p1Pad, splitScreen, splitMode == SPLIT_VERTICAL ? "vertical" : "horizontal",
		deadzone, forceLevel, watchdog, splitTest, verbose);
}
