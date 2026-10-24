#!/usr/bin/env bash
# Cross-compiles the minimal FFmpeg rng-lens needs (H.264/HEVC decoders + D3D11VA/DXVA2 + swscale)
# as Windows DLLs into third_party/ffmpeg-win. Needs mingw-w64 and nasm.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/build/FFmpeg}"
[ -d "$SRC" ] || git clone --depth 1 --branch n7.1 https://github.com/FFmpeg/FFmpeg.git "$SRC"
cd "$SRC"
./configure --prefix="$ROOT/third_party/ffmpeg-win" --cross-prefix=x86_64-w64-mingw32- --arch=x86_64 \
  --target-os=mingw32 --enable-shared --disable-static --disable-everything \
  --enable-decoder=h264,hevc --enable-parser=h264,hevc --enable-swscale --disable-programs --disable-doc \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-network --disable-debug \
  --enable-d3d11va --enable-dxva2 \
  --enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2,h264_dxva2,hevc_dxva2
make -j"$(nproc)" && make install
