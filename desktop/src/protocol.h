#pragma once
// Wire format, mirror of android/src/dev/rengi/lens/Protocol.java. See docs/SPEC.md.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "platform.h"

namespace proto {

constexpr int kVersion = 1;
constexpr int kPort = 38440;
constexpr int kDiscoveryPort = 38441;

enum Type : uint8_t {
    HELLO = 0x01,
    CONFIG = 0x02,
    FRAME = 0x03,
    STATE = 0x04,
    CONTROL = 0x10,
    PING = 0x11,
};

enum Codec : int { H264 = 0, HEVC = 1 };

constexpr uint32_t kMaxMessage = 64u * 1024u * 1024u;

inline uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
inline uint64_t u64(const uint8_t* p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

// Reads one message into `payload` (reused between calls to avoid reallocations).
inline bool read(sock_t s, uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t h[5];
    if (!plat::recvAll(s, h, 5)) return false;
    type = h[0];
    uint32_t len = u32(h + 1);
    if (len > kMaxMessage) return false;
    if (payload.capacity() < len) payload.reserve(len + len / 4);
    payload.resize(len);
    return len == 0 || plat::recvAll(s, payload.data(), len);
}

inline bool write(sock_t s, uint8_t type, const std::string& body) {
    std::vector<uint8_t> m(5 + body.size());
    m[0] = type;
    uint32_t n = (uint32_t)body.size();
    m[1] = (uint8_t)n;
    m[2] = (uint8_t)(n >> 8);
    m[3] = (uint8_t)(n >> 16);
    m[4] = (uint8_t)(n >> 24);
    if (n) memcpy(m.data() + 5, body.data(), n);
    return plat::sendAll(s, m.data(), m.size());
}

}  // namespace proto
