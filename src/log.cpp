#include "log.h"
#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>

static FILE*            g_log = nullptr;
static CRITICAL_SECTION g_logLock;
static bool             g_lockReady = false;
static DWORD            g_logStart = 0;

void LogOpen() {
	if (g_log) return;
	if (!g_lockReady) { InitializeCriticalSection(&g_logLock); g_lockReady = true; }
	// Deny other writers: a second copy of the game (the exe exits on its own
	// once it finds the first window, but the ASI loads before that) must not
	// truncate the log of the one that is actually running.
	g_log = _fsopen("xxl_coop.log", "w", _SH_DENYWR);
	if (!g_log) g_log = _fsopen("xxl_coop_2.log", "w", _SH_DENYWR);
	g_logStart = GetTickCount();
	if (g_log) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(g_log, "=== XXL co-op mod log, %04d-%02d-%02d %02d:%02d:%02d ===\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		fflush(g_log);
	}
}

void LogClose() {
	if (!g_lockReady) return;
	EnterCriticalSection(&g_logLock);
	if (g_log) { fclose(g_log); g_log = nullptr; }
	LeaveCriticalSection(&g_logLock);
}

// Every line is stamped with seconds since startup, and written under a lock:
// the watchdog thread and the game thread both log, and a line split across
// the two is worse than useless when reconstructing what happened in a frame.
static void vlog(const char* fmt, va_list ap) {
	if (!g_log || !g_lockReady) return;
	char line[1024];
	DWORD ms = GetTickCount() - g_logStart;
	int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%4lu.%03lu] ", ms / 1000, ms % 1000);
	if (n < 0) n = 0;
	_vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
	EnterCriticalSection(&g_logLock);
	if (g_log) {
		fputs(line, g_log);
		fputc('\n', g_log);
		fflush(g_log);  // the game can hard-crash; never buffer
	}
	LeaveCriticalSection(&g_logLock);
}

void Log(const char* fmt, ...) {
	va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap);
}

void LogV(const char* fmt, ...) {
	if (!g_config.verbose) return;
	va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap);
}

void LineBuf::Add(const char* fmt, ...) {
	if (truncated) return;
	// Leave room for the " ..." marker so a truncated line still says so.
	const size_t reserve = 5;
	if (len + reserve >= cap) { truncated = true; return; }

	va_list ap;
	va_start(ap, fmt);
	int written = _vsnprintf_s(buf + len, cap - len - reserve,
	                           _TRUNCATE, fmt, ap);
	va_end(ap);

	if (written < 0) {
		truncated = true;
		strcpy(buf + len, " ...");
		len += 4;
		return;
	}
	len += (size_t)written;
}
