#pragma once
struct Prefs;
// Runs the desktop window until it is closed. Returns the process exit code.
#include <string>
struct AppOptions {
    std::string connectHost; // connect right away (also handy for a desktop shortcut)
    int connectPort = 38440;
    std::string dumpOutput;  // bench: after `dumpAfter` frames, write what the virtual camera gets as PNG and quit
    int dumpAfter = 30;
};
int runApp(Prefs& prefs, const AppOptions& options);
