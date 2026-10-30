#include "shell.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "platform.h"

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kShowMsg = WM_APP + 2; // sent by a second rng-lens started while this one runs
constexpr UINT kTrayId = 1;
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

struct KeyDef {
    int id;
    UINT mods, vk;
    const char* keys;
    const char* what;
};
const KeyDef kKeys[] = {
    {Shell::ToggleMute, MOD_CONTROL | MOD_ALT, 'M', "Ctrl+Alt+M", "mute the phone mic"},
    {Shell::ToggleMirror, MOD_CONTROL | MOD_ALT, 'R', "Ctrl+Alt+R", "mirror"},
    {Shell::ToggleTorch, MOD_CONTROL | MOD_ALT, 'T', "Ctrl+Alt+T", "torch"},
    {Shell::ToggleBackground, MOD_CONTROL | MOD_ALT, 'B', "Ctrl+Alt+B", "background blur on/off"},
    {Shell::ToggleFrame, MOD_CONTROL | MOD_ALT, 'F', "Ctrl+Alt+F", "auto-frame on/off"},
    {Shell::TogglePause, MOD_CONTROL | MOD_ALT, 'P', "Ctrl+Alt+P", "pause the camera (Be right back)"},
};

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Shell* self = (Shell*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_HOTKEY) {
        self->dispatch((int)wp);
        return 0;
    }
    if (msg == kShowMsg) {
        self->dispatch(Shell::Show);
        return 0;
    }
    if (msg == kTrayMsg) {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            self->dispatch(Shell::Show);
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, Shell::Show, L"Open rng-lens");
            AppendMenuW(m, MF_STRING, Shell::ToggleMute, L"Mute / unmute mic");
            AppendMenuW(m, MF_STRING, Shell::ToggleMirror, L"Mirror");
            AppendMenuW(m, MF_STRING, Shell::TogglePause, L"Pause / resume camera");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, Shell::Quit, L"Quit (turns the camera off)");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(h); // so the menu closes when clicking elsewhere
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(m);
            PostMessageW(h, WM_NULL, 0, 0);
            if (cmd) self->dispatch(cmd);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

Shell::~Shell() { shutdown(); }

bool Shell::supported() const { return true; }

bool Shell::alreadyRunning() {
    // Kept open for the life of the process; the OS releases it when we exit.
    HANDLE m = CreateMutexW(nullptr, FALSE, L"Local\\rng-lens-single-instance");
    if (!m || GetLastError() != ERROR_ALREADY_EXISTS) return false;
    for (int i = 0; i < 20; i++) { // the first one may still be starting up
        if (HWND h = FindWindowW(L"rng-lens-shell", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(h, kShowMsg, 0, 0);
            break;
        }
        Sleep(100);
    }
    return true;
}

bool Shell::init(GLFWwindow*, std::function<void(int)> onCommand) {
    onCommand_ = std::move(onCommand);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"rng-lens-shell";
    RegisterClassW(&wc);
    // A never-shown popup window: owns the tray icon, the tray menu and the hotkeys.
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"rng-lens", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!h) return false;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)this);
    hwnd_ = h;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = h;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = LoadIconW(inst, L"GLFW_ICON");
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcscpy(nid.szTip, L"rng-lens");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return true;
}

void Shell::setHotkeysEnabled(bool on) {
    HWND h = (HWND)hwnd_;
    if (!h) return;
    for (const KeyDef& k : kKeys) UnregisterHotKey(h, k.id);
    hotkeys_.clear();
    for (const KeyDef& k : kKeys) {
        bool ok = on && RegisterHotKey(h, k.id, k.mods | 0x4000 /* MOD_NOREPEAT */, k.vk);
        hotkeys_.push_back({k.keys, k.what, ok});
    }
}

void Shell::setTooltip(const std::string& text) {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = (HWND)hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nid.szTip, (int)(sizeof(nid.szTip) / sizeof(wchar_t)) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Shell::shutdown() {
    HWND h = (HWND)hwnd_;
    if (!h) return;
    for (const KeyDef& k : kKeys) UnregisterHotKey(h, k.id);
    if (trayAdded_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = h;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded_ = false;
    }
    DestroyWindow(h);
    hwnd_ = nullptr;
}

void Shell::dispatch(int command) {
    if (onCommand_) onCommand_(command);
}

bool Shell::autostart() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool found = RegQueryValueExW(k, L"rng-lens", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return found;
}

void Shell::setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(k, L"rng-lens", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"rng-lens");
    }
    RegCloseKey(k);
}

#else  // ------------------------------------------------------------------ other platforms

Shell::~Shell() {}
bool Shell::init(GLFWwindow*, std::function<void(int)> onCommand) {
    onCommand_ = std::move(onCommand);
    return false;
}
void Shell::shutdown() {}
bool Shell::supported() const { return false; }
void Shell::setTooltip(const std::string&) {}
void Shell::setHotkeysEnabled(bool) {}
bool Shell::alreadyRunning() { return false; }
bool Shell::autostart() { return false; }
void Shell::setAutostart(bool) {}
void Shell::dispatch(int command) {
    if (onCommand_) onCommand_(command);
}

#endif
