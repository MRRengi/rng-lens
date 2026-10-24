#pragma once
// Desktop integration: tray icon, global hotkeys, start with Windows. Windows only; elsewhere a no-op.

#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

class Shell {
public:
    enum Command { Show = 1, Quit, ToggleMute, ToggleMirror, ToggleTorch, ToggleBackground, ToggleFrame };

    struct Hotkey {
        const char* keys;
        const char* what;
        bool registered;
    };

    ~Shell();
    // `onCommand` runs on the main thread (from inside glfwWaitEvents).
    bool init(GLFWwindow* window, std::function<void(int)> onCommand);
    void shutdown();
    bool supported() const;

    void setTooltip(const std::string& text);
    void setHotkeysEnabled(bool on);
    const std::vector<Hotkey>& hotkeys() const { return hotkeys_; }

    static bool autostart();
    static void setAutostart(bool on);

    void dispatch(int command);

private:
    std::function<void(int)> onCommand_;
    std::vector<Hotkey> hotkeys_;
    void* hwnd_ = nullptr;
    bool trayAdded_ = false;
};
