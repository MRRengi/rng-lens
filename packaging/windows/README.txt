rng-lens - your phone as a webcam. Free, open source, no watermark.

1. Phone: install rng-lens.apk and open it (allow camera access).
2. PC: run install-camera.bat once (it asks for admin) - this adds the "rng-lens" camera.
3. PC: run rng-lens.exe. Your phone shows up on the right if both are on the same Wi-Fi - click it.
4. In Zoom / Discord / OBS / Meet pick the camera called "rng-lens".

USB instead of Wi-Fi: enable USB debugging on the phone, plug it in, and put adb.exe
(+ AdbWinApi.dll, AdbWinUsbApi.dll from Google's platform-tools) in a folder named "adb"
next to rng-lens.exe, or have adb in PATH. Then press USB.

Tips
- If Windows asks whether rng-lens may use the network, allow it for private networks.
- Keep the rng-lens app open on the phone. It blacks its screen out after a few seconds to save battery;
  double-tap to wake it.
- Everything (lens, resolution up to 4K, 60 fps, zoom, focus, exposure, torch, HEVC) is set from the PC window.
- Shortcut that connects straight away:  rng-lens.exe --connect 192.168.1.23
- Remove the camera: uninstall-camera.bat
