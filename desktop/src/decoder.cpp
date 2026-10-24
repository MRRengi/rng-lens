#include "decoder.h"

#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "protocol.h"

Decoder::Decoder() {
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    hwTransfer_ = av_frame_alloc();
}

Decoder::~Decoder() {
    close();
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
    av_frame_free(&hwTransfer_);
    std::lock_guard<std::mutex> lock(m_);
    av_frame_free(&latest_);
}

void Decoder::close() {
    avcodec_free_context(&ctx_);
    av_buffer_unref(&hwDevice_);
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    hardware_ = false;
    std::lock_guard<std::mutex> lock(m_);
    desc_ = "idle";
}

enum AVPixelFormat Decoder::pickFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    (void)ctx;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    // No hardware surface offered: take the first software format.
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return fmts[0];
}

bool Decoder::configure(int codec, int width, int height, const uint8_t* csd, size_t csdLen, bool allowHardware) {
    close();
    codec_ = codec;
    width_ = width;
    height_ = height;
    allowHardware_ = allowHardware;
    hwErrors_ = 0;
    csd_.assign(csd, csd + csdLen);
    prependCsd_ = true;
#ifdef _WIN32
    if (allowHardware && open(true)) return true;
#endif
    return open(false);
}

bool Decoder::open(bool hardware) {
    avcodec_free_context(&ctx_);
    av_buffer_unref(&hwDevice_);
    const AVCodec* dec = avcodec_find_decoder(codec_ == proto::HEVC ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!dec) return false;
    ctx_ = avcodec_alloc_context3(dec);
    ctx_->width = width_;
    ctx_->height = height_;
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    if (!csd_.empty()) {
        ctx_->extradata = (uint8_t*)av_mallocz(csd_.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx_->extradata, csd_.data(), csd_.size());
        ctx_->extradata_size = (int)csd_.size();
    }
    if (hardware) {
        if (av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0) {
            avcodec_free_context(&ctx_);
            return false;
        }
        ctx_->hw_device_ctx = av_buffer_ref(hwDevice_);
        ctx_->get_format = &Decoder::pickFormat;
        ctx_->thread_count = 1;
    } else if ((int64_t)width_ * height_ > 1920 * 1088) {
        // Software 4K: one slice per frame from phones, so only frame threads help (costs a few frames of latency).
        ctx_->thread_type = FF_THREAD_FRAME;
        ctx_->thread_count = 4;
        ctx_->flags &= ~AV_CODEC_FLAG_LOW_DELAY;
    } else {
        ctx_->thread_type = FF_THREAD_SLICE;
        ctx_->thread_count = 0;
    }
    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        av_buffer_unref(&hwDevice_);
        return false;
    }
    hardware_ = hardware;
    std::lock_guard<std::mutex> lock(m_);
    desc_ = std::string(codec_ == proto::HEVC ? "HEVC" : "H.264") + (hardware ? " (GPU)" : " (CPU)");
    return true;
}

bool Decoder::decode(const uint8_t* data, size_t len, int64_t ptsUs) {
    if (!ctx_) return false;
    auto t0 = std::chrono::steady_clock::now();
    const uint8_t* src = data;
    size_t n = len;
    if (prependCsd_ && !csd_.empty()) {
        scratch_.resize(csd_.size() + len);
        memcpy(scratch_.data(), csd_.data(), csd_.size());
        memcpy(scratch_.data() + csd_.size(), data, len);
        src = scratch_.data();
        n = scratch_.size();
        prependCsd_ = false;
    }
    // FFmpeg wants padded input; the network buffer has no padding, so copy into the packet.
    if (av_new_packet(pkt_, (int)n) < 0) return false;
    memcpy(pkt_->data, src, n);
    pkt_->pts = ptsUs;
    int rc = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        if (hardware_ && ++hwErrors_ > 3) { // driver trouble: fall back to software for this stream
            open(false);
            prependCsd_ = true;
        }
        return false;
    }
    bool got = false;
    while (true) {
        rc = avcodec_receive_frame(ctx_, frame_);
        if (rc < 0) break;
        AVFrame* out = frame_;
        if (frame_->format == AV_PIX_FMT_D3D11) {
            av_frame_unref(hwTransfer_);
            if (av_hwframe_transfer_data(hwTransfer_, frame_, 0) < 0) {
                av_frame_unref(frame_);
                continue;
            }
            av_frame_copy_props(hwTransfer_, frame_);
            out = hwTransfer_;
        }
        publish(out);
        av_frame_unref(frame_);
        got = true;
    }
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    decodeMs_.store(ms);
    return got || rc == AVERROR(EAGAIN);
}

void Decoder::publish(AVFrame* src) {
    AVFrame* copy = av_frame_alloc();
    if (src->format == AV_PIX_FMT_YUV420P || src->format == AV_PIX_FMT_YUVJ420P || src->format == AV_PIX_FMT_NV12) {
        av_frame_ref(copy, src);
    } else {
        // Anything else (10-bit, 4:2:2...) is converted once to 8-bit 4:2:0 so the renderer has two cases only.
        copy->format = AV_PIX_FMT_YUV420P;
        copy->width = src->width;
        copy->height = src->height;
        av_frame_get_buffer(copy, 32);
        sws_ = sws_getCachedContext(sws_, src->width, src->height, (AVPixelFormat)src->format, src->width, src->height,
                                    AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        sws_scale(sws_, src->data, src->linesize, 0, src->height, copy->data, copy->linesize);
        av_frame_copy_props(copy, src);
    }
    frames_++;
    std::lock_guard<std::mutex> lock(m_);
    av_frame_free(&latest_);
    latest_ = copy;
}

AVFrame* Decoder::takeLatest() {
    std::lock_guard<std::mutex> lock(m_);
    AVFrame* f = latest_;
    latest_ = nullptr;
    return f;
}

std::string Decoder::description() const {
    std::lock_guard<std::mutex> lock(m_);
    return desc_;
}
