#!/usr/bin/env bash
# Headless end-to-end bench: FakePhone (the phone app's own protocol code + ffmpeg as camera/encoder)
# -> rng-lens --headless (real network, real decoder) -> PNG. No phone, no window, no GPU.
#   tools/bench.sh [path/to/rng-lens]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/desktop/build-linux/rng-lens}"
OUT="$ROOT/tools/bench-out"
PORT=38460
rm -rf "$OUT" && mkdir -p "$OUT/classes"

javac -nowarn -d "$OUT/classes" "$ROOT/android/src/dev/rengi/lens/Protocol.java" \
  "$ROOT/android/src/dev/rengi/lens/Link.java" "$ROOT/tools/fakephone/FakePhone.java" 2>&1 | grep -v JAVA_TOOL_OPTIONS

java -cp "$OUT/classes" FakePhone $PORT > "$OUT/fakephone.log" 2>&1 &
PHONE=$!
trap 'kill $PHONE 2>/dev/null' EXIT
sleep 1.5

pass=0; fail=0
check() { # name, expected WxH, args...
  local name=$1 size=$2; shift 2
  if "$BIN" --headless --connect 127.0.0.1:$PORT --png "$OUT/$name.png" "$@" > "$OUT/$name.json" 2> "$OUT/$name.err"; then
    if python3 - "$OUT/$name.png" "$size" <<'EOF'
import sys
from PIL import Image, ImageStat
im = Image.open(sys.argv[1]); w, h = map(int, sys.argv[2].split("x"))
st = ImageStat.Stat(im)
ok = im.size == (w, h) and max(st.stddev) > 20  # right size and a real picture, not a flat frame
print(f"  {im.size[0]}x{im.size[1]} stddev={max(st.stddev):.0f}")
sys.exit(0 if ok else 1)
EOF
    then echo "PASS $name"; pass=$((pass+1)); else echo "FAIL $name (picture)"; fail=$((fail+1)); fi
  else
    echo "FAIL $name: $(cat "$OUT/$name.err")"; fail=$((fail+1))
  fi
  cat "$OUT/$name.json"
}

if "$BIN" --discover 3 > "$OUT/discover.txt" 2>&1 && grep -q "FakePhone at .*:$PORT" "$OUT/discover.txt"; then
  echo "PASS discovery"; pass=$((pass+1))
else
  echo "FAIL discovery: $(cat "$OUT/discover.txt")"; fail=$((fail+1))
fi
cat "$OUT/discover.txt"

check h264-720p 1280x720 --frames 60
check switch-to-1080p 1920x1080 --frames 90 --control '{"width":1920,"height":1080}'
check hevc 1920x1080 --frames 60 --control '{"codec":1}'
check back-to-h264-60fps 1280x720 --frames 120 --control '{"codec":0,"width":1280,"height":720,"fps":60}'

grep CONTROL "$OUT/fakephone.log" | sed 's/^/  phone got: /'
echo "bench: $pass passed, $fail failed"
[ $fail -eq 0 ]
