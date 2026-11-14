#pragma once
#include <cstdint>

enum SplitMode {
	SPLIT_HORIZONTAL = 0,  // stacked, player 1 on top
	SPLIT_VERTICAL   = 1,  // side by side, player 1 on the left
};

struct CoopConfig {
	bool  enabled       = true;
	bool  verbose       = false;
	int   forceLevel    = -1;     // debug: jump straight to this level once
	bool  watchdog      = false;  // background thread reporting engine state every 2 s
	bool  vtProfile     = false;  // count calls through hero / hero-life vtables
	bool  renderProbe   = false;  // find the scene-render call via RwCamera.beginUpdate
	bool  splitTest     = false;
	bool  showLeader    = false;  // the trio's leader written on the screen
	bool  sidekickViewAlways = false;  // single player: the sidekick view always on (watching the AI)
	bool  cheats        = false;  // test helpers and the mod buttons (R3 + A / B / X / Y / D-pad); single player too with SoloMod()
	int   splitMode     = SPLIT_VERTICAL;
	bool  splitScreen   = true;   // off = shared camera on the leader
	bool  gameCamera    = true;   // split screen: each player gets a copy of the game's camera (off = the mod's follow camera)
	float splitFov      = 15.0f;  // split screen: degrees added to each half's field of view
	int   p1Pad         = -1;     // controller the game reads (player 1): XInput slot 0-3, 4 = keyboard; -1 = detect
	bool  invertCamX    = false;  // [controls] invert_camera_x / _y: the options' Invert Camera X / Y rows (ctrlopts.cpp)
	bool  invertCamY    = false;
	float p1Deadzone    = 0.0f;   // [controls] p1_deadzone: player 1's sticks (the game's input), 0 = the game's own
	char  iniPath[260]  = {};     // the ini's full path (the options write [controls] back)
	int   dropHoldMs    = 1500;
	bool  obelixPotion  = true;   // the magic potion works on a player's Obelix as on Asterix (single player: Obelix leading, SoloMod())
	bool  twisterNerf   = true;   // the plain Twister takes no Romans still to come off a squad; each Roman it catches counts twisterNerfPerRoman (co-op, SoloMod())
	float twisterNerfPerRoman = 4.0f;   // ... a caught Roman with the ones it stands for (1: itself only)
	bool  skipCutscenes = false;  // debug: every cutscene ends on Enter (the game's own skip)
	bool  individual    = true;   // each hero its own health, shields and combo bar; a fallen hero can be revived
	bool  romansGuardDowned = true; // ... the basic Romans near a fallen hero stay round it, shielded, until it is revived
	bool  hintJoin    = true;   // [hints] hint_join: how player 2 joins, at a level's start
	bool  hintPass    = true;   // [hints] hint_pass_roman: "LB passes the Roman" while spinning one
	bool  hintCombos  = true;   // [hints] hint_combos: the combo hints (combohints.cpp)
	bool  hintMachineGun = true;   // [hints] hint_machine_gun: who does what at the machine gun (shared.cpp)
	float reviveHold    = 5.0f;   // ... seconds the other player holds the grab button by it to revive it
	float reviveHoldCatapultWindow = 0.4f; // ... under catapult fire: this share of the time between its stones (at most reviveHold)
	float reviveLimit   = 0.0f;   // ... seconds to revive it before the game over: 0 none, -1 auto (the walk there + reviveGrace)
	float reviveGrace   = 30.0f;  // ... the limit's seconds for the revive itself once there
	bool  safeSequence  = true;   // the non-leading player can't set off levers meant for the leader's switch
	int   sectorCatchUpMs = 1000; // a hero left in an unloaded sector this long is brought to the other
	int   p2Hero        = 1;      // player 2's hero: 0 Asterix, 1 Obelix, -1 the one player 1 is not leading with
	bool  noLock        = true;   // the level never takes a player's hero (waits, walks to markers); cable cars still do
	bool  noLockExceptCorkscrews = true;   // no-lock mode: a corkscrew's circle (light pillar launching its hero) still locks it
	bool  noLockExceptBeams = false;   // no-lock mode: a rotary beam's circle still locks its hero, through the ride and after
	bool  carRiderLeads = false;      // a hero boarding a cable car while the other leads takes the lead at once
	bool  romansInView  = false;      // every spawn marker of a squad in a player's view: its Romans come anyway (farthest marker)
	bool  markerTeleport = true;      // a hero the level walks to a marker it has no way to (pathfinding) is put on it (co-op: nolock off only)
	float deadzone      = 0.24f;

