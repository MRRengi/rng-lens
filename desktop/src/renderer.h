#pragma once
// GPU side: uploads decoded YUV, converts to RGB with rotation/mirror/flip, letterboxes it into the
// output size, and reads the result back asynchronously (two PBOs) for the virtual camera.
// The output texture is also what the preview shows, so the preview is exactly what apps receive.

#include <cstdint>

#include <glad/gl.h>

struct AVFrame;

struct OutputTransform {
    int quarterTurns = 0; // clockwise
    bool mirror = false;
    bool flip = false;
};

class Renderer {
public:
    bool init();
    void shutdown();

    // Takes ownership of the frame.
    void upload(AVFrame* frame);
    bool hasFrame() const { return srcW_ > 0; }
    int sourceWidth() const { return srcW_; }
    int sourceHeight() const { return srcH_; }

    // Draws the current frame into the output texture (recreated when the size changes).
    void render(int outW, int outH, const OutputTransform& t);
    GLuint outputTexture() const { return outTex_; }
    int outputWidth() const { return outW_; }
    int outputHeight() const { return outH_; }

    // Starts an async read of the output into a PBO (call right after render()). Do other work (draw
    // the UI), then mapReadback() returns top-down BGR24 (outW*outH*3) or nullptr; unmapReadback() after use.
    void beginReadback();
    const uint8_t* mapReadback();
    void unmapReadback();

private:
    void ensureOutput(int w, int h);
    void ensurePlanes(int w, int h, bool nv12);

    GLuint program_ = 0, vao_ = 0;
    GLint uRect_ = -1, uTurns_ = -1, uMirror_ = -1, uFlip_ = -1, uNv12_ = -1, uMat_ = -1, uOff_ = -1, uHasFrame_ = -1;
    GLuint texY_ = 0, texU_ = 0, texV_ = 0;
    int srcW_ = 0, srcH_ = 0;
    bool nv12_ = false;
    float mat_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float off_[3] = {0, 0, 0};

    GLuint fbo_ = 0, outTex_ = 0;
    int outW_ = 0, outH_ = 0;
    GLuint pbo_ = 0;
    GLsync fence_ = nullptr;
    bool mapped_ = false;
};
