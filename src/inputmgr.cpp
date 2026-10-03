#include "inputmgr.h"

InputManager GetInputManager() {
	InputManager m;
	// Class group 0 is "Managers"; CKInput is a singleton in that pool.
	m.obj = GetLvlObject(0, 4, 0);
	return m;
}
