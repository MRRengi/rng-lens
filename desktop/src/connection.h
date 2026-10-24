#pragma once
// One phone connection: a reader thread that receives HELLO/CONFIG/FRAME/STATE, decodes frames
// in place (TCP backpressure makes the phone drop frames when we are slow) and sends CONTROL.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "audio.h"
#include "json.hpp"
#include "platform.h"

class Decoder;

class Connection {
public:
    enum class State { Idle, Connecting, Streaming, Failed };

    explicit Connection(Decoder& decoder);

    // Called on the network thread after a frame is decoded or the phone state changes
    // (the GUI uses it to wake its event loop). Set before connect().
    std::function<void()> onActivity;
    // Decoded phone audio, interleaved stereo float at `rate` Hz. Called on the network thread. Set before connect().
    std::function<void(const float* frames, int count, int rate)> onAudio;
    ~Connection();

    void connect(const std::string& host, int port, bool hardwareDecode);
    void disconnect();

    State state() const { return state_.load(); }
    std::string error() const;
    std::string peer() const;

    // Sends a CONTROL message (any subset of the settings in docs/SPEC.md).
    void sendControl(const nlohmann::json& control);

    // Latest HELLO / STATE from the phone; the counters change whenever a new one arrives.
    nlohmann::json hello() const;
    nlohmann::json phoneState() const;
    uint64_t helloSerial() const { return helloSerial_.load(); }
    uint64_t stateSerial() const { return stateSerial_.load(); }

    uint64_t bytesReceived() const { return bytes_.load(); }
    uint64_t framesReceived() const { return framesIn_.load(); }
    int streamWidth() const { return width_.load(); }
    int streamHeight() const { return height_.load(); }
    std::string audioCodec() const;
    uint64_t audioPackets() const { return audioPackets_.load(); }

private:
    void run(std::string host, int port);
    void requestKeyframe();

    Decoder& decoder_;
    std::thread thread_;
    std::atomic<sock_t> sock_{kBadSocket};
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Idle};
    bool hardware_ = true;

    mutable std::mutex m_;
    std::string error_, peer_;
    nlohmann::json hello_, state_json_;
    std::atomic<uint64_t> helloSerial_{0}, stateSerial_{0};

    std::mutex sendM_;
    std::atomic<uint64_t> bytes_{0}, framesIn_{0}, audioPackets_{0};
    AudioDecoder audio_;
    std::string audioCodec_;
    std::atomic<int> width_{0}, height_{0};
    int64_t lastKeyRequestMs_ = 0;
};
