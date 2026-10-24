#pragma once
// User preferences saved between runs (rng-lens.json in the per-user config folder).

#include <string>

#include "json.hpp"

struct Prefs {
    std::string lastHost;
    int lastPort = 38440;
    bool autoConnect = true; // reconnect to the last phone as soon as its beacon shows up
    bool hardwareDecode = true;
    bool autoRotate = true;
    int extraTurns = 0;
    bool mirror = false;
    bool flip = false;
    int outWidth = 1920;
    int outHeight = 1080;
    int outFps = 30;
    bool virtualCamera = true;
    nlohmann::json phone = nlohmann::json::object(); // camera/width/height/fps/codec/bitrate to ask for on connect

    void load();
    void save() const;
};
