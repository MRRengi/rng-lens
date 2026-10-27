// The desktop window: preview on the left, controls on the right.
// One Session per phone (the main one, plus the optional second). The loop sleeps until something
// happens (input, a decoded frame, a phone state change), so an idle rng-lens costs nothing.

#include "app.h"
#include "autoframe.h"
#include "ui.h"
#include "updater.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#include <dwmapi.h>
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

// Draws a capsule badge ("Be right back", "Reconnecting...") into the renderer's badge target, using
// the ImGui font atlas and the ImGui GL backend on an off-screen draw list.
void drawBadge(Renderer& r, const char* icon, const char* text, int outH) {
    if (ImGui::GetFrameCount() < 1) return; // fonts not ready yet (started straight into the tray)
    ImFont* f = ui::fonts().badge;
    float size = std::max(16.f, outH * 0.04f);
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, text);
    float iconW = icon ? size * 1.35f : 0.f, padX = size * 0.95f, padY = size * 0.55f;
    int w = (int)(ts.x + iconW + 2 * padX), h = (int)(ts.y + 2 * padY);
    if (!r.beginBadge(w, h)) return;
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushClipRect(ImVec2(0, 0), ImVec2((float)w, (float)h));
    dl.PushTextureID(ImGui::GetIO().Fonts->TexID);
    dl.AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(16, 16, 18, 200), h / 2.f);
    if (icon) dl.AddText(f, size, ImVec2(padX, padY), IM_COL32(255, 106, 61, 255), icon);
    dl.AddText(f, size, ImVec2(padX + iconW, padY), IM_COL32_WHITE, text);
    ImDrawData dd;
    dd.Valid = true;
    dd.CmdLists.push_back(&dl);
    dd.CmdListsCount = 1;
    dd.TotalVtxCount = dl.VtxBuffer.Size;
    dd.TotalIdxCount = dl.IdxBuffer.Size;
    dd.DisplayPos = ImVec2(0, 0);
    dd.DisplaySize = ImVec2((float)w, (float)h);
    dd.FramebufferScale = ImVec2(1, 1);
    ImGui_ImplOpenGL3_RenderDrawData(&dd);
    r.endBadge();
}

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

    // Face-priority exposure: tell the phone where the face is (in the streamed frame's own coordinates,
    // before our rotation), so its auto exposure meters on the face and not on a bright window behind it.
    bool aeSent = false;
    float aeLast[4] = {0, 0, 0, 0};
    int64_t aeAt = 0;
    void updateFaceExposure(int turns) {
        int64_t now = nowMs();
        bool on = sp.faceExposure && streaming() && vision.ready() && jv(cam, "aeRegions", false);
        FaceBox fb = on ? vision.face() : FaceBox();
        bool face = on && fb.score > 0 && now - fb.atMs < 2000;
        if (!face) {
            if (aeSent && (!on || now - aeAt > 2000)) {
                conn.sendControl({{"aeRegion", nullptr}});
                aeSent = false;
                aeAt = now;
            }
            return;
        }
        // face box, a little larger (forehead and cheeks), in the upright picture
        float cx = fb.x + fb.w / 2, cy = fb.y + fb.h / 2, hw = fb.w * 0.6f, hh = fb.h * 0.6f;
        float ux[2] = {cx - hw, cx + hw}, uy[2] = {cy - hh, cy + hh};
        float r[4] = {1, 1, 0, 0};
        for (float px : ux)
            for (float py : uy) {
                float sx = px, sy = py; // same mapping as the renderer's convert pass
                if (turns == 1) sx = py, sy = 1 - px;
                else if (turns == 2) sx = 1 - px, sy = 1 - py;
                else if (turns == 3) sx = 1 - py, sy = px;
                r[0] = std::min(r[0], sx), r[1] = std::min(r[1], sy), r[2] = std::max(r[2], sx), r[3] = std::max(r[3], sy);
            }
        for (float& v : r) v = std::clamp(v, 0.f, 1.f);
        float moved = 0;
        for (int i = 0; i < 4; i++) moved = std::max(moved, std::fabs(r[i] - aeLast[i]));
        if (aeSent && (moved < 0.04f || now - aeAt < 700)) return;
        conn.sendControl({{"aeRegion", {r[0], r[1], r[2], r[3]}}});
        std::copy(r, r + 4, aeLast);
        aeSent = true;
        aeAt = now;
    }
    bool glassWanted = false; // the window is visible: keep the frosted copy for the UI fresh
    bool paused = false, hadStream = false;
    int holdKind = 0, badgeKind = 0; // 0 live, 1 paused, 2 phone dropped out
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
        bool wantMask = f.background != "off", wantFace = f.autoFrame || sp.faceExposure;
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
        updateFaceExposure(t.quarterTurns);
        bool cropMoved = updateCrop(dt, renderer.workWidth(t.quarterTurns), renderer.workHeight(t.quarterTurns));

        // Hold: paused, or the phone dropped out mid-call. Apps keep getting a still picture with a badge
        // instead of a frozen or vanished camera.
        if (streaming() && framesShown > 0) hadStream = true;
        if (userIdle) hadStream = false;
        bool reconnecting = hadStream && !streaming() && renderer.hasFrame();
        int kind = paused && renderer.hasFrame() ? 1 : reconnecting ? 2 : 0;
        if (kind != holdKind) {
            if (kind && !holdKind) renderer.captureHold();
            if (!kind) dirty = true; // live again: re-render right away
            holdKind = kind;
            badgeKind = 0;
        }
        if (holdKind) {
            int want = holdKind == 1 ? 1 : sp.autoConnect ? 2 : 3;
            if (badgeKind != want && ImGui::GetFrameCount() > 0) {
                const char* texts[] = {"", "Be right back", "Reconnecting...", "Phone disconnected"};
                const char* icons[] = {nullptr, ui::icon::pause, ui::icon::wifiOff, ui::icon::wifiOff};
                drawBadge(renderer, icons[want], texts[want], renderer.outputHeight());
                badgeKind = want;
            }
            bool pendingHold = false;
            if (now - lastRenderMs >= 33) {
                renderer.renderHold(holdKind == 1 ? 1.f : 0.f, holdKind == 1 ? 0.25f : 0.35f);
                if (glassWanted) renderer.updateGlass();
                lastRenderMs = now;
                if (dumpNow || (vcam && vcam->running() && vcam->watched())) {
                    renderer.beginReadback();
                    pendingHold = true;
                }
            }
            rates(now);
            readbackPending = pendingHold;
            return;
        }

        bool changed = t.quarterTurns != lastT.quarterTurns || t.mirror != lastT.mirror || t.flip != lastT.flip ||
                       sp.outWidth != lastOutW || sp.outHeight != lastOutH || dirty;
        bool idleRefresh = !renderer.hasFrame() && now - lastRenderMs > 500; // keep apps fed while waiting
        bool pending = false;
        if (newFrame || changed || cropMoved || idleRefresh) {
            renderer.render(sp.outWidth, sp.outHeight, t, f, crop, newFrame);
            if (glassWanted) renderer.updateGlass();
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
    ~App() {
        if (phoneUpd_.joinable()) phoneUpd_.join();
    }
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
            bool holding = false;
            for (int i = 0; i < active(); i++) holding |= sessions_[i]->holdKind != 0;
            glfwWaitEventsTimeout(holding ? 0.03 : 0.25);
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
        scale_ = xs;
        ui::loadFonts(xs);
        dark_ = ui::resolveDark(prefs_.theme);
        ui::applyTheme(dark_, xs);
        titleBarTheme();
        glfwSetWindowSizeLimits(win_, (int)(980 * xs), (int)(620 * xs), GLFW_DONT_CARE, GLFW_DONT_CARE);
        ImGui_ImplGlfw_InitForOpenGL(win_, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
        return true;
    }
    static bool opt_dumpless(App* app) { return app->opt_.dumpOutput.empty(); }

    // Windows 10/11 title bar in the same light/dark as the app.
    void titleBarTheme() {
#ifdef _WIN32
        BOOL dark = dark_ ? TRUE : FALSE;
        DwmSetWindowAttribute(glfwGetWin32Window(win_), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
#endif
    }

    // Follows the Windows theme while the app runs.
    void syncTheme() {
        int64_t now = nowMs();
        if (now - lastThemeAt_ < 1500) return;
        lastThemeAt_ = now;
        bool d = ui::resolveDark(prefs_.theme);
        if (d != dark_) {
            dark_ = d;
            ui::applyTheme(dark_, scale_);
            titleBarTheme();
        }
    }

    // ---------------------------------------------------------------- loop

    int active() const { return prefs_.secondPhone ? 2 : 1; }

    void tick() {
        for (int i = 0; i < active(); i++) autoConnect(*sessions_[i], i);
        bool dump = !opt_.dumpOutput.empty() && sessions_[0]->framesShown >= (uint64_t)opt_.dumpAfter;
        for (int i = 0; i < active(); i++) sessions_[i]->tick(dump && i == 0, opt_.dumpOutput, quit_);

        bool visible = glfwGetWindowAttrib(win_, GLFW_VISIBLE) && !glfwGetWindowAttrib(win_, GLFW_ICONIFIED);
        for (int i = 0; i < active(); i++) sessions_[i]->glassWanted = visible && i == tab_;
        if (visible) syncTheme();
        int64_t nowU = nowMs();
        if (prefs_.autoUpdate && (lastUpdateCheck_ == 0 || nowU - lastUpdateCheck_ > 6 * 3600 * 1000LL)) {
            lastUpdateCheck_ = nowU;
            updater_.check(RNGLENS_VERSION);
        }
        if (updater_.state() == Updater::State::Ready && updater_.launchInstaller()) quit_ = true;
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
            case Shell::TogglePause: {
                bool p = !s.paused;
                for (int i = 0; i < active(); i++) sessions_[i]->paused = p;
                break;
            }
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

    // The phone app is older than this PC app (or too old to say its version): some controls won't work.
    static bool phoneOutdated(const Session& s) {
        if (!s.streaming() || s.hello.empty()) return false;
        std::string v = jv(s.hello, "version", std::string());
        return v.empty() || Updater::newer(RNGLENS_VERSION, v);
    }

    void startPhoneUpdate() {
        if (phoneUpdState_ == 1) return;
        if (phoneUpd_.joinable()) phoneUpd_.join();
        phoneUpdState_ = 1;
        phoneUpd_ = std::thread([this] {
            auto done = [this](int st, const std::string& m) {
                {
                    std::lock_guard<std::mutex> lock(phoneUpdM_);
                    phoneUpdMsg_ = m;
                }
                phoneUpdState_ = st;
                glfwPostEmptyEvent();
            };
#ifdef _WIN32
            char tmp[MAX_PATH];
            GetTempPathA(MAX_PATH, tmp);
            std::string apk = std::string(tmp) + "rng-lens-phone.apk", err;
            std::string url = std::string("https://github.com/MRRengi/rng-lens/releases/download/v") + RNGLENS_VERSION + "/rng-lens.apk";
            if (!Updater::fetch(url, apk, err)) return done(3, "Couldn't download the phone app: " + err);
            std::vector<std::string> adbs = {"\"" + plat::exeDir() + "adb\\adb.exe\"", "adb"};
            for (const std::string& adb : adbs) {
                std::string out;
                int rc = plat::run(adb + " install -r \"" + apk + "\"", out);
                if (rc < 0) continue; // this adb doesn't exist
                if (out.find("Success") != std::string::npos) return done(2, "Phone app updated. Open rng-lens on the phone again.");
                if (out.find("no devices") != std::string::npos)
                    return done(3, "No phone on USB. Plug it in with USB debugging on, or install rng-lens.apk from the release page on the phone.");
                if (out.find("unauthorized") != std::string::npos) return done(3, "Tap \"Allow\" on the phone's USB debugging prompt, then try again.");
                if (out.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos)
                    return done(3, "The phone has a test build signed differently: uninstall rng-lens on the phone, then try again.");
                return done(3, out.empty() ? std::string("adb install failed") : out);
            }
            done(3, "adb not found. Reinstall rng-lens on the PC, or install rng-lens.apk on the phone from the release page.");
#else
            done(3, "Install rng-lens.apk from the release page on the phone.");
#endif
        });
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

    enum Section { kPhone, kCamera, kEffects, kOutput, kSound, kSettings, kInfo, kSections };

    void drawUi() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const ui::Palette& P = ui::P();
        const float S = ui::S();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);
        if (tab_ >= active()) tab_ = 0;
        Session& s = *sessions_[tab_];

        float sideW = 224.f * S, inspW = 372.f * S;
        float h = ImGui::GetContentRegionAvail().y;

        // Sidebar
        ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(o, ImVec2(o.x + sideW, o.y + h), ui::col(P.sidebar));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(o.x + sideW - 0.5f, o.y), ImVec2(o.x + sideW - 0.5f, o.y + h), ui::col(P.separator));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 14 * S));
        ImGui::BeginChild("sidebar", ImVec2(sideW, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        drawSidebar(s);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::SameLine(0, 0);

        // Preview
        float prevW = ImGui::GetContentRegionAvail().x - inspW;
        ImGui::BeginChild("preview", ImVec2(prevW, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawPreview(s);
        ImGui::EndChild();
        ImGui::SameLine(0, 0);

        // Inspector
        o = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(o.x, o.y), ImVec2(o.x, o.y + h), ui::col(P.separator));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * S, 18 * S));
        ImGui::BeginChild("inspector", ImVec2(inspW, h), ImGuiChildFlags_AlwaysUseWindowPadding);
        drawInspector(s);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();

        ImGui::Render();
        int w, hh;
        glfwGetFramebufferSize(win_, &w, &hh);
        glViewport(0, 0, w, hh);
        glClearColor(P.window.x, P.window.y, P.window.z, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    // ---------------------------------------------------------------- sidebar

    void drawSidebar(Session& s) {
        const ui::Palette& P = ui::P();
        const float S = ui::S();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6 * S);
        ui::title("rng-lens");
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * S);
        ImGui::PushFont(ui::fonts().small);
        ui::textSecondary(RNGLENS_VERSION);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 10 * S));

        if (prefs_.secondPhone) {
            const char* labels[] = {"Phone 1", "Phone 2"};
            ui::segmented("phones", labels, 2, &tab_, ImGui::GetContentRegionAvail().x);
            ImGui::Dummy(ImVec2(0, 10 * S));
        }

        // Phone card
        ImVec2 a = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(w, 10 * S));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12 * S);
        ImGui::BeginGroup();
        Connection::State st = s.conn.state();
        bool live = st == Connection::State::Streaming;
        std::string dev = s.device();
        ImGui::PushFont(ui::fonts().strong);
        ImGui::TextUnformatted(live ? (dev.empty() ? "Phone" : dev.c_str()) : "No phone");
        ImGui::PopFont();
        ImGui::PushFont(ui::fonts().small);
        ImVec2 dotAt = ImGui::GetCursorScreenPos();
        float dr = 3.5f * S;
        ImVec4 dotCol = live ? P.good : st == Connection::State::Connecting ? P.warn : P.tertiary;
        dl->AddCircleFilled(ImVec2(dotAt.x + dr, dotAt.y + ImGui::GetTextLineHeight() / 2), dr, ui::col(dotCol));
        ImGui::SetCursorScreenPos(ImVec2(dotAt.x + dr * 2 + 6 * S, dotAt.y));
        ui::textSecondary(live ? "Connected" : st == Connection::State::Connecting ? "Connecting..." : "Not connected");
        if (live) {
            int bat = jv(s.pstate, "battery", -1);
            bool charging = jv(s.pstate, "charging", false);
            double temp = jv(s.pstate, "tempC", 0.0);
            json th = s.pstate.contains("thermal") ? s.pstate["thermal"] : json::object();
            int level = jv(th, "level", 0);
            char b[96];
            if (bat >= 0) {
                snprintf(b, sizeof(b), "%s %d%%", charging ? ui::icon::batteryCharging : bat < 20 ? ui::icon::batteryLow : ui::icon::battery, bat);
                ui::text(b, bat < 15 && !charging ? P.bad : P.secondary);
                ImGui::SameLine(0, 10 * S);
            }
            snprintf(b, sizeof(b), "%s %.0f\xc2\xb0" "C", ui::icon::thermometer, temp);
            ui::text(b, level >= 3 || temp >= 45 ? P.bad : level >= 1 || temp >= 40 ? P.warn : P.secondary);
            ImGui::SameLine(0, 10 * S);
            ui::textSecondary(clock(nowMs() - s.streamStart).c_str());
            if (phoneOutdated(s)) {
                std::string m = std::string(ui::icon::alert) + " Phone app needs an update";
                ui::text(m.c_str(), P.warn);
            }
        }
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(w, 10 * S));
        ImGui::EndGroup();
        ImVec2 b2 = ImGui::GetItemRectMax();
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(a, ImVec2(a.x + w, b2.y), ui::col(P.group, P.dark ? 1.f : 0.85f), 10 * S);
        dl->ChannelsMerge();
        ImGui::Dummy(ImVec2(0, 14 * S));

        struct Item {
            Section id;
            const char* icon;
            const char* label;
        };
        const Item items[] = {
            {kPhone, ui::icon::phone, "Phone"},       {kCamera, ui::icon::camera, "Camera"},
            {kEffects, ui::icon::sparkles, "Effects"}, {kOutput, ui::icon::monitor, "Output"},
            {kSound, ui::icon::mic, "Sound"},         {kSettings, ui::icon::settings, "Settings"},
            {kInfo, ui::icon::activity, "Info"},
        };
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * S));
        for (const Item& it : items) {
            const char* badge = nullptr;
            if (it.id == kEffects && s.sp.filters.anyActive()) badge = "On";
            if (it.id == kSound && prefs_.audio) badge = prefs_.muted ? "Muted" : "On";
            if (ui::sidebarItem(it.icon, it.label, section_ == it.id, badge)) section_ = it.id;
        }
        ImGui::PopStyleVar();
        drawUpdateCard();
    }

    // Bottom of the sidebar: a newer release is out.
    void drawUpdateCard() {
        Updater::State st = updater_.state();
        if (st != Updater::State::Available && st != Updater::State::Downloading && st != Updater::State::Ready) return;
        const ui::Palette& P = ui::P();
        const float S = ui::S();
        float w = ImGui::GetContentRegionAvail().x, h = 92 * S;
        float y = ImGui::GetWindowHeight() - h - 14 * S;
        if (ImGui::GetCursorPosY() > y) return; // window too short
        ImGui::SetCursorPosY(y);
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), ui::col(P.group, P.dark ? 1.f : 0.85f), 10 * S);
        ImGui::SetCursorScreenPos(ImVec2(a.x + 12 * S, a.y + 10 * S));
        ImGui::BeginGroup();
        ImGui::PushFont(ui::fonts().strong);
        std::string head = std::string(ui::icon::download) + "  rng-lens " + updater_.latest();
        ui::text(head.c_str(), P.label);
        ImGui::PopFont();
        ImGui::PushFont(ui::fonts().small);
        ui::textSecondary(st == Updater::State::Available ? "A new version is out." : "Downloading...");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4 * S));
        ImGui::PushItemWidth(w - 24 * S);
        if (st == Updater::State::Available) {
            if (ui::button("Update now", true, w - 24 * S)) updater_.download();
        } else {
            ui::meter(updater_.progress(), P.accent);
        }
        ImGui::PopItemWidth();
        ImGui::EndGroup();
    }

    // ---------------------------------------------------------------- preview

    void drawPreview(Session& s) {
        const ui::Palette& P = ui::P();
        const float S = ui::S();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float m = 20 * S;
        ImVec2 a(origin.x + m, origin.y + m), b(origin.x + avail.x - m, origin.y + avail.y - m);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float r = 14 * S;
        dl->AddRectFilled(a, b, ui::col(P.preview), r);

        bool live = (s.streaming() || s.holdKind) && s.renderer.hasFrame();
        ImVec2 pa = a, pb = b;
        float ow = (float)s.renderer.outputWidth(), oh = (float)s.renderer.outputHeight();
        if (live && ow > 0 && oh > 0) {
            float sc = std::min((b.x - a.x) / ow, (b.y - a.y) / oh);
            ImVec2 size(ow * sc, oh * sc);
            pa = ImVec2(a.x + (b.x - a.x - size.x) / 2, a.y + (b.y - a.y - size.y) / 2);
            pb = ImVec2(pa.x + size.x, pa.y + size.y);
            dl->AddImageRounded((ImTextureID)(intptr_t)s.renderer.outputTexture(), pa, pb, ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32_WHITE, r);
        } else {
            drawEmptyState(s, a, b);
        }
        if (!live) return;

        // Top-left: what the camera is doing.
        char info[96];
        bool watched = s.vcam && s.vcam->running() && s.vcam->watched();
        snprintf(info, sizeof(info), "%s  %dx%d  %.0f fps", watched ? "LIVE" : "PREVIEW", s.sp.outWidth, s.sp.outHeight, s.fpsShown);
        ImFont* sf = ui::fonts().small;
        ImVec2 ts = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0, info);
        ImVec2 ia(pa.x + 14 * S, pa.y + 14 * S), ib(ia.x + ts.x + 30 * S, ia.y + ts.y + 12 * S);
        ui::glassPanel(dl, ia, ib, s.renderer.glassTexture(), pa, pb, (ib.y - ia.y) / 2);
        float dr = 3.5f * S;
        dl->AddCircleFilled(ImVec2(ia.x + 12 * S, (ia.y + ib.y) / 2), dr, ui::col(watched ? P.bad : P.tertiary));
        dl->AddText(sf, sf->FontSize, ImVec2(ia.x + 20 * S, ia.y + 6 * S), ui::col(P.dark ? P.label : ImVec4(0.1f, 0.1f, 0.11f, 1)), info);

        // Bottom: the glass toolbar with the things people toggle mid-call.
        FilterPrefs& f = s.sp.filters;
        bool torch = jv(s.cam, "flash", false);
        int n = 6 + (torch ? 1 : 0);
        float bs = 38 * S, gap = 6 * S, padx = 8 * S;
        float tw = n * bs + (n - 1) * gap + 2 * padx, th = bs + 2 * 7 * S;
        ImVec2 ta((pa.x + pb.x - tw) / 2, pb.y - th - 16 * S), tb(ta.x + tw, ta.y + th);
        ui::glassPanel(dl, ta, tb, s.renderer.glassTexture(), pa, pb, th / 2);
        ImGui::SetCursorScreenPos(ImVec2(ta.x + padx, ta.y + 7 * S));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
        bool ch = false;
        if (ui::glassButton(s.paused ? ui::icon::play : ui::icon::pause, s.paused ? "Resume camera" : "Pause camera (Be right back)", s.paused, bs))
            command(Shell::TogglePause);
        ImGui::SameLine();
        if (ui::glassButton(ui::icon::focus, "Background blur", f.background == "blur", bs)) {
            f.background = f.background == "blur" ? "off" : "blur";
            ch = true;
        }
        ImGui::SameLine();
        if (ui::glassButton(ui::icon::scanFace, "Auto-frame", f.autoFrame, bs)) {
            f.autoFrame = !f.autoFrame;
            ch = true;
        }
        ImGui::SameLine();
        if (ui::glassButton(ui::icon::mirror, "Mirror", s.sp.mirror, bs)) {
            s.sp.mirror = !s.sp.mirror;
            ch = true;
        }
        ImGui::SameLine();
        if (ui::glassButton(ui::icon::rotate, "Rotate 90\xc2\xb0", false, bs)) {
            s.sp.extraTurns = (s.sp.extraTurns + 1) & 3;
            ch = true;
        }
        ImGui::SameLine();
        bool micOn = prefs_.audio && !prefs_.muted;
        if (ui::glassButton(micOn ? ui::icon::mic : ui::icon::micOff, micOn ? "Phone mic on" : "Phone mic off", micOn, bs)) {
            if (!prefs_.audio) {
                prefs_.audio = true;
                prefs_.muted = false;
                sendMic();
            } else {
                prefs_.muted = !prefs_.muted;
            }
            audio_.setMuted(prefs_.muted);
            prefs_.save();
        }
        if (torch) {
            ImGui::SameLine();
            if (ui::glassButton(ui::icon::torch, "Torch", s.ui.torch, bs)) {
                s.ui.torch = !s.ui.torch;
                s.conn.sendControl({{"torch", s.ui.torch}});
            }
        }
        ImGui::PopStyleVar();
        if (ch) {
            s.dirty = true;
            prefs_.save();
        }
    }

    void drawEmptyState(Session& s, ImVec2 a, ImVec2 b) {
        const float S = ui::S();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 fg(0.92f, 0.92f, 0.96f, 1.f), fg2(0.92f, 0.92f, 0.96f, 0.55f); // the preview well is always dark
        ImFont* icf = ui::fonts().icons;
        float isz = icf->FontSize * 2.6f;
        const char* ic = ui::icon::phone;
        ImVec2 isize = icf->CalcTextSizeA(isz, FLT_MAX, 0, ic);
        float cx = (a.x + b.x) / 2, y = (a.y + b.y) / 2 - 90 * S;
        dl->AddText(icf, isz, ImVec2(cx - isize.x / 2, y), ui::col(ui::P().accent), ic);
        y += isize.y + 16 * S;
        bool connecting = s.conn.state() == Connection::State::Connecting;
        const char* head = connecting ? "Connecting to your phone..." : "Connect your phone";
        ImFont* tf = ui::fonts().title;
        ImVec2 hs = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0, head);
        dl->AddText(tf, tf->FontSize, ImVec2(cx - hs.x / 2, y), ui::col(fg), head);
        y += hs.y + 12 * S;
        const char* lines[] = {"Open rng-lens on the phone, on the same Wi-Fi.",
                               "It shows up under Phone. Or plug in USB with USB debugging on."};
        ImFont* bf = ui::fonts().body;
        for (const char* l : lines) {
            ImVec2 ls = bf->CalcTextSizeA(bf->FontSize, FLT_MAX, 0, l);
            dl->AddText(bf, bf->FontSize, ImVec2(cx - ls.x / 2, y), ui::col(fg2), l);
            y += ls.y + 6 * S;
        }
    }

    // ---------------------------------------------------------------- inspector

    void drawInspector(Session& s) {
        static const char* titles[] = {"Phone", "Camera", "Effects", "Output", "Sound", "Settings", "Info"};
        ui::title(titles[section_]);
        ImGui::Dummy(ImVec2(0, 10 * ui::S()));
        switch (section_) {
        case kPhone: drawPhone(s); break;
        case kCamera: drawCamera(s); break;
        case kEffects: drawEffects(s); break;
        case kOutput: drawOutput(s); break;
        case kSound: drawAudio(); break;
        case kSettings: drawSettings(); break;
        case kInfo: drawInfo(s); break;
        default: break;
        }
    }

    void drawPhoneUpdateGroup(Session& s) {
        int st = phoneUpdState_.load();
        if (!phoneOutdated(s) && st != 2) return;
        std::string v = jv(s.hello, "version", std::string());
        ui::beginGroup("Phone app");
        ui::rowBlock();
        std::string msg = st == 2 ? std::string("Updated.")
                                  : "The phone runs " + (v.empty() ? std::string("an old version") : "version " + v) +
                                        " and this PC app is " RNGLENS_VERSION ". Zoom, focus, torch and other camera controls need both up to date.";
        ui::caption(msg.c_str(), st == 2 ? ui::P().good : ui::P().warn);
        if (st != 2) {
            float w = ImGui::CalcItemWidth();
            if (ui::button(st == 1 ? "Updating the phone..." : "Update the phone over USB", true, w) && st != 1) startPhoneUpdate();
        }
        ui::rowEnd();
        std::string foot;
        {
            std::lock_guard<std::mutex> lock(phoneUpdM_);
            foot = phoneUpdMsg_;
        }
        if (foot.empty() && st == 0) foot = "Or download rng-lens.apk on the phone from the release page and install it.";
        ui::endGroup(foot.c_str());
    }

    void drawPhone(Session& s) {
        const ui::Palette& P = ui::P();
        Connection::State st = s.conn.state();
        drawPhoneUpdateGroup(s);
        if (st == Connection::State::Streaming || st == Connection::State::Connecting) {
            bool live = st == Connection::State::Streaming;
            std::string dev = s.device();
            ui::beginGroup(live ? "Connected" : "Connecting");
            ui::row("Phone");
            ui::value(dev.empty() ? s.conn.peer().c_str() : dev.c_str(), P.secondary);
            ui::rowEnd();
            ui::row("Address");
            ui::value(s.conn.peer().c_str(), P.secondary);
            ui::rowEnd();
            if (live) {
                int bat = jv(s.pstate, "battery", -1);
                bool charging = jv(s.pstate, "charging", false);
                double temp = jv(s.pstate, "tempC", 0.0);
                json th = s.pstate.contains("thermal") ? s.pstate["thermal"] : json::object();
                int level = jv(th, "level", 0);
                char b[64];
                if (bat >= 0) {
                    ui::row("Battery");
                    snprintf(b, sizeof(b), "%d%%%s", bat, charging ? ", charging" : "");
                    ui::value(b, bat < 15 && !charging ? P.bad : P.secondary);
                    ui::rowEnd();
                }
                ui::row("Temperature");
                snprintf(b, sizeof(b), "%.0f\xc2\xb0" "C%s", temp, level >= 3 ? ", hot" : level >= 1 ? ", warm" : "");
                ui::value(b, level >= 3 || temp >= 45 ? P.bad : level >= 1 || temp >= 40 ? P.warn : P.secondary);
                ui::rowEnd();
                ui::row("Streaming for");
                ui::value(clock(nowMs() - s.streamStart).c_str(), P.secondary);
                ui::rowEnd();
                std::string note;
                ImVec4 nc = P.secondary;
                if (jv(th, "throttled", false)) {
                    note = "The phone is hot, so it lowered to " + jv(th, "limit", std::string("less")) +
                           ". Full quality comes back when it cools.";
                    nc = P.warn;
                } else if (level == 1) {
                    note = "The phone is warming up. Locking its screen and taking it off the charger helps.";
                }
                if (bat >= 0 && bat < 15 && !charging) {
                    note = "The phone battery is low: plug it in.";
                    nc = P.bad;
                }
                if (!note.empty()) {
                    ui::rowBlock();
                    ui::caption(note.c_str(), nc);
                    ui::rowEnd();
                }
            }
            ui::endGroup();
            if (ui::button("Disconnect", false, ImGui::GetContentRegionAvail().x)) {
                s.userIdle = true;
                s.conn.disconnect();
            }
            return;
        }

        std::vector<Discovery::Phone> phones = disc_.phones();
        const Session& other = *sessions_[1 - s.index];
        ui::beginGroup("Phones on this Wi-Fi");
        int shown = 0;
        for (const Discovery::Phone& p : phones) {
            if (active() == 2 && other.streaming() && other.conn.peer() == p.ip) continue; // already on the other tab
            ImGui::PushID(p.ip.c_str());
            ui::row(p.name.c_str(), 96 * ui::S());
            if (ui::button("Connect", true, -1)) s.connectTo(p.ip, p.port);
            ui::rowEnd();
            ImGui::PopID();
            shown++;
        }
        if (!shown) {
            ui::rowBlock();
            ui::caption(disc_.listening() ? "Looking for phones... Open rng-lens on the phone."
                                          : "Wi-Fi discovery is blocked (port 38441). Connect by address below.",
                        P.secondary);
            ui::rowEnd();
        }
        ui::endGroup(st == Connection::State::Failed ? s.conn.error().c_str() : nullptr);

        ui::beginGroup("Other ways");
        if (s.index == 0) {
            ui::row("USB cable", 96 * ui::S());
            if (ui::button("Connect##usb", false, -1)) {
                s.usbMsg.clear();
                if (adbForward(s.usbMsg)) s.connectTo("127.0.0.1", proto::kPort);
            }
            ui::rowEnd();
        }
        ui::rowBlock();
        float w = ImGui::CalcItemWidth();
        ImGui::SetNextItemWidth(w - 96 * ui::S() - 8 * ui::S());
        ImGui::InputTextWithHint("##ip", "Phone address, e.g. 192.168.1.20", s.ipBuf, sizeof(s.ipBuf));
        ImGui::SameLine();
        if (ui::button("Connect##ip", false, 96 * ui::S()) && s.ipBuf[0]) s.connectTo(s.ipBuf, proto::kPort);
        ui::rowEnd();
        if (toggleRow("Reconnect automatically", &s.sp.autoConnect)) prefs_.save();
        ui::endGroup(s.usbMsg.empty() ? nullptr : s.usbMsg.c_str());
    }

    static std::string cameraLabel(const json& c) {
        std::string facing = jv(c, "facing", std::string("back"));
        facing[0] = (char)toupper(facing[0]);
        char buf[96];
        double focal = jv(c, "focal", 0.0);
        if (focal > 0) snprintf(buf, sizeof(buf), "%s %.1f mm", facing.c_str(), focal);
        else snprintf(buf, sizeof(buf), "%s (#%s)", facing.c_str(), jv(c, "id", std::string()).c_str());
        return buf;
    }

    // Row with a switch on the right.
    bool toggleRow(const char* label, bool* v) {
        const float S = ui::S();
        ui::row(label, 36 * S);
        bool r = ui::toggle(label, v);
        ui::rowEnd();
        return r;
    }

    void needPhone(const char* what) {
        ui::beginGroup();
        ui::rowBlock();
        ui::caption(what, ui::P().secondary);
        ui::rowEnd();
        ui::endGroup();
    }

    void drawCamera(Session& s) {
        if (!s.streaming() || s.hello.empty() || !s.hello.contains("cameras")) {
            needPhone("Connect a phone to pick its lens, resolution, zoom and focus.");
            return;
        }
        const json& cams = s.hello["cameras"];
        ui::beginGroup("Lens and quality");
        ui::row("Lens");
        std::string cur = !s.cam.empty() ? cameraLabel(s.cam) : s.ui.camera;
        if (ui::beginPopupButton("##lens", cur.c_str())) {
            for (const json& c : cams) {
                std::string id = jv(c, "id", std::string());
                if (ui::option((cameraLabel(c) + "##" + id).c_str(), id == s.ui.camera) && id != s.ui.camera) {
                    s.ui.camera = id;
                    s.control({{"camera", id}});
                }
            }
            ImGui::EndCombo();
        }
        ui::rowEnd();
        if (s.cam.empty()) {
            ui::endGroup();
            return;
        }
        int maxFpsForSize = 30;
        char cur2[64];
        snprintf(cur2, sizeof(cur2), "%d x %d", s.ui.width, s.ui.height);
        ui::row("Resolution");
        if (ui::beginPopupButton("##size", cur2)) {
            for (const json& z : s.cam["sizes"]) {
                int w = z[0], h = z[1], mf = z[2];
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "%d x %d  \xc2\xb7  %d fps", w, h, mf);
                if (ui::option(lbl, w == s.ui.width && h == s.ui.height)) {
                    s.ui.width = w;
                    s.ui.height = h;
                    s.control({{"width", w}, {"height", h}});
                }
            }
            ImGui::EndCombo();
        }
        ui::rowEnd();
        for (const json& z : s.cam["sizes"])
            if ((int)z[0] == s.ui.width && (int)z[1] == s.ui.height) maxFpsForSize = z[2];
        std::vector<int> fpsList;
        for (int f : {15, 24, 30, 60})
            if (f <= maxFpsForSize || f == s.ui.fps) fpsList.push_back(f);
        std::vector<std::string> fpsLbl;
        for (int f : fpsList) fpsLbl.push_back(std::to_string(f));
        std::vector<const char*> fpsPtr;
        for (auto& l : fpsLbl) fpsPtr.push_back(l.c_str());
        int fsel = 0;
        for (size_t i = 0; i < fpsList.size(); i++)
            if (fpsList[i] == s.ui.fps) fsel = (int)i;
        ui::row("Frame rate");
        if (ui::segmented("fps", fpsPtr.data(), (int)fpsPtr.size(), &fsel)) {
            s.ui.fps = fpsList[fsel];
            s.control({{"fps", s.ui.fps}});
        }
        ui::rowEnd();
        if (jv(s.hello, "hevc", false)) {
            const char* codecs[] = {"H.264", "HEVC"};
            ui::row("Codec");
            if (ui::segmented("codec", codecs, 2, &s.ui.codec)) s.control({{"codec", s.ui.codec}});
            ui::rowEnd();
        }
        bool autoRate = s.ui.bitrate == 0;
        if (toggleRow("Automatic bitrate", &autoRate)) {
            s.ui.bitrate = autoRate ? 0 : jv(s.pstate, "streamBitrate", 8000000);
            s.control({{"bitrate", s.ui.bitrate}});
        }
        if (!autoRate) {
            float mb = s.ui.bitrate / 1e6f;
            ui::row("Bitrate");
            if (ui::slider("##br", &mb, 1.f, 60.f, "%.0f Mbit/s")) s.ui.bitrate = (int)(mb * 1e6f);
            if (ImGui::IsItemDeactivatedAfterEdit()) s.control({{"bitrate", s.ui.bitrate}});
            ui::rowEnd();
        }
        ui::endGroup();

        PhoneUi& ui_ = s.ui;
        ui::beginGroup("Picture");
        json z = jv(s.cam, "zoom", json::array({1.0, 1.0}));
        float zmin = z[0], zmax = z[1];
        if (zmax > zmin + 0.01f) {
            ui::row("Zoom");
            if (ui::slider("##zoom", &ui_.zoom, zmin, std::min(zmax, 30.f), "%.1fx", true)) s.conn.sendControl({{"zoom", ui_.zoom}});
            ui::rowEnd();
            std::vector<float> presets;
            std::vector<std::string> pl;
            if (zmin < 0.99f) {
                presets.push_back(zmin);
                pl.push_back("Wide");
            }
            for (float p : {1.f, 2.f, 5.f})
                if (p <= zmax) {
                    presets.push_back(p);
                    char lb[8];
                    snprintf(lb, sizeof(lb), "%gx", p);
                    pl.push_back(lb);
                }
            std::vector<const char*> pp;
            for (auto& l : pl) pp.push_back(l.c_str());
            int zsel = -1;
            for (size_t i = 0; i < presets.size(); i++)
                if (std::fabs(presets[i] - ui_.zoom) < 0.02f) zsel = (int)i;
            int before = zsel;
            ui::row("");
            if (ui::segmented("zoompre", pp.data(), (int)pp.size(), &zsel) || (zsel != before && zsel >= 0)) {
                ui_.zoom = presets[zsel];
                s.conn.sendControl({{"zoom", ui_.zoom}});
            }
            ui::rowEnd();
        }
        std::vector<const char*> afl = {"Auto", "Lock"};
        bool manualOk = jv(s.cam, "afManual", false);
        if (manualOk) afl.push_back("Manual");
        int afSel = ui_.af == "lock" ? 1 : ui_.af == "manual" && manualOk ? 2 : 0;
        ui::row("Focus");
        if (ui::segmented("af", afl.data(), (int)afl.size(), &afSel)) {
            ui_.af = afSel == 1 ? "lock" : afSel == 2 ? "manual" : "auto";
            s.conn.sendControl({{"af", ui_.af}});
        }
        ui::rowEnd();
        if (ui_.af == "manual") {
            ui::row("Distance");
            if (ui::slider("##focus", &ui_.focus, 0.f, 1.f, ui_.focus < 0.02f ? "far" : "%.2f")) s.conn.sendControl({{"focus", ui_.focus}});
            ui::rowEnd();
        }
        json evr = jv(s.cam, "ev", json::array({0, 0, 1.0}));
        int evMin = evr[0], evMax = evr[1];
        float evStep = evr[2];
        if (evMax > evMin) {
            char fmt[32];
            snprintf(fmt, sizeof(fmt), "%+.1f EV", ui_.ev * evStep);
            ui::row("Exposure");
            if (ui::sliderInt("##ev", &ui_.ev, evMin, evMax, fmt)) s.conn.sendControl({{"ev", ui_.ev}});
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                ui_.ev = 0;
                s.conn.sendControl({{"ev", 0}});
            }
            ui::rowEnd();
        }
        if (jv(s.cam, "aeRegions", false) && toggleRow("Expose for my face", &s.sp.faceExposure)) prefs_.save();
        bool aeLock = ui_.ae == "lock", wbLock = ui_.awb == "lock";
        if (toggleRow("Lock exposure", &aeLock)) {
            ui_.ae = aeLock ? "lock" : "auto";
            s.conn.sendControl({{"ae", ui_.ae}});
        }
        if (toggleRow("Lock white balance", &wbLock)) {
            ui_.awb = wbLock ? "lock" : "auto";
            s.conn.sendControl({{"awb", ui_.awb}});
        }
        if (jv(s.cam, "flash", false) && toggleRow("Torch", &ui_.torch)) s.conn.sendControl({{"torch", ui_.torch}});
        if (jv(s.cam, "stab", false) && toggleRow("Stabilization", &ui_.stab)) s.conn.sendControl({{"stab", ui_.stab}});
        ui::endGroup(s.sp.faceExposure ? "Expose for my face keeps you bright even with a window behind you." : nullptr);
    }

    // Effect slider row; double-click resets.
    bool fx(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
        ui::row(label);
        std::string id = std::string("##") + label;
        bool changed = ui::slider(id.c_str(), v, lo, hi, fmt);
        if (ImGui::IsItemDeactivatedAfterEdit()) prefs_.save();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            *v = lo < 0 ? 0.f : (std::string(label) == "Strength" ? 1.f : 0.f);
            changed = true;
            prefs_.save();
        }
        ui::rowEnd();
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
        const ui::Palette& P = ui::P();
        FilterPrefs& f = s.sp.filters;
        bool ch = false;

        ui::beginGroup("Background");
        int bg = f.background == "blur" ? 1 : f.background == "image" ? 2 : 0;
        const char* bgl[] = {"Off", "Blur", "Picture"};
        ui::row("Background");
        if (ui::segmented("bg", bgl, 3, &bg)) {
            f.background = bg == 1 ? "blur" : bg == 2 ? "image" : "off";
            ch = true;
            if (f.background == "image" && f.backgroundImage.empty()) pickImage(f.backgroundImage);
            if (f.background == "image") s.renderer.setBackgroundImage(f.backgroundImage);
            prefs_.save();
        }
        ui::rowEnd();
        if (f.background == "blur") ch |= fx("Amount", &f.blur, 0.f, 1.f);
        if (f.background == "image") {
            std::string name = f.backgroundImage.empty() ? std::string("Choose...") : f.backgroundImage;
            size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            ui::row("Picture");
            if (ui::button((name + "##pic").c_str(), false, -1) && pickImage(f.backgroundImage)) {
                if (!s.renderer.setBackgroundImage(f.backgroundImage)) s.visionWhy = "Couldn't open that picture";
                ch = true;
                prefs_.save();
            }
            ui::rowEnd();
        }
        if (toggleRow("Auto-frame", &f.autoFrame)) {
            ch = true;
            prefs_.save();
        }
        if (f.autoFrame) {
            ch |= fx("Framing", &f.frameTightness, 0.f, 1.f, f.frameTightness < 0.35f ? "wide" : f.frameTightness > 0.7f ? "close" : "medium");
            ch |= fx("Follow speed", &f.frameSpeed, 0.f, 1.f, f.frameSpeed < 0.35f ? "slow" : f.frameSpeed > 0.7f ? "fast" : "normal");
        }
        std::string foot;
        if ((f.background != "off" || f.autoFrame) && s.vision.failed()) foot = s.vision.error();
        else if ((f.background != "off" || f.autoFrame) && !s.vision.ready()) foot = "Loading the background and face models...";
        if (!s.visionWhy.empty()) foot = s.visionWhy;
        ui::endGroup(foot.empty() ? "Auto-frame keeps your face in the shot and follows you when you move." : foot.c_str());

        ui::beginGroup("Look");
        const char* cur = "None";
        for (const Look& l : kLooks)
            if (f.look == l.id) cur = l.label;
        ui::row("Look");
        if (ui::beginPopupButton("##look", cur)) {
            for (const Look& l : kLooks) {
                if (ui::option(l.label, f.look == l.id)) {
                    f.look = l.id;
                    ch = true;
                    prefs_.save();
                }
            }
            ImGui::EndCombo();
        }
        ui::rowEnd();
        if (f.look != "none") ch |= fx("Strength", &f.lookAmount, 0.f, 1.f);
        ch |= fx("Brightness", &f.brightness, -1.f, 1.f, "%+.2f");
        ch |= fx("Contrast", &f.contrast, -1.f, 1.f, "%+.2f");
        ch |= fx("Saturation", &f.saturation, -1.f, 1.f, "%+.2f");
        ch |= fx("Warmth", &f.warmth, -1.f, 1.f, "%+.2f");
        ui::endGroup("Double-click a slider to reset it.");

        ui::beginGroup("Enhance");
        ch |= fx("Sharpen", &f.sharpen, 0.f, 1.f);
        ch |= fx("Smooth skin", &f.smooth, 0.f, 1.f);
        ch |= fx("Night mode", &f.night, 0.f, 1.f);
        ui::endGroup("Night mode cleans up grain in low light by averaging frames where nothing moves.");

        if (f.anyActive() && ui::button("Reset all effects", false, ImGui::GetContentRegionAvail().x)) {
            f = FilterPrefs();
            ch = true;
            prefs_.save();
        }
        (void)P;
        if (ch) s.dirty = true;
    }

    void drawOutput(Session& s) {
        const ui::Palette& P = ui::P();
        SlotPrefs& sp = s.sp;
        bool save = false, restart = false;
        std::string vlabel = "Camera \"" + s.name() + "\"";
        ui::beginGroup("Virtual camera");
        if (toggleRow(vlabel.c_str(), &sp.virtualCamera)) restart = true;
        const char* cur = "Custom";
        for (const OutputSize& o : kOutputs)
            if (o.w == sp.outWidth && o.h == sp.outHeight) cur = o.label;
        ui::row("Size");
        if (ui::beginPopupButton("##out", cur)) {
            for (const OutputSize& o : kOutputs) {
                if (ui::option(o.label, o.w == sp.outWidth && o.h == sp.outHeight)) {
                    sp.outWidth = o.w;
                    sp.outHeight = o.h;
                    restart = true;
                }
            }
            ImGui::EndCombo();
        }
        ui::rowEnd();
        int fsel = sp.outFps == 60 ? 1 : 0;
        const char* fl[] = {"30 fps", "60 fps"};
        ui::row("Frame rate");
        if (ui::segmented("ofps", fl, 2, &fsel)) {
            sp.outFps = fsel ? 60 : 30;
            restart = true;
        }
        ui::rowEnd();
        std::string foot;
        if (sp.virtualCamera) {
            if (!s.vcamWhy.empty()) foot = s.vcamWhy;
            else if (s.vcam && s.vcam->running()) {
                if (s.vcam->watched()) {
                    char b[96];
                    snprintf(b, sizeof(b), "An app is using the camera (%llu frames sent).", (unsigned long long)s.vcamSent);
                    foot = b;
                } else {
                    foot = "Ready: pick \"" + s.name() + "\" in Zoom, Teams, Discord or OBS.";
                }
            }
        }
        ui::endGroup(foot.empty() ? nullptr : foot.c_str());
        (void)P;

        ui::beginGroup("Orientation");
        save |= toggleRow("Auto-rotate", &sp.autoRotate);
        save |= toggleRow("Mirror", &sp.mirror);
        save |= toggleRow("Flip upside down", &sp.flip);
        ui::row("Rotate", 96 * ui::S());
        if (ui::button("90\xc2\xb0", false, -1)) {
            sp.extraTurns = (sp.extraTurns + 1) & 3;
            save = true;
        }
        ui::rowEnd();
        ui::endGroup("Mirror only changes what you send. Most call apps mirror your self-view on their own.");
        if (restart) {
            s.restartVcam();
            save = true;
        }
        if (save) {
            s.dirty = true;
            prefs_.save();
        }
    }

    void sendMic() {
        for (int i = 0; i < active(); i++)
            sessions_[i]->conn.sendControl({{"audio", prefs_.audio && prefs_.audioSlot == i}, {"mic", prefs_.micMode}});
        audio_.clear();
    }

    void drawAudio() {
        const ui::Palette& P = ui::P();
        if (audioRate_ != openedRate_) openAudio(); // stream switched sample rate
        bool save = false;
        ui::beginGroup("Phone microphone");
        if (toggleRow("Use the phone's mic", &prefs_.audio)) {
            sendMic();
            save = true;
        }
        if (prefs_.audio) {
            int mode = prefs_.micMode == "raw" ? 1 : 0;
            const char* ml[] = {"Voice", "Raw"};
            ui::row("Sound");
            if (ui::segmented("mic", ml, 2, &mode)) {
                prefs_.micMode = mode ? "raw" : "voice";
                sendMic();
                save = true;
            }
            ui::rowEnd();
            if (prefs_.secondPhone) {
                const char* al[] = {"Phone 1", "Phone 2"};
                ui::row("From");
                if (ui::segmented("as", al, 2, &prefs_.audioSlot)) {
                    sendMic();
                    save = true;
                }
                ui::rowEnd();
            }
        }
        const Session& as = *sessions_[prefs_.audioSlot];
        std::string foot = "Voice cleans up noise and echo for calls. Raw keeps the sound untouched for music and streams.";
        if (prefs_.audio && !as.hello.empty() && !jv(as.hello, "mic", true))
            foot = "Microphone access is off on the phone (Settings > Apps > rng-lens).";
        ui::endGroup(foot.c_str());

        if (prefs_.audio) {
            ui::beginGroup("Playback");
            std::string cur = audio_.isOpen() ? audio_.deviceName() : std::string("(none)");
            ui::row("Play to");
            if (ui::beginPopupButton("##aout", cur.c_str())) {
                if (devices_.empty() || ImGui::IsWindowAppearing()) devices_ = audio_.devices();
                if (ui::option("System default", prefs_.audioDevice.empty())) {
                    prefs_.audioDevice.clear();
                    openAudio();
                    save = true;
                }
                for (const AudioOutput::Device& d : devices_) {
                    if (ui::option(d.name.c_str(), d.name == prefs_.audioDevice)) {
                        prefs_.audioDevice = d.name;
                        openAudio();
                        save = true;
                    }
                }
                ImGui::EndCombo();
            }
            ui::rowEnd();
            float vol = prefs_.volume * 100.f;
            ui::row("Volume");
            if (ui::slider("##vol", &vol, 0.f, 200.f, "%.0f%%")) {
                prefs_.volume = vol / 100.f;
                audio_.setVolume(prefs_.volume);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
            ui::rowEnd();
            if (toggleRow("Mute", &prefs_.muted)) {
                audio_.setMuted(prefs_.muted);
                save = true;
            }
            meter_ = std::max(audio_.peak(), meter_ * 0.85f);
            ui::row("Level");
            ui::meter(meter_, meter_ > 0.95f ? P.bad : P.accent);
            ui::rowEnd();
            ui::endGroup(!audioWhy_.empty() ? audioWhy_.c_str()
                                            : "To use the phone as a microphone in calls, install the free VB-Cable and pick \"CABLE Input\" here.");
        }
        if (save) prefs_.save();
    }

    void drawSettings() {
        bool save = false;
        ui::beginGroup("Appearance");
        int th = prefs_.theme == "light" ? 1 : prefs_.theme == "dark" ? 2 : 0;
        const char* tl[] = {"System", "Light", "Dark"};
        ui::row("Theme");
        if (ui::segmented("theme", tl, 3, &th)) {
            prefs_.theme = th == 1 ? "light" : th == 2 ? "dark" : "system";
            lastThemeAt_ = 0;
            save = true;
        }
        ui::rowEnd();
        ui::endGroup();

        ui::beginGroup("General");
        if (shell_.supported()) {
            if (toggleRow("Start with Windows", &autostart_)) Shell::setAutostart(autostart_);
            if (toggleRow("Keep running in the tray", &prefs_.closeToTray)) save = true;
            if (toggleRow("Hotkeys", &prefs_.hotkeys)) {
                shell_.setHotkeysEnabled(prefs_.hotkeys);
                save = true;
            }
            if (prefs_.hotkeys) {
                for (const Shell::Hotkey& k : shell_.hotkeys()) {
                    ui::row(k.what);
                    ui::value(k.registered ? k.keys : "taken by another app", k.registered ? ui::P().secondary : ui::P().warn);
                    ui::rowEnd();
                }
            }
        }
        if (toggleRow("GPU decoding", &prefs_.hardwareDecode)) save = true;
        bool second = prefs_.secondPhone;
        if (toggleRow("Second phone", &second)) {
            prefs_.secondPhone = second;
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
        ui::endGroup("A second phone shows up as its own camera, \"rng-lens 2\".");

        ui::beginGroup("Updates");
        if (toggleRow("Check for updates", &prefs_.autoUpdate)) save = true;
        ui::row("Version " RNGLENS_VERSION, 120 * ui::S());
        Updater::State us = updater_.state();
        bool busy = us == Updater::State::Checking || us == Updater::State::Downloading;
        if (us == Updater::State::Available) {
            if (ui::button("Update", true, -1)) updater_.download();
        } else if (ui::button(busy ? "Checking..." : "Check now", false, -1) && !busy) {
            updater_.check(RNGLENS_VERSION);
        }
        ui::rowEnd();
        std::string foot;
        switch (us) {
        case Updater::State::UpToDate: foot = "You have the latest version."; break;
        case Updater::State::Available: foot = "rng-lens " + updater_.latest() + " is out."; break;
        case Updater::State::Downloading: foot = "Downloading the installer..."; break;
        case Updater::State::Failed: foot = "Couldn't check: " + updater_.error() + "."; break;
        case Updater::State::Unsupported: foot = "Updates are checked on Windows only."; break;
        default: foot = "Only the version number is fetched from GitHub. Nothing about you is sent."; break;
        }
        ui::endGroup(foot.c_str());
        if (save) prefs_.save();
    }

    void drawInfo(Session& s) {
        const ui::Palette& P = ui::P();
        char b[160];
        ui::beginGroup("Stream");
        if (!s.streaming()) {
            ui::rowBlock();
            ui::caption("Not streaming.", P.secondary);
            ui::rowEnd();
        } else {
            ui::row("Video");
            snprintf(b, sizeof(b), "%dx%d, %.0f fps, %.1f Mbit/s", s.conn.streamWidth(), s.conn.streamHeight(), s.fpsShown, s.mbps);
            ui::value(b, P.secondary);
            ui::rowEnd();
            ui::row("Decoder");
            snprintf(b, sizeof(b), "%s, %.1f ms", s.decoder.description().c_str(), s.decoder.lastDecodeMs());
            ui::value(b, P.secondary);
            ui::rowEnd();
            std::string ac = s.conn.audioCodec();
            if (!ac.empty() && prefs_.audioSlot == s.index) {
                ui::row("Sound");
                snprintf(b, sizeof(b), "%s, %.0f ms, %llu dropouts", ac.c_str(), audio_.bufferedMs(), (unsigned long long)audio_.underruns());
                ui::value(b, P.secondary);
                ui::rowEnd();
            }
            if (s.vision.ready() && (s.sp.filters.background != "off" || s.sp.filters.autoFrame)) {
                ui::row("Vision");
                snprintf(b, sizeof(b), "mask %.0f ms, face %.0f ms", s.vision.segMs(), s.vision.faceMs());
                ui::value(b, P.secondary);
                ui::rowEnd();
            }
            int dropped = jv(s.pstate, "dropped", 0);
            if (dropped > 0) {
                ui::row("Dropped on phone");
                snprintf(b, sizeof(b), "%d (network too slow)", dropped);
                ui::value(b, P.warn);
                ui::rowEnd();
            }
        }
        ui::endGroup();

        if (s.temps.size() >= 2) {
            std::vector<float> t(s.temps.begin(), s.temps.end());
            float lo = *std::min_element(t.begin(), t.end()), hi = *std::max_element(t.begin(), t.end());
            snprintf(b, sizeof(b), "Phone temperature: %.0f\xc2\xb0" "C now, %.0f to %.0f\xc2\xb0" "C", t.back(), lo, hi);
            ui::beginGroup("Temperature");
            ui::rowBlock();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PlotLines("##temp", t.data(), (int)t.size(), 0, nullptr, std::min(lo, 30.f), std::max(hi, 45.f),
                             ImVec2(-1, 70 * ui::S()));
            ImGui::PopStyleColor();
            ui::caption(b, P.secondary);
            ui::rowEnd();
            ui::endGroup();
        }

        ui::beginGroup("About");
        ui::row("Version");
        ui::value(RNGLENS_VERSION, P.secondary);
        ui::rowEnd();
        ui::row("Source");
        ui::value("github.com/MRRengi/rng-lens", P.secondary);
        ui::rowEnd();
        ui::endGroup("Free and open source (MIT).");
    }

    Prefs& prefs_;
    AppOptions opt_;
    GLFWwindow* win_ = nullptr;
    std::unique_ptr<Session> sessions_[2];
    int tab_ = 0;
    Discovery disc_;
    Shell shell_;
    bool autostart_ = false, quit_ = false;
    int64_t lastTrayAt_ = 0, lastThemeAt_ = 0, lastUpdateCheck_ = 0;
    Updater updater_;
    // Updating the phone app over USB (adb install of the APK from the matching release)
    std::thread phoneUpd_;
    std::atomic<int> phoneUpdState_{0}; // 0 idle, 1 working, 2 done, 3 failed
    std::mutex phoneUpdM_;
    std::string phoneUpdMsg_;
    int section_ = 0;
    float scale_ = 1.f;
    bool dark_ = true;
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
