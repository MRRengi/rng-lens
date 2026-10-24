#pragma once
// FFmpeg H.264/HEVC decoder. Hardware (D3D11VA) on Windows when available, software otherwise.
// Decoded frames end up as YUV420P or NV12 in system memory; the newest one is kept for the renderer.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // (Re)opens the decoder for a new stream. `csd` is AnnexB SPS/PPS(/VPS).
    bool configure(int codec, int width, int height, const uint8_t* csd, size_t csdLen, bool allowHardware);
    void close();

    // Feeds one AnnexB access unit. Returns false on a hard error (caller should ask for a key frame).
    bool decode(const uint8_t* data, size_t len, int64_t ptsUs);

    // Returns the newest decoded frame since the last call, or nullptr. Caller owns it (av_frame_free).
    AVFrame* takeLatest();

    std::string description() const;
    double lastDecodeMs() const { return decodeMs_.load(); }
    uint64_t framesDecoded() const { return frames_.load(); }

private:
    bool open(bool hardware);
    void publish(AVFrame* src);
    static enum AVPixelFormat pickFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts);

    AVCodecContext* ctx_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* hwTransfer_ = nullptr;
    struct SwsContext* sws_ = nullptr;
    int codec_ = 0, width_ = 0, height_ = 0;
    bool hardware_ = false;
    bool allowHardware_ = true;
    int hwErrors_ = 0;
    std::vector<uint8_t> csd_;
    bool prependCsd_ = false;
    std::vector<uint8_t> scratch_;

    mutable std::mutex m_;
    AVFrame* latest_ = nullptr;
    std::string desc_ = "idle";
    std::atomic<double> decodeMs_{0};
    std::atomic<uint64_t> frames_{0};
};
