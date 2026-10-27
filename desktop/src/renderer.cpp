#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "stb_image.h"

static constexpr float Vision_kMaskSize = 192.f; // person mask resolution (vision.h kMaskSize)

// Convention everywhere: texture y = 0 is the top of the picture, and so is framebuffer row 0.

static const char* kVertex = R"(#version 330 core
uniform vec4 uRect; // x0, y0, x1, y1 in NDC
out vec2 vUv;
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vUv = c;
    gl_Position = vec4(mix(uRect.xy, uRect.zw, c), 0.0, 1.0);
}
)";

// YUV -> RGB, rotated upright.
static const char* kConvert = R"(#version 330 core
in vec2 vUv; out vec4 o;
uniform sampler2D texY, texU, texV;
uniform int uTurns; uniform bool uNv12; uniform mat3 uMat; uniform vec3 uOff;
void main() {
    vec2 p = vUv, s = p;
    if (uTurns == 1) s = vec2(p.y, 1.0 - p.x);
    else if (uTurns == 2) s = vec2(1.0 - p.x, 1.0 - p.y);
    else if (uTurns == 3) s = vec2(1.0 - p.y, p.x);
    float y = texture(texY, s).r;
    vec2 uv = uNv12 ? texture(texU, s).rg : vec2(texture(texU, s).r, texture(texV, s).r);
    o = vec4(clamp(uMat * (vec3(y, uv) - uOff), 0.0, 1.0), 1.0);
}
)";

// Night mode: blend into the running average, but follow quickly wherever the picture really changed.
static const char* kNight = R"(#version 330 core
in vec2 vUv; out vec4 o;
uniform sampler2D cur, hist; uniform vec2 texel; uniform float strength;
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float box(sampler2D t) { // 3x3 luma average, 2 px apart: noise averages out, real motion doesn't
    float s = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) s += lum(texture(t, vUv + vec2(x, y) * texel * 2.0).rgb);
    return s / 9.0;
}
void main() {
    vec3 c = texture(cur, vUv).rgb, h = texture(hist, vUv).rgb;
    float motion = abs(box(cur) - box(hist));
    float keep = mix(0.0, 0.9, strength);               // how much history a still pixel keeps
    float a = clamp((1.0 - keep) + smoothstep(0.035, 0.11, motion), 0.0, 1.0);
    o = vec4(mix(h, c, a), 1.0);
}
)";

static const char* kDown = R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D src; uniform vec2 texel;
void main() { // 4 bilinear taps = 16 texels: a clean 4x reduction
    o = 0.25 * (texture(src, vUv + texel * vec2(-1, -1)) + texture(src, vUv + texel * vec2(1, -1))
              + texture(src, vUv + texel * vec2(-1, 1)) + texture(src, vUv + texel * vec2(1, 1)));
}
)";

static const char* kGauss = R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D src; uniform vec2 dir;
void main() { // 9-tap gaussian using linear sampling (5 fetches)
    vec4 c = texture(src, vUv) * 0.2270270270;
    c += (texture(src, vUv + dir * 1.3846153846) + texture(src, vUv - dir * 1.3846153846)) * 0.3162162162;
    c += (texture(src, vUv + dir * 3.2307692308) + texture(src, vUv - dir * 3.2307692308)) * 0.0702702703;
    o = c;
}
)";

// Background: refine the low-res person mask against the full-res picture (joint bilateral), then mix.
static const char* kComposite = R"(#version 330 core
in vec2 vUv; out vec4 o;
uniform sampler2D cur, back, mask; uniform vec2 maskTexel; uniform bool useImage; uniform vec4 imgRect;
void main() {
    vec3 c = texture(cur, vUv).rgb;
    float sum = 0.0, wsum = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 uv = vUv + vec2(x, y) * maskTexel;
            vec3 n = texture(cur, uv).rgb;
            float w = exp(-dot(n - c, n - c) * 40.0) * exp(-float(x * x + y * y) * 0.5);
            sum += texture(mask, uv).r * w;
            wsum += w;
        }
    float m = smoothstep(0.3, 0.7, sum / max(wsum, 1e-4));
    vec3 b = useImage ? texture(back, imgRect.xy + vUv * imgRect.zw).rgb : texture(back, vUv).rgb;
    o = vec4(mix(b, c, m), 1.0);
}
)";

