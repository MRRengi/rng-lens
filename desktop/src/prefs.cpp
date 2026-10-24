#include "prefs.h"

#include <fstream>

#include "platform.h"

using nlohmann::json;

static std::string path() { return plat::configDir() + "rng-lens.json"; }

void Prefs::load() {
    std::ifstream in(path());
    if (!in) return;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    try {
    lastHost = j.value("lastHost", lastHost);
    lastPort = j.value("lastPort", lastPort);
    autoConnect = j.value("autoConnect", autoConnect);
    hardwareDecode = j.value("hardwareDecode", hardwareDecode);
    autoRotate = j.value("autoRotate", autoRotate);
    extraTurns = j.value("extraTurns", extraTurns) & 3;
    mirror = j.value("mirror", mirror);
    flip = j.value("flip", flip);
    outWidth = j.value("outWidth", outWidth);
    outHeight = j.value("outHeight", outHeight);
    outFps = j.value("outFps", outFps);
    virtualCamera = j.value("virtualCamera", virtualCamera);
    audio = j.value("audio", audio);
    micMode = j.value("micMode", micMode);
    audioDevice = j.value("audioDevice", audioDevice);
    volume = j.value("volume", volume);
    muted = j.value("muted", muted);
    if (j.contains("phone") && j["phone"].is_object()) phone = j["phone"];
    } catch (...) { // a hand-edited or old file: keep the defaults for whatever didn't parse
    }
}

void Prefs::save() const {
    json j = {
        {"lastHost", lastHost},     {"lastPort", lastPort},       {"autoConnect", autoConnect},
        {"hardwareDecode", hardwareDecode}, {"autoRotate", autoRotate}, {"extraTurns", extraTurns},
        {"mirror", mirror},         {"flip", flip},               {"outWidth", outWidth},
        {"outHeight", outHeight},   {"outFps", outFps},           {"virtualCamera", virtualCamera},
        {"phone", phone},           {"audio", audio},             {"micMode", micMode},
        {"audioDevice", audioDevice}, {"volume", volume},         {"muted", muted},
    };
    std::ofstream out(path());
    out << j.dump(2);
}
