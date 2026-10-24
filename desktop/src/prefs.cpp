#include "prefs.h"

#include <fstream>

#include "platform.h"

using nlohmann::json;

static std::string path() { return plat::configDir() + "rng-lens.json"; }

// Reads `key` into `v` when present and of a usable type; anything else keeps the default.
template <class T>
static void get(const json& j, const char* key, T& v) {
    if (!j.is_object()) return;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        v = it->get<T>();
    } catch (...) {
    }
}

json FilterPrefs::toJson() const {
    return {{"brightness", brightness}, {"contrast", contrast}, {"saturation", saturation}, {"warmth", warmth},
            {"look", look}, {"lookAmount", lookAmount}, {"sharpen", sharpen}, {"smooth", smooth}, {"night", night},
            {"background", background}, {"blur", blur}, {"backgroundImage", backgroundImage},
            {"autoFrame", autoFrame}, {"frameTightness", frameTightness}, {"frameSpeed", frameSpeed}};
}

void FilterPrefs::fromJson(const json& j) {
    get(j, "brightness", brightness);
    get(j, "contrast", contrast);
    get(j, "saturation", saturation);
    get(j, "warmth", warmth);
    get(j, "look", look);
    get(j, "lookAmount", lookAmount);
    get(j, "sharpen", sharpen);
    get(j, "smooth", smooth);
    get(j, "night", night);
    get(j, "background", background);
    get(j, "blur", blur);
    get(j, "backgroundImage", backgroundImage);
    get(j, "autoFrame", autoFrame);
    get(j, "frameTightness", frameTightness);
    get(j, "frameSpeed", frameSpeed);
}

bool FilterPrefs::anyActive() const {
    return brightness != 0 || contrast != 0 || saturation != 0 || warmth != 0 || look != "none" || sharpen > 0 ||
           smooth > 0 || night > 0 || background != "off" || autoFrame;
}

json SlotPrefs::toJson() const {
    return {{"lastHost", lastHost}, {"lastPort", lastPort},     {"autoConnect", autoConnect},
            {"autoRotate", autoRotate}, {"extraTurns", extraTurns}, {"mirror", mirror},
            {"flip", flip},           {"outWidth", outWidth},     {"outHeight", outHeight},
            {"outFps", outFps},       {"virtualCamera", virtualCamera}, {"phone", phone},
            {"filters", filters.toJson()}};
}

void SlotPrefs::fromJson(const json& j) {
    get(j, "lastHost", lastHost);
    get(j, "lastPort", lastPort);
    get(j, "autoConnect", autoConnect);
    get(j, "autoRotate", autoRotate);
    get(j, "extraTurns", extraTurns);
    extraTurns &= 3;
    get(j, "mirror", mirror);
    get(j, "flip", flip);
    get(j, "outWidth", outWidth);
    get(j, "outHeight", outHeight);
    get(j, "outFps", outFps);
    get(j, "virtualCamera", virtualCamera);
    if (j.is_object() && j.contains("phone") && j["phone"].is_object()) phone = j["phone"];
    if (j.is_object() && j.contains("filters")) filters.fromJson(j["filters"]);
}

void Prefs::load() {
    std::ifstream in(path());
    if (!in) return;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    if (j.contains("slots") && j["slots"].is_array()) {
        for (size_t i = 0; i < 2 && i < j["slots"].size(); i++) slot[i].fromJson(j["slots"][i]);
    } else {
        slot[0].fromJson(j); // settings from 0.1 / 0.2 were flat: they belong to the main phone
    }
    get(j, "secondPhone", secondPhone);
    get(j, "hardwareDecode", hardwareDecode);
    get(j, "audio", audio);
    get(j, "audioSlot", audioSlot);
    audioSlot &= 1;
    get(j, "micMode", micMode);
    get(j, "audioDevice", audioDevice);
    get(j, "volume", volume);
    get(j, "muted", muted);
    get(j, "closeToTray", closeToTray);
    get(j, "hotkeys", hotkeys);
}

void Prefs::save() const {
    json j = {{"slots", json::array({slot[0].toJson(), slot[1].toJson()})},
              {"secondPhone", secondPhone},
              {"hardwareDecode", hardwareDecode},
              {"audio", audio},
              {"audioSlot", audioSlot},
              {"micMode", micMode},
              {"audioDevice", audioDevice},
              {"volume", volume},
              {"muted", muted},
              {"closeToTray", closeToTray},
              {"hotkeys", hotkeys}};
    std::ofstream out(path());
    out << j.dump(2);
}