	// Co-op difficulty ([coop_difficulty]; read at startup)
	bool  coopOnly         = true;  // squads and combo bar: only while player 2 plays (single player: the game's own)
	bool  rulesStatic      = false; // ... on all the time in co-op, not the game's own while the heroes are apart
	bool  apartByPath      = true;  // the heroes are apart too where neither can walk to the other (pathfinding)
	bool  toDefeatStays    = false; // a fight begun with the heroes apart keeps the level's Romans to defeat
	float squadAliveFactor = 2.0f;  // Romans of a squad alive at once
	bool  rebuiltLevels    = true;  // levels rebuilt (coop_levels\): Roman pools for the factor, crates and bonuses x2
	float squadCountFactor = 2.0f;  // Romans of each squad pool (phase) to defeat
	float priceFactor      = 2.0f;  // the merchant's prices
	float comboFactor      = 0.5f;  // combo bar hits counted per Roman hit
	float comboDrainFactor = 1.5f;  // the combo bar losing its hits (before it is full)
	float comboFullDrainFactor = 1.5f; // a full combo bar running out
	bool  noLockRomans     = true;  // no-lock mode: Romans come for a hero the level would have kept waiting ([coop_difficulty.experimental])
	bool  noLockRomansAnyMarkers = false; // ... at any squad's spawn markers near it, not only its own squad's

	// Single player ([single_player]; co-op features without player 2). sp_isolation
	// (on) keeps single player the game's own: every sp_ option below is then off
	// whatever the ini says (Load clears them), and so are the general options'
	// single player parts (obelix_potion, cheats) -- SoloMod().
	bool  spIsolation    = true;   // sp_isolation: single player as the game's (only player 2's join hint)
	bool  spComboHints   = true;   // sp_combo_hints: the combo hints in player 1's box
	bool  spCoopIcons    = true;   // sp_coop_icons: the mod's button icons (pad or keyboard) in the game's texts
	bool  spHeroSwitch   = true;   // sp_hero_switch: a button gives the lead to the other hero
	uint16_t spSwitchButton = 0x0001;   // ... its pad button (PAD_*: D-pad up)
	struct PadButton { const char* name; uint16_t bit; };
	static const int kNumPadButtons = 15;
	static const PadButton kPadButtons[kNumPadButtons];   // ini names of the pad buttons (config.cpp)
	int   spSwitchKey    = 0x09;   // ... its key with player 1 on the keyboard (TAB)
	bool  spSwitchLock   = true;   // sp_hero_switch_lock: the level's lead switches ignored, a lock's switch undone at its end
	bool  spSecondChance = false;  // sp_second_chance: each hero its own health; a fallen one waits to be revived
	bool  spSidekickView = true;   // sp_sidekick_view: a small view of the other hero (switch refused, partner down)
	float spSecondChanceComboDrain = 0.33f;   // sp_second_chance_combo_drain: the other hero's combo bar runs down at this share of the game's pace
	// The mod's features in single player at all (sp_isolation off).
	bool  SoloMod() const { return !spIsolation; }

	// Keyboard as player 2's controller ("pad" slot kKeyboardPad): virtual-key
	// codes, [keyboard] section of the ini.
	bool  keyboard      = true;
	enum { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_LOOK_UP, KEY_LOOK_DOWN, KEY_LOOK_LEFT,
	       KEY_LOOK_RIGHT, KEY_JUMP, KEY_ATTACK, KEY_GRAB, KEY_DASH, KEY_LB, KEY_RB, KEY_START,
	       KEY_BACK, KEY_CENTER_CAMERA, KEY_HUD, KEY_SWITCH, KEY_CHEAT, KEY_COUNT };
	// KEY_CHEAT (cheat_modifier): held like the right stick button for the
	// mod's buttons (BACKSPACE), without centring the camera.
	int   keys[KEY_COUNT] = { 'W', 'S', 'A', 'D', 0x26, 0x28, 0x25, 0x27, 0x20, 'J', 'K', 'L', 'U', 'O',
	                          0x0D, 'P', 'M', 'C', 'X', 0x08 };
	static const char* const kKeyNames[KEY_COUNT];   // their ini names ([keyboard])

	// Player 2's pad layout ([p2_pad]): the physical button of each of its
	// actions (the mod reads player 2's pad through it: the rest of the mod sees
	// the default buttons, P2_DEFAULT). Back opens the pause menu, D-pad down
	// shows player 2's HUD, D-pad up asks for a hero swap.
	enum { P2B_JUMP, P2B_ATTACK, P2B_GRAB, P2B_DASH, P2B_LB, P2B_RB, P2B_MENU, P2B_HUD, P2B_CENTER, P2B_SWITCH, P2B_COUNT };
	static const uint16_t kP2Default[P2B_COUNT];
	static const char* const kP2PadNames[P2B_COUNT];   // their ini names ([p2_pad])
	uint16_t p2Pad[P2B_COUNT] = { 0x1000, 0x4000, 0x2000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0002, 0x0080, 0x0001 };
	bool  p2InvertCamX = false;   // [controls] p2_invert_camera_x / _y: player 2's Invert Camera rows
	bool  p2InvertCamY = false;

	void Load(const char* iniPath);
};

extern CoopConfig g_config;

// "key = value" written into [section] of xxl_coop.ini (the options menus).
void IniWrite(const char* section, const char* key, const char* value);
