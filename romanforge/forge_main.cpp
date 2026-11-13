// romanforge test tool: builds one level as the co-op mod does in the game
// (forge_lib.h: converted, Roman pools filled up to factor times).
//   romanforge <game dir> <out dir> <level> [factor]
#include "KEnvironment.h"
#include "ClassRegister.h"
#include "Duplicator.h"
#include "EditorInterface.h"
#include "GamePatcher.h"
#include "CoreClasses/CKGroup.h"
#include "CoreClasses/CKHook.h"
#include "CoreClasses/CKService.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <filesystem>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI Crash(EXCEPTION_POINTERS* ep) {
	HANDLE p = GetCurrentProcess(), t = GetCurrentThread();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize(p, NULL, TRUE);
	CONTEXT c = *ep->ContextRecord;
	STACKFRAME64 f = {};
	f.AddrPC.Offset = c.Eip; f.AddrPC.Mode = AddrModeFlat;
	f.AddrFrame.Offset = c.Ebp; f.AddrFrame.Mode = AddrModeFlat;
	f.AddrStack.Offset = c.Esp; f.AddrStack.Mode = AddrModeFlat;
	printf("CRASH %08X at %08X (addr %p)\n", ep->ExceptionRecord->ExceptionCode, c.Eip,
	       ep->ExceptionRecord->NumberParameters > 1 ? (void*)ep->ExceptionRecord->ExceptionInformation[1] : nullptr);
	for (int i = 0; i < 40 && StackWalk64(IMAGE_FILE_MACHINE_I386, p, t, &f, &c, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL); i++) {
		char buf[sizeof(SYMBOL_INFO) + 256] = {};
		SYMBOL_INFO* s = (SYMBOL_INFO*)buf; s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 255;
		DWORD64 d = 0; DWORD d2 = 0; IMAGEHLP_LINE64 l = { sizeof(l) };
		const char* name = SymFromAddr(p, f.AddrPC.Offset, &d, s) ? s->Name : "?";
		if (SymGetLineFromAddr64(p, f.AddrPC.Offset, &d2, &l)) printf("  %s  %s:%lu\n", name, l.FileName, l.LineNumber);
		else printf("  %s\n", name);
	}
	ExitProcess(3);
}

#include "forge_lib.h"

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	SetUnhandledExceptionFilter(Crash);
	if (argc < 4) { printf("usage: romanforge <game dir> <out dir> <level> [factor]\n"); return 1; }
	std::filesystem::path game = argv[1];
	FILE* f = _wfopen((game / "axxl.exe").c_str(), L"rb");
	if (!f) { printf("no axxl.exe\n"); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	std::vector<unsigned char> elb(n);
	fread(elb.data(), 1, n, f); fclose(f);
	printf("module known: %d\n", ForgeModuleKnown(elb.data(), elb.size()));
	int cloned = 0;
	char report[2048], shine[1024];
	bool ok = ForgeLevel(elb.data(), elb.size(), argv[1], argv[2], atoi(argv[3]), argc > 4 ? (float)atof(argv[4]) : 1.0f,
	                     &cloned, report, sizeof(report), shine, sizeof(shine));
	printf("ok %d, %d objects added:%s\nshine copies: %s\n", ok, cloned, report, shine);
	return ok ? 0 : 2;
}
