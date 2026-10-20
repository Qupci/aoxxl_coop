#include "crash.h"
#include "coop.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>

namespace {

const uintptr_t kTextStart = 0x401000, kTextEnd = 0x621000;   // axxl.exe .text

volatile long g_reports = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
DWORD g_gameThreadId = 0;

// "axxl.exe+0x9C228" or "xxl_coop.asi+0x1234" or the raw address.
void Describe(uintptr_t a, char* out, size_t n) {
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &mod) && mod) {
		char name[MAX_PATH] = {};
		GetModuleFileNameA(mod, name, MAX_PATH);
		const char* base = strrchr(name, '\\');
		_snprintf_s(out, n, _TRUNCATE, "%s+0x%X", base ? base + 1 : name,
			(unsigned)(a - (uintptr_t)mod));
	} else {
		_snprintf_s(out, n, _TRUNCATE, "%08X", (unsigned)a);
	}
}

// Return addresses on the stack that point into the game's code: a poor man's
// backtrace that needs no symbols and survives a corrupted frame chain.
void LogStack(const char* tag, uintptr_t esp) {
	char buf[512];
	LineBuf line(buf);
	line.Add("%s stack:", tag);
	int found = 0;
	__try {
		for (int i = 0; i < 256 && found < 12; i++) {
			uintptr_t v = ((uintptr_t*)esp)[i];
			if (v >= kTextStart && v < kTextEnd) {
				line.Add(" %06X", (unsigned)v);
				found++;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		line.Add(" (stack unreadable)");
	}
	Log("%s", line.c_str());
}

void LogHeroes(const char* tag) {
	__try {
		Log("%s co-op: p2=%s suspended=%d", tag,
			CoopP2Joined() ? HeroName(CoopP2Hero()) : "-", CoopSuspended());
		CoopLogState(tag);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Log("%s (hero state unreadable)", tag);
	}
}

bool IsFatal(DWORD code) {
	switch (code) {
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_STACK_OVERFLOW:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		case EXCEPTION_IN_PAGE_ERROR:
		case STATUS_STACK_BUFFER_OVERRUN:
			return true;
		default:
			return false;
	}
}

void Report(EXCEPTION_POINTERS* ep, const char* kind) {
	if (InterlockedIncrement(&g_reports) > 5) return;   // never flood the log
	EXCEPTION_RECORD* er = ep->ExceptionRecord;
	CONTEXT* c = ep->ContextRecord;
	char where[MAX_PATH];
	Describe((uintptr_t)er->ExceptionAddress, where, sizeof(where));
	Log("CRASH (%s): exception %08X at %s", kind, (unsigned)er->ExceptionCode, where);
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
		Log("CRASH   %s address %08X", er->ExceptionInformation[0] ? "writing" : "reading",
			(unsigned)er->ExceptionInformation[1]);
	Log("CRASH   eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X",
		(unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx,
		(unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp);
	LogStack("CRASH  ", c->Esp);
	LogHeroes("CRASH  ");
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
	// First-chance: the game may still handle it, so only fatal-looking codes
	// raised in the game's own code are reported, and the search continues.
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
	if (IsFatal(code) && at >= kTextStart && at < kTextEnd) Report(ep, "first chance");
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
	Report(ep, "unhandled");
	return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// Diagnostics: hardware write watchpoints (debug registers DR0..DR3) on up to
// four bytes in the game thread; every write is logged with the writing code.
// Set from the watchdog thread (a thread cannot reliably set its own DRs).
const void* volatile g_watchAddr[4] = {};
volatile long g_watchCount = 0, g_watchPending = 0, g_watchHits = 0;
uintptr_t g_watchIgnoreLo = 0, g_watchIgnoreHi = 0;
volatile long g_watchReads = 0;   // R/W = 11: reads too (WatchBytesRW)
volatile long g_watchJump = 0;    // WatchFloatJump
float g_watchLast = 0;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
	CONTEXT* c = ep->ContextRecord;
	DWORD hit = c->Dr6 & 0xF;
	if (!hit) return EXCEPTION_CONTINUE_SEARCH;
	c->Dr6 = 0;
	uintptr_t eip = c->Eip;
	if (g_watchJump) {   // a float that moves by more than 10 at once
		float now = *(const float*)g_watchAddr[0];
		float d = now - g_watchLast;
		g_watchLast = now;
		float lim = g_watchJump == 2 ? 0.02f : 10.0f;   // 2: any rise (a jump starting)
		if ((d > lim || (g_watchJump == 1 && d < -lim)) && InterlockedIncrement(&g_watchHits) < 50) {
			char where[96];
			Describe(eip, where, sizeof(where));
			Log("watch: %p jumped by %.1f to %.1f, by the instruction before %s", g_watchAddr[0], d, now, where);
			LogStack("watch", c->Esp);
		}
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if (g_watchReads) {   // read/write watch: each instruction once, no stack
		static uintptr_t seen[128];
		static int nSeen = 0;
		for (int i = 0; i < nSeen; i++) if (seen[i] == eip) return EXCEPTION_CONTINUE_EXECUTION;
		if (nSeen < 128) seen[nSeen++] = eip;
		char where[96];
		Describe(eip, where, sizeof(where));
		Log("watch: access to %p (now %02X) by the instruction before %s", g_watchAddr[0], *(const uint8_t*)g_watchAddr[0],
		    where);
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if (!(eip >= g_watchIgnoreLo && eip < g_watchIgnoreHi) && InterlockedIncrement(&g_watchHits) < 200) {
		for (int i = 0; i < 4; i++)
			if (hit & (1u << i)) {
				char where[96];
				Describe(eip, where, sizeof(where));
				Log("watch: write to %p (now %02X) by the instruction before %s", g_watchAddr[i],
				    *(const uint8_t*)g_watchAddr[i], where);
				LogStack("watch", c->Esp);
			}
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

void ApplyWatches() {
	if (!g_watchPending || !g_gameThreadId) return;
	g_watchPending = 0;
	HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_gameThreadId);
	if (!th) return;
	if (SuspendThread(th) != (DWORD)-1) {
		CONTEXT c = {};
		c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		if (GetThreadContext(th, &c)) {
			DWORD* dr[4] = { &c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3 };
			c.Dr7 = 0;
			for (int i = 0; i < g_watchCount && i < 4; i++) {
				*dr[i] = (DWORD)(uintptr_t)g_watchAddr[i];
				// local enable, R/W = 01 (write) or 11 (read/write), LEN = 00 (1 byte)
				c.Dr7 |= (1u << (i * 2)) | ((g_watchReads ? 3u : 1u) << (16 + i * 4));
			}
			SetThreadContext(th, &c);
			Log("watch: %ld write watchpoint(s) set", g_watchCount);
		}
		ResumeThread(th);
	}
	CloseHandle(th);
}

} // namespace

void WatchBytes(const void* const* addrs, int n, uintptr_t ignoreLo, uintptr_t ignoreHi) {
	for (int i = 0; i < n && i < 4; i++) g_watchAddr[i] = addrs[i];
	g_watchIgnoreLo = ignoreLo;
	g_watchIgnoreHi = ignoreHi;
	g_watchCount = n < 4 ? n : 4;
	g_watchReads = 0;
	g_watchJump = 0;
	g_watchPending = 1;
}

void WatchFloatJump(const float* addr) {
	g_watchAddr[0] = addr;
	g_watchLast = *addr;
	g_watchCount = 1;
	g_watchReads = 0;
	g_watchJump = 1;
	g_watchPending = 1;
}

void WatchFloatRise(const float* addr) {
	WatchFloatJump(addr);
	g_watchJump = 2;
}

void LogBacktrace(const char* tag, uintptr_t esp) { LogStack(tag, esp); }

void WatchBytesRW(const void* addr) {
	g_watchAddr[0] = addr;
	g_watchCount = 1;
	g_watchReads = 1;
	g_watchJump = 0;
	g_watchPending = 1;
}

void CrashHandlerInstall() {
	AddVectoredExceptionHandler(1, WatchHandler);
	AddVectoredExceptionHandler(1, VectoredHandler);
	g_prevFilter = SetUnhandledExceptionFilter(UnhandledFilter);
}

void StallRegisterGameThread() {
	if (!g_gameThreadId) g_gameThreadId = GetCurrentThreadId();
}

void StallCheck(long frames) {
	ApplyWatches();
	static long lastFrames = -1;
	static int  stillChecks = 0;
	static bool reported = false;
	if (frames != lastFrames) {
		if (reported) Log("stall: frames are running again");
		lastFrames = frames;
		stillChecks = 0;
		reported = false;
		return;
	}
	if (++stillChecks < 3 || reported || !g_gameThreadId) return;
	reported = true;

	HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
	                       FALSE, g_gameThreadId);
	if (!th) { Log("stall: no frames for 6 s (cannot open the game thread)"); return; }
	if (SuspendThread(th) != (DWORD)-1) {
		CONTEXT c = {};
		c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		if (GetThreadContext(th, &c)) {
			char where[MAX_PATH];
			Describe(c.Eip, where, sizeof(where));
			Log("stall: no frames for 6 s; game thread at %s (eip=%08X esp=%08X)", where,
				(unsigned)c.Eip, (unsigned)c.Esp);
			LogStack("stall:", c.Esp);
		}
		ResumeThread(th);
	}
	CloseHandle(th);
	LogHeroes("stall:");
	StallDumpThreads();
}

// Every thread of the process: where it is, the return addresses on its stack in
// any module, and -- for the game thread -- critical sections on its stack that
// another thread owns (a freeze with the game thread in
// NtWaitForAlertByThreadId is a wait on a lock; this names who holds it).
// Each thread is suspended only to copy its registers and stack; the names are
// looked up after it runs again (that takes the loader lock, which a suspended
// thread could hold).
void StallDumpThreads() {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE) return;
	DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
	DWORD ids[64];
	int nIds = 0;
	THREADENTRY32 te = { sizeof(te) };
	for (BOOL ok = Thread32First(snap, &te); ok && nIds < 64; ok = Thread32Next(snap, &te))
		if (te.th32OwnerProcessID == pid) ids[nIds++] = te.th32ThreadID;
	CloseHandle(snap);
	static uintptr_t stack[1024];
	for (int t = 0; t < nIds; t++) {
		if (ids[t] == self) continue;
		HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, ids[t]);
		if (!h) continue;
		CONTEXT c = {};
		c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		int n = 0;
		bool got = false;
		if (SuspendThread(h) != (DWORD)-1) {
			got = GetThreadContext(h, &c) != 0;
			if (got) {
				__try {
					for (; n < 1024; n++) stack[n] = ((uintptr_t*)c.Esp)[n];
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			ResumeThread(h);
		}
		CloseHandle(h);
		if (!got) continue;
		char where[MAX_PATH];
		Describe(c.Eip, where, sizeof(where));
		char buf[1024];
		LineBuf line(buf);
		line.Add("stall: thread %lu%s at %s; stack:", ids[t], ids[t] == g_gameThreadId ? " (game)" : "", where);
		int found = 0;
		for (int i = 0; i < n && found < 20; i++) {
			MEMORY_BASIC_INFORMATION mbi;
			if (stack[i] < 0x10000 || !VirtualQuery((void*)stack[i], &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
			    !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
				continue;
			char d[MAX_PATH];
			Describe(stack[i], d, sizeof(d));
			line.Add(" %s", d);
			found++;
		}
		Log("%s", line.c_str());
		if (ids[t] != g_gameThreadId) continue;
		for (int i = 0; i < 64 && i < n; i++) {
			RTL_CRITICAL_SECTION* cs = (RTL_CRITICAL_SECTION*)stack[i];
			__try {
				DWORD owner = (DWORD)(uintptr_t)cs->OwningThread;
				if (!owner || owner == g_gameThreadId || cs->RecursionCount <= 0 || cs->RecursionCount > 1000) continue;
				bool known = false;
				for (int k = 0; k < nIds; k++) known |= ids[k] == owner;
				if (!known) continue;
				char d[MAX_PATH];
				Describe((uintptr_t)cs, d, sizeof(d));
				Log("stall: the game thread's stack names critical section %s, held by thread %lu (recursion %ld)",
				    d, owner, cs->RecursionCount);
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}
	}
}

// Diagnostics (env XXL_PROFILE): samples the game thread every ~1 ms and, every
// 10 s, logs where its time went: the module it was in, and the innermost game
// code return address on its stack (which game call led there). Suspends the
// thread only to copy registers and stack, as StallDumpThreads does.
namespace {

struct ProfBucket { uintptr_t base; uintptr_t gameRet; long hits; };

DWORD WINAPI ProfileThread(LPVOID) {
	static ProfBucket b[512];
	int nb = 0;
	long total = 0, reports = 0;
	DWORD start = GetTickCount();
	while (reports < 12) {
		Sleep(1);
		if (!g_gameThreadId) continue;
		HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_gameThreadId);
		if (!h) continue;
		CONTEXT c = {};
		c.ContextFlags = CONTEXT_CONTROL;
		uintptr_t stack[256];
		int n = 0;
		bool got = false;
		if (SuspendThread(h) != (DWORD)-1) {
			got = GetThreadContext(h, &c) != 0;
			if (got) {
				__try {
					for (; n < 256; n++) stack[n] = ((uintptr_t*)c.Esp)[n];
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			ResumeThread(h);
		}
		CloseHandle(h);
		if (!got) continue;
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t base = VirtualQuery((void*)(uintptr_t)c.Eip, &mbi, sizeof(mbi)) ? (uintptr_t)mbi.AllocationBase : 0;
		uintptr_t gameRet = c.Eip >= kTextStart && c.Eip < kTextEnd ? c.Eip : 0;
		for (int i = 0; i < n && !gameRet; i++)
			if (stack[i] >= kTextStart && stack[i] < kTextEnd) gameRet = stack[i];
		if (base == 0x400000) gameRet &= ~0xFFu;   // game code itself: by 256-byte block
		int k = 0;
		while (k < nb && (b[k].base != base || b[k].gameRet != gameRet)) k++;
		if (k == nb && nb < 512) b[nb++] = { base, gameRet, 0 };
		if (k < nb) b[k].hits++;
		total++;
		if (GetTickCount() - start < 10000) continue;
		// Per module first, then the top call sites.
		uintptr_t mods[32]; long modHits[32]; int nm = 0;
		for (int i = 0; i < nb; i++) {
			int m = 0;
			while (m < nm && mods[m] != b[i].base) m++;
			if (m == nm && nm < 32) { mods[nm] = b[i].base; modHits[nm++] = 0; }
			if (m < nm) modHits[m] += b[i].hits;
		}
		char buf[1024];
		LineBuf line(buf);
		line.Add("profile: %ld samples; modules:", total);
		for (int m = 0; m < nm; m++) {
			char name[MAX_PATH] = "?";
			if (mods[m]) GetModuleFileNameA((HMODULE)mods[m], name, MAX_PATH);
			const char* s = strrchr(name, '\\');
			if (modHits[m] * 100 >= total) line.Add(" %s %.0f%%", s ? s + 1 : name, modHits[m] * 100.0 / total);
		}
		Log("%s", line.c_str());
		for (int top = 0; top < 15; top++) {
			int best = -1;
			for (int i = 0; i < nb; i++)
				if (b[i].hits > 0 && (best < 0 || b[i].hits > b[best].hits)) best = i;
			if (best < 0 || b[best].hits * 200 < total) break;
			char name[MAX_PATH] = "?";
			if (b[best].base) GetModuleFileNameA((HMODULE)b[best].base, name, MAX_PATH);
			const char* s = strrchr(name, '\\');
			Log("profile:   %5.1f%% in %s, game code %06X", b[best].hits * 100.0 / total, s ? s + 1 : name,
			    (unsigned)b[best].gameRet);
			b[best].hits = -b[best].hits;
		}
		nb = 0;
		total = 0;
		reports++;
		start = GetTickCount();
	}
	return 0;
}

} // namespace

void ProfileStart() {
	if (getenv("XXL_PROFILE")) CloseHandle(CreateThread(nullptr, 0, ProfileThread, nullptr, 0, nullptr));
}
