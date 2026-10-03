// The CKInput manager -- the engine's mapped-action layer.
//
// Registered as class (0, 4), instance size 0x6C, vtable 0x623F38. Its per-frame
// update lives in vtable slot 20 (0x436360) and ends with:
//
//     for (i = 0; i < mgr->numActions; i++)
//         if (mgr->bindings[i].active == 1)
//             CKInput_updateBinding(&mgr->values[i], timeInfo);   // 0x435A50
//
// so `mgr->values` is one byte per mapped action -- jump, hit, and so on, after
// the keyboard/pad binding and any analog mapping have been applied. That single
// array is what the hero state machines consume, which makes it the natural
// place to split input between two players: swap its contents around each
// hero's Life tick, exactly like the movement command block.

#pragma once
#include "kal.h"

struct InputManager {
	KClass* obj = nullptr;

	explicit operator bool() const { return obj != nullptr; }
	char* raw() const { return (char*)obj; }

	// +0x3A: number of mapped actions.
	uint8_t NumActions() const { return obj ? *(uint8_t*)(raw() + 0x3A) : 0; }

	// +0x1C: pointer to the action value array, NumActions() bytes long.
	uint8_t* Values() const { return obj ? *(uint8_t**)(raw() + 0x1C) : nullptr; }

	// +0x18: array of 0x20-byte binding descriptors, parallel to Values().
	// Byte +4 of a descriptor is the "this binding is active" flag.
	char* Binding(int i) const { return obj ? *(char**)(raw() + 0x18) + 0x20 * i : nullptr; }
	bool  BindingActive(int i) const {
		char* b = Binding(i);
		return b && b[4] == 1;
	}

	// +0x10 / +0x04: input device count and array.
	uint8_t NumDevices() const { return obj ? *(uint8_t*)(raw() + 0x10) : 0; }

	// +0x68: set once the manager has run for this frame.
	uint8_t UpdatedThisFrame() const { return obj ? *(uint8_t*)(raw() + 0x68) : 0; }
};

InputManager GetInputManager();

const size_t kMaxActions = 64;  // sanity cap when copying the value array
