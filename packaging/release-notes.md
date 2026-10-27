Your phone as a webcam and microphone. Free, open source, no watermark, no account.

**Install**
1. Phone (Android 6+): download `rng-lens.apk` below, open it, allow camera and microphone.
2. PC (Windows 10/11, 64-bit): run `rng-lens-setup.exe`. It adds the camera **rng-lens**.
   The installer is not code-signed, so Windows may say "unknown publisher": More info > Run anyway.
3. Start rng-lens on the PC. On the same Wi-Fi the phone shows up by itself. USB works too (USB debugging on).
4. Pick **rng-lens** as the camera in Zoom, Discord, OBS, Teams, Meet.

**What's in it**
- Up to 4K / 60 fps, H.264 and HEVC, every lens, zoom, focus, exposure and white-balance locks, torch, stabilization
- Background blur or picture, auto-frame that follows your face, looks, colour controls, sharpen,
  skin smoothing, night mode. All on the PC's graphics card, so the phone doesn't heat up for it
- Heat guard: the phone lowers fps / resolution before it overheats and goes back when it cools.
  Battery and temperature are shown on the PC
- Lock the phone's screen and keep streaming
- Phone microphone (voice or raw). To use it as a mic in calls, install the free
  [VB-Cable](https://vb-audio.com/Cable/) and pick "CABLE Input" in rng-lens
- New macOS-style look, light and dark with Windows; a glass toolbar over the preview for the things you toggle mid-call
- Tray icon, start with Windows, hotkeys, optional second phone as camera "rng-lens 2"

Background blur and auto-frame need the Microsoft Visual C++ Runtime (most PCs already have it):
https://aka.ms/vs/17/release/vc_redist.x64.exe
