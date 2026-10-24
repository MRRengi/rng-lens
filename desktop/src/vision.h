#pragma once
// On-PC vision for background effects and auto-framing. Runs ONNX Runtime (loaded at run time, so
// rng-lens works without it) on a worker thread over a small copy of the picture:
//   person mask: PP-HumanSeg (Apache-2.0, 192x192)
//   faces:       YuNet (MIT, 640x640, a few times a second)

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FaceBox {
    float x = 0, y = 0, w = 0, h = 0; // 0..1 of the upright picture
    float score = 0;
    int64_t atMs = 0;
};

class Vision {
public:
    ~Vision();
    // Starts loading the runtime and models on the worker thread (never blocks the UI).
    void start();
    bool ready() const { return ready_.load(); }
    bool failed() const { return failed_.load(); }
    std::string error() const;

    // Hands over the newest small RGB picture (row 0 = top). Cheap: copies and wakes the worker.
    void submit(const uint8_t* rgb, int w, int h, bool wantMask, bool wantFace);

    // Newest person mask (0..255), if a new one is ready since the last call.
    bool takeMask(std::vector<uint8_t>& out, int& w, int& h);
    FaceBox face() const;
    double segMs() const { return segMs_.load(); }
    double faceMs() const { return faceMs_.load(); }

    static constexpr int kMaskSize = 192;
    static constexpr int kFaceSize = 640;

private:
    void run();
    bool load(std::string& why);
    bool runSeg(const std::vector<uint8_t>& rgb, int w, int h);
    bool runFace(const std::vector<uint8_t>& rgb, int w, int h);
    void* session(const std::string& file);

    const struct OrtApi* api_ = nullptr;
    struct OrtEnv* env_ = nullptr;
    struct OrtSession* seg_ = nullptr;
    struct OrtSession* faceNet_ = nullptr;
    struct OrtMemoryInfo* mem_ = nullptr;
    void* lib_ = nullptr;
    std::string modelsDir_;

    std::thread thread_;
    std::atomic<bool> stop_{false}, ready_{false}, failed_{false};
    std::string error_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<uint8_t> input_;
    int inW_ = 0, inH_ = 0;
    bool hasInput_ = false, wantMask_ = false, wantFace_ = false;
    int64_t lastFaceMs_ = 0, lastSegMs_ = 0;

    std::vector<float> maskAcc_; // temporally smoothed mask
    std::vector<uint8_t> mask_;
    bool maskNew_ = false;
    FaceBox face_;
    std::atomic<double> segMs_{0}, faceMs_{0};
};
