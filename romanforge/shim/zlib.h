// romanforge shim: only the gzip file wrapper (unused for XXL1 PC levels) needs zlib.
#pragma once
#include <cstddef>
typedef void* gzFile;
inline gzFile gzopen_w(const wchar_t*, const char*) { return nullptr; }
inline int gzclose(gzFile) { return 0; }
inline int gzread(gzFile, void*, unsigned) { return 0; }
inline int gzwrite(gzFile, const void*, unsigned) { return 0; }
inline long gzseek(gzFile, long, int) { return -1; }
inline long gztell(gzFile) { return -1; }
