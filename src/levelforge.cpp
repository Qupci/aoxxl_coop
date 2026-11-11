#include "levelforge.h"
#include "config.h"
#include "log.h"
#include "../romanforge/forge_lib.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Bumped when what the forge writes changes: rebuilds every cached level.
const char* const kForgeVersion = "romanforge 2";   // 2: archers, jumping Romans, bonuses and crates
const int kLevels = 9;

std::vector<unsigned char> g_exe;
std::string g_gameDir, g_cacheDir;
float g_factor = 1.0f;
CRITICAL_SECTION g_lock;   // one level built at a time (the editor's code keeps globals)

enum { LV_PENDING = 0, LV_WORKING = 1, LV_DONE = 2 };
struct Level {
	volatile LONG state = LV_PENDING;
	HANDLE done = nullptr;
	bool ok = false;      // a rebuilt level file to serve
	int cloned = 0;       // objects added (its sectors rebuilt too)
};
Level g_lv[kLevels];

typedef HANDLE(WINAPI* CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_origCreateFileA = nullptr;

// What a cached level was built from: the forge, the factor, the game module
// and the level's own files (sizes, times).
std::string LevelKey(int lvl) {
	char b[160];
	snprintf(b, sizeof(b), "%s factor %.3f exe %u", kForgeVersion, g_factor, (unsigned)g_exe.size());
	std::string k = b;
	char dir[32];
	snprintf(dir, sizeof(dir), "LVL%03d", lvl);
	std::error_code ec;
	std::vector<std::string> names;
	for (const auto& e : fs::directory_iterator(fs::u8path(g_gameDir) / dir, ec)) {
		std::string n = e.path().filename().u8string();
		if (n.size() > 4 && _stricmp(n.c_str() + n.size() - 4, ".KWN") == 0 &&
		    (_strnicmp(n.c_str(), "LVL", 3) == 0 || _strnicmp(n.c_str(), "STR", 3) == 0))
			names.push_back(n);
	}
	std::sort(names.begin(), names.end());
	for (const std::string& n : names) {
		WIN32_FILE_ATTRIBUTE_DATA a;
		std::string p = g_gameDir + "\\" + dir + "\\" + n;
		if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a)) continue;
		snprintf(b, sizeof(b), " %s %u %08X%08X", n.c_str(), (unsigned)a.nFileSizeLow, (unsigned)a.ftLastWriteTime.dwHighDateTime,
		         (unsigned)a.ftLastWriteTime.dwLowDateTime);
		k += b;
	}
	return k;
}

std::string KeyPath(int lvl) {
	char b[64];
	snprintf(b, sizeof(b), "\\LVL%03d\\forge.key", lvl);
	return g_cacheDir + b;
}

