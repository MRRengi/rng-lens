#!/usr/bin/env bash
# Downloads what background blur and auto-frame need into third_party/vision:
#   ONNX Runtime 1.20.1 (MIT) for Windows and Linux, PP-HumanSeg (Apache-2.0) and YuNet (MIT) from opencv_zoo.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$ROOT/third_party/vision"
ORT=1.20.1
mkdir -p "$V/models" "$V/win" "$V/linux"
cd "$V"
if [ ! -f win/onnxruntime.dll ]; then
  curl -sSL -o ort-win.zip "https://github.com/microsoft/onnxruntime/releases/download/v$ORT/onnxruntime-win-x64-$ORT.zip"
  unzip -qjo ort-win.zip "onnxruntime-win-x64-$ORT/lib/onnxruntime.dll" "onnxruntime-win-x64-$ORT/LICENSE" -d win
  rm ort-win.zip
fi
if [ ! -f linux/libonnxruntime.so.1 ]; then
  curl -sSL "https://github.com/microsoft/onnxruntime/releases/download/v$ORT/onnxruntime-linux-x64-$ORT.tgz" | tar xz
  cp "onnxruntime-linux-x64-$ORT/lib/libonnxruntime.so.$ORT" linux/libonnxruntime.so.1
  rm -rf "onnxruntime-linux-x64-$ORT"
fi
Z=https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models
[ -f models/human_segmentation_pphumanseg_2023mar.onnx ] || \
  curl -sSL -o models/human_segmentation_pphumanseg_2023mar.onnx "$Z/human_segmentation_pphumanseg/human_segmentation_pphumanseg_2023mar.onnx"
[ -f models/face_detection_yunet_2023mar.onnx ] || \
  curl -sSL -o models/face_detection_yunet_2023mar.onnx "$Z/face_detection_yunet/face_detection_yunet_2023mar.onnx"
cat > models/LICENSES.txt <<'EOF'
human_segmentation_pphumanseg_2023mar.onnx - PP-HumanSeg (PaddleSeg), Apache License 2.0, via opencv_zoo
face_detection_yunet_2023mar.onnx - YuNet (Shiqi Yu et al.), MIT License, via opencv_zoo
EOF
ls -la win linux models
