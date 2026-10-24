# rng-lens - spec v1

Open-source phone-as-webcam. Every feature free, no watermark, no account.
The phone only captures and hardware-encodes; everything else (decode, rotate,
mirror, filters, virtual camera) runs on the PC.

## Goals

- Any app on the PC (Zoom, Discord, OBS, Meet in Chrome, Teams) sees a camera called **rng-lens**.
- Wi-Fi (auto-discovery) and USB (adb forward).
- Up to 4K and 60 fps when the phone supports it; H.264 by default, HEVC as an option.
- Every lens the phone exposes, zoom, focus auto/lock/manual, exposure lock, EV, white-balance lock, torch, stabilization.
- Mirror / flip / rotate on the PC side (free for the phone).
- Phone stays cool: no CPU frame copies, no preview rendering, black screen while streaming.

Sound (v0.2): phone mic, 48 kHz mono, Opus on Android 10+ and AAC-LC before. "voice" = communication
source + echo cancel / noise suppression / AGC, "raw" = unprocessed source. The PC plays it to any output
device through a 60 ms jitter buffer (skips ahead past 220 ms, trims clock drift ~0.2%). A system
microphone device would need a Microsoft-signed kernel driver, so it is out of scope: users route it
with VB-Cable.

Heat guard (1.0): every 2 s the phone reads Android's thermal status (API 29+) and thermal headroom
forecast (API 30+), or the battery temperature on older phones (ThermalPolicy.java). Heating reacts at
once, cooling waits a minute per level. Levels: 2 = max 30 fps, 75% bitrate; 3 = max 1080p, 30 fps, 50%;
4+ = max 720p, 24 fps, 40%. The phone's StreamService keeps the stream alive with the screen locked and
exits when the screen is closed and no PC is connected.

Effects (1.0, PC GPU): convert+rotate -> night (motion-adaptive temporal average) -> background
(PP-HumanSeg mask at 192x192, ~20/s, refined by a joint-bilateral pass) -> skin smoothing (edge-aware,
skin-coloured areas only) -> sharpen + colour + looks -> crop (auto-frame from YuNet faces, dead zone and
eased follow) -> letterbox. Effects run at the resolution the output needs. ONNX Runtime is loaded at run
time on a worker thread; without it (or the VC++ runtime) everything except background / auto-frame works.

Later: Windows 11 Media Foundation virtual camera, iOS, macOS.

## Architecture

```
PHONE (Android, Java, no libraries)
Camera2 --Surface--> MediaCodec HW encoder --> AnnexB access units --> TCP :38440
                       (zero copy)                                  UDP beacon :38441
PC (C++17)
TCP client -> FFmpeg decode (threaded, low delay) -> YUV planes -> OpenGL
  -> preview (ImGui window)  + FBO at output size -> PBO readback -> virtual camera
                                                       Windows: rebranded Softcam (DirectShow)
                                                       Linux:   v4l2loopback
```

Phone details
- Encoder input is a `Surface`; the camera writes straight into it. The app never touches pixels.
- Encoder in async mode on its own HandlerThread. Output buffers go to a bounded send queue; if the
  network falls behind, non-key frames are dropped until the next key frame and a sync frame is requested.
- `KEY_PRIORITY=0` (realtime), `latency=1` (API 30+), `KEY_REPEAT_PREVIOUS_FRAME_AFTER` so a static
  scene costs nothing, I-frame every 2 s, keyframe on demand.
- Zoom via `CONTROL_ZOOM_RATIO` (API 30+, through reflection - this lets ultra-wide go below 1x on
  logical cameras) with `SCALER_CROP_REGION` fallback.
- Streaming screen: pure black, brightness at minimum, keep-screen-on, high-perf Wi-Fi lock.
- Phone UI: status, IP, start/stop, "blackout" toggle. All camera controls come from the PC.
- Built without Gradle: javac (release 8) against android-23 platform jar -> dx -> aapt -> zipalign -> apksigner.
  minSdk 23, targetSdk 28.

PC details
- One window: preview + control panel (Dear ImGui on GLFW/OpenGL 3.3).
- Device list: Wi-Fi beacons, "USB" button (runs `adb forward tcp:38440 tcp:38440`), manual IP.
- Rotation from the phone's sensor orientation + user rotate; mirror/flip in the shader.
- Virtual camera output size is fixed by the user (720p / 1080p / 4K, default 1080p30); the stream is
  letterboxed into it. Changing it recreates the camera.
- `--headless` mode: connect, decode N frames, dump the last one to PNG, print stats. No window, no GL. Used by the bench.

## Protocol v1 (TCP, little-endian)

Every message: `u8 type | u32 length | payload[length]`.

Phone -> PC
| type | name   | payload |
|------|--------|---------|
| 0x01 | HELLO  | UTF-8 JSON: `{proto, device, cameras:[{id, facing, sensorOrientation, sizes:[[w,h]], fps:[[min,max]], zoom:[min,max], flash, afManual, minFocus}], state}` |
| 0x02 | CONFIG | `u8 codec (0=h264,1=hevc)`, `u16 width`, `u16 height`, then codec-config bytes (AnnexB SPS/PPS/VPS) |
| 0x03 | FRAME  | `u64 ptsUs`, `u8 flags (1=key)`, AnnexB access unit |
| 0x04 | STATE  | UTF-8 JSON of the current settings + `battery`, `tempC`, `charging`, `thermal: {level, status, headroom, throttled, limit}` |
| 0x05 | AUDIO_CONFIG | `u8 codec (0=opus,1=aac)`, `u32 sampleRate`, `u8 channels`, codec config (OpusHead / AudioSpecificConfig, may be empty) |
| 0x06 | AUDIO  | `u64 ptsUs`, one encoded packet (20 ms Opus or 1024-sample AAC) |

PC -> phone
| type | name    | payload |
|------|---------|---------|
| 0x10 | CONTROL | UTF-8 JSON, any subset of `{camera, width, height, fps, codec, bitrate, zoom, af:"auto|lock|manual", focus, ae:"auto|lock", ev, awb:"auto|lock", torch, stab, keyframe, audio, mic:"voice|raw"}` |
| 0x11 | PING    | empty |

Discovery: phone broadcasts `{"rnglens":1,"name":...,"port":38440}` to UDP 255.255.255.255:38441 once per second while idle.

## Test bench (headless, runs in CI / cloud, no phone)

`tools/fakephone`: a JVM program that reuses the phone's own `Protocol.java`, feeds it H.264 from
`ffmpeg -f lavfi testsrc2`, serves TCP :38440 and logs CONTROL messages. The desktop app in
`--headless` mode connects, decodes, dumps a PNG. Green = PNG matches the test pattern size and is not blank.

## Repo

```
android/   phone app + build.sh
desktop/   PC app (CMake; Linux native, Windows via mingw toolchain file)
third_party/softcam  rebranded Softcam (own CLSID, name "rng-lens")
tools/     fakephone, bench script
docs/
```
License MIT. FFmpeg is linked dynamically (LGPL).
