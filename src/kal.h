// kal.h -- Kal engine (Etranges Libellules) runtime structures and addresses
// Target: Asterix & Obelix XXL (2004), PC retail, axxl.exe
//         MD5 6953FB1930610B2D0420550479C0A40D
//         SHA1 EE7D6A20B9C07ABB591CF5E36DF50EF200B32033
//
// Offsets and function addresses below were taken from AdrienTD's xxl-inspector
// and XXL-Editor (see coop/docs/RESEARCH.md for the provenance of each one).
// Anything marked TODO is not confirmed yet.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstddef>

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;

// ---------------------------------------------------------------------------
// Small math types matching the engine's layout
// ---------------------------------------------------------------------------

struct Vector3 {
	float x, y, z;
	Vector3() : x(0), y(0), z(0) {}
	Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
	Vector3 operator+(const Vector3& o) const { return Vector3(x + o.x, y + o.y, z + o.z); }
	Vector3 operator-(const Vector3& o) const { return Vector3(x - o.x, y - o.y, z - o.z); }
	Vector3 operator*(float f) const { return Vector3(x * f, y * f, z * f); }
	float dot(const Vector3& o) const { return x * o.x + y * o.y + z * o.z; }
	Vector3 cross(const Vector3& o) const {
		return Vector3(y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x);
	}
	float len2() const { return x * x + y * y + z * z; }
};

struct CMatrix {
	void* u[4];
	float c[16];
	float d[16];
};

// ---------------------------------------------------------------------------
// Base engine classes
// ---------------------------------------------------------------------------

struct KFile;

// Every serialisable engine object derives from this. The vtable layout is the
// one xxl-inspector uses for XXL1; do not reorder.
struct KClass {
	virtual void destructor_placeholder() = 0;
	virtual char isSubclass(int cls) = 0;
	virtual void reset() = 0;
	virtual int getClassGroup() = 0;
	virtual int getClassID() = 0;
	virtual int sendEvent(int event, void* param) = 0;
	virtual char unknown3(void* a0, void* a4, void* a8, void* aC) = 0;
	virtual int unknown4(void* a0) = 0;
	virtual int serialize(KFile* file, void* a4) = 0;
	virtual int deserialize(KFile* file, void* a4) = 0;

	char isSubclass(int clgrp, int clid) { return isSubclass(clgrp | (clid << 6)); }
};

struct CNode : KClass {
	CMatrix* matrix;
	CNode* nextNode;
	CNode* parentNode;
	void *p_10, *p_14;
	CNode* childNode;
	void* geometry;
};

struct KHook : KClass {
	KHook* nextHook;
	int unk_08;
	KClass* life;
	CNode* node;
};

struct KGroup : KClass {
	KGroup* next;
	int unk_08;
	KGroup* child;
	int unk_10;
	KGroup* parent;
	KClass* life;
	KClass* bundle;
	KHook* hook;
};

struct CKService : KClass {
	CKService* next;
	int cunknown;
};

struct KclInfos {
	struct Tab {
		void* pnt;
		uint allocatedSize;
		uint numElements;
	};
	void* vtbl;
	Tab cls[15];
};

struct CKLoadingManager {
	char unknown[0x480];
	uint currentSector;
	uint ss0, ss1, ss2;
	uint sectorToLoad;
};

// The engine's service locator. Everything reachable at runtime hangs off this.
struct CKYellowPages {
	void *p_00, *p_04, *p_08, *p_0C;
	void *p_10, *p_14, *p_18; KclInfos* kclInfos;
	void *p_20, *p_24, *p_28, *p_2C;
	void *gameLoop, *p_34, *p_38, *p_3C;
	void* p_40; CKLoadingManager* loadingManager; void *savingManager, *p_4C;
	void *srvCollision, *p_54, *p_58, *p_5C;
	void *p_60, *p_64, *p_68, *p_6C;
	void *p_70, *p_74, *srvBeacon, *p_7C;
	void *p_80, *p_84, *p_88, *gameManager;
};

#pragma pack(push, 1)
// One per (class group, class id) pair; holds every live instance of that class.
struct ClassContainer {
	void* objects;
	uint flags;
	void* u1;
	ushort u2, count;
	ushort clSize; char pl, u3;
};
#pragma pack(pop)

struct SGameStartInfo {
	HWND hwnd;
	uint width, height;
	uint unk1, unk2;
	char fullscreen;
};

// ---------------------------------------------------------------------------
// Static addresses (axxl.exe is not ASLR-relocated, base 0x400000)
// ---------------------------------------------------------------------------

