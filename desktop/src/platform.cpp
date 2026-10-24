#include "platform.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace plat {

bool netInit() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

static void setNonBlocking(sock_t s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

sock_t tcpConnect(const std::string& host, int port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return kBadSocket;
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == kBadSocket) {
        freeaddrinfo(res);
        return kBadSocket;
    }
    setNonBlocking(s, true);
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    bool ok = rc == 0;
    if (!ok) {
#ifdef _WIN32
        bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        bool pending = errno == EINPROGRESS;
#endif
        if (pending) {
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(s, &wr);
            FD_SET(s, &ex);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select((int)s + 1, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr)) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                ok = err == 0;
            }
        }
    }
    if (!ok) {
        closeSocket(s);
        return kBadSocket;
    }
    setNonBlocking(s, false);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    int buf = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
    return s;
}

bool sendAll(sock_t s, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
#ifdef _WIN32
        int n = send(s, p, (int)len, 0);
#else
        ssize_t n = send(s, p, len, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool recvAll(sock_t s, void* data, size_t len) {
    char* p = (char*)data;
    while (len > 0) {
#ifdef _WIN32
        int n = recv(s, p, (int)len, 0);
#else
        ssize_t n = recv(s, p, len, 0);
#endif
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

void closeSocket(sock_t s) {
    if (s == kBadSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void shutdownSocket(sock_t s) {
    if (s == kBadSocket) return;
#ifdef _WIN32
    shutdown(s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
}

sock_t udpListen(int port, int timeoutMs) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSocket) return kBadSocket;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof(one));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        closeSocket(s);
        return kBadSocket;
    }
#ifdef _WIN32
    DWORD tv = (DWORD)timeoutMs;
#else
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    return s;
}

int udpReceive(sock_t s, char* buf, int len, std::string& fromIp) {
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    int n = (int)recvfrom(s, buf, len, 0, (sockaddr*)&from, &fl);
    if (n < 0) {
#ifdef _WIN32
        int e = WSAGetLastError();
        return (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? 0 : -1;
#else
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
#endif
    }
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    fromIp = ip;
    return n;
}

bool udpBroadcast(sock_t s, int port, const void* data, int len) {
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    return sendto(s, (const char*)data, len, 0, (sockaddr*)&to, sizeof(to)) == len;
}

int run(const std::string& cmdline, std::string& output) {
    output.clear();
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<char> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(0);
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    char buf[512];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) output.append(buf, n);
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int)code;
#else
    FILE* p = popen((cmdline + " 2>&1").c_str(), "r");
    if (!p) return -1;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) output.append(buf, n);
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

std::string exeDir() {
#ifdef _WIN32
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/") + 1);
#else
    char path[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    std::string s(path, n > 0 ? (size_t)n : 0);
    return s.substr(0, s.find_last_of('/') + 1);
#endif
}

std::string configDir() {
#ifdef _WIN32
    char path[MAX_PATH] = {0};
    std::string dir;
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) dir = std::string(path) + "\\rng-lens\\";
    else dir = exeDir();
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string base = xdg && *xdg ? xdg : std::string(home ? home : ".") + "/.config";
    mkdir(base.c_str(), 0755);
    std::string dir = base + "/rng-lens/";
    mkdir(dir.c_str(), 0755);
    return dir;
#endif
}

bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

}  // namespace plat
