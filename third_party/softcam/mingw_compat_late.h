// rng-lens: the parts of the Windows SDK amvideo.h that mingw-w64 lacks. Included from streams.h.
#pragma once
#ifdef __MINGW32__
#ifndef SIZE_MASKS
#define SIZE_MASKS (iMASK_COLORS * sizeof(DWORD))
#endif
#ifndef SIZE_EGA_PALETTE
#define SIZE_EGA_PALETTE (iEGA_COLORS * sizeof(RGBQUAD))
#endif
#ifndef SIZE_PALETTE
#define SIZE_PALETTE (iPALETTE_COLORS * sizeof(RGBQUAD))
#endif
#ifndef SIZE_VIDEOHEADER
#define SIZE_VIDEOHEADER (sizeof(BITMAPINFOHEADER) + SIZE_PREHEADER)
#endif
#ifndef PALETTISED
#define PALETTISED(pVideoInfo) ((pVideoInfo)->bmiHeader.biBitCount <= iPALETTE)
#endif
#ifndef PALETTE_ENTRIES
#define PALETTE_ENTRIES(pVideoInfo) ((DWORD)1 << (pVideoInfo)->bmiHeader.biBitCount)
#endif
#ifndef BITMASKS
#define BITMASKS(pVideoInfo) ((DWORD*)(((LPBYTE) & ((pVideoInfo)->bmiHeader)) + (pVideoInfo)->bmiHeader.biSize))
#endif
#ifndef TRUECOLOR
#define TRUECOLOR(pVideoInfo) ((TRUECOLORINFO*)(((LPBYTE) & ((pVideoInfo)->bmiHeader)) + (pVideoInfo)->bmiHeader.biSize))
#endif
#ifndef COLORS
#define COLORS(pVideoInfo) ((RGBQUAD*)(((LPBYTE) & ((pVideoInfo)->bmiHeader)) + (pVideoInfo)->bmiHeader.biSize))
#endif
STDAPI_(BOOL) ContainsPalette(const VIDEOINFOHEADER* pVideoInfo);
STDAPI_(const RGBQUAD*) GetBitmapPalette(const VIDEOINFOHEADER* pVideoInfo);
#endif
