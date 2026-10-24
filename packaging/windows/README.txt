rng-lens - your phone as a webcam. Free, open source, no watermark.

1. Phone: install rng-lens.apk and open it (allow camera access).
2. PC: install rng-lens-setup.exe - it adds the "rng-lens" camera.
3. PC: run rng-lens.exe. Your phone shows up on the right if both are on the same Wi-Fi - click it.
4. In Zoom / Discord / OBS / Meet pick the camera called "rng-lens".

USB instead of Wi-Fi: enable USB debugging on the phone (Settings > About > tap Build number 7 times,
then Developer options > USB debugging), plug it in and press USB. adb comes with the installer.

Sound: the phone's microphone plays on the output you pick under "Sound". To use it as a microphone
in Zoom/Discord, install the free VB-Cable (vb-audio.com/Cable) and pick "CABLE Input" there, then
choose "CABLE Output" as the microphone in your app.

Tips
- Keep the rng-lens app open on the phone. It blacks its screen out after a few seconds to save battery;
  double-tap to wake it.
- Everything (lens, resolution up to 4K, 60 fps, zoom, focus, exposure, torch, HEVC) is set from the PC window.
- Shortcut that connects straight away:  rng-lens.exe --connect 192.168.1.23
- Remove: Settings > Apps > rng-lens > Uninstall
- Windows may warn that the installer is from an unknown publisher (it is not code-signed):
  click More info > Run anyway.
