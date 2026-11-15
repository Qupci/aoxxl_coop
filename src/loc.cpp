#include "loc.h"
#include "config.h"
#include "kal.h"
#include "log.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

const int kLanguages = 5;
const char* const kSection[kLanguages] = { "French", "English", "German", "Spanish", "Italian" };
const int kEnglish = 1;
const DWORD kCheckMs = 500;   // how often the file's time is looked at

std::unordered_map<std::string, std::wstring> g_text[kLanguages];
bool     g_loaded = false;
FILETIME g_fileTime = {};
DWORD    g_checkedAt = 0;
int      g_forced = -2;   // -2: XXL_LANG not read yet; -1: the game's language

void PathOf(char* out, size_t n) {
	// Next to xxl_coop.ini (its full path), else the game's folder.
	const char* ini = g_config.iniPath;
	const char* slash = ini ? strrchr(ini, '\\') : nullptr;
	if (slash) snprintf(out, n, "%.*s\\xxl_coop_lang.ini", (int)(slash - ini), ini);
	else snprintf(out, n, "xxl_coop_lang.ini");
}

std::wstring Trim(const std::wstring& s) {
	size_t a = s.find_first_not_of(L" \t\r"), b = s.find_last_not_of(L" \t\r");
	return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

// A value: quotes keep its outer spaces; "\n" a new line, "\\" a backslash.
std::wstring Unescape(std::wstring v) {
	if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
	std::wstring out;
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] == L'\\' && i + 1 < v.size()) {
			wchar_t c = v[++i];
			out += c == L'n' ? L'\n' : c;
			continue;
		}
		out += v[i];
	}
	return out;
}

void Load(const char* path) {
	for (auto& m : g_text) m.clear();
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) { Log("loc: %s not found -- the texts show as their keys", path); return; }
	DWORD size = GetFileSize(f, nullptr), got = 0;
	std::string bytes(size, '\0');
	if (size && !ReadFile(f, &bytes[0], size, &got, nullptr)) got = 0;
	CloseHandle(f);
	bytes.resize(got);
	if (bytes.size() >= 3 && (uint8_t)bytes[0] == 0xEF && (uint8_t)bytes[1] == 0xBB && (uint8_t)bytes[2] == 0xBF) bytes.erase(0, 3);
	int wn = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
	std::wstring w(wn, L'\0');
	if (wn) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &w[0], wn);
	int lang = -1, keys = 0;
	size_t at = 0;
	while (at <= w.size()) {
		size_t end = w.find(L'\n', at);
		if (end == std::wstring::npos) end = w.size();
		std::wstring line = Trim(w.substr(at, end - at));
		at = end + 1;
		if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
		if (line[0] == L'[') {
			size_t close = line.find(L']');
			std::wstring name = line.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
			lang = -1;
			for (int i = 0; i < kLanguages; i++) {
				std::wstring s(kSection[i], kSection[i] + strlen(kSection[i]));
				if (_wcsicmp(name.c_str(), s.c_str()) == 0) lang = i;
			}
			if (lang < 0) Log("loc: unknown section [%S] skipped", name.c_str());
			continue;
		}
		size_t eq = line.find(L'=');
		if (lang < 0 || eq == std::wstring::npos) continue;
		std::wstring key = Trim(line.substr(0, eq));
		std::string k;
		for (wchar_t c : key) k += (char)c;   // (keys are ASCII)
		g_text[lang][k] = Unescape(Trim(line.substr(eq + 1)));
		keys++;
	}
	Log("loc: %s -- %d texts (French %u, English %u, German %u, Spanish %u, Italian %u)", path, keys,
	    (unsigned)g_text[0].size(), (unsigned)g_text[1].size(), (unsigned)g_text[2].size(), (unsigned)g_text[3].size(),
	    (unsigned)g_text[4].size());
}

// Loaded at the first text, again when the file changes.
void Refresh() {
	DWORD now = GetTickCount();
	if (g_loaded && now - g_checkedAt < kCheckMs) return;
	g_checkedAt = now;
	char path[MAX_PATH];
	PathOf(path, sizeof(path));
	WIN32_FILE_ATTRIBUTE_DATA a = {};
	FILETIME t = {};
	if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) t = a.ftLastWriteTime;
	if (g_loaded && CompareFileTime(&t, &g_fileTime) == 0) return;
	if (g_loaded) Log("loc: %s changed -- read again", path);
	g_fileTime = t;
	g_loaded = true;
	Load(path);
}

int GameLanguage() {
	CKYellowPages* yp = YellowPages();
	char* ym = yp ? *(char**)((char*)yp + 0x94) : nullptr;
	if (!ym || IsBadReadPtr(ym + 0x24, 2)) return kEnglish;
	int l = *(short*)(ym + 0x24);
	return l >= 0 && l < kLanguages ? l : kEnglish;
}

} // namespace

int LocLanguage() {
	if (g_forced == -2) {
		const char* e = getenv("XXL_LANG");
		g_forced = e && *e ? atoi(e) : -1;
		if (g_forced >= kLanguages) g_forced = -1;
		if (g_forced >= 0) Log("loc: texts in %s (XXL_LANG)", kSection[g_forced]);
	}
	int l = g_forced >= 0 ? g_forced : GameLanguage();
	static int told = -1;
	if (l != told) { told = l; Log("loc: the texts' language is %s", kSection[l]); }
	return l;
}

void LocForceLanguage(int lang) {
	g_forced = lang >= 0 && lang < kLanguages ? lang : -1;
	Log("loc: texts in %s", g_forced >= 0 ? kSection[g_forced] : "the game's language");
}

const wchar_t* Loc(const char* key) {
	Refresh();
	int l = LocLanguage();
	auto it = g_text[l].find(key);
	if (it != g_text[l].end()) return it->second.c_str();
	it = g_text[kEnglish].find(key);
	static int logs = 0;
	if (logs < 40) {
		logs++;
		Log("loc: \"%s\" missing in [%s]%s", key, kSection[l], it != g_text[kEnglish].end() ? " -- the English one" : " and [English]");
	}
	if (it != g_text[kEnglish].end()) return it->second.c_str();
	// Kept for good: the key as the text.
	static std::unordered_map<std::string, std::wstring> s_keys;
	std::wstring& w = s_keys[key];
	if (w.empty()) w.assign(key, key + strlen(key));
	return w.c_str();
}

void LocFormat(wchar_t* out, int n, const char* key, std::initializer_list<LocArg> args) {
	if (!out || n <= 0) return;
	const wchar_t* s = Loc(key);
	int k = 0;
	for (const wchar_t* c = s; *c && k < n - 1; c++) {
		if (*c == L'{') {
			const wchar_t* close = wcschr(c, L'}');
			if (close) {
				std::string name;
				for (const wchar_t* q = c + 1; q < close; q++) name += (char)*q;
				const wchar_t* value = nullptr;
				for (const LocArg& a : args)
					if (name == a.name) value = a.value;
				if (value) {
					for (const wchar_t* v = value; *v && k < n - 1; v++) out[k++] = *v;
					c = close;
					continue;
				}
			}
		}
		out[k++] = *c;
	}
	out[k] = 0;
}