// Skin smoothing: edge-preserving blur, only where the colour looks like skin.
static const char* kSmooth = R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D src; uniform vec2 texel; uniform float strength;
void main() {
    vec3 c = texture(src, vUv).rgb;
    // Local average first: both the skin test and the edge weights use it, so grain can't fool them.
    vec3 k = c;
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.7853982;
        k += texture(src, vUv + vec2(cos(a), sin(a)) * texel * 2.0).rgb;
    }
    k /= 9.0;
    vec3 sum = c; float wsum = 1.0;
    for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.7853982 + 0.3926991;
        for (int r = 1; r <= 3; r++) {
            vec2 off = vec2(cos(a + float(r)), sin(a + float(r))) * texel * float(r) * 3.0;
            vec3 n = texture(src, vUv + off).rgb;
            float w = exp(-dot(n - k, n - k) * 45.0);
            sum += n * w; wsum += w;
        }
    }
    vec3 smoothed = sum / max(wsum, 1e-4);
    float Y = dot(k, vec3(0.299, 0.587, 0.114));
    float cb = 0.5 + (k.b - Y) * 0.564, cr = 0.5 + (k.r - Y) * 0.713;
    float skin = smoothstep(0.0, 0.03, 0.09 - abs(cb - 0.43)) * smoothstep(0.0, 0.03, 0.07 - abs(cr - 0.585));
    o = vec4(mix(c, smoothed, skin * strength), 1.0);
}
)";

// Sharpen, then brightness / contrast / saturation / warmth, then a look.
static const char* kColor = R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D src; uniform vec2 texel;
uniform float sharpen, brightness, contrast, saturation, warmth, lookAmount; uniform int look;
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 sat(vec3 c, float s) { return mix(vec3(luma(c)), c, s); }
vec3 applyLook(vec3 c) {
    if (look == 1) return sat(c * vec3(1.06, 1.0, 0.90), 1.05);                       // warm
    if (look == 2) return c * vec3(0.93, 1.0, 1.07);                                   // cool
    if (look == 3) {                                                                    // film
        c = 0.04 + c * 0.94; c = c * c * (3.0 - 2.0 * c) * 0.35 + c * 0.65;
        float l = luma(c);
        return sat(c + mix(vec3(-0.02, 0.01, 0.04), vec3(0.04, 0.01, -0.03), l), 0.9);
    }
    if (look == 4) return clamp(sat((c - 0.5) * 1.1 + 0.5, 1.35), 0.0, 1.0);          // vivid
    if (look == 5) return sat(0.08 + c * 0.86, 0.75);                                  // fade
    if (look == 6) { float l = luma(c); l = clamp((l - 0.5) * 1.12 + 0.5, 0.0, 1.0); return vec3(l); } // b&w
    return c;
}
void main() {
    vec3 c = texture(src, vUv).rgb;
    if (sharpen > 0.0) {
        vec3 b = (texture(src, vUv + vec2(texel.x, 0)).rgb + texture(src, vUv - vec2(texel.x, 0)).rgb
                + texture(src, vUv + vec2(0, texel.y)).rgb + texture(src, vUv - vec2(0, texel.y)).rgb) * 0.25;
        c = clamp(c + (c - b) * sharpen * 2.0, 0.0, 1.0);
    }
    c += brightness * 0.25;
    c = (c - 0.5) * (1.0 + contrast * 0.6) + 0.5;
    c = sat(c, 1.0 + saturation);
    c *= vec3(1.0 + warmth * 0.08, 1.0, 1.0 - warmth * 0.10);
    c = clamp(c, 0.0, 1.0);
    if (look > 0) c = mix(c, clamp(applyLook(c), 0.0, 1.0), lookAmount);
    o = vec4(c, 1.0);
}
)";

