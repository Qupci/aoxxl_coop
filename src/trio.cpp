#include "trio.h"

Trio GetTrio() {
	Trio t;
	if (!KalLevelIsLoaded()) return t;
	t.grp = (KGroup*)GetLvlObject(ax::kCKGrpTrio, 0);
	return t;
}

const char* HeroName(int idx) {
	switch (idx) {
		case HERO_ASTERIX: return "Asterix";
		case HERO_OBELIX:  return "Obelix";
		case HERO_IDEFIX:  return "Idefix";
		default:           return "?";
	}
}

const char* AiOrderName(int order) {
	// Mirrors the engine's own table at 0x64CBD8, translated.
	static const char* const kNames[16] = {
		"follow leader", "go to position", "jump to position", "wait",
		"hit an enemy", "throw an enemy", "bite an enemy", "go to ballista",
		"hang on beam", "use cable car", "take a hit", "clear path",
		"under external control", "go to hint man", "go to shop",
		"go to position (no pathfinding)",
	};
	return (order >= 0 && order < 16) ? kNames[order] : "?";
}
