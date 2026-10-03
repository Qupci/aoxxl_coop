// Typed view over CKGrpTrio and the three hero hooks.
// Offsets are documented in coop/docs/RESEARCH.md section 4 and 5.

#pragma once
#include "kal.h"

enum HeroIndex { HERO_ASTERIX = 0, HERO_OBELIX = 1, HERO_IDEFIX = 2, HERO_COUNT = 3 };

// AI orders written to hero+0x564. Names are the engine's own French debug
// strings from the table at 0x64CBD8.
enum AiOrder {
	AI_FOLLOW_LEADER   = 0,   // Suivre Leader
	AI_GOTO_POS        = 1,   // Aller a position
	AI_JUMP_TO_POS     = 2,   // Sauter a position
	AI_WAIT            = 3,   // Attendre
	AI_HIT_ENEMY       = 4,   // Frapper un ennemi
	AI_THROW_ENEMY     = 5,   // Envoyer un ennemi
	AI_BITE_ENEMY      = 6,   // Mordre un ennemi
	AI_GOTO_BALLISTA   = 7,
	AI_HANG_ON_BEAM    = 8,
	AI_USE_CABLECAR    = 9,
	AI_TAKE_HIT        = 10,
	AI_CLEAR_PATH      = 11,
	AI_EXTERNAL_CTRL   = 12,  // Sous controle exterieur
	AI_GOTO_HINT_MAN   = 13,
	AI_GOTO_SHOP       = 14,
	AI_GOTO_POS_NO_PF  = 15,
};

// A hero hook with the accessors co-op needs.
struct Hero {
	KHook* hook = nullptr;

	explicit operator bool() const { return hook != nullptr; }
	char* raw() const { return (char*)hook; }

	uint8_t  Flags18() const { return hook ? *(uint8_t*)(raw() + 0x18) : 0; }

	// Bit 0 of +0x18 marks the hero as the AI companion rather than the one the
	// player is driving. CKHkAsterix::update resets the AI order to 0 whenever
	// it is set, and CKGrpTrio's switch handler refuses to switch *to* a hero
	// unless it is set -- you can only take over someone you are not already
	// controlling. So bit 0 clear == this is the player's hero.
	bool     IsCompanion() const { return hook && (Flags18() & 1) != 0; }
	bool     IsPlayerControlled() const { return hook && (Flags18() & 1) == 0; }

	// Bits 1..4 are the control state the command block writes.
	uint8_t  ControlBits() const { return (uint8_t)((Flags18() >> 1) & 0xF); }

	// Presence in the level is a separate question from who is driving: the hook
	// simply does not exist before Obelix joins in level 1.
	bool     Exists() const { return hook != nullptr; }

	// +0x1A: non-zero means "ignore the shared player command block", i.e. this
	// hero is being driven by its AI rather than by a player.
	bool     IgnoresCommands() const { return *(uint8_t*)(raw() + 0x1A) != 0; }
	void     SetIgnoresCommands(bool v) { *(uint8_t*)(raw() + 0x1A) = v ? 1 : 0; }

	AiOrder  Order() const { return (AiOrder)*(uint8_t*)(raw() + 0x564); }
	void     SetOrder(AiOrder o) { *(uint8_t*)(raw() + 0x564) = (uint8_t)o; }
	uint8_t  Action() const { return *(uint8_t*)(raw() + 0x561); }

	Vector3& Pos() const { return *(Vector3*)(raw() + ax::kHero_pos); }
	Vector3& Dir() const { return *(Vector3*)(raw() + ax::kHero_dir); }
	Vector3& AiTargetPos() const { return *(Vector3*)(raw() + 0x568); }
	Vector3& AiTargetDir() const { return *(Vector3*)(raw() + 0x574); }

	uint32_t Flags390() const { return *(uint32_t*)(raw() + 0x390); }
	uint32_t Flags884() const { return *(uint32_t*)(raw() + 0x884); }

	// +0x390 bit 2 is set while the hero is locked into a scripted action
	// (belly ride, boat, cutscene, shop...). Co-op must not fight those.
	bool     Busy() const { return (Flags390() & 0x4) != 0; }
};

// CKGrpTrio: owns the three heroes and decides which one the player drives.
struct Trio {
	KGroup* grp = nullptr;

	explicit operator bool() const { return grp != nullptr; }
	char* raw() const { return (char*)grp; }

	int  Leader() const { return *(uint8_t*)(raw() + 0x2C); }
	int  Companion() const { return *(uint8_t*)(raw() + 0x2D); }

	Hero GetHero(int idx) const {
		Hero h;
		if (grp && idx >= 0 && idx < HERO_COUNT)
			h.hook = *(KHook**)(raw() + 0x30 + 4 * idx);
		return h;
	}

	uint32_t Flags() const { return *(uint32_t*)(raw() + 0x3F4); }
	float    SwitchRadius() const { return *(float*)(raw() + 0x38C); }

	// Hand control to `idx` (HERO_ASTERIX or HERO_OBELIX). This is the same
	// event the blue circles on the ground fire.
	//
	// 0x4800 sets leader=Asterix/companion=Obelix, 0x4801 the other way round,
	// so the hero index goes straight into the low byte. Either one is a no-op
	// unless the hero being switched to has bit 0 of +0x18 set.
	void SwitchTo(int idx) const {
		if (!grp) return;
		if (idx != HERO_ASTERIX && idx != HERO_OBELIX) return;
		grp->sendEvent(0x4800 | idx, nullptr);
	}
	void Toggle() const { SwitchTo(Leader() == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX); }

	// Companion behaviour, via the trio's own event handler.
	void CompanionFollow() const { if (grp) grp->sendEvent(0x4803, nullptr); }
	void CompanionWait() const { if (grp) grp->sendEvent(0x4802, nullptr); }
};

// Fetch the live trio for the current level, or a null Trio outside gameplay.
Trio GetTrio();

const char* HeroName(int idx);
const char* AiOrderName(int order);