// Crop + mirror/flip into the letterboxed output.
static const char* kOutput = R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D src; uniform vec4 crop; uniform bool mirror, flip, hasFrame;
void main() {
    if (!hasFrame) { o = vec4(0.055, 0.055, 0.07, 1.0); return; }
    vec2 p = vUv;
    if (flip) p.y = 1.0 - p.y;
    if (mirror) p.x = 1.0 - p.x;
    o = vec4(texture(src, crop.xy + p * (crop.zw - crop.xy)).rgb, 1.0);
}
)";

// Still picture for pause / reconnect: blurred and dimmed snapshot, plus the badge (premultiplied RGBA).
static const char* kHold = R"(#version 330 core
in vec2 vUv; out vec4 o; uniform sampler2D src, blurred, badge; uniform float blurMix, dim;
uniform vec4 rect; uniform bool hasBadge;
void main() {
    vec3 c = mix(texture(src, vUv).rgb, texture(blurred, vUv).rgb, blurMix) * (1.0 - dim);
    if (hasBadge && vUv.x >= rect.x && vUv.x <= rect.z && vUv.y >= rect.y && vUv.y <= rect.w) {
        vec2 b = (vUv - rect.xy) / (rect.zw - rect.xy);
        b.y = 1.0 - b.y; // the badge was drawn top-down by ImGui, the output is stored top row first
        vec4 t = texture(badge, b);
        c = c * (1.0 - t.a) + t.rgb;
    }
    o = vec4(c, 1.0);
}
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

