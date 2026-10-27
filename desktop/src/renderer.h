#pragma once
// GPU side. Per frame:
//   decoded YUV -> RGB, upright (rotation)          "work" image at the stream's resolution
//   -> night mode (motion-adaptive temporal denoise)
//   -> background blur / replace (person mask from the ML worker, refined against the picture)
//   -> skin smoothing -> sharpen + colour + looks
//   -> crop (auto-frame) + mirror/flip + letterbox into the output size
// The output texture is what the preview shows and what the virtual camera receives.

#include <cstdint>
#include <string>
#include <vector>

#include <glad/gl.h>

#include "prefs.h"

struct AVFrame;

struct OutputTransform {
    int quarterTurns = 0; // clockwise
    bool mirror = false;
    bool flip = false;
};

// Part of the upright picture to show, in 0..1 (auto-frame); {0,0,1,1} = everything.
struct CropRect {
    float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
};

class Renderer {
public:
    bool init();
    void shutdown();

    // Takes ownership of the frame.
    void upload(AVFrame* frame);
    bool hasFrame() const { return srcW_ > 0; }
    // Size of the upright picture for the given rotation.
    int workWidth(int quarterTurns) const { return (quarterTurns & 1) ? srcH_ : srcW_; }
    int workHeight(int quarterTurns) const { return (quarterTurns & 1) ? srcW_ : srcH_; }

    // Person mask (0..255, row 0 = top of the upright picture) from the ML worker.
    void setMask(const uint8_t* mask, int w, int h);
    void clearMask() { hasMask_ = false; }
    // Loads the replacement background (empty path = none). Returns false if the file can't be read.
    bool setBackgroundImage(const std::string& path);

    // Runs the whole chain. `newFrame` = a new picture arrived since the last call (drives night mode).
    void render(int outW, int outH, const OutputTransform& t, const FilterPrefs& f, const CropRect& crop, bool newFrame);
    GLuint outputTexture() const { return out_.tex; }
    int outputWidth() const { return out_.w; }
    int outputHeight() const { return out_.h; }
    // Heavily blurred small copy of the output for the UI's frosted-glass panels. updateGlass() after render().
    void updateGlass();
    GLuint glassTexture() const { return glass_.tex; }

    // Hold: keep sending a still picture (pause, or the phone dropped out). captureHold() snapshots the
    // current output; renderHold() rebuilds the output from that snapshot, blurred / dimmed, with the badge.
    void captureHold();
    void renderHold(float blur, float dim);
    // Badge drawn by the caller (it has the fonts): a w x h RGBA target to render into, shown centred near
    // the bottom of the picture. Call beginBadge(), draw, endBadge().
    bool beginBadge(int w, int h);
    void endBadge();
    void clearBadge() { badgeOn_ = false; }

    // Async read of the output (call right after render()), then map/unmap: top-down BGR24, outW*outH*3.
    void beginReadback();
    const uint8_t* mapReadback();
    void unmapReadback();

    // Asks the next render() for a small upright RGB copy of the picture (width `w`, height by aspect),
    // taken before any effect, for the ML worker; map it after that render, unmap when done.
    void requestAnalysis(int w) { analysisWant_ = w; }
    const uint8_t* mapAnalysis(int& w, int& h);
    void unmapAnalysis();

private:
    struct Target {
        GLuint tex = 0, fbo = 0;
        int w = 0, h = 0;
        void ensure(int w, int h, GLenum filter = GL_LINEAR);
        void release();
    };
    struct Program {
        GLuint id = 0;
        GLint loc(const char* name) const { return glGetUniformLocation(id, name); }
    };

    Program build(const char* fragment);
    void draw(const Target& dst, float x0 = -1, float y0 = -1, float x1 = 1, float y1 = 1);
    static void bindTex(int unit, GLuint tex);
    void ensurePlanes(int w, int h, bool nv12);
    void blurInto(const Target& src, float strength);
    void readAnalysis(int w);

    Program convert_, night_, down_, gauss_, composite_, smooth_, color_, output_;
    GLuint vao_ = 0;
    GLuint texY_ = 0, texU_ = 0, texV_ = 0, maskTex_ = 0, bgTex_ = 0;
    int srcW_ = 0, srcH_ = 0, bgW_ = 0, bgH_ = 0;
    bool nv12_ = false, hasMask_ = false, histValid_ = false;
    float mat_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float off_[3] = {0, 0, 0};
    std::string bgPath_;

    Target work_, tmp_, hist_, histNext_, q1_, q2_, analysis_, out_, glassA_, glassB_, glass_, hold_, badge_;
    Program hold_prog_;
    bool badgeOn_ = false;

    GLuint pbo_ = 0, apbo_ = 0;
    GLsync fence_ = nullptr, afence_ = nullptr;
    bool mapped_ = false, amapped_ = false;
    size_t pboSize_ = 0, apboSize_ = 0;
    int analysisWant_ = 0;
};
