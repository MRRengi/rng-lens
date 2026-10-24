#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

#include "miniaudio.h"

namespace {
constexpr int kTargetMs = 60;   // what we try to keep queued: absorbs Wi-Fi jitter
constexpr int kMaxMs = 220;     // more than this and we jump back to the target (latency beats completeness)
constexpr int kRingMs = 1000;
}  // namespace

// ------------------------------------------------------------------ decoder

AudioDecoder::AudioDecoder() {
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
}

AudioDecoder::~AudioDecoder() {
    close();
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
}

void AudioDecoder::close() { avcodec_free_context(&ctx_); }

bool AudioDecoder::configure(int codec, int sampleRate, int channels, const uint8_t* csd, size_t csdLen) {
    close();
    codec_ = codec;
    rate_ = sampleRate > 0 ? sampleRate : 48000;
    const AVCodec* dec = avcodec_find_decoder(codec == 0 ? AV_CODEC_ID_OPUS : AV_CODEC_ID_AAC);
    if (!dec) return false;
    ctx_ = avcodec_alloc_context3(dec);
    ctx_->sample_rate = rate_;
    av_channel_layout_default(&ctx_->ch_layout, channels > 0 ? channels : 1);
    std::vector<uint8_t> extra;
    if (codec == 0) {
        // Android wraps OpusHead in its own markers on some versions; find it, or build a plain one.
        const char* tag = "OpusHead";
        const uint8_t* found = csd && csdLen >= 19 ? std::search(csd, csd + csdLen, tag, tag + 8) : nullptr;
        if (found && found + 19 <= csd + csdLen) {
            extra.assign(found, found + 19);
        } else {
            extra = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, (uint8_t)(channels > 0 ? channels : 1), 0x38, 0x01,
                     (uint8_t)rate_, (uint8_t)(rate_ >> 8), (uint8_t)(rate_ >> 16), (uint8_t)(rate_ >> 24), 0, 0, 0};
        }
    } else if (csd && csdLen > 0) {
        extra.assign(csd, csd + csdLen); // AudioSpecificConfig
    }
    if (!extra.empty()) {
        ctx_->extradata = (uint8_t*)av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(ctx_->extradata, extra.data(), extra.size());
        ctx_->extradata_size = (int)extra.size();
    }
    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        return false;
    }
    return true;
}

bool AudioDecoder::decode(const uint8_t* data, size_t len, std::vector<float>& out) {
    if (!ctx_) return false;
    if (av_new_packet(pkt_, (int)len) < 0) return false;
    memcpy(pkt_->data, data, len);
    int rc = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) return false;
    while (avcodec_receive_frame(ctx_, frame_) >= 0) {
        int n = frame_->nb_samples, ch = frame_->ch_layout.nb_channels;
        size_t base = out.size();
        out.resize(base + (size_t)n * 2);
        float* o = out.data() + base;
        auto sample = [&](int c, int i) -> float {
            switch (frame_->format) {
                case AV_SAMPLE_FMT_FLTP: return ((const float*)frame_->extended_data[c])[i];
                case AV_SAMPLE_FMT_FLT: return ((const float*)frame_->data[0])[i * ch + c];
                case AV_SAMPLE_FMT_S16P: return ((const int16_t*)frame_->extended_data[c])[i] / 32768.f;
                case AV_SAMPLE_FMT_S16: return ((const int16_t*)frame_->data[0])[i * ch + c] / 32768.f;
                default: return 0.f;
            }
        };
        for (int i = 0; i < n; i++) {
            float l = sample(0, i), r = ch > 1 ? sample(1, i) : l;
            o[i * 2] = l;
            o[i * 2 + 1] = r;
        }
        av_frame_unref(frame_);
    }
    return true;
}

std::string AudioDecoder::description() const {
    if (!ctx_) return "off";
    return codec_ == 0 ? "Opus" : "AAC";
}

// ------------------------------------------------------------------ output

struct AudioOutput::Impl {
    ma_context ctx;
    bool ctxOk = false;
    ma_device device;
    bool open = false;
    std::string name;
};

static void maCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    ((AudioOutput*)dev->pUserData)->render((float*)out, frames);
}

AudioOutput::AudioOutput() : impl_(new Impl) {
    impl_->ctxOk = ma_context_init(nullptr, 0, nullptr, &impl_->ctx) == MA_SUCCESS;
}

AudioOutput::~AudioOutput() {
    close();
    if (impl_->ctxOk) ma_context_uninit(&impl_->ctx);
}

