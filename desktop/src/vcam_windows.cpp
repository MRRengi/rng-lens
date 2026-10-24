// Windows virtual camera: loads rnglens_vcam.dll (rebranded Softcam) from next to the exe.
#ifdef _WIN32
#include <windows.h>

#include "platform.h"
#include "vcam.h"

namespace {

// Must match third_party/softcam/softcam/softcam.cpp
const char* kClsidKey = "CLSID\\{ADB68203-9BA3-43EC-92B9-8328DFA6B196}\\InprocServer32";

using CreateFn = void*(__cdecl*)(int, int, float);
using DeleteFn = void(__cdecl*)(void*);
using SendFn = void(__cdecl*)(void*, const void*);
using ConnectedFn = bool(__cdecl*)(void*);

class SoftcamCamera : public VirtualCamera {
public:
    ~SoftcamCamera() override {
        stop();
        if (dll_) FreeLibrary(dll_);
    }

    bool available(std::string& why) override {
        if (!load()) {
            why = "rnglens_vcam.dll is missing next to rng-lens.exe";
            return false;
        }
        HKEY key;
        if (RegOpenKeyExA(HKEY_CLASSES_ROOT, kClsidKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            why = "Virtual camera is not installed yet: run install-camera.bat once (as administrator)";
            return false;
        }
        RegCloseKey(key);
        return true;
    }

    bool start(int width, int height, int fps) override {
        stop();
        if (!load()) return false;
        cam_ = create_(width, height, (float)fps);
        return cam_ != nullptr;
    }

    void stop() override {
        if (cam_) {
            delete_(cam_);
            cam_ = nullptr;
        }
    }

    bool running() const override { return cam_ != nullptr; }

    bool watched() override { return cam_ && connected_(cam_); }

    void send(const uint8_t* bgr) override {
        if (cam_) send_(cam_, bgr);
    }

private:
    bool load() {
        if (dll_) return true;
        dll_ = LoadLibraryA((plat::exeDir() + "rnglens_vcam.dll").c_str());
        if (!dll_) return false;
        create_ = (CreateFn)GetProcAddress(dll_, "scCreateCamera");
        delete_ = (DeleteFn)GetProcAddress(dll_, "scDeleteCamera");
        send_ = (SendFn)GetProcAddress(dll_, "scSendFrame");
        connected_ = (ConnectedFn)GetProcAddress(dll_, "scIsConnected");
        if (!create_ || !delete_ || !send_ || !connected_) {
            FreeLibrary(dll_);
            dll_ = nullptr;
            return false;
        }
        return true;
    }

    HMODULE dll_ = nullptr;
    void* cam_ = nullptr;
    CreateFn create_ = nullptr;
    DeleteFn delete_ = nullptr;
    SendFn send_ = nullptr;
    ConnectedFn connected_ = nullptr;
};

}  // namespace

std::unique_ptr<VirtualCamera> VirtualCamera::create() { return std::make_unique<SoftcamCamera>(); }
#endif
