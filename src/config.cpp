#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

CoopConfig g_config;

// "key = value" in [section] of the ini (WritePrivateProfileString writes
// "key=value"): the key's line replaced where it is, else added after the
// section's last line that is not blank, the section added at the end if it
// is missing. The file's line ends kept.
void IniWrite(const char* section, const char* key, const char* value) {
	const char* path = g_config.iniPath;
	if (!path[0]) return;
	FILE* f = fopen(path, "rb");
	std::string text;
	if (f) {
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
		fclose(f);
	}
	const char* nl = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
	std::string line = std::string(key) + " = " + value;
	// The lines, without their ends.
	std::vector<std::string> lines;
	for (size_t at = 0; at <= text.size();) {
		size_t e = text.find('\n', at);
		if (e == std::string::npos) { if (at < text.size()) lines.push_back(text.substr(at)); break; }
		std::string l = text.substr(at, e - at);
		if (!l.empty() && l.back() == '\r') l.pop_back();
		lines.push_back(l);
		at = e + 1;
	}
	auto trim = [](std::string s) {
		size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
		return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
	};
	auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	std::string want = lower(std::string("[") + section + "]");
	int sec = -1, last = -1, found = -1;
	for (int i = 0; i < (int)lines.size(); i++) {
		std::string t = trim(lines[i]);
		if (!t.empty() && t[0] == '[') {
			if (sec >= 0) break;
			if (lower(t) == want) { sec = i; last = i; }
			continue;
		}
		if (sec < 0) continue;
		if (!t.empty()) last = i;
		if (t.empty() || t[0] == '#' || t[0] == ';') continue;
		size_t eq = t.find('=');
		if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(key)) { found = i; break; }
	}
	if (found >= 0) lines[found] = line;
	else if (sec >= 0) lines.insert(lines.begin() + last + 1, line);
	else {
		if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
		lines.push_back(std::string("[") + section + "]");
		lines.push_back(line);
	}
	std::string out;
	for (size_t i = 0; i < lines.size(); i++) { out += lines[i]; out += nl; }
	f = fopen(path, "wb");
	if (!f) return;
	fwrite(out.data(), 1, out.size(), f);
	fclose(f);
}

const char* const CoopConfig::kKeyNames[CoopConfig::KEY_COUNT] = {
	"move_up", "move_down", "move_left", "move_right", "look_up", "look_down", "look_left",
	"look_right", "jump", "attack", "grab", "dash", "lb", "rb", "start", "back", "center_camera", "hud", "switch_hero", "cheat_modifier" };
const uint16_t CoopConfig::kP2Default[CoopConfig::P2B_COUNT] = {
	0x1000, 0x4000, 0x2000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0002, 0x0080, 0x0001 };
const char* const CoopConfig::kP2PadNames[CoopConfig::P2B_COUNT] = {
	"jump", "attack", "grab", "dash", "lb", "rb", "menu", "hud", "center_camera", "switch_hero" };

