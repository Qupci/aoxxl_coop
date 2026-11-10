// romanforge: the few editor pieces the level code calls, without the GUI.
#include "GuiUtils.h"
#include "Encyclopedia.h"
#include "NodeCollisionUtils.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <malloc.h>

std::string GuiUtils::latinToUtf8(std::string_view text) {
	int ws = MultiByteToWideChar(1252, 0, text.data(), (int)text.size(), NULL, 0);
	std::wstring w(ws, L'\0');
	MultiByteToWideChar(1252, 0, text.data(), (int)text.size(), w.data(), ws);
	int us = WideCharToMultiByte(CP_UTF8, 0, w.data(), ws, NULL, 0, NULL, NULL);
	std::string u(us, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), ws, u.data(), us, NULL, NULL);
	return u;
}
std::string GuiUtils::utf8ToLatin(std::string_view text) {
	int ws = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), NULL, 0);
	std::wstring w(ws, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), w.data(), ws);
	int ls = WideCharToMultiByte(1252, 0, w.data(), ws, NULL, 0, "?", NULL);
	std::string l(ls, '\0');
	WideCharToMultiByte(1252, 0, w.data(), ws, l.data(), ls, "?", NULL);
	return l;
}
void NodeCollisionUtils::LoadNodeCollisionInfo(int) {}
void NodeCollisionUtils::CreateCollisionsForObject(KEnvironment&, CKObject*) {}
const nlohmann::json* Encyclopedia::getClassJson(int) { return nullptr; }
#include "Image.h"
// Texture conversion (XXL2+ / export only): never reached for XXL1 PC levels.
Image Image::convertToRGBA32() const { return *this; }
