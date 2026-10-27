#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imgui_internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fonts {
const unsigned char* find(const char* name, size_t& size);
}

namespace ui {
namespace {

Palette g_p;
Fonts g_f;
float g_s = 1.f;

ImVec4 rgb(int hex, float a = 1.f) {
    return ImVec4(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// Smoothly animated value per widget id (0..1 towards `target`).
float animate(ImGuiID id, float target, float speed = 16.f) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float* v = st->GetFloatRef(id, target);
    float dt = ImGui::GetIO().DeltaTime;
    *v += (target - *v) * std::min(1.f, dt * speed);
    if (std::fabs(target - *v) < 0.001f) *v = target;
    return *v;
}

float itemWidth(float w) { return w > 0 ? w : ImGui::CalcItemWidth(); }

ImFont* addFont(const char* file, float px, const ImWchar* ranges, bool merge, ImVec2 offset = ImVec2(0, 0)) {
    size_t size = 0;
    const unsigned char* data = fonts::find(file, size);
    if (!data) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false; // static data
    cfg.MergeMode = merge;
    cfg.GlyphOffset = offset;
    cfg.OversampleH = 2;
    cfg.PixelSnapH = !merge;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)data, (int)size, px, &cfg, ranges);
}

// Inter (latin + latin-ext + cyrillic) at one weight, with the icons merged in.
ImFont* addText(const char* weight, float px) {
    static const ImWchar latin[] = {0x0020, 0x00FF, 0x2000, 0x206F, 0x20AC, 0x20AC, 0x2190, 0x2193, 0};
    static const ImWchar latinExt[] = {0x0100, 0x024F, 0};
    static const ImWchar cyrillic[] = {0x0400, 0x052F, 0};
    static const ImWchar icons[] = {0xE000, 0xE600, 0};
    char f[64];
    snprintf(f, sizeof(f), "inter-latin-%s.ttf", weight);
    ImFont* font = addFont(f, px, latin, false);
    if (!font) return nullptr;
    snprintf(f, sizeof(f), "inter-latin-ext-%s.ttf", weight);
    addFont(f, px, latinExt, true);
    snprintf(f, sizeof(f), "inter-cyrillic-%s.ttf", weight);
    addFont(f, px, cyrillic, true);
    addFont("lucide.ttf", px * 1.05f, icons, true, ImVec2(0, px * 0.12f));
    return font;
}

struct GroupState {
    ImDrawListSplitter split;
    ImVec2 start;
    float width = 0;
    int rows = 0;
    ImVec2 line;
    bool block = false, open = false;
};
GroupState g_group;

}  // namespace

void loadFonts(float scale) {
    g_s = scale;
    ImGuiIO& io = ImGui::GetIO();
    g_f.body = addText("400", 14.f * scale);
    g_f.strong = addText("600", 14.f * scale);
    g_f.small = addText("400", 12.f * scale);
    g_f.title = addText("600", 20.f * scale);
    static const ImWchar icons[] = {0xE000, 0xE600, 0};
    g_f.icons = addFont("lucide.ttf", 18.f * scale, icons, false);
    g_f.badge = addText("600", 44.f);
    if (!g_f.body) { // fonts not embedded: fall back to ImGui's own
        ImFontConfig cfg;
        cfg.SizePixels = 14.f * scale;
        g_f.body = g_f.strong = g_f.small = g_f.title = g_f.icons = g_f.badge = io.Fonts->AddFontDefault(&cfg);
    }
    io.FontDefault = g_f.body;
}

