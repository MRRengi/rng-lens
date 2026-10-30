// Linux virtual camera: writes YUYV frames into a v4l2loopback device.
// sudo modprobe v4l2loopback exclusive_caps=1 card_label="rng-lens"
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
}

#include "vcam.h"

namespace {

class LoopbackCamera : public VirtualCamera {
public:
    explicit LoopbackCamera(int index) : index_(index & 1) {}
    ~LoopbackCamera() override {
        stop();
        if (sws_) sws_freeContext(sws_);
    }

    bool available(std::string& why) override {
        if (findDevice().empty()) {
            why = "No v4l2loopback device: sudo modprobe v4l2loopback exclusive_caps=1 card_label=Lens";
            return false;
        }
        return true;
    }

    bool start(int width, int height, int fps) override {
        (void)fps;
        stop();
        std::string dev = findDevice();
        if (dev.empty()) return false;
        fd_ = open(dev.c_str(), O_RDWR);
        if (fd_ < 0) return false;
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        f.fmt.pix.width = (unsigned)width;
        f.fmt.pix.height = (unsigned)height;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        f.fmt.pix.bytesperline = (unsigned)width * 2;
        f.fmt.pix.sizeimage = (unsigned)(width * height * 2);
        f.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
        if (ioctl(fd_, VIDIOC_S_FMT, &f) < 0) {
            stop();
            return false;
        }
        w_ = width;
        h_ = height;
        buf_.resize((size_t)width * height * 2);
        return true;
    }

    void stop() override {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

    bool running() const override { return fd_ >= 0; }

    bool watched() override { return fd_ >= 0; }

    void send(const uint8_t* bgr) override {
        if (fd_ < 0) return;
        sws_ = sws_getCachedContext(sws_, w_, h_, AV_PIX_FMT_BGR24, w_, h_, AV_PIX_FMT_YUYV422, SWS_FAST_BILINEAR,
                                    nullptr, nullptr, nullptr);
        const uint8_t* src[1] = {bgr};
        int srcStride[1] = {w_ * 3};
        uint8_t* dst[1] = {buf_.data()};
        int dstStride[1] = {w_ * 2};
        sws_scale(sws_, src, srcStride, 0, h_, dst, dstStride);
        ssize_t n = write(fd_, buf_.data(), buf_.size());
        (void)n;
    }

private:
    // Camera 1 takes the device labelled "rng-lens" (or the first loopback), camera 2 "rng-lens 2" (or the second).
    std::string findDevice() const {
        DIR* d = opendir("/sys/class/video4linux");
        if (!d) return {};
        std::vector<std::pair<std::string, std::string>> devs; // (name, path)
        while (dirent* e = readdir(d)) {
            if (strncmp(e->d_name, "video", 5) != 0) continue;
            std::string base = std::string("/sys/class/video4linux/") + e->d_name;
            FILE* f = fopen((base + "/name").c_str(), "r");
            if (!f) continue;
            char name[128] = {0};
            size_t n = fread(name, 1, sizeof(name) - 1, f);
            (void)n;
            fclose(f);
            std::string s(name);
            while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (s.find("rng-lens") != std::string::npos || s.find("Lens") != std::string::npos || s.find("Dummy") != std::string::npos ||
                s.find("loopback") != std::string::npos)
                devs.push_back({s, std::string("/dev/") + e->d_name});
        }
        closedir(d);
        std::sort(devs.begin(), devs.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        const char* want = index_ ? "Lens 2" : "Lens";
        const char* old = index_ ? "rng-lens 2" : "rng-lens"; // label used before the rename
        for (const auto& dv : devs)
            if (dv.first == want || dv.first == old) return dv.second;
        return (size_t)index_ < devs.size() ? devs[index_].second : std::string();
    }

    int index_ = 0;
    int fd_ = -1;
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> buf_;
    SwsContext* sws_ = nullptr;
};

}  // namespace

std::unique_ptr<VirtualCamera> VirtualCamera::create(int index) { return std::make_unique<LoopbackCamera>(index); }
#endif
