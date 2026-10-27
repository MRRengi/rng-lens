#pragma once
// Update check: asks GitHub for the latest release, and on request downloads the installer and starts it.
// Windows only (WinHTTP); elsewhere it reports Unsupported. All network work runs on a worker thread.
// RNGLENS_UPDATE_URL overrides the release API URL (used by the tests).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed, Unsupported };

    ~Updater();
    void check(const std::string& currentVersion); // no-op while a check or download runs
    void download();                               // after Available
    bool launchInstaller();                        // after Ready; the caller quits right after

    State state() const { return state_.load(); }
    std::string latest() const;
    std::string error() const;
    float progress() const { return progress_.load(); } // 0..1 while downloading

    static bool newer(const std::string& a, const std::string& b); // a > b, "1.10.0" > "1.9.3"
    // Blocking download of `url` into `path` (Windows only). For the phone APK.
    static bool fetch(const std::string& url, const std::string& path, std::string& err);

private:
    void join();
    void set(State s, const std::string& err = std::string());

    std::thread worker_;
    std::atomic<State> state_{State::Idle};
    std::atomic<float> progress_{0.f};
    mutable std::mutex m_;
    std::string latest_, assetUrl_, error_, file_;
};