const Fonts& fonts() { return g_f; }
const Palette& P() { return g_p; }
float S() { return g_s; }
ImU32 col(const ImVec4& c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

bool resolveDark(const std::string& pref) {
    if (pref == "light") return false;
    if (pref == "dark") return true;
    if (const char* e = getenv("RNGLENS_THEME")) return strcmp(e, "light") != 0;
#ifdef _WIN32
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v == 0;
#endif
    return true;
}

void applyTheme(bool dark, float scale) {
    Palette& p = g_p;
    p.dark = dark;
    if (dark) {
        p.window = rgb(0x1E1E1E);
        p.sidebar = rgb(0x262628);
        p.group = rgb(0x2B2B2D);
        p.preview = rgb(0x111112);
        p.separator = ImVec4(1, 1, 1, 0.08f);
        p.label = rgb(0xF2F2F7);
        p.secondary = ImVec4(0.92f, 0.92f, 0.96f, 0.58f);
        p.tertiary = ImVec4(0.92f, 0.92f, 0.96f, 0.30f);
        p.fill = ImVec4(0.47f, 0.47f, 0.50f, 0.32f);
        p.fillHover = ImVec4(0.47f, 0.47f, 0.50f, 0.44f);
        p.fillActive = ImVec4(0.47f, 0.47f, 0.50f, 0.56f);
        p.segment = rgb(0x636366);
        p.knob = rgb(0xFFFFFF);
        p.accent = rgb(0xFF6A3D);
        p.accentHover = rgb(0xFF805A);
        p.accentPressed = rgb(0xE85A2F);
        p.good = rgb(0x32D74B);
        p.warn = rgb(0xFFD60A);
        p.bad = rgb(0xFF453A);
        p.glassTint = ImVec4(0.12f, 0.12f, 0.13f, 0.42f);
        p.glassEdge = ImVec4(1, 1, 1, 0.16f);
        p.shadow = ImVec4(0, 0, 0, 0.45f);
    } else {
        p.window = rgb(0xF2F2F4);
        p.sidebar = rgb(0xE7E7EB);
        p.group = rgb(0xFFFFFF);
        p.preview = rgb(0x1C1C1E);
        p.separator = ImVec4(0, 0, 0, 0.08f);
        p.label = rgb(0x1D1D1F);
        p.secondary = ImVec4(0.24f, 0.24f, 0.26f, 0.62f);
        p.tertiary = ImVec4(0.24f, 0.24f, 0.26f, 0.32f);
        p.fill = ImVec4(0.47f, 0.47f, 0.50f, 0.14f);
        p.fillHover = ImVec4(0.47f, 0.47f, 0.50f, 0.22f);
        p.fillActive = ImVec4(0.47f, 0.47f, 0.50f, 0.30f);
        p.segment = rgb(0xFFFFFF);
        p.knob = rgb(0xFFFFFF);
        p.accent = rgb(0xF2582C);
        p.accentHover = rgb(0xF46B44);
        p.accentPressed = rgb(0xD94C22);
        p.good = rgb(0x248A3D);
        p.warn = rgb(0xA05A00);
        p.bad = rgb(0xD70015);
        p.glassTint = ImVec4(1, 1, 1, 0.50f);
        p.glassEdge = ImVec4(1, 1, 1, 0.55f);
        p.shadow = ImVec4(0, 0, 0, 0.18f);
    }
    p.onAccent = rgb(0xFFFFFF);

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.ScrollbarRounding = 6;
    s.TabRounding = 6;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 9;
    s.GrabMinSize = 10;
    s.SelectableTextAlign = ImVec2(0, 0.5f);
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = p.label;
    c[ImGuiCol_TextDisabled] = p.secondary;
    c[ImGuiCol_WindowBg] = p.window;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = dark ? rgb(0x2C2C2E) : rgb(0xFFFFFF);
    c[ImGuiCol_Border] = p.separator;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.fill;
    c[ImGuiCol_FrameBgHovered] = p.fillHover;
    c[ImGuiCol_FrameBgActive] = p.fillActive;
    c[ImGuiCol_Button] = p.fill;
    c[ImGuiCol_ButtonHovered] = p.fillHover;
    c[ImGuiCol_ButtonActive] = p.fillActive;
    c[ImGuiCol_Header] = mix(p.accent, p.group, 0.75f);
    c[ImGuiCol_HeaderHovered] = p.accent;
    c[ImGuiCol_HeaderActive] = p.accentPressed;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accentHover;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = p.fillHover;
    c[ImGuiCol_ScrollbarGrabHovered] = p.fillActive;
    c[ImGuiCol_ScrollbarGrabActive] = p.fillActive;
    c[ImGuiCol_Separator] = p.separator;
    c[ImGuiCol_PlotLines] = p.accent;
    c[ImGuiCol_PlotHistogram] = p.accent;
    c[ImGuiCol_TextSelectedBg] = mix(p.accent, p.window, 0.6f);
    c[ImGuiCol_NavHighlight] = p.accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.3f);
}

// ---------------------------------------------------------------- text

