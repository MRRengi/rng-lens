#pragma once
// Phone microphone on the PC: FFmpeg decode (Opus / AAC) and low-latency playback through miniaudio.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

// Decodes AUDIO packets into interleaved stereo float at the stream's sample rate.
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool configure(int codec, int sampleRate, int channels, const uint8_t* csd, size_t csdLen);
    void close();
    // Appends decoded stereo frames to `out`. Returns false on a decode error (packet is skipped).
    bool decode(const uint8_t* data, size_t len, std::vector<float>& out);
    int sampleRate() const { return rate_; }
    std::string description() const;

private:
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int codec_ = 0, rate_ = 48000;
};

// Plays stereo float through the chosen output device with a small jitter buffer.
class AudioOutput {
public:
    struct Device {
        std::string name;
        bool isDefault = false;
    };

    AudioOutput();
    ~AudioOutput();

    std::vector<Device> devices();
    // Empty name = system default. Returns false (and plays nothing) if the device can't be opened.
    bool open(const std::string& deviceName, int sampleRate);
    void close();
    bool isOpen() const;
    std::string deviceName() const;

    // Called from the network thread with interleaved stereo frames.
    void push(const float* frames, int count);
    void clear();

    void setVolume(float v) { volume_ = v; }
    void setMuted(bool m) { muted_ = m; }
    float peak() { return peak_.exchange(0.f); } // since the last call, 0..1
    double bufferedMs() const;
    uint64_t underruns() const { return underruns_.load(); }

    void render(float* out, uint32_t frames); // device callback

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<float> volume_{1.f}, peak_{0.f};
    std::atomic<bool> muted_{false};
    std::atomic<uint64_t> underruns_{0};

    std::mutex m_;
    std::vector<float> ring_; // stereo frames
    size_t read_ = 0, size_ = 0;
    bool primed_ = false;
    int rate_ = 48000;
};