// A level already built from the same files: served as it is.
bool CacheValid(int lvl, int* cloned) {
	FILE* f = fopen(KeyPath(lvl).c_str(), "rb");
	if (!f) return false;
	char buf[4096] = {};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	char* nl = strchr(buf, '\n');
	if (!nl) return false;
	*nl = 0;
	if (LevelKey(lvl) != buf) return false;
	*cloned = atoi(nl + 1);
	char lvlFile[64];
	snprintf(lvlFile, sizeof(lvlFile), "\\LVL%03d\\LVL%02d.KWN", lvl, lvl);
	return GetFileAttributesA((g_cacheDir + lvlFile).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ForgeSafe(int lvl, const char* outDir, float factor, int* cloned, char* report, size_t reportSize) {
	__try {
		return ForgeLevel(g_exe.data(), g_exe.size(), g_gameDir.c_str(), outDir, lvl, factor, cloned, report, reportSize);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		snprintf(report, reportSize, " crashed (%08X)", (unsigned)GetExceptionCode());
		return false;
	}
}

void Build(int lvl) {
	Level& L = g_lv[lvl];
	EnterCriticalSection(&g_lock);
	DWORD t0 = GetTickCount();
	if (CacheValid(lvl, &L.cloned)) {
		L.ok = true;
		Log("levelforge: level %d from the cache (%d objects added)", lvl, L.cloned);
		LeaveCriticalSection(&g_lock);
		return;
	}
	char dir[32];
	snprintf(dir, sizeof(dir), "LVL%03d", lvl);
	fs::path cache = fs::u8path(g_cacheDir), tmp = cache / "build";
	std::error_code ec;
	char report[2048] = {};
	int cloned = 0;
	bool ok = false;
	for (float f : { g_factor, 1.0f }) {   // failing that, just converted (the module reads no other format)
		fs::remove_all(tmp, ec);
		fs::create_directories(tmp, ec);
		cloned = 0;
		ok = ForgeSafe(lvl, tmp.u8string().c_str(), f, &cloned, report, sizeof(report));
		Log("levelforge: level %d built (factor %.2f) in %u ms: %s%s", lvl, f, GetTickCount() - t0, ok ? "" : "FAILED", report);
		if (ok || f == 1.0f) break;
	}
	if (ok) {
		fs::remove_all(cache / dir, ec);
		fs::rename(tmp / dir, cache / dir, ec);
		if (ec) {
			Log("levelforge: level %d: moving it into the cache failed (%s)", lvl, ec.message().c_str());
			ok = false;
		} else {
			FILE* f = fopen(KeyPath(lvl).c_str(), "wb");
			if (f) {
				fprintf(f, "%s\n%d\n", LevelKey(lvl).c_str(), cloned);
				fclose(f);
			}
		}
	}
	fs::remove_all(tmp, ec);
	L.ok = ok;
	L.cloned = ok ? cloned : 0;
	LeaveCriticalSection(&g_lock);
}

// Builds level `lvl` here unless it is built (or being built: waits for it).
void EnsureLevel(int lvl) {
	Level& L = g_lv[lvl];
	if (L.state == LV_DONE) return;
	if (InterlockedCompareExchange(&L.state, LV_WORKING, LV_PENDING) == LV_PENDING) {
		Build(lvl);
		InterlockedExchange(&L.state, LV_DONE);
		SetEvent(L.done);
		return;
	}
	DWORD t0 = GetTickCount();
	WaitForSingleObject(L.done, INFINITE);
	Log("levelforge: waited %u ms for level %d to be built", GetTickCount() - t0, lvl);
}

DWORD WINAPI Worker(LPVOID) {
	int order[kLevels + 1] = { 0, g_config.forceLevel, 1, 2, 3, 4, 5, 6, 7, 8 };
	for (int lvl : order)
		if (lvl >= 0 && lvl < kLevels) EnsureLevel(lvl);
	Log("levelforge: all levels ready");
	return 0;
}

// "...LVL004\LVL04.KWN" / "...LVL004\STR04_03.KWN": the level and the file name.
bool ParseLevelPath(const char* path, int* lvl, const char** file) {
	const char* best = nullptr;
	for (const char* p = path; *p; p++)
		if (_strnicmp(p, "LVL", 3) == 0 && isdigit((unsigned char)p[3]) && isdigit((unsigned char)p[4]) &&
		    isdigit((unsigned char)p[5]) && (p[6] == '\\' || p[6] == '/'))
			best = p;
	if (!best) return false;
	int l = (best[3] - '0') * 100 + (best[4] - '0') * 10 + (best[5] - '0');
	const char* f = best + 7;
	if (l < 0 || l >= kLevels || strchr(f, '\\') || strchr(f, '/')) return false;
	int a = -1, b = -1;
	char ext[8] = {};
	bool lvlFile = sscanf(f, "%*1[Ll]%*1[Vv]%*1[Ll]%2d.%4s", &a, ext) == 2 && a == l;
	bool strFile = !lvlFile && sscanf(f, "%*1[Ss]%*1[Tt]%*1[Rr]%2d_%2d.%4s", &a, &b, ext) == 3 && a == l;
	if ((!lvlFile && !strFile) || _stricmp(ext, "KWN") != 0) return false;
	*lvl = l;
	*file = f;
	return true;
}

HANDLE WINAPI CreateFileAHook(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                              DWORD flags, HANDLE tmpl) {
	int lvl = -1;
	const char* file = nullptr;
	static int s_seen = 0;
	if (getenv("XXL_FORGEOPENS") && s_seen++ < 200) Log("levelforge: open %s", name ? name : "(null)");
	if (name && ParseLevelPath(name, &lvl, &file)) {
		EnsureLevel(lvl);
		const Level& L = g_lv[lvl];
		bool isLvl = _strnicmp(file, "LVL", 3) == 0;
		if (L.ok && (isLvl || L.cloned > 0)) {
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s\\LVL%03d\\%s", g_cacheDir.c_str(), lvl, file);
			HANDLE h = g_origCreateFileA(path, access, share, sa, disp, flags, tmpl);
			static int s_told = 0;
			if (isLvl && s_told++ < 40)
				Log("levelforge: %s -> %s (%s)", name, path, h != INVALID_HANDLE_VALUE ? "opened" : "FAILED");
			if (h != INVALID_HANDLE_VALUE) return h;
		} else if (isLvl) {
			Log("levelforge: level %d has no rebuilt file -- the original is read (the game will refuse it)", lvl);
		}
	}
	return g_origCreateFileA(name, access, share, sa, disp, flags, tmpl);
}

// The game's calls through its import of kernel32!CreateFileA (call/jmp
// [slot]) are pointed at g_hookPtr instead: the ASI loader rewrites the import
// slot itself after our DllMain (its own file overrides), calling kernel32
// straight on -- a hook in the slot never saw an open.
void* g_hookPtr = nullptr;

bool HookCreateFileA() {
	char* base = (char*)GetModuleHandleA(nullptr);
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
		if (_stricmp(base + d->Name, "KERNEL32.DLL") != 0 || !d->OriginalFirstThunk) continue;
		IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk);
		IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((const char*)ibn->Name, "CreateFileA") != 0) continue;
			g_origCreateFileA = (CreateFileAFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileA");
			g_hookPtr = (void*)CreateFileAHook;
			uint32_t slot = (uint32_t)(uintptr_t)&slots->u1.Function, mine = (uint32_t)(uintptr_t)&g_hookPtr;
			IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
			int sites = 0;
			for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
				if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
				unsigned char* p = (unsigned char*)base + sec->VirtualAddress;
				size_t size = sec->Misc.VirtualSize;
				for (size_t k = 0; k + 6 <= size; k++) {
					if (p[k] != 0xFF || (p[k + 1] != 0x15 && p[k + 1] != 0x25) || memcmp(p + k + 2, &slot, 4) != 0) continue;
					DWORD old;
					VirtualProtect(p + k + 2, 4, PAGE_EXECUTE_READWRITE, &old);
					memcpy(p + k + 2, &mine, 4);
					VirtualProtect(p + k + 2, 4, old, &old);
					sites++;
				}
			}
			Log("levelforge: %d calls to CreateFileA go through the level redirect", sites);
			return g_origCreateFileA && sites > 0;
		}
	}
	return false;
}

} // namespace

