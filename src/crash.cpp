#include "crash.h"
#include "coop.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <cstdio>

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

} // namespace

void CrashHandlerInstall() {
	AddVectoredExceptionHandler(1, VectoredHandler);
	g_prevFilter = SetUnhandledExceptionFilter(UnhandledFilter);
}

void StallRegisterGameThread() {
	if (!g_gameThreadId) g_gameThreadId = GetCurrentThreadId();
}

void StallCheck(long frames) {
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
}
