// rng-lens desktop. Without arguments: the window. With --headless: the test-bench client.
//   rng-lens [--connect 192.168.1.5]
//   rng-lens --headless --connect 127.0.0.1[:38440] [--frames 120] [--png out.png] [--control '{"zoom":2}']

#include <cstdlib>
#include <cstring>
#include <string>

#include "app.h"
#include "headless.h"
#include "prefs.h"
#include "protocol.h"

int main(int argc, char** argv) {
    bool headless = false;
    std::string host = "127.0.0.1", png, control, wav;
    AppOptions opt;
    bool connect = false;
    int port = proto::kPort, frames = 120;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--headless") headless = true;
        else if (a == "--discover") return runDiscover(atoi(next().c_str()));
        else if (a == "--dump-output") opt.dumpOutput = next();
        else if (a == "--dump-after") opt.dumpAfter = atoi(next().c_str());
        else if (a == "--connect") {
            connect = true;
            host = next();
            size_t c = host.find(':');
            if (c != std::string::npos) {
                port = atoi(host.c_str() + c + 1);
                host = host.substr(0, c);
            }
        } else if (a == "--frames") frames = atoi(next().c_str());
        else if (a == "--png") png = next();
        else if (a == "--control") control = next();
        else if (a == "--wav") wav = next();
    }
    if (headless) return runHeadless(host, port, frames, png, control, wav);
    Prefs prefs;
    prefs.load();
    if (connect) {
        opt.connectHost = host;
        opt.connectPort = port;
    }
    return runApp(prefs, opt);
}
