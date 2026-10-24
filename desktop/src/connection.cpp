#include "connection.h"

#include <chrono>
#include <vector>

#include "decoder.h"
#include "protocol.h"

using nlohmann::json;

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Connection::Connection(Decoder& decoder) : decoder_(decoder) {}

Connection::~Connection() { disconnect(); }

void Connection::connect(const std::string& host, int port, bool hardwareDecode) {
    disconnect();
    hardware_ = hardwareDecode;
    stop_ = false;
    state_ = State::Connecting;
    {
        std::lock_guard<std::mutex> lock(m_);
        error_.clear();
        peer_ = host;
        hello_ = json();
        state_json_ = json();
    }
    width_ = 0;
    height_ = 0;
    thread_ = std::thread(&Connection::run, this, host, port);
}

void Connection::disconnect() {
    stop_ = true;
    sock_t s = sock_.exchange(kBadSocket);
    plat::shutdownSocket(s);
    if (thread_.joinable()) thread_.join();
    plat::closeSocket(s);
    state_ = State::Idle;
    decoder_.close();
}

std::string Connection::error() const {
    std::lock_guard<std::mutex> lock(m_);
    return error_;
}

std::string Connection::audioCodec() const {
    std::lock_guard<std::mutex> lock(m_);
    return audioCodec_;
}

std::string Connection::peer() const {
    std::lock_guard<std::mutex> lock(m_);
    return peer_;
}

json Connection::hello() const {
    std::lock_guard<std::mutex> lock(m_);
    return hello_;
}

json Connection::phoneState() const {
    std::lock_guard<std::mutex> lock(m_);
    return state_json_;
}

void Connection::sendControl(const json& control) {
    sock_t s = sock_.load();
    if (s == kBadSocket) return;
    std::lock_guard<std::mutex> lock(sendM_);
    proto::write(s, proto::CONTROL, control.dump());
}

void Connection::requestKeyframe() {
    int64_t t = nowMs();
    if (t - lastKeyRequestMs_ < 500) return;
    lastKeyRequestMs_ = t;
    sendControl(json{{"keyframe", true}});
}

void Connection::run(std::string host, int port) {
    sock_t s = kBadSocket;
    for (int attempt = 0; attempt < 3 && !stop_ && s == kBadSocket; attempt++) {
        s = plat::tcpConnect(host, port, 3000);
    }
    if (s == kBadSocket) {
        std::lock_guard<std::mutex> lock(m_);
        error_ = "Could not reach the phone at " + host + ":" + std::to_string(port) +
                 ". Is rng-lens open on it and on the same network?";
        state_ = State::Failed;
        return;
    }
    if (stop_) {
        plat::closeSocket(s);
        return;
    }
    sock_ = s;
    std::vector<uint8_t> payload;
    std::vector<float> pcm;
    uint8_t type = 0;
    bool configured = false;
    bool sawKey = false;
    while (!stop_ && proto::read(s, type, payload)) {
        bytes_ += payload.size() + 5;
        switch (type) {
            case proto::HELLO: {
                json h = json::parse(payload.begin(), payload.end(), nullptr, false);
                std::lock_guard<std::mutex> lock(m_);
                hello_ = h;
                if (h.contains("state")) state_json_ = h["state"];
                helloSerial_++;
                stateSerial_++;
                if (onActivity) onActivity();
                break;
            }
            case proto::STATE: {
                json st = json::parse(payload.begin(), payload.end(), nullptr, false);
                std::lock_guard<std::mutex> lock(m_);
                state_json_ = st;
                stateSerial_++;
                if (onActivity) onActivity();
                break;
            }
            case proto::CONFIG: {
                if (payload.size() < 5) break;
                int codec = payload[0];
                int w = proto::u16(&payload[1]), h = proto::u16(&payload[3]);
                configured = decoder_.configure(codec, w, h, payload.data() + 5, payload.size() - 5, hardware_);
                width_ = w;
                height_ = h;
                sawKey = false;
                state_ = State::Streaming;
                break;
            }
            case proto::FRAME: {
                if (payload.size() < 9 || !configured) break;
                framesIn_++;
                int64_t pts = (int64_t)proto::u64(&payload[0]);
                bool key = payload[8] & 1;
                if (!sawKey) {
                    if (!key) {
                        requestKeyframe();
                        break;
                    }
                    sawKey = true;
                }
                uint64_t before = decoder_.framesDecoded();
                if (!decoder_.decode(payload.data() + 9, payload.size() - 9, pts)) {
                    sawKey = false;
                    requestKeyframe();
                }
                if (onActivity && decoder_.framesDecoded() != before) onActivity();
                break;
            }
            case proto::AUDIO_CONFIG: {
                if (payload.size() < 6) break;
                int codec = payload[0], rate = (int)proto::u32(&payload[1]), ch = payload[5];
                bool ok = audio_.configure(codec, rate, ch, payload.data() + 6, payload.size() - 6);
                std::lock_guard<std::mutex> lock(m_);
                audioCodec_ = ok ? audio_.description() : std::string("unsupported");
                break;
            }
            case proto::AUDIO: {
                if (payload.size() < 9) break;
                audioPackets_++;
                pcm.clear();
                audio_.decode(payload.data() + 8, payload.size() - 8, pcm);
                if (onAudio && !pcm.empty()) onAudio(pcm.data(), (int)(pcm.size() / 2), audio_.sampleRate());
                break;
            }
            default:
                break;
        }
    }
    sock_t mine = sock_.exchange(kBadSocket);
    plat::closeSocket(mine);
    audio_.close();
    {
        std::lock_guard<std::mutex> lock(m_);
        audioCodec_.clear();
    }
    if (!stop_) {
        std::lock_guard<std::mutex> lock(m_);
        error_ = "Phone disconnected";
        state_ = State::Failed;
    }
}
