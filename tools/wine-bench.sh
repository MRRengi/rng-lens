#!/usr/bin/env bash
# Windows end-to-end bench under Wine + Xvfb (no Windows machine, no phone):
# FakePhone -> rng-lens.exe (Windows build, GUI, OpenGL) -> rnglens_vcam.dll (DirectShow)
# -> dshow-probe.exe (pulls frames like Zoom/OBS do), for the 64-bit and the 32-bit filter.
# Needs: wine (64+32), Xvfb, mesa, mingw-w64. Run tools/bench.sh first (it compiles FakePhone).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="${STAGE:-/tmp/rnglens-wine}"
PORT=38462
rm -rf "$STAGE" && mkdir -p "$STAGE/x86"
cp "$ROOT/desktop/build-win/rng-lens.exe" "$ROOT"/desktop/build-win/rnglens_vcam*.dll "$ROOT"/third_party/ffmpeg-win/bin/*.dll "$STAGE/"
cp "$ROOT"/desktop/build-win32/rnglens_vcam*.dll "$STAGE/x86/"
cp "$ROOT/third_party/vision/win/onnxruntime.dll" "$STAGE/" && mkdir -p "$STAGE/models" && cp "$ROOT"/third_party/vision/models/*.onnx "$STAGE/models/"
x86_64-w64-mingw32-g++-posix -O2 -o "$STAGE/probe64.exe" "$ROOT/tools/dshow-probe/probe.cpp" -static -lstrmiids -lole32 -loleaut32 -luuid
i686-w64-mingw32-g++-posix -O2 -o "$STAGE/probe32.exe" "$ROOT/tools/dshow-probe/probe.cpp" -static -lstrmiids -lole32 -loleaut32 -luuid

export WINEPREFIX="$STAGE/prefix" WINEDEBUG=-all DISPLAY="${DISPLAY:-:97}"
if ! xdpyinfo >/dev/null 2>&1; then Xvfb "$DISPLAY" -screen 0 1400x1000x24 >/dev/null 2>&1 & XVFB=$!; sleep 2; fi
wineboot -i >/dev/null 2>&1
(cd "$STAGE" && for d in rnglens_vcam.dll rnglens_vcam2.dll; do wine regsvr32 /s $d; wine 'C:\windows\syswow64\regsvr32.exe' /s "x86\\$d"; done) 2>/dev/null

java -cp "$ROOT/tools/bench-out/classes" FakePhone $PORT > "$STAGE/fakephone.log" 2>&1 & PHONE=$!
java -cp "$ROOT/tools/bench-out/classes" FakePhone $((PORT+1)) > "$STAGE/fakephone2.log" 2>&1 & PHONE2=$!
sleep 2
# Second phone on: phone 2 is found by Wi-Fi discovery (same machine, so same IP, other port).
APPDATA_DIR="$WINEPREFIX/drive_c/users/$(whoami)/AppData/Roaming/rng-lens"
mkdir -p "$APPDATA_DIR"
IP=$("$ROOT/desktop/build-linux/rng-lens" --discover 3 | grep -o "at [0-9.]*:$((PORT+1))" | head -1 | sed 's/at //; s/:.*//')
echo "{\"slots\":[{\"autoConnect\":false},{\"lastHost\":\"$IP\",\"lastPort\":$((PORT+1)),\"autoConnect\":true}],\"secondPhone\":true,\"audio\":false}" > "$APPDATA_DIR/rng-lens.json"
(cd "$STAGE" && timeout 100 wine rng-lens.exe --connect 127.0.0.1:$PORT > app.log 2>&1 &)
sleep 26

pass=0; fail=0
for p in probe64 probe32 probe64:2; do
  exe=${p%%:*}; cam="Lens"; [ "$p" != "$exe" ] && cam="Lens 2"
  (cd "$STAGE" && timeout 30 wine $exe.exe "${p/:/_}.bmp" 2 "$cam" 2>/dev/null > "${p/:/_}.log")
  p=${p/:/_}
  if grep -q "^OK" "$STAGE/$p.log" && python3 -c "
import sys; from PIL import Image, ImageStat
im = Image.open('$STAGE/$p.bmp'); s = ImageStat.Stat(im)
sys.exit(0 if im.size == (1920, 1080) and max(s.stddev) > 20 else 1)"; then
    echo "PASS $p: $(grep -E 'samples delivered|frame:' "$STAGE/$p.log" | tr '\n' ' ')"; pass=$((pass+1))
  else
    echo "FAIL $p: $(tail -3 "$STAGE/$p.log" | tr '\n' ' ')"; fail=$((fail+1))
  fi
done
kill $PHONE $PHONE2 2>/dev/null; wineserver -k 2>/dev/null; [ -n "${XVFB:-}" ] && kill $XVFB
echo "wine bench: $pass passed, $fail failed"
[ $fail -eq 0 ]
