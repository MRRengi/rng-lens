// rng-lens: lets the DirectShow BaseClasses build with mingw-w64 (no old-style SAL macros there).
#pragma once
#ifdef __MINGW32__
// Pull in the C++ library first: it uses __in/__out as identifiers.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <sal.h>
#ifndef AM_NOVTABLE
#define AM_NOVTABLE
#endif
#define __in
#define __in_opt
#define __inout
#define __out
#define __deref_out
#define __inout_opt
#define __out_opt
#define __deref_out_opt
#define __deref_inout_opt
#define __deref_in
#define __format_string
#define __control_entrypoint(x)
#define __in_bcount(x)
#define __in_bcount_opt(x)
#define __out_bcount(x)
#define __out_ecount(x)
#define __in_ecount(x)
#define __inout_ecount_full(x)
#define __out_ecount_part(x, y)
#define __field_ecount_opt(x)
#define __out_range(x, y)
#define __deref_out_range(x, y)
#define __success(x)
#endif

#ifdef __MINGW32__
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
// Video helpers the Windows SDK's amvideo.h has and mingw's lacks.
#ifndef HEADER
#define HEADER(pVideoInfo) (&(((VIDEOINFOHEADER*)(pVideoInfo))->bmiHeader))
#endif
#ifndef SIZE_PREHEADER
#define SIZE_PREHEADER (FIELD_OFFSET(VIDEOINFOHEADER, bmiHeader))
#endif
#ifndef WIDTHBYTES
#define WIDTHBYTES(bits) ((DWORD)(((bits) + 31) & (~31)) / 8)
#endif
#ifndef DIBWIDTHBYTES
#define DIBWIDTHBYTES(bi) (DWORD) WIDTHBYTES((DWORD)(bi).biWidth * (DWORD)(bi).biBitCount)
#endif
#ifndef DIBSIZE
#define _DIBSIZE(bi) (DIBWIDTHBYTES(bi) * (DWORD)(bi).biHeight)
#define DIBSIZE(bi) ((bi).biHeight < 0 ? (-1) * (_DIBSIZE(bi)) : _DIBSIZE(bi))
#endif
#endif
