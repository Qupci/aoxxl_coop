// Hero Life objects -- the layer that decides, every frame, whether a hero is
// driven by the player or by its AI.
//
// Every hero hook points at a Life object (hero+0x0C), and the Life points back
// at its hero (life+0x10). The Life's per-frame tick is vtable slot 17 (+0x44),
// __thiscall with one stack argument (ret 4):
//
//     CKHkAsterixLife  vtable 0x625C90  tick 0x4AA9C0
//     CKHkObelixLife   vtable 0x625D18  tick 0x4AB7E0
//
// The tick picks its state straight from the hero's control bits,
//     state = (hero+0x18 >> 1) & 0xF          (stored at life+0x1D)
// and runs that state's handler, one of Life vtable slots 24..33. Even states
// are the *player* variant of a mode, odd states the *AI* variant (0/1 normal
// play, 6/7 cinematic, ...). Bit 0 of hero+0x18 is the companion flag and always
// equals the low bit of the state, so `(hero+0x18 & 3) == 0` means "a player
// drives this hero". See docs/RESEARCH.md for the full picture.

#pragma once
#include "kal.h"
#include "trio.h"

namespace axl {
const uintptr_t kAsterixLifeVtbl = 0x625C90;
const uintptr_t kObelixLifeVtbl  = 0x625D18;
const size_t    kLifeTickSlot    = 17;          // +0x44
const uintptr_t kAsterixLifeTick = 0x4AA9C0;
const uintptr_t kObelixLifeTick  = 0x4AB7E0;

const size_t kLife_hero  = 0x10;   // KHook* back-pointer
const size_t kLife_state = 0x1D;   // current Life state, 0xFF = none
const size_t kHook_life  = 0x0C;   // hero -> its Life
}

// Control-state helpers for hero+0x18.
inline uint8_t HeroCtrlState(uint8_t f18) { return (uint8_t)((f18 >> 1) & 0xF); }
inline bool    HeroIsPlayerVariant(uint8_t f18) { return (f18 & 3) == 0; }
inline uint8_t HeroPlayerVariant(uint8_t f18) { return (uint8_t)(f18 & ~3u); }
inline uint8_t HeroAiVariant(uint8_t f18) { return (uint8_t)(f18 | 3u); }
inline uint8_t HeroMode(uint8_t f18) { return (uint8_t)(HeroCtrlState(f18) >> 1); }

// Modes a player can drive in. Mode 0 is ordinary play and mode 4 is swimming --
// both have a player and an AI handler for every hero, and the hero moves between
// them by itself, keeping its companion bit. Modes 1..3 (set by trio events
// 0x4807/0x4809/0x480B-F: rides, ballista, belly slide, cinematics) only have a
// real handler for one of the two heroes, so they stay single-player.
inline bool HeroModePlayable(uint8_t f18) {
	uint8_t m = HeroMode(f18);
	return m == 0 || m == 4;
}

// The Life state a hero was in at the end of its last tick (0xFF if unknown).
uint8_t LifeLastState(int heroIndex);

// Installs the slot-17 hooks on both Life vtables. Reversible vtable swaps.
bool LifeInstallHooks();
