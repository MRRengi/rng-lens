#pragma once
// Sockets, processes and paths that differ between Windows and Linux.

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
constexpr sock_t kBadSocket = INVALID_SOCKET;
#else
using sock_t = int;
constexpr sock_t kBadSocket = -1;
#endif

namespace plat {

bool netInit();

// TCP connect with a timeout. Returns kBadSocket on failure.
sock_t tcpConnect(const std::string& host, int port, int timeoutMs);
bool sendAll(sock_t s, const void* data, size_t len);
bool recvAll(sock_t s, void* data, size_t len);
void closeSocket(sock_t s);
void shutdownSocket(sock_t s);

// UDP socket bound to `port` on all interfaces, with a receive timeout. kBadSocket on failure.
sock_t udpListen(int port, int timeoutMs);
// Returns bytes received (0 on timeout, <0 on error); fills the sender's IPv4 address.
int udpReceive(sock_t s, char* buf, int len, std::string& fromIp);

// Broadcasts a datagram to 255.255.255.255:port from socket `s`.
bool udpBroadcast(sock_t s, int port, const void* data, int len);

// Runs a command without a console window; returns exit code and captured stdout+stderr.
int run(const std::string& cmdline, std::string& output);

// Directory of the running executable, with a trailing separator.
std::string exeDir();
// Per-user config directory (created if missing), with a trailing separator.
std::string configDir();

bool fileExists(const std::string& path);

}  // namespace plat
