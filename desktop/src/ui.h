#pragma once
// rng-lens design system: macOS-style look on Dear ImGui.
// Inter (OFL) for text, Lucide (ISC) for icons, light and dark palettes that follow the Windows theme,
// the brand orange as the accent, and Apple-style controls: switch, segmented control, slider,
// grouped "inset" cards with rows, sidebar items and a frosted-glass toolbar.

#include <string>

#include "imgui.h"

namespace ui {

// Lucide icons (font range U+E000..). Use inside any text: ui::text(icon::video).
namespace icon {
constexpr const char* video = "\xee\x86\xa5";
constexpr const char* sparkles = "\xee\x90\x97";
constexpr const char* monitor = "\xee\x84\xa1";
constexpr const char* mic = "\xee\x84\x9c";
constexpr const char* micOff = "\xee\x84\x9d";
constexpr const char* settings = "\xee\x85\x98";
constexpr const char* activity = "\xee\x80\xb8";
constexpr const char* phone = "\xee\x85\xa7";
constexpr const char* battery = "\xee\x81\x97";
constexpr const char* batteryCharging = "\xee\x81\x98";
constexpr const char* batteryLow = "\xee\x81\x9a";
constexpr const char* thermometer = "\xee\x86\x86";
constexpr const char* mirror = "\xee\x8d\xa2";
constexpr const char* scanFace = "\xee\x8d\xb5";
constexpr const char* focus = "\xee\x8a\x9e";
constexpr const char* image = "\xee\x83\xb9";
constexpr const char* torch = "\xee\x83\x98";
constexpr const char* rotate = "\xee\x85\x8d";
constexpr const char* wifi = "\xee\x86\xae";
constexpr const char* usb = "\xee\x8d\x9a";
constexpr const char* unplug = "\xee\x91\xa2";
constexpr const char* chevronDown = "\xee\x81\xb1";
constexpr const char* check = "\xee\x81\xb0";
constexpr const char* x = "\xee\x86\xb2";
constexpr const char* link = "\xee\x84\x88";
constexpr const char* zap = "\xee\x86\xb4";
constexpr const char* sun = "\xee\x85\xbc";
constexpr const char* moon = "\xee\x84\xa2";
constexpr const char* info = "\xee\x83\xbe";
constexpr const char* alert = "\xee\x86\x93";
constexpr const char* ok = "\xee\x88\xa6";
constexpr const char* gauge = "\xee\x86\xbf";
constexpr const char* scan = "\xee\x89\x97";
constexpr const char* eye = "\xee\x82\xbe";
constexpr const char* camera = "\xee\x81\xa8";
constexpr const char* power = "\xee\x85\x84";
constexpr const char* refresh = "\xee\x85\x89";
constexpr const char* hand = "\xee\x87\x97";
constexpr const char* pause = "\xee\x84\xb2";
constexpr const char* play = "\xee\x85\x80";
constexpr const char* download = "\xee\x82\xb6";
constexpr const char* wifiOff = "\xee\x86\xaf";
constexpr const char* sunMedium = "\xee\x8a\xb1";
}  // namespace icon

struct Palette {
    bool dark = true;
    ImVec4 window, sidebar, group, preview, separator;
    ImVec4 label, secondary, tertiary;
    ImVec4 fill, fillHover, fillActive, segment, knob;
    ImVec4 accent, accentHover, accentPressed, onAccent;
    ImVec4 good, warn, bad;
    ImVec4 glassTint, glassEdge, shadow;
};

struct Fonts {
    ImFont* body = nullptr;   // 14 regular (+ icons)
    ImFont* strong = nullptr; // 14 semibold (+ icons)
    ImFont* small = nullptr;  // 12 regular (+ icons)
    ImFont* title = nullptr;  // 20 semibold (+ icons)
    ImFont* icons = nullptr;  // 18 icons only, for toolbar buttons
    ImFont* badge = nullptr;  // 44 semibold, for text drawn into the camera picture
};

void loadFonts(float scale);
const Fonts& fonts();

// "system" | "light" | "dark". System = the Windows "apps" theme (re-read every couple of seconds).
bool resolveDark(const std::string& pref);
void applyTheme(bool dark, float scale);
const Palette& P();
float S(); // UI scale (DPI)
ImU32 col(const ImVec4& c, float alphaMul = 1.f);

// ---- text
void text(const char* s, const ImVec4& color);
void textSecondary(const char* s);
void caption(const char* s, const ImVec4& color); // small, wraps
void title(const char* s);
void value(const char* s, const ImVec4& color); // read-only value in a row: right-aligned, centred on the row

// ---- controls (width -1 = current item width)
bool toggle(const char* id, bool* v);
bool segmented(const char* id, const char* const* labels, int count, int* sel, float width = -1.f);
bool slider(const char* id, float* v, float lo, float hi, const char* fmt, bool logarithmic = false, float width = -1.f);
bool sliderInt(const char* id, int* v, int lo, int hi, const char* fmt, float width = -1.f);
bool button(const char* label, bool primary = false, float width = 0.f);
bool beginPopupButton(const char* id, const char* preview, float width = -1.f); // macOS pop-up button; EndCombo() after
bool option(const char* label, bool selected);                                  // item in a pop-up button
void meter(float v, const ImVec4& color, float width = -1.f);

// ---- grouped form (System Settings style)
void beginGroup(const char* header = nullptr);
void row(const char* label, float controlWidth = -1.f); // control on the right; -1 = 58% of the row
void rowBlock();                                        // full-width content (captions, lists)
void rowEnd();
void endGroup(const char* footer = nullptr);

// ---- sidebar and glass
bool sidebarItem(const char* icon, const char* label, bool selected, const char* badge = nullptr);
// Frosted glass capsule over the preview. `glassTex` is the blurred copy of the picture shown in `picMin..picMax`.
void glassPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, unsigned int glassTex, ImVec2 picMin, ImVec2 picMax, float rounding);
bool glassButton(const char* icon, const char* tooltip, bool active, float size);

}  // namespace ui
