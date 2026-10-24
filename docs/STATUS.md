# rng-lens - where we are

DONE
- Name, scope, stack agreed (Android Java + PC C++, all Iriun Pro features free)
- Spec v1 (docs/SPEC.md)
- Android app v0.1: Camera2 -> HW encoder surface (zero copy), TCP + Wi-Fi discovery,
  all controls, black screen while streaming, encoder fallback ladder. APK 60 KB
- PC app v0.1 (Windows + Linux): GPU decode on Windows, OpenGL output, ImGui window,
  auto-rotate, mirror/flip, vertical/square output, USB via adb, auto-reconnect
- Virtual camera "rng-lens": DirectShow (64 + 32 bit) on Windows, v4l2loopback on Linux
- Benches green: tools/bench.sh 5/5 (discovery, H.264, live 1080p switch, HEVC, 60 fps),
  tools/wine-bench.sh 2/2 (Windows build -> DirectShow camera -> client app, 64 and 32 bit)

*WE ARE HERE*
- Maks: first real test - APK on the phone, zip on Windows, pick "rng-lens" in Zoom/Discord/OBS

AHEAD
- Fix whatever the real phone shows (the only part no bench could run)
- Move repo to GitHub (MRRengi/rng-lens) and onto Maks's PC; bundle adb for USB
- v2: phone mic as audio device, filters (GPU shaders on PC), Win11 Media Foundation camera
  (for the Windows Camera app), foreground service so the phone can stream with the app in background, iOS
