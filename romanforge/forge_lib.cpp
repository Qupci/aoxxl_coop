#include "forge_lib.h"
#include "KEnvironment.h"
#include "ClassRegister.h"
#include "Duplicator.h"
#include "EditorInterface.h"
#include "GamePatcher.h"
#include "CoreClasses/CKGroup.h"
#include "CoreClasses/CKHook.h"
#include "GameClasses/CKGameX1.h"
#include "CoreClasses/CKNode.h"
#include <algorithm>
#include <vector>
#include <initializer_list>
#include <stdexcept>
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
	uint32_t offsets[9];
	return exeSize >= 0x200000 && GamePatcher::DrmOffsetsX1(exe, exeSize, offsets);
}

namespace {

const int kViewObjects = 2;   // bonus and crate objects: one set per view
const int kShinePoints = 64;  // a shine particle system's points a frame (the bonus tick's cap)

int CountHooks(CKGroup* g) {
	int n = 0;
	for (CKHook* h = g->childHook.get(); h; h = h->next.get()) n++;
	return n;
}

// Pools whose Romans come in numbers (the mod scales their squads' numbers):
// basic Romans, their leaders, archers and jumping Romans -- not the jetpack
// Romans, turtles or the mobile tower.
bool Scaled(CKHook* h) {
	if (!h) return false;
	int fid = (int)h->getClassFullID();
	return fid == GameX1::CKHkBasicEnemy::FULL_ID || fid == GameX1::CKHkBasicEnemyLeader::FULL_ID ||
	       fid == GameX1::CKHkRomanArcher::FULL_ID || fid == GameX1::CKHkJumpingRoman::FULL_ID;
}

// Clones `tmpl` into its group (the group's head) until the group holds `want`;
// `shared` objects stay as they are (referenced, not cloned). Each clone is
// handed to `fix`. Returns the hooks added.
template <class Fix>
int Fill(KEnvironment& kenv, CKGroup* group, CKHook* tmpl, int want, std::initializer_list<CKObject*> shared, Fix fix) {
	EditorUI::EditorInterface ui;
	int added = 0;
	for (int n = CountHooks(group); n < want; n++) {
		Duplicator dup(kenv, &ui);
		for (CKObject* o : shared)
			if (o) dup.keepShared(o);
		dup.doClone(tmpl);
		CKHook* clone = group->childHook.get();
		if (!clone || clone == tmpl) throw std::runtime_error("the clone is not at its group's head");
		fix(clone);
		added++;
	}
	return added;
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
                float factor, int* cloned, char* report, size_t reportSize, char* shine, size_t shineSize) {
	std::string r;
	if (shineSize) shine[0] = 0;
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
			int i = 0;
			for (CKObject* o : kenv.levelObjects.getClassType<CKGrpPoolSquad>().objects) {
				CKGrpPoolSquad* pool = o->cast<CKGrpPoolSquad>();
				CKHook* tmpl = pool->childHook.get();
				int n = CountHooks(pool);
				int want = (int)std::ceil(n * factor - 0.001f);
				if (Scaled(tmpl) && factor > 1.0f && want > n) {
					*cloned += Fill(kenv, pool, tmpl, want, {}, [](CKHook*) {});
					Add(r, " pool %d (%s) %d -> %d;", i, tmpl->getClassName(), n, CountHooks(pool));
				}
				i++;
			}
			// Bonuses (helmets, potions, hams...) and crates: objects handed to the
			// spots in view, so two views need twice the level's. A bonus pool's free
			// list is its nextBonusHook chain (nextBonus); crates and bonuses keep their
			// group's component.
			int bonuses = 0, crates = 0;
			auto bonusPools = { &kenv.levelObjects.getClassType<CKGrpBonusPool>().objects,
			                    &kenv.levelObjects.getClassType<CKGrpAsterixBonusPool>().objects };
			for (auto* list : bonusPools)
				for (CKObject* o : *list) {
					CKGrpBonusPool* pool = o->cast<CKGrpBonusPool>();
					CKHkBasicBonus* tmpl = pool->childHook.get() ? pool->childHook->dyncast<CKHkBasicBonus>() : nullptr;
					if (!tmpl) continue;
					int n = CountHooks(pool);
					bonuses += Fill(kenv, pool, tmpl, n * kViewObjects, { tmpl->cpnt.get(), pool->bonusCpnt.get() },
					                [pool, tmpl](CKHook* h) {
						CKHkBasicBonus* b = h->cast<CKHkBasicBonus>();
						b->cpnt = tmpl->cpnt;
						b->pool = pool;
						b->nextBonus = pool->nextBonusHook;
						pool->nextBonusHook = b;
					});
				}
			for (CKObject* o : kenv.levelObjects.getClassType<CKGrpCrate>().objects) {
				CKGroup* group = (CKGroup*)o;
				CKHkCrate* tmpl = group->childHook.get() ? group->childHook->dyncast<CKHkCrate>() : nullptr;
				if (!tmpl) continue;
				int n = CountHooks(group);
				crates += Fill(kenv, group, tmpl, n * kViewObjects, { tmpl->ckhcUnk1.get() },
				               [tmpl](CKHook* h) { h->cast<CKHkCrate>()->ckhcUnk1 = tmpl->ckhcUnk1; });
			}
			// A bonus pool's shine is one particle system (particleNode1) taking a point
			// per bonus each frame, at most 64: pools with more bonuses get copies of it,
			// one per 64 more (the mod's EmitShine moves on to them; their object numbers
			// in `shine`: "pool class id, pool index, particle system indexes;").
			std::string map;
			int shines = 0;
			{
				const int classIds[2] = { CKGrpBonusPool::CLASS_ID, CKGrpAsterixBonusPool::CLASS_ID };
				std::vector<std::pair<std::string, std::vector<CKSceneNode*>>> made;
				for (int c = 0; c < 2; c++) {
					auto& objs = c == 0 ? kenv.levelObjects.getClassType<CKGrpBonusPool>().objects
					                    : kenv.levelObjects.getClassType<CKGrpAsterixBonusPool>().objects;
					for (size_t pi = 0; pi < objs.size(); pi++) {
						CKGrpBonusPool* pool = objs[pi]->cast<CKGrpBonusPool>();
						CKSceneNode* sys = pool->particleNode1.get();
						int n = CountHooks(pool);
						if (!sys || !sys->dyncast<CParticlesNodeFx>() || n <= kShinePoints) continue;
						std::vector<CKSceneNode*> copies;
						for (int k = 1; k * kShinePoints < n; k++) {
							Duplicator dup(kenv, nullptr);
							copies.push_back(dup.doCloneNode(sys));
						}
						char key[32];
						snprintf(key, sizeof(key), "%d %zu", classIds[c], pi);
						made.push_back({ key, copies });
						shines += (int)copies.size();
					}
				}
				auto& parts = kenv.levelObjects.getClassType<CParticlesNodeFx>().objects;
				for (auto& m : made) {
					map += m.first;
					for (CKSceneNode* node : m.second) {
						auto it = std::find(parts.begin(), parts.end(), (CKObject*)node);
						if (it == parts.end()) throw std::runtime_error("a shine copy is not among the level's particle systems");
						char b[16];
						snprintf(b, sizeof(b), " %d", (int)(it - parts.begin()));
						map += b;
					}
					map += ";";
				}
			}
			snprintf(shine, shineSize, "%s", map.c_str());
			if (bonuses || crates) Add(r, " bonuses +%d, crates +%d, shines +%d;", bonuses, crates, shines);
			*cloned += shines;
			*cloned += bonuses + crates;
			if (*cloned > 0) {
				kenv.saveLevel(lvl);   // the level and its sectors (ids of their objects follow the level's)
			} else {
				std::filesystem::copy_file(conv / lvlfn, out / lvlfn, std::filesystem::copy_options::overwrite_existing);
				Add(r, " nothing to fill;");
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
