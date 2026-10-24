// The desktop window: preview on the left, controls on the right.
// The loop sleeps until something happens (input, a decoded frame, a phone state change),
// so an idle rng-lens costs nothing.

#include "app.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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
#include "stb_image_write.h"
#include "vcam.h"

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

// Coloured text that wraps at the panel edge (long hints and errors).
inline void wrapped(const ImVec4& c, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

const ImVec4 kAccent = ImVec4(1.00f, 0.38f, 0.25f, 1.00f);
const ImVec4 kMuted = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
const ImVec4 kGood = ImVec4(0.35f, 0.85f, 0.50f, 1.00f);
const ImVec4 kBad = ImVec4(1.00f, 0.40f, 0.40f, 1.00f);

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

class App {
public:
    App(Prefs& prefs, const AppOptions& opt) : prefs_(prefs), opt_(opt), conn_(decoder_) {}

    int run() {
        if (!initWindow()) return 1;
        plat::netInit();
        renderer_.init();
        vcam_ = VirtualCamera::create();
        restartVcam();
        disc_.start();
        conn_.onActivity = [] { glfwPostEmptyEvent(); };
        conn_.onAudio = [this](const float* f, int n, int rate) {
            audioRate_ = rate;
            audio_.push(f, n);
        };
        audio_.setVolume(prefs_.volume);
        audio_.setMuted(prefs_.muted);
        openAudio();
        snprintf(ipBuf_, sizeof(ipBuf_), "%s", prefs_.lastHost.c_str());
        if (!opt_.connectHost.empty()) connectTo(opt_.connectHost, opt_.connectPort);

        while (!glfwWindowShouldClose(win_)) {
            glfwWaitEventsTimeout(0.25);
            tick();
        }

        conn_.disconnect();
        disc_.stop();
        audio_.close();
        if (vcam_) vcam_->stop();
        prefs_.save();
        renderer_.shutdown();
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
        int ww = 1280, wh = 820;
        if (const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor())) {
            ww = std::min(ww, mode->width - 40);
            wh = std::min(940, mode->height - 120);
        }
        win_ = glfwCreateWindow(ww, wh, "rng-lens", nullptr, nullptr);
        if (!win_) return false;
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
        c[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.95f, 1.f);
        c[ImGuiCol_TextDisabled] = kMuted;
        c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.f);
        s.ScaleAllSizes(scale);
    }

    // ---------------------------------------------------------------- loop

    void tick() {
        autoConnect();
        syncFromPhone();

        bool newFrame = false;
        if (AVFrame* f = decoder_.takeLatest()) {
            renderer_.upload(f);
            newFrame = true;
            framesShown_++;
        }
        OutputTransform t = transform();
        bool changed = t.quarterTurns != lastT_.quarterTurns || t.mirror != lastT_.mirror || t.flip != lastT_.flip ||
                       prefs_.outWidth != renderer_.outputWidth() || prefs_.outHeight != renderer_.outputHeight();
        int64_t now = nowMs();
        bool idleRefresh = !renderer_.hasFrame() && now - lastRenderMs_ > 500; // keep apps fed while waiting
        bool pending = false;
        if (newFrame || changed || idleRefresh) {
            renderer_.render(prefs_.outWidth, prefs_.outHeight, t);
            lastT_ = t;
            lastRenderMs_ = now;
            bool dump = !opt_.dumpOutput.empty() && framesShown_ >= (uint64_t)opt_.dumpAfter;
            if (dump || (vcam_ && vcam_->running() && vcam_->watched())) {
                renderer_.beginReadback();
                pending = true;
            }
        }

        updateRates(now);

        bool iconified = glfwGetWindowAttrib(win_, GLFW_ICONIFIED);
        if (!iconified) drawUi();

        if (pending) {
            if (const uint8_t* px = renderer_.mapReadback()) {
                if (vcam_ && vcam_->running() && vcam_->watched()) {
                    vcam_->send(px);
                    vcamSent_++;
                }
                if (!opt_.dumpOutput.empty() && framesShown_ >= (uint64_t)opt_.dumpAfter) dumpOutput(px);
            }
            renderer_.unmapReadback();
        }
        if (!iconified) glfwSwapBuffers(win_);
    }

    void dumpOutput(const uint8_t* bgr) {
        int w = renderer_.outputWidth(), h = renderer_.outputHeight();
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (size_t i = 0; i < rgb.size(); i += 3) {
            rgb[i] = bgr[i + 2];
            rgb[i + 1] = bgr[i + 1];
            rgb[i + 2] = bgr[i];
        }
        stbi_write_png(opt_.dumpOutput.c_str(), w, h, 3, rgb.data(), w * 3);
        printf("dumped %dx%d after %llu frames, %s\n", w, h, (unsigned long long)framesShown_, decoder_.description().c_str());
        fflush(stdout);
        glfwSetWindowShouldClose(win_, GLFW_TRUE);
    }

    void updateRates(int64_t now) {
        if (now - rateAt_ < 1000) return;
        double dt = (now - rateAt_) / 1000.0;
        uint64_t b = conn_.bytesReceived();
        mbps_ = (b - rateBytes_) * 8.0 / 1e6 / dt;
        fpsShown_ = (framesShown_ - rateFrames_) / dt;
        rateBytes_ = b;
        rateFrames_ = framesShown_;
        rateAt_ = now;
    }

    OutputTransform transform() const {
        OutputTransform t;
        int turns = prefs_.extraTurns;
        if (prefs_.autoRotate && !cam_.empty()) {
            int so = jv(cam_, "sensorOrientation", 0);
            int orient = jv(pstate_, "orient", 0);
            bool front = jv(cam_, "facing", std::string()) == "front";
            int deg = front ? (so - orient + 360) % 360 : (so + orient) % 360;
            turns += deg / 90;
        }
        t.quarterTurns = turns & 3;
        t.mirror = prefs_.mirror;
        t.flip = prefs_.flip;
        return t;
    }

    void syncFromPhone() {
        if (conn_.helloSerial() != seenHello_) {
            seenHello_ = conn_.helloSerial();
            hello_ = conn_.hello();
            // Ask for what the user picked last time.
            json want = prefs_.phone.is_object() ? prefs_.phone : json::object();
            want["audio"] = prefs_.audio;
            want["mic"] = prefs_.micMode;
            conn_.sendControl(want);
            audio_.clear();
        }
        if (conn_.stateSerial() != seenState_ && !ImGui::IsAnyItemActive()) {
            seenState_ = conn_.stateSerial();
            pstate_ = conn_.phoneState();
            ui_.from(pstate_);
        }
        cam_ = json::object();
        if (hello_.is_object() && hello_.contains("cameras") && hello_["cameras"].is_array()) {
            for (const json& c : hello_["cameras"])
                if (jv(c, "id", std::string()) == ui_.camera) cam_ = c;
        }
    }

    void autoConnect() {
        Connection::State st = conn_.state();
        if (userIdle_ || !prefs_.autoConnect || prefs_.lastHost.empty()) return;
        if (st == Connection::State::Connecting || st == Connection::State::Streaming) return;
        int64_t now = nowMs();
        if (now - lastAutoTry_ < 3000) return;
        if (prefs_.lastHost == "127.0.0.1") {
            lastAutoTry_ = now + 2000; // USB: adb forward is cheap but not free
            std::string msg;
            if (adbForward(msg)) connectTo("127.0.0.1", proto::kPort);
            return;
        }
        for (const Discovery::Phone& p : disc_.phones()) {
            if (p.ip == prefs_.lastHost) {
                lastAutoTry_ = now;
                connectTo(p.ip, p.port);
                return;
            }
        }
    }

    void connectTo(const std::string& host, int port) {
        userIdle_ = false;
        seenHello_ = seenState_ = 0;
        hello_ = json::object();
        pstate_ = json::object();
        conn_.connect(host, port, prefs_.hardwareDecode);
        prefs_.lastHost = host;
        prefs_.lastPort = port;
        prefs_.save();
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
        msg = "adb not found. Put the Android platform-tools adb next to rng-lens, or install it to PATH.";
        return false;
    }

    void restartVcam() {
        if (!vcam_) return;
        vcam_->stop();
        vcamWhy_.clear();
        if (!prefs_.virtualCamera) return;
        if (!vcam_->available(vcamWhy_)) return;
        if (!vcam_->start(prefs_.outWidth, prefs_.outHeight, prefs_.outFps)) vcamWhy_ = "Could not start the virtual camera";
    }

    void control(const json& j) {
        conn_.sendControl(j);
        // Stream shape choices are remembered and re-applied on the next connect.
        for (const char* k : {"camera", "width", "height", "fps", "codec", "bitrate"})
            if (j.contains(k)) prefs_.phone[k] = j[k];
        if (j.contains("camera")) {
            prefs_.phone.erase("width");
            prefs_.phone.erase("height");
        }
        prefs_.save();
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
        float panelW = 360.f * ImGui::GetStyle().FramePadding.x / 10.f;
        float previewW = ImGui::GetContentRegionAvail().x - panelW - ImGui::GetStyle().ItemSpacing.x;
        ImGui::BeginChild("preview", ImVec2(previewW, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawPreview();
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

    void drawPreview() {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float ow = (float)renderer_.outputWidth(), oh = (float)renderer_.outputHeight();
        if (ow > 0 && oh > 0) {
            float s = std::min(avail.x / ow, avail.y / oh);
            ImVec2 size(ow * s, oh * s);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + (avail.x - size.x) / 2, origin.y + (avail.y - size.y) / 2));
            ImGui::Image((ImTextureID)(intptr_t)renderer_.outputTexture(), size);
        }
        if (conn_.state() != Connection::State::Streaming || !renderer_.hasFrame()) {
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

    static bool radio(const char* label, std::string& v, const char* value) {
        bool on = v == value;
        if (ImGui::RadioButton(label, on) && !on) {
            v = value;
            return true;
        }
        return false;
    }

    void drawPanel() {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextUnformatted("rng-lens");
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(kMuted, " v" RNGLENS_VERSION);

        drawConnection();
        if (conn_.state() == Connection::State::Streaming && hello_.is_object() && !hello_.empty()) {
            drawCamera();
            drawPicture();
        }
        drawOutput();
        drawAudio();
        drawStats();
    }

    void drawConnection() {
        ImGui::SeparatorText("Phone");
        Connection::State st = conn_.state();
        if (st == Connection::State::Streaming || st == Connection::State::Connecting) {
            std::string dev = jv(hello_, "device", std::string());
            ImGui::TextColored(st == Connection::State::Streaming ? kGood : kMuted, "%s %s",
                               st == Connection::State::Streaming ? "Connected:" : "Connecting to",
                               dev.empty() ? conn_.peer().c_str() : dev.c_str());
            if (ImGui::Button("Disconnect", ImVec2(-1, 0))) {
                userIdle_ = true;
                conn_.disconnect();
            }
            return;
        }
        if (st == Connection::State::Failed) wrapped(kBad, conn_.error().c_str());

        std::vector<Discovery::Phone> phones = disc_.phones();
        if (phones.empty()) {
            ImGui::TextColored(kMuted, disc_.listening() ? "Looking for phones on Wi-Fi..." : "Wi-Fi discovery is blocked (port 38441).");
        }
        for (const Discovery::Phone& p : phones) {
            std::string label = p.name + "   " + p.ip;
            if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) connectTo(p.ip, p.port);
        }
        if (ImGui::Button("USB", ImVec2(80, 0))) {
            usbMsg_.clear();
            if (adbForward(usbMsg_)) connectTo("127.0.0.1", proto::kPort);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-80);
        ImGui::InputTextWithHint("##ip", "phone IP", ipBuf_, sizeof(ipBuf_));
        ImGui::SameLine();
        if (ImGui::Button("Connect", ImVec2(-1, 0)) && ipBuf_[0]) connectTo(ipBuf_, proto::kPort);
        if (!usbMsg_.empty()) ImGui::TextWrapped("%s", usbMsg_.c_str());
        if (ImGui::Checkbox("Reconnect automatically", &prefs_.autoConnect)) prefs_.save();
    }

    std::string cameraLabel(const json& c) const {
        std::string facing = jv(c, "facing", std::string("back"));
        facing[0] = (char)toupper(facing[0]);
        char buf[96];
        double focal = jv(c, "focal", 0.0);
        if (focal > 0) snprintf(buf, sizeof(buf), "%s  %.1f mm  (#%s)", facing.c_str(), focal, jv(c, "id", std::string()).c_str());
        else snprintf(buf, sizeof(buf), "%s  (#%s)", facing.c_str(), jv(c, "id", std::string()).c_str());
        return buf;
    }

    void drawCamera() {
        ImGui::SeparatorText("Camera");
        const json& cams = hello_["cameras"];
        std::string cur = !cam_.empty() ? cameraLabel(cam_) : ui_.camera;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##lens", cur.c_str())) {
            for (const json& c : cams) {
                std::string id = jv(c, "id", std::string());
                if (ImGui::Selectable(cameraLabel(c).c_str(), id == ui_.camera) && id != ui_.camera) {
                    ui_.camera = id;
                    control({{"camera", id}});
                }
            }
            ImGui::EndCombo();
        }
        if (cam_.empty()) return;

        // Resolution
        int maxFpsForSize = 30;
        char cur2[64];
        snprintf(cur2, sizeof(cur2), "%d x %d", ui_.width, ui_.height);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##size", cur2)) {
            for (const json& s : cam_["sizes"]) {
                int w = s[0], h = s[1], mf = s[2];
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "%d x %d   (up to %d fps)", w, h, mf);
                if (ImGui::Selectable(lbl, w == ui_.width && h == ui_.height)) {
                    ui_.width = w;
                    ui_.height = h;
                    control({{"width", w}, {"height", h}});
                }
            }
            ImGui::EndCombo();
        }
        for (const json& s : cam_["sizes"])
            if ((int)s[0] == ui_.width && (int)s[1] == ui_.height) maxFpsForSize = s[2];

        // FPS
        ImGui::TextColored(kMuted, "FPS");
        for (int f : {15, 24, 30, 60}) {
            if (f > maxFpsForSize && f != ui_.fps) continue;
            ImGui::SameLine();
            char lbl[8];
            snprintf(lbl, sizeof(lbl), "%d", f);
            if (ImGui::RadioButton(lbl, ui_.fps == f) && ui_.fps != f) {
                ui_.fps = f;
                control({{"fps", f}});
            }
        }

        // Codec
        if (jv(hello_, "hevc", false)) {
            ImGui::TextColored(kMuted, "Codec");
            ImGui::SameLine();
            if (ImGui::RadioButton("H.264", ui_.codec == 0) && ui_.codec != 0) {
                ui_.codec = 0;
                control({{"codec", 0}});
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("HEVC", ui_.codec == 1) && ui_.codec != 1) {
                ui_.codec = 1;
                control({{"codec", 1}});
            }
        }

        // Bitrate
        bool autoRate = ui_.bitrate == 0;
        if (ImGui::Checkbox("Auto bitrate", &autoRate)) {
            ui_.bitrate = autoRate ? 0 : jv(pstate_, "streamBitrate", 8000000);
            control({{"bitrate", ui_.bitrate}});
        }
        if (!autoRate) {
            float mb = ui_.bitrate / 1e6f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##br", &mb, 1.f, 60.f, "%.0f Mbit/s")) ui_.bitrate = (int)(mb * 1e6f);
            if (ImGui::IsItemDeactivatedAfterEdit()) control({{"bitrate", ui_.bitrate}});
        }
    }

    void drawPicture() {
        if (cam_.empty()) return;
        ImGui::SeparatorText("Picture");
        json z = jv(cam_, "zoom", json::array({1.0, 1.0}));
        float zmin = z[0], zmax = z[1];
        if (zmax > zmin + 0.01f) {
            ImGui::TextColored(kMuted, "Zoom");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##zoom", &ui_.zoom, zmin, std::min(zmax, 30.f), "%.1fx", ImGuiSliderFlags_Logarithmic))
                conn_.sendControl({{"zoom", ui_.zoom}});
            if (zmin < 0.99f) {
                if (ImGui::SmallButton("Wide")) {
                    ui_.zoom = zmin;
                    conn_.sendControl({{"zoom", ui_.zoom}});
                }
                ImGui::SameLine();
            }
            for (float preset : {1.f, 2.f, 5.f}) {
                if (preset > zmax) continue;
                char lbl[8];
                snprintf(lbl, sizeof(lbl), "%gx", preset);
                if (ImGui::SmallButton(lbl)) {
                    ui_.zoom = preset;
                    conn_.sendControl({{"zoom", ui_.zoom}});
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }

        ImGui::TextColored(kMuted, "Focus");
        ImGui::SameLine();
        bool fc = false;
        fc |= radio("Auto##af", ui_.af, "auto");
        ImGui::SameLine();
        fc |= radio("Lock##af", ui_.af, "lock");
        if (jv(cam_, "afManual", false)) {
            ImGui::SameLine();
            fc |= radio("Manual##af", ui_.af, "manual");
        }
        if (fc) conn_.sendControl({{"af", ui_.af}});
        if (ui_.af == "manual") {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##focus", &ui_.focus, 0.f, 1.f, ui_.focus < 0.02f ? "far" : "%.2f"))
                conn_.sendControl({{"focus", ui_.focus}});
        }

        json evr = jv(cam_, "ev", json::array({0, 0, 1.0}));
        int evMin = evr[0], evMax = evr[1];
        float evStep = evr[2];
        if (evMax > evMin) {
            ImGui::TextColored(kMuted, "Exposure");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            char fmt[32];
            snprintf(fmt, sizeof(fmt), "%+.1f EV", ui_.ev * evStep);
            if (ImGui::SliderInt("##ev", &ui_.ev, evMin, evMax, fmt)) conn_.sendControl({{"ev", ui_.ev}});
        }
        bool aeLock = ui_.ae == "lock", wbLock = ui_.awb == "lock";
        if (ImGui::Checkbox("Lock exposure", &aeLock)) {
            ui_.ae = aeLock ? "lock" : "auto";
            conn_.sendControl({{"ae", ui_.ae}});
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Lock white balance", &wbLock)) {
            ui_.awb = wbLock ? "lock" : "auto";
            conn_.sendControl({{"awb", ui_.awb}});
        }
        if (jv(cam_, "flash", false) && ImGui::Checkbox("Torch", &ui_.torch)) conn_.sendControl({{"torch", ui_.torch}});
        if (jv(cam_, "stab", false)) {
            if (jv(cam_, "flash", false)) ImGui::SameLine();
            if (ImGui::Checkbox("Stabilization", &ui_.stab)) conn_.sendControl({{"stab", ui_.stab}});
        }
    }

    void drawOutput() {
        ImGui::SeparatorText("Output");
        bool save = false;
        save |= ImGui::Checkbox("Auto-rotate", &prefs_.autoRotate);
        ImGui::SameLine();
        if (ImGui::Button("Rotate 90")) {
            prefs_.extraTurns = (prefs_.extraTurns + 1) & 3;
            save = true;
        }
        save |= ImGui::Checkbox("Mirror", &prefs_.mirror);
        ImGui::SameLine();
        save |= ImGui::Checkbox("Flip", &prefs_.flip);

        const char* cur = "Custom";
        for (const OutputSize& o : kOutputs)
            if (o.w == prefs_.outWidth && o.h == prefs_.outHeight) cur = o.label;
        ImGui::TextColored(kMuted, "Camera size");
        ImGui::SetNextItemWidth(-1);
        bool restart = false;
        if (ImGui::BeginCombo("##out", cur)) {
            for (const OutputSize& o : kOutputs) {
                if (ImGui::Selectable(o.label, o.w == prefs_.outWidth && o.h == prefs_.outHeight)) {
                    prefs_.outWidth = o.w;
                    prefs_.outHeight = o.h;
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
            if (ImGui::RadioButton(lbl, prefs_.outFps == f) && prefs_.outFps != f) {
                prefs_.outFps = f;
                restart = true;
            }
        }
        if (ImGui::Checkbox("Virtual camera \"rng-lens\"", &prefs_.virtualCamera)) restart = true;
        if (ImGui::Checkbox("GPU decoding", &prefs_.hardwareDecode)) save = true;
        if (restart) {
            restartVcam();
            save = true;
        }
        if (save) prefs_.save();
        if (prefs_.virtualCamera) {
            if (!vcamWhy_.empty()) wrapped(kBad, vcamWhy_.c_str());
            else if (vcam_ && vcam_->running())
            {
                if (vcam_->watched()) ImGui::TextColored(kGood, "An app is using the camera (%llu frames sent)", (unsigned long long)vcamSent_);
                else ImGui::TextColored(kMuted, "Ready: pick \"rng-lens\" in Zoom, Discord, OBS...");
            }
        }
    }

    void openAudio() {
        audioWhy_.clear();
        if (!audio_.open(prefs_.audioDevice, audioRate_)) {
            audioWhy_ = "Could not open the sound output";
            if (!prefs_.audioDevice.empty() && audio_.open("", audioRate_)) audioWhy_ = "Saved output is gone, using the default";
        }
        openedRate_ = audioRate_;
    }

    void drawAudio() {
        if (audioRate_ != openedRate_) openAudio(); // stream switched sample rate
        ImGui::SeparatorText("Sound");
        bool save = false;
        if (ImGui::Checkbox("Phone microphone", &prefs_.audio)) {
            conn_.sendControl({{"audio", prefs_.audio}});
            save = true;
        }
        if (prefs_.audio) {
            ImGui::SameLine();
            if (radio("Voice##mic", prefs_.micMode, "voice")) {
                conn_.sendControl({{"mic", prefs_.micMode}});
                save = true;
            }
            ImGui::SameLine();
            if (radio("Raw##mic", prefs_.micMode, "raw")) {
                conn_.sendControl({{"mic", prefs_.micMode}});
                save = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Voice: noise suppression + echo cancel (calls)\nRaw: untouched sound (music, streams)");
            if (!hello_.empty() && !jv(hello_, "mic", true))
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
            // Level meter with a short decay so it reads like a real one.
            meter_ = std::max(audio_.peak(), meter_ * 0.85f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, meter_ > 0.95f ? kBad : kAccent);
            ImGui::ProgressBar(std::min(1.f, meter_), ImVec2(-1, 6), "");
            ImGui::PopStyleColor();
        }
        if (save) prefs_.save();
    }

    void drawStats() {
        ImGui::SeparatorText("Stats");
        if (conn_.state() != Connection::State::Streaming) {
            ImGui::TextColored(kMuted, "Not streaming");
            return;
        }
        ImGui::Text("%d x %d   %.0f fps   %.1f Mbit/s", conn_.streamWidth(), conn_.streamHeight(), fpsShown_, mbps_);
        ImGui::Text("Decode: %s, %.1f ms", decoder_.description().c_str(), decoder_.lastDecodeMs());
        int bat = jv(pstate_, "battery", -1);
        double temp = jv(pstate_, "tempC", 0.0);
        if (bat >= 0) {
            ImGui::Text("Phone: %d%%%s   %.0f C", bat, jv(pstate_, "charging", false) ? " (charging)" : "", temp);
        }
        std::string ac = conn_.audioCodec();
        if (!ac.empty()) ImGui::Text("Sound: %s, %.0f ms buffered, %llu dropouts", ac.c_str(), audio_.bufferedMs(),
                                     (unsigned long long)audio_.underruns());
        int dropped = jv(pstate_, "dropped", 0);
        if (dropped > 0) ImGui::TextColored(kMuted, "Dropped on phone: %d (network too slow)", dropped);
    }

    Prefs& prefs_;
    AppOptions opt_;
    GLFWwindow* win_ = nullptr;
    Decoder decoder_;
    Connection conn_;
    Discovery disc_;
    Renderer renderer_;
    std::unique_ptr<VirtualCamera> vcam_;
    AudioOutput audio_;
    std::vector<AudioOutput::Device> devices_;
    std::string audioWhy_;
    std::atomic<int> audioRate_{48000};
    int openedRate_ = 48000;
    float meter_ = 0.f;
    std::string vcamWhy_, usbMsg_;
    char ipBuf_[64] = {0};

    json hello_ = json::object(), pstate_ = json::object(), cam_ = json::object();
    uint64_t seenHello_ = 0, seenState_ = 0;
    PhoneUi ui_;
    OutputTransform lastT_{-1, false, false};
    int64_t lastRenderMs_ = 0, lastAutoTry_ = 0, rateAt_ = 0;
    bool userIdle_ = false;
    uint64_t framesShown_ = 0, rateFrames_ = 0, rateBytes_ = 0, vcamSent_ = 0;
    double mbps_ = 0, fpsShown_ = 0;
};

}  // namespace

int runApp(Prefs& prefs, const AppOptions& options) {
    App app(prefs, options);
    return app.run();
}