std::vector<AudioOutput::Device> AudioOutput::devices() {
    std::vector<Device> out;
    if (!impl_->ctxOk) return out;
    ma_device_info* play = nullptr;
    ma_uint32 nPlay = 0;
    if (ma_context_get_devices(&impl_->ctx, &play, &nPlay, nullptr, nullptr) != MA_SUCCESS) return out;
    for (ma_uint32 i = 0; i < nPlay; i++) out.push_back({play[i].name, play[i].isDefault != 0});
    return out;
}

bool AudioOutput::open(const std::string& deviceName, int sampleRate) {
    close();
    if (!impl_->ctxOk) return false;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = (ma_uint32)sampleRate;
    cfg.dataCallback = maCallback;
    cfg.pUserData = this;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.periodSizeInMilliseconds = 10;
    cfg.noPreSilencedOutputBuffer = MA_TRUE;
    ma_device_id id;
    if (!deviceName.empty()) {
        ma_device_info* play = nullptr;
        ma_uint32 nPlay = 0;
        if (ma_context_get_devices(&impl_->ctx, &play, &nPlay, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < nPlay; i++) {
                if (deviceName == play[i].name) {
                    id = play[i].id;
                    cfg.playback.pDeviceID = &id;
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        rate_ = sampleRate;
        ring_.assign((size_t)sampleRate * kRingMs / 1000 * 2, 0.f);
        read_ = size_ = 0;
        primed_ = false;
    }
    if (ma_device_init(&impl_->ctx, &cfg, &impl_->device) != MA_SUCCESS) return false;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        return false;
    }
    impl_->open = true;
    impl_->name = impl_->device.playback.name;
    return true;
}

void AudioOutput::close() {
    if (!impl_->open) return;
    ma_device_uninit(&impl_->device);
    impl_->open = false;
}

bool AudioOutput::isOpen() const { return impl_->open; }

std::string AudioOutput::deviceName() const { return impl_->open ? impl_->name : std::string(); }

void AudioOutput::clear() {
    std::lock_guard<std::mutex> lock(m_);
    read_ = size_ = 0;
    primed_ = false;
}

double AudioOutput::bufferedMs() const {
    return rate_ > 0 ? size_ * 1000.0 / rate_ : 0.0;
}

void AudioOutput::push(const float* frames, int count) {
    std::lock_guard<std::mutex> lock(m_);
    size_t cap = ring_.size() / 2;
    if (cap == 0) return;
    size_t maxFrames = (size_t)rate_ * kMaxMs / 1000, target = (size_t)rate_ * kTargetMs / 1000;
    for (int i = 0; i < count; i++) {
        if (size_ >= cap) { // full: drop the oldest frame
            read_ = (read_ + 1) % cap;
            size_--;
        }
        size_t w = (read_ + size_) % cap;
        ring_[w * 2] = frames[i * 2];
        ring_[w * 2 + 1] = frames[i * 2 + 1];
        size_++;
    }
    if (size_ > maxFrames) { // a burst after a Wi-Fi stall: skip ahead instead of staying late forever
        size_t drop = size_ - target;
        read_ = (read_ + drop) % cap;
        size_ -= drop;
    }
}

void AudioOutput::render(float* out, uint32_t frames) {
    float vol = muted_ ? 0.f : volume_.load(), pk = 0.f;
    std::lock_guard<std::mutex> lock(m_);
    size_t cap = ring_.size() / 2, target = (size_t)rate_ * kTargetMs / 1000;
    if (!primed_ && size_ >= target) primed_ = true;
    if (!primed_ || cap == 0) {
        memset(out, 0, frames * 2 * sizeof(float));
        return;
    }
    // Clock drift between phone and PC: when we sit above the target, skip one frame per callback (~0.2%).
    if (size_ > target + (size_t)rate_ * 40 / 1000 && size_ > frames) {
        read_ = (read_ + 1) % cap;
        size_--;
    }
    uint32_t i = 0;
    for (; i < frames && size_ > 0; i++) {
        float l = ring_[read_ * 2], r = ring_[read_ * 2 + 1];
        pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
        out[i * 2] = l * vol;
        out[i * 2 + 1] = r * vol;
        read_ = (read_ + 1) % cap;
        size_--;
    }
    if (i < frames) { // ran dry: silence and wait until the buffer refills to the target
        memset(out + i * 2, 0, (frames - i) * 2 * sizeof(float));
        primed_ = false;
        underruns_++;
    }
    if (pk > peak_.load()) peak_ = pk;
}
