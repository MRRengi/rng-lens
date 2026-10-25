# rng-lens - where we are

DONE
- v0.1: phone camera -> HW encoder (zero copy) -> PC (GPU decode, OpenGL) -> virtual camera "rng-lens".
  Tested by Maks on a real phone: works
- v0.2: phone mic (Opus / AAC, voice / raw) -> PC playback; one-file installer; CI releases; GitHub repo.
  Tested by Maks: works
- v1.0 phone: heat guard (Android thermal forecast -> fps / resolution / bitrate steps, back up when cool),
  streaming with the screen locked (foreground service + Stop notification), MIUI hint
- v1.0 PC: battery / temperature / heat warning / stream timer at the top, temperature graph;
  effects on the GPU (looks, colour, sharpen, skin smoothing, night mode); background blur / picture and
  auto-frame (ONNX Runtime on a worker thread); tray, start with Windows, hotkeys;
  optional second phone + camera "rng-lens 2"; installer with both cameras, runtime and models
- Benches green: bench.sh 8/8 (thermal policy, discovery, video x4, Opus + AAC tone),
  effects_bench 11/11 (every effect measured against a baseline), wine-bench 3/3 (Windows build,
  two phones, both cameras deliver), sound-bench (real playback), installer install/uninstall under Wine,
  effects from the installed copy under Wine

- v1.0.0 released on GitHub (CI with signing secrets)
- v1.0.1: zoom / focus / exposure / torch from the PC work again (broke in 1.0); auto-frame follows
  smoothly (filtered detections, dead zone, spring) instead of twitching. Released

*WE ARE HERE*
- Maks: install 1.0.1, check zoom and auto-frame; a long session for heat; Reddit r/SideProject + X posts

AHEAD
- Fix what the real phone / real GPU show
- Google Play (targetSdk 36, camera+mic service types, AAB, privacy page), F-Droid; support button on PC
- Windows 11 Media Foundation camera (Windows Camera app), iOS app