namespace ax {

const uintptr_t kImageBase          = 0x400000;

// --- Globals ---
const uintptr_t kYellowPagesPtr     = 0x6621F4;  // CKYellowPages**
const uintptr_t kGameStartInfo      = 0x665310;  // SGameStartInfo*

// --- Functions ---
const uintptr_t kWndProc            = 0x479C10;  // the game's own WndProc
const uintptr_t kWndProcField       = 0x479E8E;  // immediate operand in RegisterClass
const uintptr_t kCursorSetup        = 0x479EAE;  // patched out by inspector to hide cursor
const uintptr_t kKalClassInit       = 0x47A274;  // registers a class name at startup
const uintptr_t kKfRefToPnt         = 0x40D2C0;
const uintptr_t kCalcProjCamera     = 0x414B70;  // __thiscall (CRendThing*, float fov, char force)
const uintptr_t kSetViewWindow      = 0x58FFB0;  // __cdecl (uint, float ss[2]) -- RenderWare
const uintptr_t kHookGoVanish       = 0x515CB0;  // __thiscall (KHook*)
const uintptr_t kGrpBonusSpawnBonus = 0x54BE20;
const uintptr_t kBonusFollowHero    = 0x54E2C0;
const uintptr_t kBonusFall          = 0x54DF10;
const uintptr_t kBombSpawn          = 0x5354B0;

// --- CKGameManager member offsets ---
const size_t kGM_currentLevel       = 0x04;
const size_t kGM_nextLevel          = 0x08;
const size_t kGM_levelObject        = 0x20;

// --- CKLevel member offsets ---
const size_t kLevel_currentSector   = 0x24;
const size_t kLevel_nextSector      = 0x34;

// --- CKTimeManager (yellowPages->gameLoop + 8) ---
const size_t kTM_timeScale          = 0x10;

// --- CKHkHero member offsets ---
const size_t kHero_ctrlFlags        = 0x18;  // low 2 bits: 0 = player-driven, 3 = AI-driven
const size_t kHero_pos              = 0x6C;  // Vector3
const size_t kHero_dir              = 0xB4;  // Vector3, facing direction

// --- CKGrpTrio member offsets ---
const size_t kTrio_activeHero       = 0x2D;  // byte; index of the hero the player drives

// --- CKSrvCamera member offsets ---
const size_t kCam_pos               = 0x88;  // Vector3
const size_t kCam_dir               = 0xAC;  // Vector3
const size_t kCam_targets           = 0xE8;  // CNode*[]

// --- Class ids: (group, id) ---
struct ClassId { int grp, id; };
const ClassId kCKSrvCamera          = { 1, 3 };
const ClassId kCKHkAsterix          = { 2, 28 };
const ClassId kCKHkObelix           = { 2, 29 };
const ClassId kCKHkIdefix           = { 2, 30 };
const ClassId kCKHkInGameInterface  = { 2, 132 };
const ClassId kCKGrpTrio            = { 4, 12 };
const ClassId kCKGrpBonus           = { 4, 48 };
const ClassId kCKSaveGame           = { 12, 100 };

// Event sent to CKGrpTrio to hand control to hero `idx`.
inline int TrioSwitchEvent(int idx) { return 0x4800 | idx; }

} // namespace ax

// ---------------------------------------------------------------------------
// Runtime accessors
// ---------------------------------------------------------------------------

inline CKYellowPages* YellowPages() {
	return *(CKYellowPages**)ax::kYellowPagesPtr;
}

inline SGameStartInfo* GameStartInfo() {
	return (SGameStartInfo*)ax::kGameStartInfo;
}

// True once a level is actually loaded and running (currentLevel == -1 in menus).
bool KalLevelIsLoaded();

// Fetch instance `instid` of class (clgrp, clid) from the level's object pool.
// Returns null when the class has no instances (e.g. Obelix before the reunion).
KClass* GetLvlObject(uint clgrp, uint clid, uint instid);
inline KClass* GetLvlObject(ax::ClassId c, uint instid = 0) {
	return GetLvlObject(c.grp, c.id, instid);
}
uint GetNumLvlClassInstances(uint clgrp, uint clid);
inline uint GetNumLvlClassInstances(ax::ClassId c) {
	return GetNumLvlClassInstances(c.grp, c.id);
}

// ---------------------------------------------------------------------------
// Patch helpers
// ---------------------------------------------------------------------------

void SetMemProtection(void* mem, int flags);
void SetImmediateJump(void* at, uintptr_t target);   // E9 rel32
void SetImmediateCall(void* at, uintptr_t target);   // E8 rel32
void WriteBytes(void* at, const void* src, size_t n);
