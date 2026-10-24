#pragma once
#include <string>
// Connects, decodes `frames` frames, optionally sends a CONTROL and dumps the last frame as PNG.
// Used by the test bench; no window, no GPU. Returns 0 on success.
// With `wav` set it also records the phone's decoded audio there and fails if under a second arrived.
int runHeadless(const std::string& host, int port, int frames, const std::string& png, const std::string& control,
                const std::string& wav);
// Listens for phones for `seconds` and prints what it found. Returns 0 if at least one answered.
int runDiscover(int seconds);
