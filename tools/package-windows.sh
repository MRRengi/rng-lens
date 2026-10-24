#!/usr/bin/env bash
# Collects the Windows build into dist/rng-lens-windows-<version>.zip
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:-0.1.0}"
OUT="$ROOT/dist/rng-lens-windows"
rm -rf "$OUT" && mkdir -p "$OUT/x86"
cp "$ROOT/desktop/build-win/rng-lens.exe" "$ROOT/desktop/build-win/rnglens_vcam.dll" "$OUT/"
cp "$ROOT/desktop/build-win32/rnglens_vcam.dll" "$OUT/x86/"
cp "$ROOT"/third_party/ffmpeg-win/bin/*.dll "$OUT/"
cp "$ROOT"/packaging/windows/* "$OUT/"
cp "$ROOT/LICENSE" "$OUT/LICENSE.txt"
cp "$ROOT/third_party/softcam/LICENSE" "$OUT/LICENSE-softcam.txt"
printf 'FFmpeg (avcodec, avutil, swscale) is LGPL 2.1+, built from https://github.com/FFmpeg/FFmpeg n7.1\r\nwith tools/build-ffmpeg-windows.sh from the rng-lens source.\r\n' > "$OUT/LICENSE-ffmpeg.txt"
(cd "$ROOT/dist" && rm -f "rng-lens-windows-$VER.zip" && zip -qr "rng-lens-windows-$VER.zip" rng-lens-windows)
echo "dist/rng-lens-windows-$VER.zip ($(du -h "$ROOT/dist/rng-lens-windows-$VER.zip" | cut -f1))"
