#include "log.h"
#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static FILE* g_log = nullptr;

void LogOpen() {
	if (g_log) return;
	g_log = fopen("xxl_coop.log", "w");
	if (g_log) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(g_log, "=== XXL co-op mod log, %04d-%02d-%02d %02d:%02d:%02d ===\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		fflush(g_log);
	}
}

void LogClose() {
	if (g_log) { fclose(g_log); g_log = nullptr; }
}

static void vlog(const char* fmt, va_list ap) {
	if (!g_log) return;
	vfprintf(g_log, fmt, ap);
	fputc('\n', g_log);
	fflush(g_log);  // the game can hard-crash; never buffer
}

void Log(const char* fmt, ...) {
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

void LogV(const char* fmt, ...) {
	if (!g_config.verbose) return;
	va_list ap; va_start(ap, fmt); vlog(fmt, ap); va_end(ap);
}