bool LevelForgePoolsFilled(int level) {
	return g_factor > 1.0f && level >= 0 && level < kLevels && g_lv[level].state == LV_DONE && g_lv[level].ok &&
	       g_lv[level].cloned > 0;
}

void LevelForgeInstall() {
	if (!g_config.rebuiltLevels) {
		Log("levelforge: off (rebuilt_levels = off): the game's own level files -- Romans at once as their pools allow, "
		    "crates and bonuses as the levels have them");
		return;
	}
	g_factor = g_config.squadAliveFactor > 1.0f ? g_config.squadAliveFactor : 1.0f;
	char exePath[MAX_PATH];
	GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
	FILE* f = fopen(exePath, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		g_exe.resize(n > 0 ? n : 0);
		if (n > 0 && fread(g_exe.data(), 1, n, f) != (size_t)n) g_exe.clear();
		fclose(f);
	}
	if (g_exe.empty() || !ForgeModuleKnown(g_exe.data(), g_exe.size())) {
		Log("levelforge: %s is not the game module the level conversion knows -- the levels as they are", exePath);
		g_exe.clear();
		return;
	}
	ForgePatch patches[16];
	size_t np = ForgeModulePatches(patches, 16);
	for (size_t i = 0; i < np; i++) {   // the bytes in memory must be the file's (no other patch there)
		const ForgePatch& p = patches[i];
		if (p.offset + p.size > g_exe.size() || memcmp((void*)(0x400000 + p.offset), g_exe.data() + p.offset, p.size) != 0) {
			Log("levelforge: the game module differs at %08X from %s -- the levels as they are",
			    (unsigned)(0x400000 + p.offset), exePath);
			g_exe.clear();
			return;
		}
	}
	fs::path exeDir = fs::u8path(exePath).parent_path();
	g_gameDir = exeDir.u8string();
	g_cacheDir = (exeDir / "coop_levels").u8string();
	std::error_code ec;
	fs::create_directories(fs::u8path(g_cacheDir), ec);
	InitializeCriticalSection(&g_lock);
	for (Level& L : g_lv) L.done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!HookCreateFileA()) {
		Log("levelforge: kernel32!CreateFileA not imported by the game module -- the levels as they are");
		return;
	}
	for (size_t i = 0; i < np; i++) {
		const ForgePatch& p = patches[i];
		DWORD old;
		void* at = (void*)(0x400000 + p.offset);
		VirtualProtect(at, p.size, PAGE_EXECUTE_READWRITE, &old);
		memcpy(at, p.bytes, p.size);
		VirtualProtect(at, p.size, old, &old);
	}
	FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
	Log("levelforge: Roman pools (basic Romans, leaders, archers, jumping Romans) filled up to x%.2f, bonuses and "
	    "crates x2; the levels are read from %s "
	    "(the module reads XXL-Editor's format: %u patches)", g_factor, g_cacheDir.c_str(), (unsigned)np);
	CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
}
