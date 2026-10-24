#include "renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

static const char* kVertex = R"(#version 330 core
uniform vec4 uRect; // x0, y0, x1, y1 in NDC; y0 is the image top (framebuffer row 0)
out vec2 vUv;
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec2 p = mix(uRect.xy, uRect.zw, c);
    vUv = c;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

static const char* kFragment = R"(#version 330 core
in vec2 vUv;
out vec4 color;
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;
uniform int uTurns;
uniform bool uMirror;
uniform bool uFlip;
uniform bool uNv12;
uniform bool uHasFrame;
uniform mat3 uMat;
uniform vec3 uOff;
void main() {
    if (!uHasFrame) { color = vec4(0.055, 0.055, 0.07, 1.0); return; }
    vec2 p = vUv;
    if (uFlip) p.y = 1.0 - p.y;
    if (uMirror) p.x = 1.0 - p.x;
    vec2 s = p;
    if (uTurns == 1) s = vec2(p.y, 1.0 - p.x);
    else if (uTurns == 2) s = vec2(1.0 - p.x, 1.0 - p.y);
    else if (uTurns == 3) s = vec2(1.0 - p.y, p.x);
    float y = texture(texY, s).r;
    vec2 uv = uNv12 ? texture(texU, s).rg : vec2(texture(texU, s).r, texture(texV, s).r);
    vec3 rgb = uMat * (vec3(y, uv) - uOff);
    color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

static GLuint makePlane() {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

bool Renderer::init() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "texY"), 0);
    glUniform1i(glGetUniformLocation(program_, "texU"), 1);
    glUniform1i(glGetUniformLocation(program_, "texV"), 2);
    uRect_ = glGetUniformLocation(program_, "uRect");
    uTurns_ = glGetUniformLocation(program_, "uTurns");
    uMirror_ = glGetUniformLocation(program_, "uMirror");
    uFlip_ = glGetUniformLocation(program_, "uFlip");
    uNv12_ = glGetUniformLocation(program_, "uNv12");
    uHasFrame_ = glGetUniformLocation(program_, "uHasFrame");
    uMat_ = glGetUniformLocation(program_, "uMat");
    uOff_ = glGetUniformLocation(program_, "uOff");
    glGenVertexArrays(1, &vao_);
    texY_ = makePlane();
    texU_ = makePlane();
    texV_ = makePlane();
    return true;
}

void Renderer::shutdown() {
    unmapReadback();
    if (fence_) glDeleteSync(fence_);
    if (pbo_) glDeleteBuffers(1, &pbo_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (outTex_) glDeleteTextures(1, &outTex_);
    GLuint t[3] = {texY_, texU_, texV_};
    glDeleteTextures(3, t);
    glDeleteVertexArrays(1, &vao_);
    glDeleteProgram(program_);
}

void Renderer::ensurePlanes(int w, int h, bool nv12) {
    if (w == srcW_ && h == srcH_ && nv12 == nv12_) return;
    srcW_ = w;
    srcH_ = h;
    nv12_ = nv12;
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
    // Column-major for GLSL: rgb = M * (y, u, v) with u/v centred.
    float rv = 2.f * (1.f - kr) * cs, bu = 2.f * (1.f - kb) * cs;
    float gu = -bu * kb / kg, gv = -rv * kr / kg;
    float mm[9] = {ys, ys, ys, 0.f, gu, bu, rv, gv, 0.f};
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

void Renderer::ensureOutput(int w, int h) {
    if (w == outW_ && h == outH_ && fbo_) return;
    unmapReadback();
    if (fence_) {
        glDeleteSync(fence_);
        fence_ = nullptr;
    }
    outW_ = w;
    outH_ = h;
    if (!fbo_) glGenFramebuffers(1, &fbo_);
    if (!outTex_) outTex_ = makePlane();
    glBindTexture(GL_TEXTURE_2D, outTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!pbo_) glGenBuffers(1, &pbo_);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 3, nullptr, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void Renderer::render(int outW, int outH, const OutputTransform& t) {
    ensureOutput(outW, outH);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, outW, outH);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    // Letterbox: fit the rotated source into the output, keeping its aspect.
    float x0 = -1, y0 = -1, x1 = 1, y1 = 1;
    if (srcW_ > 0) {
        bool sideways = t.quarterTurns & 1;
        float sw = (float)(sideways ? srcH_ : srcW_), sh = (float)(sideways ? srcW_ : srcH_);
        float scale = std::min((float)outW / sw, (float)outH / sh);
        float w = sw * scale / outW, h = sh * scale / outH; // fraction of the output
        x0 = -w;
        x1 = w;
        y0 = -h;
        y1 = h;
    }
    glUseProgram(program_);
    glUniform4f(uRect_, x0, y0, x1, y1);
    glUniform1i(uTurns_, t.quarterTurns & 3);
    glUniform1i(uMirror_, t.mirror);
    glUniform1i(uFlip_, t.flip);
    glUniform1i(uNv12_, nv12_);
    glUniform1i(uHasFrame_, srcW_ > 0);
    glUniformMatrix3fv(uMat_, 1, GL_FALSE, mat_);
    glUniform3fv(uOff_, 1, off_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texY_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texU_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texV_);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::beginReadback() {
    if (!fbo_) return;
    unmapReadback();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
    glReadPixels(0, 0, outW_, outH_, GL_BGR, GL_UNSIGNED_BYTE, nullptr); // returns at once; DMA runs meanwhile
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
    void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)outW_ * outH_ * 3, GL_MAP_READ_BIT);
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