void text(const char* s, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}
void textSecondary(const char* s) { text(s, g_p.secondary); }
void caption(const char* s, const ImVec4& color) {
    ImGui::PushFont(g_f.small);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
void value(const char* s, const ImVec4& color) {
    float w = ImGui::CalcItemWidth(), fh = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(s);
    float x = std::max(p.x, p.x + w - ts.x);
    ImGui::SetCursorScreenPos(ImVec2(x, p.y + (fh - ts.y) / 2));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}
void title(const char* s) {
    ImGui::PushFont(g_f.title);
    ImGui::TextUnformatted(s);
    ImGui::PopFont();
}

// ---------------------------------------------------------------- controls

bool toggle(const char* id, bool* v) {
    float fh = ImGui::GetFrameHeight();
    float h = 20.f * g_s, w = 36.f * g_s;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, fh));
    if (clicked) {
        *v = !*v;
        ImGui::MarkItemEdited(ImGui::GetItemID());
    }
    float t = animate(ImGui::GetItemID(), *v ? 1.f : 0.f, 18.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(p.x, p.y + (fh - h) / 2), b(p.x + w, p.y + (fh + h) / 2);
    ImVec4 track = mix(ImGui::IsItemHovered() ? g_p.fillHover : g_p.fill, g_p.accent, t);
    dl->AddRectFilled(a, b, col(track), h / 2);
    float r = h / 2 - 2.f * g_s;
    ImVec2 c(a.x + h / 2 + t * (w - h), a.y + h / 2);
    dl->AddCircleFilled(ImVec2(c.x, c.y + 1.f * g_s), r + 0.5f * g_s, col(ImVec4(0, 0, 0, 0.22f)));
    dl->AddCircleFilled(c, r, col(g_p.knob));
    return clicked;
}

bool segmented(const char* id, const char* const* labels, int count, int* sel, float width) {
    float w = itemWidth(width), h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##seg", ImVec2(w, h));
    bool changed = false;
    float seg = w / count;
    if (clicked) {
        int i = std::clamp((int)((ImGui::GetIO().MousePos.x - p.x) / seg), 0, count - 1);
        if (i != *sel) {
            *sel = i;
            changed = true;
            ImGui::MarkItemEdited(ImGui::GetItemID());
        }
    }
    float x = animate(ImGui::GetID("##thumb"), (float)std::max(0, *sel), 20.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = 7.f * g_s, in = 2.f * g_s;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(g_p.fill), r);
    ImVec2 ta(p.x + x * seg + in, p.y + in), tb(p.x + (x + 1) * seg - in, p.y + h - in);
    if (*sel >= 0) {
        if (!g_p.dark) dl->AddRectFilled(ImVec2(ta.x, ta.y + 1 * g_s), ImVec2(tb.x, tb.y + 1 * g_s), col(ImVec4(0, 0, 0, 0.10f)), r - in);
        dl->AddRectFilled(ta, tb, col(g_p.segment), r - in);
    }
    for (int i = 0; i < count; i++) {
        bool on = i == *sel;
        ImFont* f = on ? g_f.strong : g_f.body;
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, labels[i]);
        ImVec2 tp(p.x + i * seg + (seg - ts.x) / 2, p.y + (h - ts.y) / 2);
        dl->AddText(f, f->FontSize, tp, col(on ? g_p.label : mix(g_p.label, g_p.secondary, 0.4f)), labels[i]);
        if (i > 0 && i != *sel && i - 1 != *sel)
            dl->AddLine(ImVec2(p.x + i * seg, p.y + h * 0.28f), ImVec2(p.x + i * seg, p.y + h * 0.72f), col(g_p.separator), 1.f);
    }
    ImGui::PopID();
    return changed;
}

