// romanforge stub of the editor's UI: the duplicator only records clone indexes in it.
#pragma once
#include <map>
struct CSGBranch;
namespace EditorUI {
struct EditorInterface {
	std::map<CSGBranch*, int> nodeCloneIndexMap;
};
}
