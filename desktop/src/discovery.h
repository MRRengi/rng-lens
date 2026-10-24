#pragma once
// Listens for the phones' UDP beacons on the LAN.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Discovery {
public:
    struct Phone {
        std::string name, ip;
        int port = 0;
        int64_t lastSeenMs = 0;
    };

    Discovery() = default;
    ~Discovery();
    void start();
    void stop();
    // Phones heard in the last few seconds.
    std::vector<Phone> phones() const;
    bool listening() const { return listening_.load(); }

private:
    void run();
    std::thread thread_;
    std::atomic<bool> stop_{false}, listening_{false};
    mutable std::mutex m_;
    std::vector<Phone> phones_;
};
