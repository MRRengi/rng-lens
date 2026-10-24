// Windows virtual camera: loads rnglens_vcam.dll (rebranded Softcam) from next to the exe.
#ifdef _WIN32
#include <windows.h>

#include "platform.h"
#include "vcam.h"

namespace {

// Must match third_party/softcam/softcam/softcam.cpp
const char* kClsidKey[2] = {"CLSID\\{ADB68203-9BA3-43EC-92B9-8328DFA6B196}\\InprocServer32",
                            "CLSID\\{D4EA6B25-9840-4EAD-8926-436701254668}\\InprocServer32"};
const char* kDll[2] = {"rnglens_vcam.dll", "rnglens_vcam2.dll"};

using CreateFn = void*(__cdecl*)(int, int, float);
using DeleteFn = void(__cdecl*)(void*);
using SendFn = void(__cdecl*)(void*, const void*);
using ConnectedFn = bool(__cdecl*)(void*);

class SoftcamCamera : public VirtualCamera {
public:
    explicit SoftcamCamera(int index) : index_(index & 1) {}
    ~SoftcamCamera() override {
        stop();
        if (dll_) FreeLibrary(dll_);
    }

    bool available(std::string& why) override {
        if (!load()) {
            why = std::string(kDll[index_]) + " is missing next to rng-lens.exe";
            return false;
        }
        HKEY key;
        if (RegOpenKeyExA(HKEY_CLASSES_ROOT, kClsidKey[index_], 0, KEY_READ, &key) != ERROR_SUCCESS) {
            why = "The virtual camera is not registered: reinstall rng-lens (rng-lens-setup.exe)";
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
        dll_ = LoadLibraryA((plat::exeDir() + kDll[index_]).c_str());
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

    int index_ = 0;
    HMODULE dll_ = nullptr;
    void* cam_ = nullptr;
    CreateFn create_ = nullptr;
    DeleteFn delete_ = nullptr;
    SendFn send_ = nullptr;
    ConnectedFn connected_ = nullptr;
};

}  // namespace

std::unique_ptr<VirtualCamera> VirtualCamera::create(int index) { return std::make_unique<SoftcamCamera>(index); }
#endif
