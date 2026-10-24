#pragma once
#include <string>
struct Prefs;
struct AppOptions {
    std::string connectHost; // connect right away (also handy for a desktop shortcut)
    int connectPort = 38440;
    bool startHidden = false; // --tray: start in the tray (used by "start with Windows")
    std::string dumpOutput;  // bench: after `dumpAfter` frames, write what the virtual camera gets as PNG and quit
    int dumpAfter = 30;
};
// Runs the desktop window until it is closed. Returns the process exit code.
int runApp(Prefs& prefs, const AppOptions& options);
