// The desktop window: preview on the left, controls on the right.
// One Session per phone (the main one, plus the optional second). The loop sleeps until something
// happens (input, a decoded frame, a phone state change), so an idle rng-lens costs nothing.

#include "app.h"
#include "autoframe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#endif

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "audio.h"
#include "connection.h"
#include "decoder.h"
#include "discovery.h"
#include "platform.h"
#include "prefs.h"
#include "protocol.h"
#include "renderer.h"
#include "shell.h"
#include "stb_image_write.h"
#include "vcam.h"
#include "vision.h"

using nlohmann::json;

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Tolerant JSON field read: missing, null or mistyped fields give the default instead of throwing.
template <class T>
T jv(const json& j, const char* key, const T& def) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}
inline std::string jv(const json& j, const char* key, const char* def) { return jv<std::string>(j, key, std::string(def)); }

const ImVec4 kAccent = ImVec4(1.00f, 0.38f, 0.25f, 1.00f);
const ImVec4 kMuted = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
const ImVec4 kGood = ImVec4(0.35f, 0.85f, 0.50f, 1.00f);
const ImVec4 kWarn = ImVec4(1.00f, 0.78f, 0.30f, 1.00f);
const ImVec4 kBad = ImVec4(1.00f, 0.40f, 0.40f, 1.00f);

