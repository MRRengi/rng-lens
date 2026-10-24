# rng-lens

Your phone as a webcam and microphone. Free, open source, no watermark, no account. Every "pro" feature included.

**Download:** [Releases](https://github.com/MRRengi/rng-lens/releases) - `rng-lens-setup-x.y.z.exe` for Windows, `rng-lens-x.y.z.apk` for the phone.

- Any app sees a camera called **rng-lens**: Zoom, Discord, OBS, Teams, Meet in Chrome
- Wi-Fi with auto-discovery, or USB through adb
- Up to 4K and 60 fps, H.264 or HEVC
- Every lens the phone exposes (ultra-wide via zoom below 1x), zoom, focus auto / lock / manual,
  exposure lock and EV, white-balance lock, torch, stabilization
- Mirror, flip, rotate, vertical and square output
- Phone microphone (Opus / AAC, "voice" with echo cancel and noise suppression, or "raw"), played on any
  output device. To make it a microphone in calls, route it through [VB-Cable](https://vb-audio.com/Cable/):
  a system microphone needs a Microsoft-signed kernel driver, which a free project can't ship

## Why it's light on the phone

The phone does exactly two things: the camera writes straight into the hardware encoder's input
surface (no CPU copy of a single pixel), and the encoded bytes go out over TCP. No preview is
rendered, and the screen blacks out at minimum brightness while streaming. Decoding, rotation,
mirroring, letterboxing (and the filters coming next) all run on the PC's GPU. If the network falls
behind, the phone drops frames until the next key frame instead of buffering.

## Layout

| Path | What |
|------|------|
| `android/` | Phone app, plain Java, no libraries. `build.sh` builds the APK without Gradle |
| `desktop/` | PC app, C++17: FFmpeg decode (GPU on Windows via D3D11VA), OpenGL, Dear ImGui |
| `third_party/softcam/` | Windows virtual camera: [Softcam](https://github.com/tshino/softcam) rebranded (own CLSID, name "rng-lens"), builds with mingw |
| `tools/` | Test benches, FFmpeg build script, icon generator |
| `packaging/windows/` | NSIS installer and the user README |
| `.github/workflows/` | CI: builds and benches every push; a `v*` tag publishes a Release |
| `docs/SPEC.md` | Architecture and the wire protocol |

## Build

Phone (Linux):
```
sudo apt install android-sdk-platform-23 aapt dalvik-exchange zipalign apksigner
android/build.sh            # -> android/build/rng-lens.apk
```

PC, Windows build from Linux:
```
sudo apt install mingw-w64 g++-mingw-w64-x86-64-posix g++-mingw-w64-i686-posix nasm cmake nsis
tools/build-ffmpeg-windows.sh                      # -> third_party/ffmpeg-win
cd desktop
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake && cmake --build build-win -j
cmake -B build-win32 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w32.cmake -DRNGLENS_VCAM_ONLY=ON && cmake --build build-win32 -j
cd .. && tools/package-windows.sh 0.2.0 [platform-tools]   # -> dist/rng-lens-setup-0.2.0.exe (needs nsis)
```

PC, Linux:
```
sudo apt install libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libglfw3-dev cmake
cd desktop && cmake -B build-linux && cmake --build build-linux -j
# virtual camera: sudo modprobe v4l2loopback exclusive_caps=1 card_label=rng-lens
```

## Test benches (no phone, no Windows machine, no display needed)

- `tools/bench.sh` - FakePhone (the phone app's own protocol code + ffmpeg standing in for camera and
  encoder) against `rng-lens --headless`: discovery, H.264, live switch to 1080p, HEVC, 60 fps.
  Plus the microphone: a 440 Hz tone (Opus and AAC) must come out as 440 Hz.
- `tools/wine-bench.sh` - the Windows build under Wine: real GUI and OpenGL, the DirectShow camera
  registered, and a DirectShow client pulling frames from it (64-bit and 32-bit).
- `tools/sound-bench.sh [linux|wine]` - the real app plays the tone into a PulseAudio null sink, recorded and checked.

## Releasing

Add three repository secrets once (Settings > Secrets and variables > Actions): `ANDROID_KEYSTORE_B64`
(base64 of the release keystore), `ANDROID_KEYSTORE_PASS`, `ANDROID_KEY_ALIAS`. Then
`git tag v0.2.0 && git push origin v0.2.0` - CI builds, benches and publishes the Release.

## License

MIT. Softcam is MIT (see its LICENSE), miniaudio is public domain / MIT-0. FFmpeg is LGPL and linked dynamically.
