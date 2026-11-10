// romanforge shim: texture (de)compression is never needed to clone Romans.
#pragma once
namespace squish {
enum { kDxt1 = 1, kDxt3 = 2, kDxt5 = 4, kColourRangeFit = 16 };
inline int GetStorageRequirements(int w, int h, int flags) { int b = ((w + 3) / 4) * ((h + 3) / 4); return b * ((flags & kDxt1) ? 8 : 16); }
inline void Decompress(unsigned char*, const void*, int) {}
inline void Compress(const unsigned char*, void*, int) {}
inline void DecompressImage(unsigned char*, int, int, const void*, int) {}
}