// The pad buttons by their ini names (XInput bits).
const CoopConfig::PadButton CoopConfig::kPadButtons[CoopConfig::kNumPadButtons] = {
	{ "dpad_up", 0x0001 }, { "dpad_down", 0x0002 }, { "dpad_left", 0x0004 }, { "dpad_right", 0x0008 },
	{ "start", 0x0010 }, { "back", 0x0020 }, { "l3", 0x0040 }, { "r3", 0x0080 }, { "lb", 0x0100 }, { "rb", 0x0200 },
	{ "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 }, { "y", 0x8000 }, { "none", 0 },
};

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
	strncpy(this->iniPath, full, sizeof(this->iniPath) - 1);
	invertCamX = ReadBool("controls", "invert_camera_x", invertCamX, full);
	invertCamY = ReadBool("controls", "invert_camera_y", invertCamY, full);
	{
		char db[32] = {};
		GetPrivateProfileStringA("controls", "p1_deadzone", "0", db, sizeof(db), full);
		p1Deadzone = (float)atof(db);
		if (p1Deadzone < 0.0f) p1Deadzone = 0.0f;
		if (p1Deadzone > 0.9f) p1Deadzone = 0.9f;
	}

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
		if (const char* env = getenv("XXL_P2HERO")) lstrcpynA(hb, env, sizeof(hb));   // autotests: player 2's hero
		p2Hero = (hb[0] == 'a' || hb[0] == 'A') ? 0 : (hb[0] == 'o' && hb[1] == 'b') || (hb[0] == 'O' && hb[1] == 'b') ? 1 : -1;
	}

	safeSequence = ReadBool("coop", "safe_sequence", safeSequence, full);
	individual = ReadBool("coop", "individual_heroes", individual, full);
	romansGuardDowned = ReadBool("coop", "romans_guard_downed", romansGuardDowned, full);
	hintJoin   = ReadBool("hints", "hint_join", hintJoin, full);
	hintPass   = ReadBool("hints", "hint_pass_roman", hintPass, full);
	hintCombos = ReadBool("hints", "hint_combos", hintCombos, full);
	hintMachineGun = ReadBool("hints", "hint_machine_gun", hintMachineGun, full);
	{
		// Seconds; `env` (autotests) over the ini; "off" = 0, "auto" = -1 where allowed.
		auto readSeconds = [&](const char* key, const char* env, float def, float lo, float hi, bool autoOk) {
			char sb[32] = {};
			GetPrivateProfileStringA("coop", key, "", sb, sizeof(sb), full);
			if (const char* e = getenv(env)) lstrcpynA(sb, e, sizeof(sb));
			if (!sb[0]) return def;
			if (!_stricmp(sb, "off") || !_stricmp(sb, "none")) return autoOk ? 0.0f : def;
			if (!_stricmp(sb, "auto")) return autoOk ? -1.0f : def;
			float f = (float)atof(sb);
			if (autoOk && f <= 0.0f) return 0.0f;
			return f < lo ? lo : f > hi ? hi : f;
		};
		reviveHold = readSeconds("revive_hold_seconds", "XXL_REVIVEHOLD", reviveHold, 0.1f, 60.0f, false);
		// (a share, 0.05 to 1, read the same way)
		reviveHoldCatapultWindow = readSeconds("revive_hold_catapult_window", "XXL_REVIVECATWINDOW", reviveHoldCatapultWindow,
		                                       0.05f, 1.0f, false);
		reviveLimit = readSeconds("revive_time_limit", "XXL_REVIVELIMIT", reviveLimit, 1.0f, 3600.0f, true);
		reviveGrace = readSeconds("revive_time_limit_grace", "XXL_REVIVEGRACE", reviveGrace, 0.0f, 3600.0f, false);
	}
	noLock = ReadBool("coop", "nolock", noLock, full);
	noLockExceptCorkscrews = ReadBool("coop", "nolock_except_corkscrews", noLockExceptCorkscrews, full);
	noLockExceptBeams = ReadBool("coop", "nolock_except_rotarybeams", noLockExceptBeams, full);
	carRiderLeads = ReadBool("coop", "cable_car_rider_leads", carRiderLeads, full);
	romansInView = ReadBool("coop", "romans_spawn_in_view", romansInView, full);
	markerTeleport = ReadBool("coop", "walk_to_marker_teleport", markerTeleport, full);
	noLockRomans = ReadBool("coop_difficulty.experimental", "nolock_romans", noLockRomans, full);
	noLockRomansAnyMarkers = ReadBool("coop_difficulty.experimental", "nolock_romans_any_markers", noLockRomansAnyMarkers, full);
	if (getenv("XXL_NOLOCK")) noLock = atoi(getenv("XXL_NOLOCK")) != 0;   // autotests
	if (getenv("XXL_NOLOCKCORK")) noLockExceptCorkscrews = atoi(getenv("XXL_NOLOCKCORK")) != 0;
	if (getenv("XXL_NOLOCKBEAMS")) noLockExceptBeams = atoi(getenv("XXL_NOLOCKBEAMS")) != 0;
	if (getenv("XXL_RIDERLEADS")) carRiderLeads = atoi(getenv("XXL_RIDERLEADS")) != 0;
	if (getenv("XXL_ROMANSINVIEW")) romansInView = atoi(getenv("XXL_ROMANSINVIEW")) != 0;
	if (getenv("XXL_MARKERTP")) markerTeleport = atoi(getenv("XXL_MARKERTP")) != 0;
	if (getenv("XXL_NOLOCKROMANS")) noLockRomans = atoi(getenv("XXL_NOLOCKROMANS")) != 0;
	if (getenv("XXL_NOLOCKROMANSANY")) noLockRomansAnyMarkers = atoi(getenv("XXL_NOLOCKROMANSANY")) != 0;
	obelixPotion = ReadBool("coop", "obelix_potion", obelixPotion, full);
	if (getenv("XXL_OBELIXPOTION")) obelixPotion = atoi(getenv("XXL_OBELIXPOTION")) != 0;
	spIsolation    = ReadBool("single_player", "sp_isolation", spIsolation, full);
	spComboHints   = ReadBool("single_player", "sp_combo_hints", spComboHints, full);
	spCoopIcons    = ReadBool("single_player", "sp_coop_icons", spCoopIcons, full);
	spHeroSwitch   = ReadBool("single_player", "sp_hero_switch", spHeroSwitch, full);
	spSwitchLock   = ReadBool("single_player", "sp_hero_switch_lock", spSwitchLock, full);
	spSecondChance = ReadBool("single_player", "sp_second_chance", spSecondChance, full);
	spSidekickView = ReadBool("single_player", "sp_sidekick_view", spSidekickView, full);
	{
		char sb[32] = {};
		GetPrivateProfileStringA("single_player", "sp_hero_switch_button", "dpad_up", sb, sizeof(sb), full);
		for (int i = 0; i < CoopConfig::kNumPadButtons; i++)
			if (!_stricmp(sb, CoopConfig::kPadButtons[i].name)) spSwitchButton = CoopConfig::kPadButtons[i].bit;
		GetPrivateProfileStringA("single_player", "sp_hero_switch_key", "", sb, sizeof(sb), full);
		if (sb[0]) spSwitchKey = ParseKey(sb);
	}
	if (getenv("XXL_SPISOLATION")) spIsolation = atoi(getenv("XXL_SPISOLATION")) != 0;   // autotests
	if (getenv("XXL_SPCOMBOHINTS")) spComboHints = atoi(getenv("XXL_SPCOMBOHINTS")) != 0;
	if (getenv("XXL_SPICONS")) spCoopIcons = atoi(getenv("XXL_SPICONS")) != 0;
	if (getenv("XXL_SPSWITCH")) spHeroSwitch = atoi(getenv("XXL_SPSWITCH")) != 0;
	if (getenv("XXL_SPSWITCHLOCK")) spSwitchLock = atoi(getenv("XXL_SPSWITCHLOCK")) != 0;
	if (getenv("XXL_SPSECOND")) spSecondChance = atoi(getenv("XXL_SPSECOND")) != 0;
	if (getenv("XXL_SPVIEW")) spSidekickView = atoi(getenv("XXL_SPVIEW")) != 0;
	// Isolated: single player is the game's own -- no sp_ feature, whatever the
	// ini says (the general options' single player parts ask SoloMod()).
	const bool spComboHintsIni = spComboHints, spCoopIconsIni = spCoopIcons, spHeroSwitchIni = spHeroSwitch,
	           spSwitchLockIni = spSwitchLock, spSecondChanceIni = spSecondChance;
	if (spIsolation) spComboHints = spCoopIcons = spHeroSwitch = spSwitchLock = spSecondChance = spSidekickView = false;
	if (!spHeroSwitch) spSwitchLock = false;   // (only with the switch)
	sectorCatchUpMs = GetPrivateProfileIntA("coop", "sector_catch_up_ms", sectorCatchUpMs, full);
	if (sectorCatchUpMs < 0) sectorCatchUpMs = 0;

	verbose     = ReadBool("debug", "verbose", verbose, full);
	forceLevel  = GetPrivateProfileIntA("debug", "force_level", forceLevel, full);
	watchdog    = ReadBool("debug", "watchdog", watchdog, full);
	vtProfile   = ReadBool("debug", "vtable_profile", vtProfile, full);
	renderProbe = ReadBool("debug", "render_probe", renderProbe, full);
	splitTest   = ReadBool("debug", "split_test", splitTest, full);
	cheats      = ReadBool("debug", "cheats", cheats, full);
	showLeader  = ReadBool("debug", "show_leader", showLeader, full);
	if (getenv("XXL_SHOWLEADER")) showLeader = atoi(getenv("XXL_SHOWLEADER")) != 0;   // autotests
	sidekickViewAlways = ReadBool("debug", "sidekick_view_always", sidekickViewAlways, full);
	if (getenv("XXL_SPVIEWALWAYS")) sidekickViewAlways = atoi(getenv("XXL_SPVIEWALWAYS")) != 0;
	skipCutscenes = ReadBool("debug", "skip_cutscenes", skipCutscenes, full);

	char buf[64] = {};
	GetPrivateProfileStringA("coop", "split_mode", "vertical", buf, sizeof(buf), full);
	if (const char* env = getenv("XXL_SPLITMODE")) lstrcpynA(buf, env, sizeof(buf));   // autotests: either split
	splitMode = (buf[0] == 'v' || buf[0] == 'V') ? SPLIT_VERTICAL : SPLIT_HORIZONTAL;

	keyboard = ReadBool("keyboard", "enabled", keyboard, full);
	for (int i = 0; i < KEY_COUNT; i++) {
		char kb[32] = {};
		GetPrivateProfileStringA("keyboard", kKeyNames[i], "", kb, sizeof(kb), full);
		if (kb[0]) keys[i] = ParseKey(kb);
	}
	for (int i = 0; i < P2B_COUNT; i++) {
		char pb[32] = {};
		GetPrivateProfileStringA("p2_pad", kP2PadNames[i], "", pb, sizeof(pb), full);
		for (int k = 0; pb[0] && k < kNumPadButtons; k++)
			if (!_stricmp(pb, kPadButtons[k].name)) p2Pad[i] = kPadButtons[k].bit;
	}
	p2InvertCamX = ReadBool("controls", "p2_invert_camera_x", p2InvertCamX, full);
	p2InvertCamY = ReadBool("controls", "p2_invert_camera_y", p2InvertCamY, full);

	auto readFactorText = [](const char* fb) {
		if (!_stricmp(fb, "off")) return 1.0f;
		float f = (float)atof(fb);
		return f < 0.1f ? 1.0f : f > 10.0f ? 10.0f : f;
	};
	auto readFactor = [&](const char* key, float def) {
		char fb[32] = {};
		GetPrivateProfileStringA("coop_difficulty", key, "", fb, sizeof(fb), full);
		if (!fb[0]) return def;
		return readFactorText(fb);
	};
	squadAliveFactor = readFactor("squad_romans_at_once", squadAliveFactor);
	squadCountFactor = readFactor("squad_romans_to_defeat", squadCountFactor);
	priceFactor      = readFactor("merchant_prices", priceFactor);
	comboFactor      = readFactor("combo_bar_fill", comboFactor);
	comboDrainFactor = readFactor("combo_bar_drain", comboDrainFactor);
	comboFullDrainFactor = readFactor("combo_bar_full_drain", comboFullDrainFactor);
	if (getenv("XXL_ATONCE")) squadAliveFactor = readFactorText(getenv("XXL_ATONCE"));   // autotests
	if (getenv("XXL_FACTORS1"))   // autotests: every squad and combo bar factor the game's own
		squadAliveFactor = squadCountFactor = comboFactor = comboDrainFactor = comboFullDrainFactor = 1.0f;
	coopOnly = ReadBool("coop_difficulty", "difficulty_coop_only", coopOnly, full);
	rebuiltLevels = ReadBool("coop_difficulty", "rebuilt_levels", rebuiltLevels, full);
	if (getenv("XXL_REBUILT")) rebuiltLevels = atoi(getenv("XXL_REBUILT")) != 0;   // autotests
	rulesStatic = ReadBool("coop_difficulty", "difficulty_static", rulesStatic, full);
	apartByPath = ReadBool("coop_difficulty", "difficulty_pathfinding", apartByPath, full);
	toDefeatStays = ReadBool("coop_difficulty", "difficulty_to_defeat_stays", toDefeatStays, full);
	if (getenv("XXL_COOPONLY")) coopOnly = atoi(getenv("XXL_COOPONLY")) != 0;   // autotests
	if (getenv("XXL_STATIC")) rulesStatic = atoi(getenv("XXL_STATIC")) != 0;
	if (getenv("XXL_APARTPATH")) apartByPath = atoi(getenv("XXL_APARTPATH")) != 0;
	if (getenv("XXL_DEFEATSTAYS")) toDefeatStays = atoi(getenv("XXL_DEFEATSTAYS")) != 0;
	const bool coopOnlyIni = coopOnly;
	if (spIsolation) coopOnly = true;   // (single player isolated: the game's squads and combo bar)

	GetPrivateProfileStringA("coop", "deadzone", "0.24", buf, sizeof(buf), full);
	deadzone = (float)atof(buf);
	if (deadzone < 0.0f || deadzone > 0.9f) deadzone = 0.24f;

	char limitText[64];
	if (reviveLimit > 0.0f) sprintf_s(limitText, "%.0f s, %.0f s of it to revive", reviveLimit, reviveGrace);
	else if (reviveLimit < 0.0f) sprintf_s(limitText, "auto: the walk + %.0f s", reviveGrace);
	else strcpy_s(limitText, "off");
	Log("config: enabled=%d p1Pad=%d split=%d mode=%s deadzone=%.2f forceLevel=%d watchdog=%d "
		"splitTest=%d verbose=%d keyboard=%d cheats=%d skipCutscenes=%d safeSequence=%d sectorCatchUpMs=%d individual=%d (guards %d, revive hold %.1f s / %.2f of a catapult's gap, "
		"time limit %s) nolock=%d (except corkscrews %d, rotary beams %d) riderLeads=%d romansInView=%d "
		"co-op difficulty: squad at once x%.2f, to defeat x%.2f, prices x%.2f, combo bar x%.2f drain x%.2f / x%.2f (%s, %s, "
		"pathfinding %d, to defeat stays %d), nolock romans %d (any markers %d)",
		enabled, p1Pad, splitScreen, splitMode == SPLIT_VERTICAL ? "vertical" : "horizontal",
		deadzone, forceLevel, watchdog, splitTest, verbose, keyboard, cheats, skipCutscenes, safeSequence, sectorCatchUpMs, individual,
		romansGuardDowned, reviveHold, reviveHoldCatapultWindow, limitText,
		noLock, noLockExceptCorkscrews, noLockExceptBeams, carRiderLeads, romansInView, squadAliveFactor, squadCountFactor, priceFactor, comboFactor, comboDrainFactor, comboFullDrainFactor,
		coopOnly ? "squads and combo bar in co-op only" : "squads and combo bar single player too",
		rulesStatic ? "static" : "dynamic", apartByPath, toDefeatStays, noLockRomans, noLockRomansAnyMarkers);
	Log("config: hints: join %d, pass the Roman %d, combos %d, machine gun %d", hintJoin, hintPass, hintCombos,
	    hintMachineGun);
	if (spIsolation)
		Log("config: single player: isolated -- the game's own (the ini's combo hints %d, co-op icons %d, hero switch %d, "
		    "switch lock %d, second chance %d off; obelix_potion and cheats co-op only%s)", spComboHintsIni, spCoopIconsIni,
		    spHeroSwitchIni, spSwitchLockIni, spSecondChanceIni,
		    coopOnlyIni ? "" : "; difficulty_coop_only taken as on");
	else
		Log("config: single player: combo hints %d, co-op icons %d, hero switch %d (pad %04X, key %02X, switch lock %d), "
		    "second chance %d, sidekick view %d, Obelix's potion %d, cheats %d", spComboHints, spCoopIcons, spHeroSwitch,
		    spSwitchButton, spSwitchKey, spSwitchLock, spSecondChance, spSidekickView, obelixPotion, cheats);
}
