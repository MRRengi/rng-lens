# rng-lens - where we are

DONE
- v0.1: phone camera -> HW encoder (zero copy) -> PC (GPU decode, OpenGL) -> virtual camera "rng-lens"
  (DirectShow 64 + 32 bit, v4l2loopback). Tested by Maks on a real phone: works
- Repo on GitHub: MRRengi/rng-lens
- v0.2 sound: phone mic (Opus / AAC, voice / raw) -> PC playback to any output, jitter buffer,
  device picker, volume, mute, level meter. Decision: no signed mic driver (costs money), VB-Cable instead
- v0.2 one-file installer (NSIS): camera registration, firewall rule, adb for USB, shortcuts, uninstaller
- CI: every push builds APK + Windows + runs the bench; tag v* publishes a Release
- Benches green: bench.sh 7/7 (discovery, 4x video, Opus + AAC tone), wine-bench 2/2,
  sound-bench linux + wine (real app playback), installer install/uninstall under Wine

*WE ARE HERE*
- Maks: add 3 GitHub secrets (release key) -> tag v0.2.0 -> first public Release
- Maks: real test of sound on the phone

AHEAD
- Filters (GPU shaders on PC: background blur, LUTs, beauty, etc.)
- Foreground service on the phone (stream with the app in background / screen off)
- Windows 11 Media Foundation camera (Windows Camera app sees it), iOS app
