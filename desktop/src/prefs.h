#pragma once
// User preferences saved between runs (rng-lens.json in the per-user config folder).

#include <string>

#include "json.hpp"

// Picture effects, all done on the PC's GPU. Zero = off.
struct FilterPrefs {
    float brightness = 0.f; // -1..1
    float contrast = 0.f;   // -1..1
    float saturation = 0.f; // -1..1
    float warmth = 0.f;     // -1 (cool) .. 1 (warm)
    std::string look = "none"; // none | warm | cool | film | vivid | fade | bw
    float lookAmount = 1.f;
    float sharpen = 0.f;  // 0..1
    float smooth = 0.f;   // skin smoothing 0..1
    float night = 0.f;    // temporal denoise 0..1
    std::string background = "off"; // off | blur | image
    float blur = 0.6f;                // background blur 0..1
    std::string backgroundImage;      // path for "image"
    bool autoFrame = false;
    float frameTightness = 0.5f; // 0 = loose (head and shoulders wide) .. 1 = close-up
    float frameSpeed = 0.5f;     // how fast the frame follows

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
    bool anyActive() const;
};

// Everything that belongs to one phone ("slot": the main phone, or the optional second one).
struct SlotPrefs {
    std::string lastHost;
    int lastPort = 38440;
    bool autoConnect = true;
    bool autoRotate = true;
    int extraTurns = 0;
    bool mirror = false;
    bool flip = false;
    int outWidth = 1920;
    int outHeight = 1080;
    int outFps = 30;
    bool virtualCamera = true;
    nlohmann::json phone = nlohmann::json::object(); // camera/width/height/fps/codec/bitrate to ask for on connect
    FilterPrefs filters;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

struct Prefs {
    SlotPrefs slot[2];
    bool secondPhone = false; // the optional second phone tab and camera "rng-lens 2"
    bool hardwareDecode = true;
    bool audio = true;             // stream the phone microphone
    int audioSlot = 0;             // which phone's mic plays
    std::string micMode = "voice"; // voice | raw
    std::string audioDevice;       // empty = system default output
    float volume = 1.f;
    bool muted = false;
    bool closeToTray = true;
    bool hotkeys = true;

    void load();
    void save() const;
};