static GLuint makeTex(GLenum filter = GL_LINEAR) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void Renderer::Target::ensure(int nw, int nh, GLenum filter) {
    if (nw == w && nh == h && fbo) return;
    w = nw;
    h = nh;
    if (!tex) tex = makeTex(filter);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (!fbo) glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::Target::release() {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (tex) glDeleteTextures(1, &tex);
    fbo = tex = 0;
    w = h = 0;
}

Renderer::Program Renderer::build(const char* fragment) {
    Program p;
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
    p.id = glCreateProgram();
    glAttachShader(p.id, vs);
    glAttachShader(p.id, fs);
    glLinkProgram(p.id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

bool Renderer::init() {
    convert_ = build(kConvert);
    night_ = build(kNight);
    down_ = build(kDown);
    gauss_ = build(kGauss);
    composite_ = build(kComposite);
    smooth_ = build(kSmooth);
    color_ = build(kColor);
    output_ = build(kOutput);
    hold_prog_ = build(kHold);
    glUseProgram(convert_.id);
    glUniform1i(convert_.loc("texY"), 0);
    glUniform1i(convert_.loc("texU"), 1);
    glUniform1i(convert_.loc("texV"), 2);
    glUseProgram(night_.id);
    glUniform1i(night_.loc("cur"), 0);
    glUniform1i(night_.loc("hist"), 1);
    glUseProgram(composite_.id);
    glUniform1i(composite_.loc("cur"), 0);
    glUniform1i(composite_.loc("back"), 1);
    glUniform1i(composite_.loc("mask"), 2);
    glGenVertexArrays(1, &vao_);
    texY_ = makeTex();
    texU_ = makeTex();
    texV_ = makeTex();
    maskTex_ = makeTex();
    GLint ok = 0;
    glGetProgramiv(output_.id, GL_LINK_STATUS, &ok);
    return ok != 0;
}

void Renderer::shutdown() {
    unmapReadback();
    unmapAnalysis();
    if (fence_) glDeleteSync(fence_);
    if (afence_) glDeleteSync(afence_);
    if (pbo_) glDeleteBuffers(1, &pbo_);
    if (apbo_) glDeleteBuffers(1, &apbo_);
    for (Target* t : {&work_, &tmp_, &hist_, &histNext_, &q1_, &q2_, &analysis_, &out_, &glassA_, &glassB_, &glass_, &hold_, &badge_}) t->release();
    GLuint t[5] = {texY_, texU_, texV_, maskTex_, bgTex_};
    glDeleteTextures(5, t);
    glDeleteVertexArrays(1, &vao_);
    for (Program* p : {&convert_, &night_, &down_, &gauss_, &composite_, &smooth_, &color_, &output_, &hold_prog_}) glDeleteProgram(p->id);
}

void Renderer::ensurePlanes(int w, int h, bool nv12) {
    if (w == srcW_ && h == srcH_ && nv12 == nv12_) return;
    srcW_ = w;
    srcH_ = h;
    nv12_ = nv12;
    histValid_ = false;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    glBindTexture(GL_TEXTURE_2D, texY_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, texU_);
    glTexImage2D(GL_TEXTURE_2D, 0, nv12 ? GL_RG8 : GL_R8, cw, ch, 0, nv12 ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, texV_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, nv12 ? 1 : cw, nv12 ? 1 : ch, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
}

static void colorMatrix(const AVFrame* f, float m[9], float off[3]) {
    bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    bool bt709 = f->colorspace == AVCOL_SPC_BT709;
    float kr = bt709 ? 0.2126f : 0.299f, kb = bt709 ? 0.0722f : 0.114f, kg = 1.f - kr - kb;
    float ys = full ? 1.f : 255.f / 219.f, cs = full ? 1.f : 255.f / 224.f;
    float rv = 2.f * (1.f - kr) * cs, bu = 2.f * (1.f - kb) * cs;
    float gu = -bu * kb / kg, gv = -rv * kr / kg;
    float mm[9] = {ys, ys, ys, 0.f, gu, bu, rv, gv, 0.f}; // column-major
    memcpy(m, mm, sizeof(mm));
    off[0] = full ? 0.f : 16.f / 255.f;
    off[1] = 128.f / 255.f;
    off[2] = 128.f / 255.f;
}

void Renderer::upload(AVFrame* f) {
    bool nv12 = f->format == AV_PIX_FMT_NV12;
    ensurePlanes(f->width, f->height, nv12);
    colorMatrix(f, mat_, off_);
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, texY_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height, GL_RED, GL_UNSIGNED_BYTE, f->data[0]);
    if (nv12) {
        glBindTexture(GL_TEXTURE_2D, texU_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[1] / 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RG, GL_UNSIGNED_BYTE, f->data[1]);
    } else {
        glBindTexture(GL_TEXTURE_2D, texU_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[1]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RED, GL_UNSIGNED_BYTE, f->data[1]);
        glBindTexture(GL_TEXTURE_2D, texV_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[2]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RED, GL_UNSIGNED_BYTE, f->data[2]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    AVFrame* tmp = f;
    av_frame_free(&tmp);
}

void Renderer::setMask(const uint8_t* mask, int w, int h) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, maskTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, mask);
    hasMask_ = true;
}

bool Renderer::setBackgroundImage(const std::string& path) {
    if (path == bgPath_ && (bgTex_ || path.empty())) return bgTex_ != 0 || path.empty();
    bgPath_ = path;
    if (path.empty()) return true;
    int w, h, n;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!px) {
        if (bgTex_) glDeleteTextures(1, &bgTex_);
        bgTex_ = 0;
        return false;
    }
    if (!bgTex_) bgTex_ = makeTex();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, bgTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    bgW_ = w;
    bgH_ = h;
    return true;
}

void Renderer::bindTex(int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

void Renderer::draw(const Target& dst, float x0, float y0, float x1, float y1) {
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glUniform4f(glGetUniformLocation((GLuint)prog, "uRect"), x0, y0, x1, y1);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Leaves a blurred copy of `src` (quarter size) in q1_.
void Renderer::blurInto(const Target& src, float strength) {
    int qw = std::max(16, src.w / 4), qh = std::max(16, src.h / 4);
    q1_.ensure(qw, qh);
    q2_.ensure(qw, qh);
    glUseProgram(down_.id);
    glUniform1i(down_.loc("src"), 0);
    glUniform2f(down_.loc("texel"), 1.f / src.w, 1.f / src.h);
    bindTex(0, src.tex);
    draw(q1_);
    glUseProgram(gauss_.id);
    glUniform1i(gauss_.loc("src"), 0);
    int passes = 1 + (int)std::lround(strength * 4.f);
    for (int i = 0; i < passes; i++) {
        float spread = 1.f + i * 0.75f;
        glUniform2f(gauss_.loc("dir"), spread / qw, 0.f);
        bindTex(0, q1_.tex);
        draw(q2_);
        glUniform2f(gauss_.loc("dir"), 0.f, spread / qh);
        bindTex(0, q2_.tex);
        draw(q1_);
    }
}

void Renderer::captureHold() {
    if (!out_.fbo) return;
    hold_.ensure(out_.w, out_.h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, out_.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hold_.fbo);
    glBlitFramebuffer(0, 0, out_.w, out_.h, 0, 0, hold_.w, hold_.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::renderHold(float blur, float dim) {
    if (!hold_.fbo || !out_.fbo) return;
    if (blur > 0) blurInto(hold_, blur);
    glUseProgram(hold_prog_.id);
    glUniform1i(hold_prog_.loc("src"), 0);
    glUniform1i(hold_prog_.loc("blurred"), 1);
    glUniform1i(hold_prog_.loc("badge"), 2);
    glUniform1f(hold_prog_.loc("blurMix"), blur > 0 ? 1.f : 0.f);
    glUniform1f(hold_prog_.loc("dim"), dim);
    glUniform1i(hold_prog_.loc("hasBadge"), badgeOn_ && badge_.tex);
    if (badgeOn_ && badge_.tex) {
        // In the middle of the picture (output uv y = 0 is the top row).
        float bw = (float)badge_.w / out_.w, bh = (float)badge_.h / out_.h;
        glUniform4f(hold_prog_.loc("rect"), 0.5f - bw / 2, 0.5f - bh / 2, 0.5f + bw / 2, 0.5f + bh / 2);
    }
    bindTex(0, hold_.tex);
    bindTex(1, blur > 0 ? q1_.tex : hold_.tex);
    bindTex(2, badge_.tex);
    draw(out_);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Renderer::beginBadge(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    badge_.ensure(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, badge_.fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return true;
}

void Renderer::endBadge() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    badgeOn_ = true;
}

void Renderer::updateGlass() {
    if (!out_.tex || out_.w < 64) return;
    // out -> 1/4 -> 1/16 (box-filtered), then a wide gaussian: a frosted copy for the glass UI.
    int w4 = std::max(16, out_.w / 4), h4 = std::max(16, out_.h / 4);
    int w16 = std::max(8, out_.w / 16), h16 = std::max(8, out_.h / 16);
    glassA_.ensure(w4, h4);
    glass_.ensure(w16, h16);
    glassB_.ensure(w16, h16);
    glUseProgram(down_.id);
    glUniform1i(down_.loc("src"), 0);
    glUniform2f(down_.loc("texel"), 1.f / out_.w, 1.f / out_.h);
    bindTex(0, out_.tex);
    draw(glassA_);
    glUniform2f(down_.loc("texel"), 1.f / w4, 1.f / h4);
    bindTex(0, glassA_.tex);
    draw(glass_);
    glUseProgram(gauss_.id);
    glUniform1i(gauss_.loc("src"), 0);
    for (int i = 0; i < 2; i++) {
        glUniform2f(gauss_.loc("dir"), (1.f + i) / w16, 0.f);
        bindTex(0, glass_.tex);
        draw(glassB_);
        glUniform2f(gauss_.loc("dir"), 0.f, (1.f + i) / h16);
        bindTex(0, glassB_.tex);
        draw(glass_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::render(int outW, int outH, const OutputTransform& t, const FilterPrefs& f, const CropRect& crop, bool newFrame) {
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    out_.ensure(outW, outH);
    int ww = std::max(16, workWidth(t.quarterTurns)), wh = std::max(16, workHeight(t.quarterTurns));
    // Effects run at the size the output actually needs (a 4K phone stream into a 1080p camera is
    // processed at 1080p); auto-frame keeps headroom for zooming in. Stepped so it doesn't reallocate.
    {
        bool cropped = crop.x1 - crop.x0 < 0.999f || crop.y1 - crop.y0 < 0.999f || f.autoFrame;
        float need = std::max((float)outW / ww, (float)outH / wh) * (cropped ? 2.5f : 1.f);
        if (need < 1.f) {
            float sc = std::ceil(need * 8.f) / 8.f;
            ww = std::max(16, (int)(ww * sc) & ~1);
            wh = std::max(16, (int)(wh * sc) & ~1);
        }
    }
    const Target* cur = nullptr;

    if (srcW_ > 0) {
        if (work_.w != ww || work_.h != wh) histValid_ = false;
        work_.ensure(ww, wh);
        tmp_.ensure(ww, wh);
        glUseProgram(convert_.id);
        glUniform1i(convert_.loc("uTurns"), t.quarterTurns & 3);
        glUniform1i(convert_.loc("uNv12"), nv12_);
        glUniformMatrix3fv(convert_.loc("uMat"), 1, GL_FALSE, mat_);
        glUniform3fv(convert_.loc("uOff"), 1, off_);
        bindTex(0, texY_);
        bindTex(1, texU_);
        bindTex(2, texV_);
        draw(work_);
        cur = &work_;
        if (analysisWant_ > 0) { // the ML wants the real scene, before any effect
            readAnalysis(analysisWant_);
            analysisWant_ = 0;
        }

        // Night mode keeps a running average in hist_; it only advances on a new picture.
        if (f.night > 0.f) {
            hist_.ensure(ww, wh);
            histNext_.ensure(ww, wh);
            if (newFrame || !histValid_) {
                glUseProgram(night_.id);
                glUniform2f(night_.loc("texel"), 1.f / ww, 1.f / wh);
                glUniform1f(night_.loc("strength"), histValid_ ? std::min(f.night, 1.f) : 0.f);
                bindTex(0, work_.tex);
                bindTex(1, hist_.tex);
                draw(histNext_);
                std::swap(hist_, histNext_);
                histValid_ = true;
            }
            cur = &hist_;
        } else {
            histValid_ = false;
        }

        auto pingpong = [&]() -> const Target& { return cur == &tmp_ ? work_ : tmp_; };

        if (f.background != "off" && hasMask_) {
            bool image = f.background == "image" && bgTex_;
            if (!image) blurInto(*cur, f.blur);
            const Target& dst = cur == &hist_ ? tmp_ : pingpong();
            glUseProgram(composite_.id);
            glUniform2f(composite_.loc("maskTexel"), 0.6f / Vision_kMaskSize, 0.6f / Vision_kMaskSize);
            glUniform1i(composite_.loc("useImage"), image);
            if (image) { // cover-fit the picture
                float ia = (float)bgW_ / bgH_, wa = (float)ww / wh;
                float sw = ia > wa ? wa / ia : 1.f, sh = ia > wa ? 1.f : ia / wa;
                glUniform4f(composite_.loc("imgRect"), (1.f - sw) / 2, (1.f - sh) / 2, sw, sh);
            }
            bindTex(0, cur->tex);
            bindTex(1, image ? bgTex_ : q1_.tex);
            bindTex(2, maskTex_);
            draw(dst);
            cur = &dst;
        }

        if (f.smooth > 0.f) {
            const Target& dst = cur == &hist_ ? tmp_ : pingpong();
            glUseProgram(smooth_.id);
            glUniform1i(smooth_.loc("src"), 0);
            glUniform2f(smooth_.loc("texel"), 1.f / ww, 1.f / wh);
            glUniform1f(smooth_.loc("strength"), std::min(f.smooth, 1.f));
            bindTex(0, cur->tex);
            draw(dst);
            cur = &dst;
        }

        static const char* looks[] = {"none", "warm", "cool", "film", "vivid", "fade", "bw"};
        int look = 0;
        for (int i = 1; i < 7; i++)
            if (f.look == looks[i]) look = i;
        bool colour = f.sharpen > 0 || f.brightness != 0 || f.contrast != 0 || f.saturation != 0 || f.warmth != 0 || look;
        if (colour) {
            const Target& dst = cur == &hist_ ? tmp_ : pingpong();
            glUseProgram(color_.id);
            glUniform1i(color_.loc("src"), 0);
            glUniform2f(color_.loc("texel"), 1.f / ww, 1.f / wh);
            glUniform1f(color_.loc("sharpen"), f.sharpen);
            glUniform1f(color_.loc("brightness"), f.brightness);
            glUniform1f(color_.loc("contrast"), f.contrast);
            glUniform1f(color_.loc("saturation"), f.saturation);
            glUniform1f(color_.loc("warmth"), f.warmth);
            glUniform1f(color_.loc("lookAmount"), f.lookAmount);
            glUniform1i(color_.loc("look"), look);
            bindTex(0, cur->tex);
            draw(dst);
            cur = &dst;
        }
    }

    // Letterbox the (cropped) upright picture into the output.
    glBindFramebuffer(GL_FRAMEBUFFER, out_.fbo);
    glViewport(0, 0, outW, outH);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    float x0 = -1, y0 = -1, x1 = 1, y1 = 1;
    if (cur) {
        float sw = ww * (crop.x1 - crop.x0), sh = wh * (crop.y1 - crop.y0);
        float scale = std::min(outW / sw, outH / sh);
        float w = sw * scale / outW, h = sh * scale / outH;
        x0 = -w;
        x1 = w;
        y0 = -h;
        y1 = h;
    }
    glUseProgram(output_.id);
    glUniform1i(output_.loc("src"), 0);
    glUniform4f(output_.loc("crop"), crop.x0, crop.y0, crop.x1, crop.y1);
    glUniform1i(output_.loc("mirror"), t.mirror);
    glUniform1i(output_.loc("flip"), t.flip);
    glUniform1i(output_.loc("hasFrame"), cur != nullptr);
    bindTex(0, cur ? cur->tex : 0);
    draw(out_, x0, y0, x1, y1);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::beginReadback() {
    if (!out_.fbo) return;
    unmapReadback();
    size_t need = (size_t)out_.w * out_.h * 3;
    if (!pbo_) glGenBuffers(1, &pbo_);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
    if (need != pboSize_) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)need, nullptr, GL_STREAM_READ);
        pboSize_ = need;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, out_.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, out_.w, out_.h, GL_BGR, GL_UNSIGNED_BYTE, nullptr); // returns at once; DMA runs meanwhile
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (fence_) glDeleteSync(fence_);
    fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

const uint8_t* Renderer::mapReadback() {
    if (!fence_) return nullptr;
    glClientWaitSync(fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 20 * 1000 * 1000);
    glDeleteSync(fence_);
    fence_ = nullptr;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
    void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)pboSize_, GL_MAP_READ_BIT);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    mapped_ = p != nullptr;
    return (const uint8_t*)p;
}

void Renderer::unmapReadback() {
    if (!mapped_) return;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    mapped_ = false;
}

void Renderer::readAnalysis(int w) {
    unmapAnalysis();
    int h = std::max(16, (int)std::lround((double)w * work_.h / work_.w)) & ~1;
    analysis_.ensure(w, h);
    glUseProgram(down_.id);
    glUniform1i(down_.loc("src"), 0);
    glUniform2f(down_.loc("texel"), 0.25f * work_.w / w / work_.w, 0.25f * work_.h / h / work_.h);
    bindTex(0, work_.tex);
    draw(analysis_);
    size_t need = (size_t)w * h * 3;
    if (!apbo_) glGenBuffers(1, &apbo_);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, apbo_);
    if (need != apboSize_) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)need, nullptr, GL_STREAM_READ);
        apboSize_ = need;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, analysis_.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (afence_) glDeleteSync(afence_);
    afence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

const uint8_t* Renderer::mapAnalysis(int& w, int& h) {
    if (!afence_) return nullptr;
    glClientWaitSync(afence_, GL_SYNC_FLUSH_COMMANDS_BIT, 20 * 1000 * 1000);
    glDeleteSync(afence_);
    afence_ = nullptr;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, apbo_);
    void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)apboSize_, GL_MAP_READ_BIT);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    amapped_ = p != nullptr;
    w = analysis_.w;
    h = analysis_.h;
    return (const uint8_t*)p;
}

void Renderer::unmapAnalysis() {
    if (!amapped_) return;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, apbo_);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    amapped_ = false;
}