bool slider(const char* id, float* v, float lo, float hi, const char* fmt, bool logarithmic, float width) {
    float w = itemWidth(width), h = ImGui::GetFrameHeight();
    float valueW = fmt ? 56.f * g_s : 0.f;
    float tw = std::max(40.f * g_s, w - valueW);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float knobR = 8.f * g_s, pad = knobR;
    ImGui::InvisibleButton(id, ImVec2(tw, h));
    bool changed = false;
    auto toT = [&](float x) {
        if (logarithmic && lo > 0) return std::log(x / lo) / std::log(hi / lo);
        return (x - lo) / (hi - lo);
    };
    auto fromT = [&](float t) {
        if (logarithmic && lo > 0) return lo * std::pow(hi / lo, t);
        return lo + t * (hi - lo);
    };
    if (ImGui::IsItemActive()) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - p.x - pad) / (tw - 2 * pad), 0.f, 1.f);
        float nv = fromT(t);
        if (nv != *v) {
            *v = nv;
            changed = true;
            ImGui::MarkItemEdited(ImGui::GetItemID());
        }
    }
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    float t = std::clamp(toT(*v), 0.f, 1.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cy = p.y + h / 2, th = 4.f * g_s;
    float x0 = p.x + pad, x1 = p.x + tw - pad, kx = x0 + t * (x1 - x0);
    dl->AddRectFilled(ImVec2(x0, cy - th / 2), ImVec2(x1, cy + th / 2), col(g_p.fill), th / 2);
    // Bipolar ranges fill from the centre, the rest from the left.
    float from = (lo < 0 && hi > 0 && !logarithmic) ? x0 + toT(0.f) * (x1 - x0) : x0;
    dl->AddRectFilled(ImVec2(std::min(from, kx), cy - th / 2), ImVec2(std::max(from, kx), cy + th / 2), col(g_p.accent), th / 2);
    float r = knobR * (active ? 1.08f : hovered ? 1.04f : 1.f);
    dl->AddCircleFilled(ImVec2(kx, cy + 1.f * g_s), r + 0.5f * g_s, col(ImVec4(0, 0, 0, g_p.dark ? 0.35f : 0.18f)));
    dl->AddCircleFilled(ImVec2(kx, cy), r, col(g_p.knob));
    if (!g_p.dark) dl->AddCircle(ImVec2(kx, cy), r, col(ImVec4(0, 0, 0, 0.06f)), 0, 1.f);
    if (fmt) {
        char buf[48];
        snprintf(buf, sizeof(buf), fmt, *v);
        ImFont* f = g_f.body;
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, buf);
        dl->AddText(f, f->FontSize, ImVec2(p.x + w - ts.x, p.y + (h - ts.y) / 2), col(g_p.secondary), buf);
    }
    return changed;
}

bool sliderInt(const char* id, int* v, int lo, int hi, const char* fmt, float width) {
    float f = (float)*v;
    // Integer value shown through a caller-provided fmt that expects a float (e.g. "%+.1f EV" pre-scaled).
    bool ch = slider(id, &f, (float)lo, (float)hi, nullptr, false, width > 0 ? width : itemWidth(width) - (fmt ? 56.f * g_s : 0.f));
    int nv = (int)std::lround(f);
    if (fmt) {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        float w = itemWidth(width);
        ImFont* fo = g_f.body;
        ImVec2 ts = fo->CalcTextSizeA(fo->FontSize, FLT_MAX, 0, fmt);
        ImGui::GetWindowDrawList()->AddText(fo, fo->FontSize, ImVec2(mn.x + w - ts.x, mn.y + (mx.y - mn.y - ts.y) / 2),
                                            col(g_p.secondary), fmt);
    }
    if (nv != *v) {
        *v = nv;
        return true;
    }
    (void)ch;
    return false;
}

bool button(const char* label, bool primary, float width) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, g_p.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_p.accentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_p.accentPressed);
        ImGui::PushStyleColor(ImGuiCol_Text, g_p.onAccent);
    }
    ImGui::PushFont(g_f.strong);
    bool r = ImGui::Button(label, ImVec2(width < 0 ? ImGui::CalcItemWidth() : width, 0));
    ImGui::PopFont();
    if (primary) ImGui::PopStyleColor(4);
    return r;
}

bool beginPopupButton(const char* id, const char* preview, float width) {
    float w = itemWidth(width);
    ImGui::SetNextItemWidth(w);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList(); // the parent's, also after the popup opens
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * g_s, 6 * g_s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * g_s));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyle().Colors[ImGuiCol_PopupBg]);
    bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    // chevron in its own small well, like a macOS pop-up button
    float h = ImGui::GetFrameHeight(), cw = h - 6 * g_s;
    ImVec2 a(p.x + w - cw - 3 * g_s, p.y + 3 * g_s), b(p.x + w - 3 * g_s, p.y + h - 3 * g_s);
    {
        dl->AddRectFilled(a, b, col(g_p.accent), 5 * g_s);
        ImFont* f = g_f.small;
        ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, icon::chevronDown);
        dl->AddText(f, f->FontSize, ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2), col(g_p.onAccent), icon::chevronDown);
    }
    return open;
}