// Coloured text that wraps at the panel edge (long hints and errors).
void wrapped(const ImVec4& c, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

bool radio(const char* label, std::string& v, const char* value) {
    bool on = v == value;
    if (ImGui::RadioButton(label, on) && !on) {
        v = value;
        return true;
    }
    return false;
}

std::string clock(int64_t ms) {
    int64_t s = ms / 1000;
    char b[32];
    if (s >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(b, sizeof(b), "%d:%02d", (int)(s / 60), (int)(s % 60));
    return b;
}

struct PhoneUi {
    std::string camera;
    int width = 1920, height = 1080, fps = 30, codec = 0, bitrate = 0;
    float zoom = 1.f, focus = 0.f;
    std::string af = "auto", ae = "auto", awb = "auto";
    int ev = 0;
    bool torch = false, stab = true;

    void from(const json& s) {
        if (!s.is_object()) return;
        if (s.contains("camera") && s["camera"].is_string()) camera = s["camera"];
        width = jv(s, "width", width);
        height = jv(s, "height", height);
        fps = jv(s, "fps", fps);
        codec = jv(s, "codec", codec);
        bitrate = jv(s, "bitrate", bitrate);
        zoom = (float)jv(s, "zoom", (double)zoom);
        focus = (float)jv(s, "focus", (double)focus);
        af = jv(s, "af", af);
        ae = jv(s, "ae", ae);
        awb = jv(s, "awb", awb);
        ev = jv(s, "ev", ev);
        torch = jv(s, "torch", torch);
        stab = jv(s, "stab", stab);
    }
};

struct OutputSize {
    const char* label;
    int w, h;
};
const OutputSize kOutputs[] = {
    {"1280 x 720", 1280, 720},     {"1920 x 1080", 1920, 1080}, {"2560 x 1440", 2560, 1440},
    {"3840 x 2160", 3840, 2160},   {"Vertical 1080 x 1920", 1080, 1920}, {"Square 1080 x 1080", 1080, 1080},
};

struct Look {
    const char* id;
    const char* label;
};
const Look kLooks[] = {{"none", "None"}, {"warm", "Warm"}, {"cool", "Cool"}, {"film", "Film"},
                       {"vivid", "Vivid"}, {"fade", "Faded"}, {"bw", "Black & white"}};

// ======================================================================================= Session

// Everything for one phone: connection, decoding, GPU chain, virtual camera, vision, UI state.
struct Session {
    int index;
    Prefs& prefs;
    SlotPrefs& sp;
    Decoder decoder;
    Connection conn{decoder};
    Renderer renderer;
    std::unique_ptr<VirtualCamera> vcam;
    Vision vision;
    std::string vcamWhy, usbMsg, visionWhy;
    char ipBuf[64] = {0};

    json hello = json::object(), pstate = json::object(), cam = json::object();
    uint64_t seenHello = 0, seenState = 0;
    PhoneUi ui;
    OutputTransform lastT{-1, false, false};
    int lastOutW = 0, lastOutH = 0;
    bool dirty = true; // a setting changed: re-render even without a new picture
    CropRect crop;     // current (smoothed) auto-frame crop
    int64_t lastRenderMs = 0, lastAutoTry = 0, rateAt = 0, streamStart = 0, lastTempAt = 0, lastFaceSeen = 0;
    bool userIdle = false;
    uint64_t framesShown = 0, rateFrames = 0, rateBytes = 0, vcamSent = 0;
    double mbps = 0, fpsShown = 0;
    std::deque<float> temps; // phone temperature, one sample every 2 s
    std::vector<uint8_t> maskBuf;

    Session(int i, Prefs& p) : index(i), prefs(p), sp(p.slot[i]) {}

    bool streaming() const { return conn.state() == Connection::State::Streaming; }
    std::string name() const { return index == 0 ? "rng-lens" : "rng-lens 2"; }
    std::string device() const { return jv(hello, "device", std::string()); }

    bool rendererReady = false;
    void initRenderer() {
        if (!rendererReady) rendererReady = renderer.init() || true;
    }

    void init() {
        initRenderer();
        vcam = VirtualCamera::create(index);
        restartVcam();
        snprintf(ipBuf, sizeof(ipBuf), "%s", sp.lastHost.c_str());
        if (sp.filters.background == "image") renderer.setBackgroundImage(sp.filters.backgroundImage);
    }

    void restartVcam() {
        if (!vcam) return;
        vcam->stop();
        vcamWhy.clear();
        if (!sp.virtualCamera) return;
        if (!vcam->available(vcamWhy)) return;
        if (!vcam->start(sp.outWidth, sp.outHeight, sp.outFps)) vcamWhy = "Could not start the virtual camera";
    }

    void connectTo(const std::string& host, int port) {
        userIdle = false;
        seenHello = seenState = 0;
        hello = json::object();
        pstate = json::object();
        temps.clear();
        streamStart = nowMs();
        conn.connect(host, port, prefs.hardwareDecode);
        sp.lastHost = host;
        sp.lastPort = port;
        prefs.save();
    }

    void control(const json& j) {
        conn.sendControl(j);
        for (const char* k : {"camera", "width", "height", "fps", "codec", "bitrate"})
            if (j.contains(k)) sp.phone[k] = j[k];
        if (j.contains("camera")) {
            sp.phone.erase("width");
            sp.phone.erase("height");
        }
        prefs.save();
    }

    OutputTransform transform() const {
        OutputTransform t;
        int turns = sp.extraTurns;
        if (sp.autoRotate && !cam.empty()) {
            int so = jv(cam, "sensorOrientation", 0);
            int orient = jv(pstate, "orient", 0);
            bool front = jv(cam, "facing", std::string()) == "front";
            int deg = front ? (so - orient + 360) % 360 : (so + orient) % 360;
            turns += deg / 90;
        }
        t.quarterTurns = turns & 3;
        t.mirror = sp.mirror;
        t.flip = sp.flip;
        return t;
    }

    void syncFromPhone() {
        if (conn.helloSerial() != seenHello) {
            seenHello = conn.helloSerial();
            hello = conn.hello();
            json want = sp.phone.is_object() ? sp.phone : json::object();
            want["audio"] = prefs.audio && prefs.audioSlot == index;
            want["mic"] = prefs.micMode;
            conn.sendControl(want);
        }
        if (conn.stateSerial() != seenState && !ImGui::IsAnyItemActive()) {
            seenState = conn.stateSerial();
            pstate = conn.phoneState();
            ui.from(pstate);
        }
        cam = json::object();
        if (hello.is_object() && hello.contains("cameras") && hello["cameras"].is_array()) {
            for (const json& c : hello["cameras"])
                if (jv(c, "id", std::string()) == ui.camera) cam = c;
        }
        int64_t now = nowMs();
        if (streaming() && now - lastTempAt >= 2000 && pstate.contains("tempC")) {
            lastTempAt = now;
            temps.push_back((float)jv(pstate, "tempC", 0.0));
            while (temps.size() > 450) temps.pop_front(); // 15 minutes
        }
    }

    // Starts the ML worker the first time an effect needs it (loads in the background).
    bool ensureVision() {
        vision.start();
        return vision.ready();
    }

    // Auto-frame: follow the main face with a filtered, dead-zoned, spring-driven crop (autoframe.h).
    bool updateCrop(float dt, int ww, int wh) {
        const FilterPrefs& f = sp.filters;
        FaceBox fb;
        bool valid = false;
        if (f.autoFrame && vision.ready() && ww > 0 && wh > 0) {
            fb = vision.face();
            int64_t now = nowMs();
            if (fb.score > 0 && now - fb.atMs < 1500) lastFaceSeen = now;
            valid = fb.score > 0 && now - lastFaceSeen < 2500;
        }
        if (ww <= 0 || wh <= 0) return false;
        return autoFrame.update(dt, fb, valid, ww, wh, (float)sp.outWidth / sp.outHeight, f.frameTightness,
                                f.frameSpeed, crop);
    }
    AutoFrame autoFrame;
    int64_t lastTick = 0;

    // One loop step. Returns the BGR frame for the vcam via the readback inside.
    void tick(bool dumpNow, const std::string& dumpPath, bool& quit) {
        syncFromPhone();
        int64_t now = nowMs();
        float dt = lastTick ? std::min(0.2f, (now - lastTick) / 1000.f) : 0.016f;
        lastTick = now;

        bool newFrame = false;
        if (AVFrame* f = decoder.takeLatest()) {
            renderer.upload(f);
            newFrame = true;
            framesShown++;
        }
        OutputTransform t = transform();
        const FilterPrefs& f = sp.filters;
        bool wantMask = f.background != "off", wantFace = f.autoFrame;
        if ((wantMask || wantFace) && ensureVision()) {
            int aw, ah;
            if (const uint8_t* px = renderer.mapAnalysis(aw, ah)) vision.submit(px, aw, ah, wantMask, wantFace);
            renderer.unmapAnalysis();
            int maskW, maskH;
            if (wantMask && vision.takeMask(maskBuf, maskW, maskH)) renderer.setMask(maskBuf.data(), maskW, maskH);
            if (newFrame) {
                int ww = renderer.workWidth(t.quarterTurns), wh = renderer.workHeight(t.quarterTurns);
                renderer.requestAnalysis(ww >= wh ? 640 : std::max(64, 640 * ww / std::max(1, wh)));
            }
        }
        if (!wantMask) renderer.clearMask();
        bool cropMoved = updateCrop(dt, renderer.workWidth(t.quarterTurns), renderer.workHeight(t.quarterTurns));

        bool changed = t.quarterTurns != lastT.quarterTurns || t.mirror != lastT.mirror || t.flip != lastT.flip ||
                       sp.outWidth != lastOutW || sp.outHeight != lastOutH || dirty;
        bool idleRefresh = !renderer.hasFrame() && now - lastRenderMs > 500; // keep apps fed while waiting
        bool pending = false;
        if (newFrame || changed || cropMoved || idleRefresh) {
            renderer.render(sp.outWidth, sp.outHeight, t, f, crop, newFrame);
            lastT = t;
            lastOutW = sp.outWidth;
            lastOutH = sp.outHeight;
            dirty = false;
            lastRenderMs = now;
            if (dumpNow || (vcam && vcam->running() && vcam->watched())) {
                renderer.beginReadback();
                pending = true;
            }
        }
        rates(now);
        readbackPending = pending;
        (void)dumpPath;
        (void)quit;
    }
    bool readbackPending = false;

    // Second half of the frame (after the UI was drawn, so the GPU had time): hand the picture over.
    void deliver(bool dump, const std::string& dumpPath, bool& quit) {
        if (!readbackPending) return;
        readbackPending = false;
        if (const uint8_t* px = renderer.mapReadback()) {
            if (vcam && vcam->running() && vcam->watched()) {
                vcam->send(px);
                vcamSent++;
            }
            if (dump) {
                int w = renderer.outputWidth(), h = renderer.outputHeight();
                std::vector<uint8_t> rgb((size_t)w * h * 3);
                for (size_t i = 0; i < rgb.size(); i += 3) {
                    rgb[i] = px[i + 2];
                    rgb[i + 1] = px[i + 1];
                    rgb[i + 2] = px[i];
                }
                stbi_write_png(dumpPath.c_str(), w, h, 3, rgb.data(), w * 3);
                printf("dumped %dx%d after %llu frames, %s, vision %s\n", w, h, (unsigned long long)framesShown,
                       decoder.description().c_str(), vision.ready() ? "on" : (vision.failed() ? vision.error().c_str() : "off"));
                fflush(stdout);
                quit = true;
            }
        }
        renderer.unmapReadback();
    }

    void rates(int64_t now) {
        if (now - rateAt < 1000) return;
        double dts = (now - rateAt) / 1000.0;
        uint64_t b = conn.bytesReceived();
        mbps = (b - rateBytes) * 8.0 / 1e6 / dts;
        fpsShown = (framesShown - rateFrames) / dts;
        rateBytes = b;
        rateFrames = framesShown;
        rateAt = now;
    }
};

// ======================================================================================= App

class App {
public:
    App(Prefs& prefs, const AppOptions& opt) : prefs_(prefs), opt_(opt) {
        sessions_[0] = std::make_unique<Session>(0, prefs);
        sessions_[1] = std::make_unique<Session>(1, prefs);
    }

    int run() {
        if (!initWindow()) return 1;
        plat::netInit();
        for (int i = 0; i < 2; i++) {
            Session& s = *sessions_[i];
            if (i == 1 && !prefs_.secondPhone) {
                s.initRenderer(); // cheap; the camera is only started when the second phone is on
                continue;
            }
            s.init();
        }
        disc_.start();
        for (int i = 0; i < 2; i++) {
            Session& s = *sessions_[i];
            s.conn.onActivity = [] { glfwPostEmptyEvent(); };
            s.conn.onAudio = [this, i](const float* f, int n, int rate) {
                if (prefs_.audioSlot != i) return;
                audioRate_ = rate;
                audio_.push(f, n);
            };
        }
        audio_.setVolume(prefs_.volume);
        audio_.setMuted(prefs_.muted);
        openAudio();
        shell_.init(win_, [this](int cmd) { command(cmd); });
        shell_.setHotkeysEnabled(prefs_.hotkeys);
        autostart_ = Shell::autostart();
        if (!opt_.connectHost.empty()) sessions_[0]->connectTo(opt_.connectHost, opt_.connectPort);
        if (opt_.startHidden && shell_.supported()) glfwHideWindow(win_);
        else glfwShowWindow(win_);

        while (!glfwWindowShouldClose(win_) && !quit_) {
            glfwWaitEventsTimeout(0.25);
            tick();
        }

        shell_.shutdown();
        for (auto& s : sessions_) {
            s->conn.disconnect();
            if (s->vcam) s->vcam->stop();
        }
        disc_.stop();
        audio_.close();
        prefs_.save();
        for (auto& s : sessions_)
            if (s->rendererReady) s->renderer.shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(win_);
        glfwTerminate();
        return 0;
    }

private:
    bool initWindow() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        int ww = 1320, wh = 860;
        if (const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor())) {
            ww = std::min(ww, mode->width - 40);
            wh = std::min(960, mode->height - 120);
        }
        win_ = glfwCreateWindow(ww, wh, "rng-lens", nullptr, nullptr);
        if (!win_) return false;
        glfwSetWindowUserPointer(win_, this);
        glfwSetWindowCloseCallback(win_, [](GLFWwindow* w) {
            App* app = (App*)glfwGetWindowUserPointer(w);
            if (app->prefs_.closeToTray && app->shell_.supported() && opt_dumpless(app)) {
                glfwSetWindowShouldClose(w, GLFW_FALSE);
                glfwHideWindow(w); // keeps the camera running; quit from the tray icon
            }
        });
        glfwMakeContextCurrent(win_);
        if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) return false;
        glfwSwapInterval(1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        static std::string ini = plat::configDir() + "window.ini";
        io.IniFilename = ini.c_str();
        float xs = 1.f, ys = 1.f;
        glfwGetWindowContentScale(win_, &xs, &ys);
        loadFont(16.f * xs);
        style(xs);
        ImGui_ImplGlfw_InitForOpenGL(win_, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
        return true;
    }
    static bool opt_dumpless(App* app) { return app->opt_.dumpOutput.empty(); }

    void loadFont(float px) {
        ImGuiIO& io = ImGui::GetIO();
        const char* candidates[] = {
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
        };
        for (const char* f : candidates) {
            if (plat::fileExists(f) && io.Fonts->AddFontFromFileTTF(f, px)) return;
        }
        ImFontConfig cfg;
        cfg.SizePixels = px;
        io.Fonts->AddFontDefault(&cfg);
    }

    void style(float scale) {
        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 0.f;
        s.FrameRounding = 6.f;
        s.GrabRounding = 6.f;
        s.ChildRounding = 8.f;
        s.PopupRounding = 6.f;
        s.TabRounding = 6.f;
        s.FramePadding = ImVec2(10, 6);
        s.ItemSpacing = ImVec2(10, 8);
        s.WindowPadding = ImVec2(16, 16);
        s.ScrollbarSize = 10.f;
        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.055f, 0.07f, 1.f);
        c[ImGuiCol_ChildBg] = ImVec4(0.085f, 0.085f, 0.105f, 1.f);
        c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.17f, 1.f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.23f, 1.f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.27f, 1.f);
        c[ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.20f, 1.f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.24f, 0.29f, 1.f);
        c[ImGuiCol_ButtonActive] = kAccent;
        c[ImGuiCol_CheckMark] = kAccent;
        c[ImGuiCol_SliderGrab] = kAccent;
        c[ImGuiCol_SliderGrabActive] = ImVec4(1.f, 0.55f, 0.42f, 1.f);
        c[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.22f, 1.f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.24f, 0.29f, 1.f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.20f, 0.18f, 1.f);
        c[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.24f, 1.f);
        c[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.17f, 1.f);
        c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.20f, 0.18f, 1.f);
        c[ImGuiCol_TabSelected] = ImVec4(0.38f, 0.18f, 0.14f, 1.f);
        c[ImGuiCol_PlotLines] = kAccent;
        c[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.95f, 1.f);
        c[ImGuiCol_TextDisabled] = kMuted;
        c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.f);
        s.ScaleAllSizes(scale);
    }

    // ---------------------------------------------------------------- loop

    int active() const { return prefs_.secondPhone ? 2 : 1; }

    void tick() {
        for (int i = 0; i < active(); i++) autoConnect(*sessions_[i], i);
        bool dump = !opt_.dumpOutput.empty() && sessions_[0]->framesShown >= (uint64_t)opt_.dumpAfter;
        for (int i = 0; i < active(); i++) sessions_[i]->tick(dump && i == 0, opt_.dumpOutput, quit_);

        bool visible = glfwGetWindowAttrib(win_, GLFW_VISIBLE) && !glfwGetWindowAttrib(win_, GLFW_ICONIFIED);
        if (visible) drawUi();
        for (int i = 0; i < active(); i++) sessions_[i]->deliver(dump && i == 0, opt_.dumpOutput, quit_);
        if (visible) glfwSwapBuffers(win_);
        updateTray();
    }

    void updateTray() {
        int64_t now = nowMs();
        if (now - lastTrayAt_ < 2000) return;
        lastTrayAt_ = now;
        Session& s = *sessions_[0];
        std::string tip = "rng-lens";
        if (s.streaming()) {
            tip += " - " + (s.device().empty() ? std::string("phone") : s.device());
            int bat = jv(s.pstate, "battery", -1);
            if (bat >= 0) tip += ", " + std::to_string(bat) + "%";
        } else {
            tip += " - waiting for the phone";
        }
        shell_.setTooltip(tip);
    }

    void command(int cmd) {
        Session& s = *sessions_[tab_];
        switch (cmd) {
            case Shell::Show:
                glfwShowWindow(win_);
                glfwRestoreWindow(win_);
                glfwFocusWindow(win_);
                break;
            case Shell::Quit: quit_ = true; break;
            case Shell::ToggleMute:
                prefs_.muted = !prefs_.muted;
                audio_.setMuted(prefs_.muted);
                break;
            case Shell::ToggleMirror:
                s.sp.mirror = !s.sp.mirror;
                break;
            case Shell::ToggleTorch:
                s.ui.torch = !s.ui.torch;
                s.conn.sendControl({{"torch", s.ui.torch}});
                break;
            case Shell::ToggleBackground:
                s.sp.filters.background = s.sp.filters.background == "off" ? "blur" : "off";
                break;
            case Shell::ToggleFrame:
                s.sp.filters.autoFrame = !s.sp.filters.autoFrame;
                break;
        }
        s.dirty = true;
        prefs_.save();
        glfwPostEmptyEvent();
    }

    void autoConnect(Session& s, int i) {
        Connection::State st = s.conn.state();
        if (s.userIdle || !s.sp.autoConnect || s.sp.lastHost.empty()) return;
        if (st == Connection::State::Connecting || st == Connection::State::Streaming) return;
        int64_t now = nowMs();
        if (now - s.lastAutoTry < 3000) return;
        const std::string& other = sessions_[1 - i]->sp.lastHost;
        if (active() == 2 && s.sp.lastHost == other && sessions_[1 - i]->streaming()) return; // one phone, one slot
        if (s.sp.lastHost == "127.0.0.1") {
            s.lastAutoTry = now + 2000; // USB: adb forward is cheap but not free
            std::string msg;
            if (i == 0 && adbForward(msg)) s.connectTo("127.0.0.1", proto::kPort);
            return;
        }
        for (const Discovery::Phone& p : disc_.phones()) {
            if (p.ip == s.sp.lastHost && (p.port == s.sp.lastPort || s.sp.lastPort == 0)) {
                s.lastAutoTry = now;
                s.connectTo(p.ip, p.port);
                return;
            }
        }
    }

    bool adbForward(std::string& msg) {
        std::vector<std::string> adbs;
#ifdef _WIN32
        adbs.push_back("\"" + plat::exeDir() + "adb\\adb.exe\"");
        adbs.push_back("\"" + plat::exeDir() + "adb.exe\"");
#endif
        adbs.push_back("adb");
        for (const std::string& adb : adbs) {
            std::string out;
            int rc = plat::run(adb + " forward tcp:" + std::to_string(proto::kPort) + " tcp:" + std::to_string(proto::kPort), out);
            if (rc == 0) return true;
            if (rc > 0 && out.find("no devices") != std::string::npos) {
                msg = "No phone on USB. Plug it in and allow USB debugging on the phone.";
                return false;
            }
            if (rc > 0 && out.find("unauthorized") != std::string::npos) {
                msg = "Tap \"Allow\" on the phone's USB debugging prompt, then try again.";
                return false;
            }
            if (rc > 0 && !out.empty()) {
                msg = out;
                return false;
            }
        }
        msg = "adb not found. Reinstall rng-lens (the installer brings adb) or put adb in PATH.";
        return false;
    }

    void openAudio() {
        audioWhy_.clear();
        if (!audio_.open(prefs_.audioDevice, audioRate_)) {
            audioWhy_ = "Could not open the sound output";
            if (!prefs_.audioDevice.empty() && audio_.open("", audioRate_)) audioWhy_ = "Saved output is gone, using the default";
        }
        openedRate_ = audioRate_;
    }

    // ---------------------------------------------------------------- UI

    void drawUi() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (tab_ >= active()) tab_ = 0;
        Session& s = *sessions_[tab_];
        float panelW = 380.f * ImGui::GetStyle().FramePadding.x / 10.f;
        float previewW = ImGui::GetContentRegionAvail().x - panelW - ImGui::GetStyle().ItemSpacing.x;
        ImGui::BeginChild("preview", ImVec2(previewW, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawPreview(s);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("panel", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        drawPanel();
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.055f, 0.055f, 0.07f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void drawPreview(Session& s) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float ow = (float)s.renderer.outputWidth(), oh = (float)s.renderer.outputHeight();
        if (ow > 0 && oh > 0) {
            float sc = std::min(avail.x / ow, avail.y / oh);
            ImVec2 size(ow * sc, oh * sc);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - size.x) / 2, origin.y + (avail.y - size.y) / 2));
            ImGui::Image((ImTextureID)(intptr_t)s.renderer.outputTexture(), size);
        }
        if (!s.streaming() || !s.renderer.hasFrame()) {
            const char* lines[] = {
                "Open rng-lens on your phone.",
                "Same Wi-Fi: it shows up on the right, click it.",
                "USB: enable USB debugging, plug in, press USB.",
            };
            float y = origin.y + avail.y / 2 - 50;
            for (const char* l : lines) {
                ImVec2 ts = ImGui::CalcTextSize(l);
                ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - ts.x) / 2, y));
                ImGui::TextColored(kMuted, "%s", l);
                y += ts.y + 10;
            }
        }
    }

    void drawPanel() {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextUnformatted("rng-lens");
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(kMuted, " v" RNGLENS_VERSION);

        if (prefs_.secondPhone && ImGui::BeginTabBar("phones")) {
            for (int i = 0; i < 2; i++) {
                std::string label = (i == 0 ? "Phone 1" : "Phone 2") + std::string(sessions_[i]->streaming() ? "  *" : "") +
                                    "###tab" + std::to_string(i);
                if (ImGui::BeginTabItem(label.c_str())) {
                    tab_ = i;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        Session& s = *sessions_[tab_];

        drawConnection(s);
        if (s.streaming() && !s.hello.empty()) {
            drawCamera(s);
            drawPicture(s);
        }
        drawEffects(s);
        drawOutput(s);
        drawAudio();
        drawSettings();
        drawStats(s);
    }

    // Battery, temperature, heat guard and stream time, right under the phone's name.
    void drawPhoneStatus(Session& s) {
        int bat = jv(s.pstate, "battery", -1);
        bool charging = jv(s.pstate, "charging", false);
        double temp = jv(s.pstate, "tempC", 0.0);
        json th = s.pstate.contains("thermal") ? s.pstate["thermal"] : json::object();
        int level = jv(th, "level", 0);
        if (bat >= 0) {
            ImVec4 bc = bat < 15 && !charging ? kBad : bat < 30 && !charging ? kWarn : kGood;
            ImGui::TextColored(bc, "Battery %d%%%s", bat, charging ? " (charging)" : "");
            ImGui::SameLine();
        }
        ImVec4 tc = level >= 3 || temp >= 45 ? kBad : level >= 1 || temp >= 40 ? kWarn : ImVec4(0.92f, 0.92f, 0.95f, 1.f);
        ImGui::TextColored(tc, "  %.0f C", temp);
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "  %s", clock(nowMs() - s.streamStart).c_str());
        if (jv(th, "throttled", false)) {
            std::string m = "Phone is hot: lowered to " + jv(th, "limit", std::string("less")) +
                            " so it doesn't overheat. Full quality comes back when it cools.";
            wrapped(kWarn, m.c_str());
        } else if (level == 1) {
            wrapped(kMuted, "Phone is warming up. Locking its screen and taking it off the charger helps.");
        }
        if (bat >= 0 && bat < 15 && !charging) wrapped(kBad, "Phone battery is low: plug it in.");
    }

    void drawConnection(Session& s) {
        ImGui::SeparatorText("Phone");
        Connection::State st = s.conn.state();
        if (st == Connection::State::Streaming || st == Connection::State::Connecting) {
            std::string dev = s.device();
            ImGui::TextColored(st == Connection::State::Streaming ? kGood : kMuted, "%s %s",
                               st == Connection::State::Streaming ? "Connected:" : "Connecting to",
                               dev.empty() ? s.conn.peer().c_str() : dev.c_str());
            if (st == Connection::State::Streaming) drawPhoneStatus(s);
            if (ImGui::Button("Disconnect", ImVec2(-1, 0))) {
                s.userIdle = true;
                s.conn.disconnect();
            }
            return;
        }
        if (st == Connection::State::Failed) wrapped(kBad, s.conn.error().c_str());

        std::vector<Discovery::Phone> phones = disc_.phones();
        const Session& other = *sessions_[1 - s.index];
        if (phones.empty()) {
            ImGui::TextColored(kMuted, disc_.listening() ? "Looking for phones on Wi-Fi..." : "Wi-Fi discovery is blocked (port 38441).");
        }
        for (const Discovery::Phone& p : phones) {
            if (active() == 2 && other.streaming() && other.conn.peer() == p.ip) continue; // already on the other tab
            std::string label = p.name + "   " + p.ip;
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) s.connectTo(p.ip, p.port);
        }
        if (s.index == 0) {
            if (ImGui::Button("USB", ImVec2(80, 0))) {
                s.usbMsg.clear();
                if (adbForward(s.usbMsg)) s.connectTo("127.0.0.1", proto::kPort);
            }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(-80);
        ImGui::InputTextWithHint("##ip", "phone IP", s.ipBuf, sizeof(s.ipBuf));
        ImGui::SameLine();
        if (ImGui::Button("Connect", ImVec2(-1, 0)) && s.ipBuf[0]) s.connectTo(s.ipBuf, proto::kPort);
        if (!s.usbMsg.empty()) ImGui::TextWrapped("%s", s.usbMsg.c_str());
        if (ImGui::Checkbox("Reconnect automatically", &s.sp.autoConnect)) prefs_.save();
    }

    static std::string cameraLabel(const json& c) {
        std::string facing = jv(c, "facing", std::string("back"));
        facing[0] = (char)toupper(facing[0]);
        char buf[96];
        double focal = jv(c, "focal", 0.0);
        if (focal > 0) snprintf(buf, sizeof(buf), "%s  %.1f mm  (#%s)", facing.c_str(), focal, jv(c, "id", std::string()).c_str());
        else snprintf(buf, sizeof(buf), "%s  (#%s)", facing.c_str(), jv(c, "id", std::string()).c_str());
        return buf;
    }

    void drawCamera(Session& s) {
        ImGui::SeparatorText("Camera");
        if (!s.hello.contains("cameras")) return;
        const json& cams = s.hello["cameras"];
        std::string cur = !s.cam.empty() ? cameraLabel(s.cam) : s.ui.camera;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##lens", cur.c_str())) {
            for (const json& c : cams) {
                std::string id = jv(c, "id", std::string());
                if (ImGui::Selectable(cameraLabel(c).c_str(), id == s.ui.camera) && id != s.ui.camera) {
                    s.ui.camera = id;
                    s.control({{"camera", id}});
                }
            }
            ImGui::EndCombo();
        }
        if (s.cam.empty()) return;

        int maxFpsForSize = 30;
        char cur2[64];
        snprintf(cur2, sizeof(cur2), "%d x %d", s.ui.width, s.ui.height);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##size", cur2)) {
            for (const json& z : s.cam["sizes"]) {
                int w = z[0], h = z[1], mf = z[2];
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "%d x %d   (up to %d fps)", w, h, mf);
                if (ImGui::Selectable(lbl, w == s.ui.width && h == s.ui.height)) {
                    s.ui.width = w;
                    s.ui.height = h;
                    s.control({{"width", w}, {"height", h}});
                }
            }
            ImGui::EndCombo();
        }
        for (const json& z : s.cam["sizes"])
            if ((int)z[0] == s.ui.width && (int)z[1] == s.ui.height) maxFpsForSize = z[2];

        ImGui::TextColored(kMuted, "FPS");
        for (int f : {15, 24, 30, 60}) {
            if (f > maxFpsForSize && f != s.ui.fps) continue;
            ImGui::SameLine();
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%d", f);
            if (ImGui::RadioButton(lbl, s.ui.fps == f) && s.ui.fps != f) {
                s.ui.fps = f;
                s.control({{"fps", f}});
            }
        }
        if (jv(s.hello, "hevc", false)) {
            ImGui::TextColored(kMuted, "Codec");
            ImGui::SameLine();
            if (ImGui::RadioButton("H.264", s.ui.codec == 0) && s.ui.codec != 0) {
                s.ui.codec = 0;
                s.control({{"codec", 0}});
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("HEVC", s.ui.codec == 1) && s.ui.codec != 1) {
                s.ui.codec = 1;
                s.control({{"codec", 1}});
            }
        }
        bool autoRate = s.ui.bitrate == 0;
        if (ImGui::Checkbox("Auto bitrate", &autoRate)) {
            s.ui.bitrate = autoRate ? 0 : jv(s.pstate, "streamBitrate", 8000000);
            s.control({{"bitrate", s.ui.bitrate}});
        }
        if (!autoRate) {
            float mb = s.ui.bitrate / 1e6f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##br", &mb, 1.f, 60.f, "%.0f Mbit/s")) s.ui.bitrate = (int)(mb * 1e6f);
            if (ImGui::IsItemDeactivatedAfterEdit()) s.control({{"bitrate", s.ui.bitrate}});
        }
    }

    void drawPicture(Session& s) {
        if (s.cam.empty()) return;
        ImGui::SeparatorText("Picture");
        PhoneUi& ui = s.ui;
        json z = jv(s.cam, "zoom", json::array({1.0, 1.0}));
        float zmin = z[0], zmax = z[1];
        if (zmax > zmin + 0.01f) {
            ImGui::TextColored(kMuted, "Zoom");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##zoom", &ui.zoom, zmin, std::min(zmax, 30.f), "%.1fx", ImGuiSliderFlags_Logarithmic))
                s.conn.sendControl({{"zoom", ui.zoom}});
            if (zmin < 0.99f) {
                if (ImGui::SmallButton("Wide")) {
                    ui.zoom = zmin;
                    s.conn.sendControl({{"zoom", ui.zoom}});
                }
                ImGui::SameLine();
            }
            for (float preset : {1.f, 2.f, 5.f}) {
                if (preset > zmax) continue;
                char lbl[8];
                snprintf(lbl, sizeof(lbl), "%gx", preset);
                if (ImGui::SmallButton(lbl)) {
                    ui.zoom = preset;
                    s.conn.sendControl({{"zoom", ui.zoom}});
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        ImGui::TextColored(kMuted, "Focus");
        ImGui::SameLine();
        bool fc = false;
        fc |= radio("Auto##af", ui.af, "auto");
        ImGui::SameLine();
        fc |= radio("Lock##af", ui.af, "lock");
        if (jv(s.cam, "afManual", false)) {
            ImGui::SameLine();
            fc |= radio("Manual##af", ui.af, "manual");
        }
        if (fc) s.conn.sendControl({{"af", ui.af}});
        if (ui.af == "manual") {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##focus", &ui.focus, 0.f, 1.f, ui.focus < 0.02f ? "far" : "%.2f"))
                s.conn.sendControl({{"focus", ui.focus}});
        }
        json evr = jv(s.cam, "ev", json::array({0, 0, 1.0}));
        int evMin = evr[0], evMax = evr[1];
        float evStep = evr[2];
        if (evMax > evMin) {
            ImGui::TextColored(kMuted, "Exposure");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            char fmt[32];
            snprintf(fmt, sizeof(fmt), "%+.1f EV", ui.ev * evStep);
            if (ImGui::SliderInt("##ev", &ui.ev, evMin, evMax, fmt)) s.conn.sendControl({{"ev", ui.ev}});
        }
        bool aeLock = ui.ae == "lock", wbLock = ui.awb == "lock";
        if (ImGui::Checkbox("Lock exposure", &aeLock)) {
            ui.ae = aeLock ? "lock" : "auto";
            s.conn.sendControl({{"ae", ui.ae}});
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Lock white balance", &wbLock)) {
            ui.awb = wbLock ? "lock" : "auto";
            s.conn.sendControl({{"awb", ui.awb}});
        }
        if (jv(s.cam, "flash", false) && ImGui::Checkbox("Torch", &ui.torch)) s.conn.sendControl({{"torch", ui.torch}});
        if (jv(s.cam, "stab", false)) {
            if (jv(s.cam, "flash", false)) ImGui::SameLine();
            if (ImGui::Checkbox("Stabilization", &ui.stab)) s.conn.sendControl({{"stab", ui.stab}});
        }
    }

    bool slider(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
        ImGui::TextColored(kMuted, "%s", label);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.36f);
        ImGui::SetNextItemWidth(-1);
        std::string id = std::string("##") + label;
        bool changed = ImGui::SliderFloat(id.c_str(), v, lo, hi, fmt);
        if (ImGui::IsItemDeactivatedAfterEdit()) prefs_.save();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { // double-click: back to neutral
            *v = lo < 0 ? 0.f : (std::string(label) == "Look strength" ? 1.f : 0.f);
            changed = true;
            prefs_.save();
        }
        return changed;
    }

    bool pickImage(std::string& path) {
#ifdef _WIN32
        wchar_t file[MAX_PATH] = {0};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = glfwGetWin32Window(win_);
        ofn.lpstrFilter = L"Pictures\0*.jpg;*.jpeg;*.png;*.bmp\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return false;
        char utf8[MAX_PATH * 3];
        WideCharToMultiByte(CP_ACP, 0, file, -1, utf8, sizeof(utf8), nullptr, nullptr); // stb opens ANSI paths
        path = utf8;
        return true;
#else
        (void)path;
        return false;
#endif
    }

    void drawEffects(Session& s) {
        ImGui::SeparatorText("Effects");
        FilterPrefs& f = s.sp.filters;
        bool ch = false;

        // Look
        const char* cur = "None";
        for (const Look& l : kLooks)
            if (f.look == l.id) cur = l.label;
        ImGui::TextColored(kMuted, "Look");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.36f);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##look", cur)) {
            for (const Look& l : kLooks) {
                if (ImGui::Selectable(l.label, f.look == l.id)) {
                    f.look = l.id;
                    ch = true;
                    prefs_.save();
                }
            }
            ImGui::EndCombo();
        }
        if (f.look != "none") ch |= slider("Look strength", &f.lookAmount, 0.f, 1.f);

        if (ImGui::TreeNode("Adjust colours")) {
            ch |= slider("Brightness", &f.brightness, -1.f, 1.f);
            ch |= slider("Contrast", &f.contrast, -1.f, 1.f);
            ch |= slider("Saturation", &f.saturation, -1.f, 1.f);
            ch |= slider("Warmth", &f.warmth, -1.f, 1.f);
            ImGui::TextColored(kMuted, "Double-click a slider to reset it.");
            ImGui::TreePop();
        }
        ch |= slider("Sharpen", &f.sharpen, 0.f, 1.f);
        ch |= slider("Smooth skin", &f.smooth, 0.f, 1.f);
        ch |= slider("Night mode", &f.night, 0.f, 1.f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cleans up grain in low light by averaging frames where nothing moves.");

        // Background and auto-frame need the vision worker.
        ImGui::TextColored(kMuted, "Background");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.36f);
        bool bc = false;
        bc |= radio("Off##bg", f.background, "off");
        ImGui::SameLine();
        bc |= radio("Blur##bg", f.background, "blur");
        ImGui::SameLine();
        bc |= radio("Picture##bg", f.background, "image");
        if (bc) {
            ch = true;
            if (f.background == "image" && f.backgroundImage.empty()) pickImage(f.backgroundImage);
            if (f.background == "image") s.renderer.setBackgroundImage(f.backgroundImage);
            prefs_.save();
        }
        if (f.background == "blur") ch |= slider("Blur", &f.blur, 0.f, 1.f);
        if (f.background == "image") {
            std::string name = f.backgroundImage.empty() ? std::string("(choose a picture)") : f.backgroundImage;
            size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (ImGui::Button(("Picture: " + name).c_str(), ImVec2(-1, 0)) && pickImage(f.backgroundImage)) {
                if (!s.renderer.setBackgroundImage(f.backgroundImage)) s.visionWhy = "Couldn't open that picture";
                ch = true;
                prefs_.save();
            }
        }
        if (ImGui::Checkbox("Auto-frame (follow my face)", &f.autoFrame)) {
            ch = true;
            prefs_.save();
        }
        if (f.autoFrame) {
            ch |= slider("Framing", &f.frameTightness, 0.f, 1.f, f.frameTightness < 0.35f ? "wide" : f.frameTightness > 0.7f ? "close" : "medium");
            ch |= slider("Follow speed", &f.frameSpeed, 0.f, 1.f, f.frameSpeed < 0.35f ? "slow" : f.frameSpeed > 0.7f ? "fast" : "normal");
        }
        if ((f.background != "off" || f.autoFrame) && s.vision.failed()) wrapped(kBad, s.vision.error().c_str());
        else if ((f.background != "off" || f.autoFrame) && !s.vision.ready()) wrapped(kMuted, "Loading the background / face models...");
        if (!s.visionWhy.empty()) wrapped(kBad, s.visionWhy.c_str());
        if (f.anyActive() && ImGui::SmallButton("Reset all effects")) {
            f = FilterPrefs();
            ch = true;
            prefs_.save();
        }
        if (ch) s.dirty = true;
    }

    void drawOutput(Session& s) {
        ImGui::SeparatorText("Output");
        SlotPrefs& sp = s.sp;
        bool save = false;
        save |= ImGui::Checkbox("Auto-rotate", &sp.autoRotate);
        ImGui::SameLine();
        if (ImGui::Button("Rotate 90")) {
            sp.extraTurns = (sp.extraTurns + 1) & 3;
            save = true;
        }
        save |= ImGui::Checkbox("Mirror", &sp.mirror);
        ImGui::SameLine();
        save |= ImGui::Checkbox("Flip", &sp.flip);

        const char* cur = "Custom";
        for (const OutputSize& o : kOutputs)
            if (o.w == sp.outWidth && o.h == sp.outHeight) cur = o.label;
        ImGui::TextColored(kMuted, "Camera size");
        ImGui::SetNextItemWidth(-1);
        bool restart = false;
        if (ImGui::BeginCombo("##out", cur)) {
            for (const OutputSize& o : kOutputs) {
                if (ImGui::Selectable(o.label, o.w == sp.outWidth && o.h == sp.outHeight)) {
                    sp.outWidth = o.w;
                    sp.outHeight = o.h;
                    restart = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextColored(kMuted, "Camera fps");
        for (int f : {30, 60}) {
            ImGui::SameLine();
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%d##o", f);
            if (ImGui::RadioButton(lbl, sp.outFps == f) && sp.outFps != f) {
                sp.outFps = f;
                restart = true;
            }
        }
        std::string vlabel = "Virtual camera \"" + s.name() + "\"";
        if (ImGui::Checkbox(vlabel.c_str(), &sp.virtualCamera)) restart = true;
        if (restart) {
            s.restartVcam();
            save = true;
        }
        if (save) {
            s.dirty = true;
            prefs_.save();
        }
        if (sp.virtualCamera) {
            if (!s.vcamWhy.empty()) wrapped(kBad, s.vcamWhy.c_str());
            else if (s.vcam && s.vcam->running()) {
                if (s.vcam->watched()) ImGui::TextColored(kGood, "An app is using the camera (%llu frames sent)", (unsigned long long)s.vcamSent);
                else {
                    std::string m = "Ready: pick \"" + s.name() + "\" in Zoom, Discord, OBS...";
                    wrapped(kMuted, m.c_str());
                }
            }
        }
    }

    void drawAudio() {
        if (audioRate_ != openedRate_) openAudio(); // stream switched sample rate
        ImGui::SeparatorText("Sound");
        bool save = false;
        auto sendMic = [&]() {
            for (int i = 0; i < active(); i++)
                sessions_[i]->conn.sendControl({{"audio", prefs_.audio && prefs_.audioSlot == i}, {"mic", prefs_.micMode}});
            audio_.clear();
        };
        if (ImGui::Checkbox("Phone microphone", &prefs_.audio)) {
            sendMic();
            save = true;
        }
        if (prefs_.audio) {
            ImGui::SameLine();
            if (radio("Voice##mic", prefs_.micMode, "voice")) {
                sendMic();
                save = true;
            }
            ImGui::SameLine();
            if (radio("Raw##mic", prefs_.micMode, "raw")) {
                sendMic();
                save = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Voice: noise suppression + echo cancel (calls)\nRaw: untouched sound (music, streams)");
            if (prefs_.secondPhone) {
                ImGui::TextColored(kMuted, "From");
                ImGui::SameLine();
                if (ImGui::RadioButton("Phone 1##as", prefs_.audioSlot == 0) && prefs_.audioSlot != 0) {
                    prefs_.audioSlot = 0;
                    sendMic();
                    save = true;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Phone 2##as", prefs_.audioSlot == 1) && prefs_.audioSlot != 1) {
                    prefs_.audioSlot = 1;
                    sendMic();
                    save = true;
                }
            }
            const Session& as = *sessions_[prefs_.audioSlot];
            if (!as.hello.empty() && !jv(as.hello, "mic", true))
                wrapped(kBad, "Microphone access is off on the phone (Settings > Apps > rng-lens)");

            std::string cur = audio_.isOpen() ? audio_.deviceName() : std::string("(none)");
            ImGui::TextColored(kMuted, "Play to");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##aout", cur.c_str())) {
                if (devices_.empty() || ImGui::IsWindowAppearing()) devices_ = audio_.devices();
                if (ImGui::Selectable("System default", prefs_.audioDevice.empty())) {
                    prefs_.audioDevice.clear();
                    openAudio();
                    save = true;
                }
                for (const AudioOutput::Device& d : devices_) {
                    if (ImGui::Selectable(d.name.c_str(), d.name == prefs_.audioDevice)) {
                        prefs_.audioDevice = d.name;
                        openAudio();
                        save = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (!audioWhy_.empty()) wrapped(kBad, audioWhy_.c_str());
            wrapped(kMuted, "Tip: pick \"CABLE Input\" (VB-Cable) here to use the phone as a mic in Zoom/Discord.");
            float vol = prefs_.volume * 100.f;
            ImGui::SetNextItemWidth(-90);
            if (ImGui::SliderFloat("##vol", &vol, 0.f, 200.f, "%.0f%%")) {
                prefs_.volume = vol / 100.f;
                audio_.setVolume(prefs_.volume);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Mute", &prefs_.muted)) {
                audio_.setMuted(prefs_.muted);
                save = true;
            }
            meter_ = std::max(audio_.peak(), meter_ * 0.85f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, meter_ > 0.95f ? kBad : kAccent);
            ImGui::ProgressBar(std::min(1.f, meter_), ImVec2(-1, 6), "");
            ImGui::PopStyleColor();
        }
        if (save) prefs_.save();
    }

    void drawSettings() {
        ImGui::SeparatorText("Settings");
        bool save = false;
        if (ImGui::Checkbox("GPU decoding", &prefs_.hardwareDecode)) save = true;
        if (shell_.supported()) {
            if (ImGui::Checkbox("Start with Windows (in the tray)", &autostart_)) Shell::setAutostart(autostart_);
            if (ImGui::Checkbox("Closing the window keeps the camera on (tray)", &prefs_.closeToTray)) save = true;
            if (ImGui::Checkbox("Hotkeys", &prefs_.hotkeys)) {
                shell_.setHotkeysEnabled(prefs_.hotkeys);
                save = true;
            }
            if (prefs_.hotkeys && ImGui::TreeNode("Which keys")) {
                for (const Shell::Hotkey& k : shell_.hotkeys())
                    ImGui::TextColored(k.registered ? ImVec4(0.92f, 0.92f, 0.95f, 1.f) : kMuted, "%s  %s%s", k.keys, k.what,
                                       k.registered ? "" : "  (taken by another app)");
                ImGui::TreePop();
            }
        }
        if (ImGui::Checkbox("Second phone (camera \"rng-lens 2\")", &prefs_.secondPhone)) {
            Session& s2 = *sessions_[1];
            if (prefs_.secondPhone) {
                if (!s2.vcam) s2.init();
                else s2.restartVcam();
            } else {
                s2.conn.disconnect();
                if (s2.vcam) s2.vcam->stop();
                if (prefs_.audioSlot == 1) prefs_.audioSlot = 0;
                tab_ = 0;
            }
            save = true;
        }
        if (save) prefs_.save();
    }

    void drawStats(Session& s) {
        ImGui::SeparatorText("Stats");
        if (!s.streaming()) {
            ImGui::TextColored(kMuted, "Not streaming");
            return;
        }
        ImGui::Text("%d x %d   %.0f fps   %.1f Mbit/s", s.conn.streamWidth(), s.conn.streamHeight(), s.fpsShown, s.mbps);
        ImGui::Text("Decode: %s, %.1f ms", s.decoder.description().c_str(), s.decoder.lastDecodeMs());
        std::string ac = s.conn.audioCodec();
        if (!ac.empty() && prefs_.audioSlot == s.index)
            ImGui::Text("Sound: %s, %.0f ms buffered, %llu dropouts", ac.c_str(), audio_.bufferedMs(), (unsigned long long)audio_.underruns());
        if (s.vision.ready() && (s.sp.filters.background != "off" || s.sp.filters.autoFrame))
            ImGui::Text("Vision: mask %.0f ms, face %.0f ms", s.vision.segMs(), s.vision.faceMs());
        int dropped = jv(s.pstate, "dropped", 0);
        if (dropped > 0) ImGui::TextColored(kMuted, "Dropped on phone: %d (network too slow)", dropped);
        if (s.temps.size() >= 2) {
            std::vector<float> t(s.temps.begin(), s.temps.end());
            float lo = *std::min_element(t.begin(), t.end()), hi = *std::max_element(t.begin(), t.end());
            char ov[48];
            snprintf(ov, sizeof(ov), "phone %.0f C (%.0f..%.0f)", t.back(), lo, hi);
            ImGui::PlotLines("##temp", t.data(), (int)t.size(), 0, ov, std::min(lo, 30.f), std::max(hi, 45.f), ImVec2(-1, 48));
        }
    }

    Prefs& prefs_;
    AppOptions opt_;
    GLFWwindow* win_ = nullptr;
    std::unique_ptr<Session> sessions_[2];
    int tab_ = 0;
    Discovery disc_;
    Shell shell_;
    bool autostart_ = false, quit_ = false;
    int64_t lastTrayAt_ = 0;
    AudioOutput audio_;
    std::vector<AudioOutput::Device> devices_;
    std::string audioWhy_;
    std::atomic<int> audioRate_{48000};
    int openedRate_ = 48000;
    float meter_ = 0.f;
};

}  // namespace

int runApp(Prefs& prefs, const AppOptions& options) {
    App app(prefs, options);
    return app.run();
}
