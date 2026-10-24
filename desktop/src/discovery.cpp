#include "discovery.h"

#include <chrono>

#include "json.hpp"
#include "platform.h"
#include "protocol.h"

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Discovery::~Discovery() { stop(); }

void Discovery::start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&Discovery::run, this);
}

void Discovery::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

std::vector<Discovery::Phone> Discovery::phones() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<Phone> out;
    int64_t t = nowMs();
    for (const Phone& p : phones_)
        if (t - p.lastSeenMs < 4000) out.push_back(p);
    return out;
}

void Discovery::run() {
    sock_t s = plat::udpListen(proto::kDiscoveryPort, 500);
    listening_ = s != kBadSocket;
    if (s == kBadSocket) return;
    char buf[1024];
    static const char kProbe[] = "{\"rnglensProbe\":1}";
    int64_t lastProbe = 0;
    while (!stop_) {
        // Ask phones to answer us directly: replies to our own broadcast pass Windows Firewall
        // even before the user has allowed inbound traffic for rng-lens.
        if (nowMs() - lastProbe > 2000) {
            lastProbe = nowMs();
            plat::udpBroadcast(s, proto::kDiscoveryPort, kProbe, (int)sizeof(kProbe) - 1);
        }
        std::string ip;
        int n = plat::udpReceive(s, buf, sizeof(buf) - 1, ip);
        if (n <= 0) continue;
        auto j = nlohmann::json::parse(buf, buf + n, nullptr, false);
        if (j.is_discarded() || !j.contains("rnglens")) continue;
        Phone p;
        try {
            p.name = j.value("name", std::string("phone"));
            p.port = j.value("port", proto::kPort);
        } catch (...) {
            continue;
        }
        p.ip = ip;
        p.lastSeenMs = nowMs();
        std::lock_guard<std::mutex> lock(m_);
        bool found = false;
        for (Phone& q : phones_) {
            if (q.ip == p.ip && q.port == p.port) {
                q = p;
                found = true;
            }
        }
        if (!found) phones_.push_back(p);
    }
    plat::closeSocket(s);
}
