#!/usr/bin/env bash
# Playback bench: the real GUI plays the fake phone's 440 Hz into a PulseAudio null sink; we record
# the sink's monitor and check the tone. Covers miniaudio + the jitter buffer, i.e. what users hear.
#   tools/sound-bench.sh [linux|wine]     (run tools/bench.sh first: it compiles FakePhone)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-linux}"
OUT="$ROOT/tools/bench-out"
PORT=38470
export DISPLAY="${DISPLAY:-:96}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/rnglens-xdg}" HOME="${BENCH_HOME:-/tmp/rnglens-home}"
mkdir -p "$XDG_RUNTIME_DIR" "$HOME" && chmod 700 "$XDG_RUNTIME_DIR"

xdpyinfo >/dev/null 2>&1 || { Xvfb "$DISPLAY" -screen 0 1400x1000x24 >/dev/null 2>&1 & XVFB=$!; sleep 2; }
pulseaudio --daemonize=yes --exit-idle-time=-1 --system=false -n \
  --load="module-native-protocol-unix" --load="module-null-sink sink_name=bench" >/dev/null 2>&1
sleep 1
pactl set-default-sink bench

java -cp "$OUT/classes" FakePhone $PORT opus > "$OUT/sound-phone.log" 2>&1 & PHONE=$!
sleep 1.5
if [ "$MODE" = wine ]; then
  STAGE="${STAGE:-/tmp/rnglens-wine}"
  (cd "$STAGE" && WINEPREFIX="$STAGE/prefix" WINEDEBUG=-all timeout 40 wine rng-lens.exe --connect 127.0.0.1:$PORT > "$OUT/sound-app.log" 2>&1 &)
  sleep 18
else
  timeout 30 "$ROOT/desktop/build-linux/rng-lens" --connect 127.0.0.1:$PORT > "$OUT/sound-app.log" 2>&1 & APP=$!
  sleep 6
fi
timeout 4 parec -d bench.monitor --format=s16le --rate=48000 --channels=2 --file-format=wav "$OUT/sound-$MODE.wav"
kill $PHONE 2>/dev/null
[ -n "${APP:-}" ] && kill $APP 2>/dev/null; [ "$MODE" = wine ] && WINEPREFIX="$STAGE/prefix" wineserver -k 2>/dev/null
pulseaudio --kill 2>/dev/null; [ -n "${XVFB:-}" ] && kill $XVFB

if python3 "$ROOT/tools/check_tone.py" "$OUT/sound-$MODE.wav" 440; then echo "PASS playback-$MODE"; else echo "FAIL playback-$MODE"; exit 1; fi
