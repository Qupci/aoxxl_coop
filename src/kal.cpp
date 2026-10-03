#include "kal.h"
#include "log.h"

bool KalLevelIsLoaded() {
	CKYellowPages* yp = YellowPages();
	if (!yp || !yp->gameManager) return false;
	return *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) != -1;
}

// Mirrors xxl-inspector's GetLvlObject. The object pool is an array of 15 class
// groups; each group is an array of ClassContainer indexed by class id. How the
// instances are stored depends on the container's flags:
//   pl != 0            -> instances are packed inline, clSize bytes apart
//   flags >> 17 == 1   -> a single instance, `objects` points straight at it
//   otherwise          -> `objects` is an array of pointers
KClass* GetLvlObject(uint clgrp, uint clid, uint instid) {
	CKYellowPages* yp = YellowPages();
	if (!yp || !yp->loadingManager) return nullptr;
	void* kcls = *(void**)((char*)yp->loadingManager + 4);
	if (!kcls) return nullptr;
	void* grp = *(void**)((char*)kcls + 12 * clgrp);
	if (!grp) return nullptr;
	ClassContainer* cl = (ClassContainer*)grp + clid;
	if (!cl->count || instid >= cl->count) return nullptr;

	char* fstobj = (char*)cl->objects;
	if (!fstobj) return nullptr;
	if (cl->pl)
		return (KClass*)(fstobj + cl->clSize * instid);
	if (cl->flags >> 17 == 1)
		return (KClass*)fstobj;
	return (KClass*)(((void**)fstobj)[instid]);
}

uint GetNumLvlClassInstances(uint clgrp, uint clid) {
	CKYellowPages* yp = YellowPages();
	if (!yp || !yp->loadingManager) return 0;
	void* kcls = *(void**)((char*)yp->loadingManager + 4);
	if (!kcls) return 0;
	void* grp = *(void**)((char*)kcls + 12 * clgrp);
	if (!grp) return 0;
	return ((ClassContainer*)grp + clid)->count;
}

// ---------------------------------------------------------------------------

void SetMemProtection(void* mem, int flags) {
	MEMORY_BASIC_INFORMATION mbi;
	DWORD unused;
	VirtualQuery(mem, &mbi, sizeof(mbi));
	VirtualProtect(mem, mbi.RegionSize, (mbi.Protect & 0xFFFFFF00) | flags, &unused);
}

void SetImmediateJump(void* at, uintptr_t target) {
	*(uint8_t*)at = 0xE9;
	*(uint32_t*)((char*)at + 1) = (uint32_t)(target - ((uintptr_t)at + 5));
}

void SetImmediateCall(void* at, uintptr_t target) {
	*(uint8_t*)at = 0xE8;
	*(uint32_t*)((char*)at + 1) = (uint32_t)(target - ((uintptr_t)at + 5));
}

void WriteBytes(void* at, const void* src, size_t n) {
	memcpy(at, src, n);
}