bool option(const char* label, bool selected) {
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * g_s, 5 * g_s));
    float h = ImGui::GetTextLineHeight() + 10 * g_s;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = ImGui::Selectable("##opt", false, 0, ImVec2(0, h));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 tc = hov ? g_p.onAccent : g_p.label;
    if (selected) dl->AddText(g_f.body, g_f.body->FontSize, ImVec2(p.x + 6 * g_s, p.y + (h - g_f.body->FontSize) / 2), col(tc), icon::check);
    dl->AddText(g_f.body, g_f.body->FontSize, ImVec2(p.x + 26 * g_s, p.y + (h - g_f.body->FontSize) / 2), col(tc), label);
    ImGui::PopStyleVar();
    ImGui::PopID();
    return r;
}

void meter(float v, const ImVec4& color, float width) {
    float w = itemWidth(width), h = 6.f * g_s;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fh = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(w, fh));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = p.y + (fh - h) / 2;
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), col(g_p.fill), h / 2);
    if (v > 0.002f) dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * std::min(1.f, v), y + h), col(color), h / 2);
}

// ---------------------------------------------------------------- grouped form

static float padX() { return 14.f * g_s; }
// PushTextWrapPos wants window-local x.
static void wrapAt(float screenX) { ImGui::PushTextWrapPos(screenX - ImGui::GetWindowPos().x + ImGui::GetScrollX()); }
static float padY() { return 8.f * g_s; }

void beginGroup(const char* header) {
    GroupState& g = g_group;
    if (header) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4 * g_s);
        ImGui::PushFont(g_f.strong);
        ImGui::TextUnformatted(header);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 0));
    }
    g.start = ImGui::GetCursorScreenPos();
    g.width = ImGui::GetContentRegionAvail().x;
    g.rows = 0;
    g.open = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    g.split.Split(dl, 2);
    g.split.SetCurrentChannel(dl, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * g_s, 0));
    ImGui::BeginGroup();
}

static void rowStart() {
    GroupState& g = g_group;
    ImVec2 c = ImGui::GetCursorScreenPos();
    if (g.rows > 0)
        ImGui::GetWindowDrawList()->AddLine(ImVec2(g.start.x + padX(), c.y), ImVec2(g.start.x + g.width - padX(), c.y),
                                            col(g_p.separator), 1.f);
    ImGui::Dummy(ImVec2(g.width, padY()));
    g.line = ImGui::GetCursorScreenPos();
}

void row(const char* label, float controlWidth) {
    GroupState& g = g_group;
    rowStart();
    g.block = false;
    float fh = ImGui::GetFrameHeight();
    float cw = controlWidth > 0 ? controlWidth : (g.width - 2 * padX()) * 0.58f;
    ImGui::SetCursorScreenPos(ImVec2(g.line.x + padX(), g.line.y + (fh - ImGui::GetTextLineHeight()) / 2));
    wrapAt(g.start.x + g.width - padX() - cw - 8 * g_s);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(g.start.x + g.width - padX() - cw, g.line.y));
    ImGui::PushItemWidth(cw);
}

void rowBlock() {
    GroupState& g = g_group;
    rowStart();
    g.block = true;
    ImGui::SetCursorScreenPos(ImVec2(g.line.x + padX(), g.line.y));
    ImGui::PushItemWidth(g.width - 2 * padX());
    wrapAt(g.start.x + g.width - padX());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * g_s, 6 * g_s));
    ImGui::BeginGroup();
}

void rowEnd() {
    GroupState& g = g_group;
    if (g.block) {
        ImGui::EndGroup();
        ImGui::PopStyleVar();
        ImGui::PopTextWrapPos();
    }
    ImGui::PopItemWidth();
    float bottom = std::max(ImGui::GetItemRectMax().y, g.line.y + (g.block ? 0.f : ImGui::GetFrameHeight()));
    ImGui::SetCursorScreenPos(ImVec2(g.start.x, bottom));
    ImGui::Dummy(ImVec2(g.width, padY()));
    g.rows++;
}

