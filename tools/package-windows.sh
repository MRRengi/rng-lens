#!/usr/bin/env bash
# Builds the one-file Windows installer: dist/rng-lens-setup-<version>.exe
#   tools/package-windows.sh <version> [platform-tools dir with adb.exe]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:-0.0.0}"
ADB="${2:-}"
STAGE="$ROOT/dist/stage"
rm -rf "$STAGE" && mkdir -p "$STAGE/x86" "$STAGE/models"
[ -f "$ROOT/third_party/vision/win/onnxruntime.dll" ] || "$ROOT/tools/fetch-vision.sh"
cp "$ROOT/desktop/build-win/rng-lens.exe" "$ROOT"/desktop/build-win/rnglens_vcam*.dll "$STAGE/"
cp "$ROOT"/desktop/build-win32/rnglens_vcam*.dll "$STAGE/x86/"
cp "$ROOT/third_party/vision/win/onnxruntime.dll" "$STAGE/"
cp "$ROOT/third_party/vision/win/LICENSE" "$STAGE/LICENSE-onnxruntime.txt"
cp "$ROOT"/third_party/vision/models/*.onnx "$ROOT/third_party/vision/models/LICENSES.txt" "$STAGE/models/"
cp "$ROOT"/third_party/ffmpeg-win/bin/*.dll "$STAGE/"
cp "$ROOT/packaging/windows/README.txt" "$ROOT/desktop/res/rng-lens.ico" "$STAGE/"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$ROOT/third_party/softcam/LICENSE" "$STAGE/LICENSE-softcam.txt"
cp "$ROOT/desktop/res/fonts/LICENSE-inter.txt" "$ROOT/desktop/res/fonts/LICENSE-lucide.txt" "$STAGE/"
printf 'FFmpeg (avcodec, avutil, swscale, swresample) is LGPL 2.1+, built from\r\nhttps://github.com/FFmpeg/FFmpeg n7.1 with tools/build-ffmpeg-windows.sh from the rng-lens source.\r\n' > "$STAGE/LICENSE-ffmpeg.txt"
printf 'miniaudio by David Reid, public domain / MIT-0. Dear ImGui (MIT), GLFW (zlib), nlohmann/json (MIT).\r\n' > "$STAGE/LICENSE-others.txt"
ARGS=(-V2 -DVERSION="$VER" -DSRC="$STAGE" -DOUTFILE="$ROOT/dist/rng-lens-setup-$VER.exe")
[ -n "$ADB" ] && ARGS+=(-DADB_DIR="$ADB")
makensis "${ARGS[@]}" "$ROOT/packaging/windows/installer.nsi"
echo "dist/rng-lens-setup-$VER.exe ($(du -h "$ROOT/dist/rng-lens-setup-$VER.exe" | cut -f1))"
