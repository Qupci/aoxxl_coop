#include "forge_lib.h"
#include "KEnvironment.h"
#include "ClassRegister.h"
#include "Duplicator.h"
#include "EditorInterface.h"
#include "GamePatcher.h"
#include "CoreClasses/CKGroup.h"
#include "CoreClasses/CKHook.h"
#include "GameClasses/CKGameX1.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

size_t ForgeModulePatches(ForgePatch* out, size_t max) {
	GamePatcher::ModulePatch p[16];
	size_t n = GamePatcher::ModulePatchesX1(p, 16);
	for (size_t i = 0; i < n && i < max; i++) out[i] = { p[i].offset, p[i].bytes, p[i].size };
	return n;
}

bool ForgeModuleKnown(const void* exe, size_t exeSize) {
	static const unsigned char drmValSignature[32] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x41,
		0x0C, 0x00, 0x01, 0x00, 0x44, 0x00, 0x01, 0x00
	};
	const unsigned char* m = (const unsigned char*)exe;
	if (exeSize < 0x2F0000) return false;
	for (size_t p = 0; p + sizeof(drmValSignature) <= exeSize; p++)
		if (memcmp(m + p, drmValSignature, sizeof(drmValSignature)) == 0) return true;
	return false;
}

namespace {

int CountHooks(CKGroup* g) {
	int n = 0;
	for (CKHook* h = g->childHook.get(); h; h = h->next.get()) n++;
	return n;
}

// Pools whose Romans come in numbers (the mod scales their squads' numbers):
// basic Romans and their leaders.
bool Scaled(CKHook* h) {
	if (!h) return false;
	int fid = (int)h->getClassFullID();
	return fid == GameX1::CKHkBasicEnemy::FULL_ID || fid == GameX1::CKHkBasicEnemyLeader::FULL_ID;
}

void Add(std::string& r, const char* fmt, ...) {
	char b[256];
	va_list a;
	va_start(a, fmt);
	vsnprintf(b, sizeof(b), fmt, a);
	va_end(a);
	r += b;
}

}

bool ForgeLevel(const void* exe, size_t exeSize, const char* gameDir, const char* outDir, int lvl,
                float factor, int* cloned, char* report, size_t reportSize) {
	std::string r;
	*cloned = 0;
	bool ok = false;
	try {
		std::filesystem::path game = std::filesystem::u8path(gameDir), out = std::filesystem::u8path(outDir);
		std::filesystem::path conv = out / "conv";
		std::string err;
		char lvlfn[64];
		snprintf(lvlfn, sizeof(lvlfn), "LVL%03u/LVL%02u.KWN", lvl, lvl);
		std::filesystem::create_directories((out / lvlfn).parent_path());
		if (!GamePatcher::ConvertLevelX1(exe, exeSize, game, conv, lvl, err)) {
			Add(r, "converting: %s", err.c_str());
		} else {
			KEnvironment kenv;
			ClassRegister::registerClassesForXXL1PC(kenv);
			kenv.loadGame(game.u8string().c_str(), KEnvironment::KVERSION_XXL1, KEnvironment::PLATFORM_PC, false);
			kenv.lvlFileOverride = (conv / lvlfn).u8string();
			kenv.outGamePath = out.u8string();
			kenv.loadLevel(lvl);
			EditorUI::EditorInterface ui;
			int i = 0;
			for (CKObject* o : kenv.levelObjects.getClassType<CKGrpPoolSquad>().objects) {
				CKGrpPoolSquad* pool = o->cast<CKGrpPoolSquad>();
				CKHook* tmpl = pool->childHook.get();
				int n = CountHooks(pool);
				int want = (int)std::ceil(n * factor - 0.001f);
				if (Scaled(tmpl) && factor > 1.0f && want > n) {
					for (int k = n; k < want; k++) {
						Duplicator dup(kenv, &ui);
						dup.doClone(tmpl);
					}
					int now = CountHooks(pool);
					*cloned += now - n;
					Add(r, " pool %d (%s) %d -> %d;", i, tmpl->getClassName(), n, now);
				}
				i++;
			}
			if (*cloned > 0) {
				kenv.saveLevel(lvl);   // the level and its sectors (ids of their objects follow the level's)
			} else {
				std::filesystem::copy_file(conv / lvlfn, out / lvlfn, std::filesystem::copy_options::overwrite_existing);
				Add(r, " no Roman pools to fill;");
			}
			ok = true;
		}
		std::error_code ec;
		std::filesystem::remove_all(conv, ec);
	} catch (const std::exception& e) {
		Add(r, " failed: %s", e.what());
		ok = false;
	}
	snprintf(report, reportSize, "%s", r.c_str());
	return ok;
}