void endGroup(const char* footer) {
    GroupState& g = g_group;
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    g.split.SetCurrentChannel(dl, 0);
    ImVec2 a = g.start, b(g.start.x + g.width, mx.y);
    dl->AddRectFilled(a, b, col(g_p.group), 10.f * g_s);
    dl->AddRect(a, b, col(g_p.dark ? ImVec4(1, 1, 1, 0.05f) : ImVec4(0, 0, 0, 0.05f)), 10.f * g_s, 0, 1.f);
    g.split.Merge(dl);
    g.open = false;
    if (footer) {
        ImGui::Dummy(ImVec2(0, 2 * g_s));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4 * g_s);
        wrapAt(g.start.x + g.width - 4 * g_s);
        caption(footer, g_p.secondary);
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0, 12.f * g_s));
}

// ---------------------------------------------------------------- sidebar and glass

bool sidebarItem(const char* ic, const char* label, bool selected, const char* badge) {
    float w = ImGui::GetContentRegionAvail().x, h = 30.f * g_s;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##side", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(g_p.accent), 7.f * g_s);
    else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(g_p.fill), 7.f * g_s);
    ImFont* f = g_f.body;
    float ty = p.y + (h - f->FontSize) / 2;
    dl->AddText(f, f->FontSize * 1.1f, ImVec2(p.x + 10 * g_s, ty - f->FontSize * 0.05f), col(selected ? g_p.onAccent : g_p.accent), ic);
    dl->AddText(f, f->FontSize, ImVec2(p.x + 36 * g_s, ty), col(selected ? g_p.onAccent : g_p.label), label);
    if (badge) {
        ImVec2 ts = g_f.small->CalcTextSizeA(g_f.small->FontSize, FLT_MAX, 0, badge);
        dl->AddText(g_f.small, g_f.small->FontSize, ImVec2(p.x + w - ts.x - 10 * g_s, p.y + (h - ts.y) / 2),
                    col(selected ? g_p.onAccent : g_p.secondary, selected ? 0.85f : 1.f), badge);
    }
    return clicked;
}

void glassPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, unsigned int tex, ImVec2 picMin, ImVec2 picMax, float r) {
    // Soft shadow
    for (int i = 3; i >= 1; i--) {
        float o = i * 3.f * g_s;
        dl->AddRectFilled(ImVec2(a.x - o * 0.3f, a.y + o * 0.5f), ImVec2(b.x + o * 0.3f, b.y + o),
                          col(ImVec4(0, 0, 0, g_p.shadow.w * 0.18f)), r + o);
    }
    if (tex && picMax.x > picMin.x && picMax.y > picMin.y) {
        ImVec2 uv0((a.x - picMin.x) / (picMax.x - picMin.x), (a.y - picMin.y) / (picMax.y - picMin.y));
        ImVec2 uv1((b.x - picMin.x) / (picMax.x - picMin.x), (b.y - picMin.y) / (picMax.y - picMin.y));
        dl->AddImageRounded((ImTextureID)(intptr_t)tex, a, b, uv0, uv1, IM_COL32_WHITE, r);
    }
    dl->AddRectFilled(a, b, col(g_p.glassTint), r);
    // Light from the top edge, like the system glass.
    dl->AddRect(a, b, col(g_p.glassEdge), r, 0, 1.f);
    dl->AddLine(ImVec2(a.x + r, a.y + 1.f), ImVec2(b.x - r, a.y + 1.f), col(ImVec4(1, 1, 1, g_p.dark ? 0.18f : 0.7f)), 1.f);
}

bool glassButton(const char* ic, const char* tooltip, bool active, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(tooltip);
    bool clicked = ImGui::InvisibleButton("##gb", ImVec2(size, size));
    bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + size / 2, p.y + size / 2);
    float t = animate(ImGui::GetID(tooltip), active ? 1.f : 0.f, 18.f);
    if (t > 0.01f) dl->AddCircleFilled(c, size / 2, col(g_p.accent, t));
    if (hov && !active) dl->AddCircleFilled(c, size / 2, col(g_p.dark ? ImVec4(1, 1, 1, held ? 0.22f : 0.12f) : ImVec4(0, 0, 0, held ? 0.14f : 0.07f)));
    ImFont* f = g_f.icons;
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, ic);
    ImVec4 ic_col = active ? g_p.onAccent : (g_p.dark ? g_p.label : ImVec4(0.11f, 0.11f, 0.12f, 1.f));
    dl->AddText(f, f->FontSize, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col(ic_col), ic);
    if (hov && tooltip) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * g_s, 5 * g_s));
        ImGui::SetTooltip("%s", tooltip);
        ImGui::PopStyleVar();
    }
    return clicked;
}

}  // namespace ui
