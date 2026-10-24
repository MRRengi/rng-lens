#pragma once
// The system-wide virtual camera other apps see as "rng-lens".
// Windows: our rebranded Softcam DirectShow filter (rnglens_vcam.dll, registered once by the installer).
// Linux: a v4l2loopback device.

#include <cstdint>
#include <memory>
#include <string>

class VirtualCamera {
public:
    static std::unique_ptr<VirtualCamera> create();
    virtual ~VirtualCamera() = default;

    // False with a human-readable reason when the driver is not installed.
    virtual bool available(std::string& why) = 0;
    virtual bool start(int width, int height, int fps) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    // True while some app is reading the camera; when false we skip the GPU readback entirely.
    virtual bool watched() = 0;
    // Top-down BGR24, width*height*3 bytes.
    virtual void send(const uint8_t* bgr) = 0;
};
