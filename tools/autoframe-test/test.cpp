// Auto-frame motion test: a sitting person with detector noise, then a real move. Compares the old
// follow logic with AutoFrame. Prints how much the frame wanders while the head is still, the
// biggest frame-to-frame speed jump (a twitch), and whether it caught up after the move.
#include <cmath>
#include <cstdio>
#include <random>
#include "autoframe.h"

struct OldFollow {  // the 1.0 logic, copied
    bool moving = false;
    bool update(float dt, const FaceBox& fb, int ww, int wh, float aspect, float tight, float speed, CropRect& crop) {
        CropRect target;
        float k = 4.6f - 2.4f * tight;
        float ch = std::min(1.f, std::max(0.35f, fb.h * k));
        float cw = ch * wh * aspect / ww;
        if (cw > 1.f) { ch /= cw; cw = 1.f; }
        float cx = fb.x + fb.w / 2, cy = fb.y + fb.h / 2 + ch * 0.12f;
        target.x0 = std::min(1.f - cw, std::max(0.f, cx - cw / 2));
        target.y0 = std::min(1.f - ch, std::max(0.f, cy - ch / 2));
        target.x1 = target.x0 + cw; target.y1 = target.y0 + ch;
        float ccw = crop.x1 - crop.x0, tw = target.x1 - target.x0;
        float dx = (target.x0 + target.x1 - crop.x0 - crop.x1) / 2, dy = (target.y0 + target.y1 - crop.y0 - crop.y1) / 2;
        bool far = std::fabs(dx) > ccw * 0.06f || std::fabs(dy) > ccw * 0.06f || std::fabs(tw - ccw) > ccw * 0.08f;
        if (!far && !moving) return false;
        moving = std::fabs(dx) > 0.002f || std::fabs(dy) > 0.002f || std::fabs(tw - ccw) > 0.002f;
        float rate = 1.f - std::exp(-dt * (0.8f + 4.f * speed));
        crop.x0 += (target.x0 - crop.x0) * rate; crop.y0 += (target.y0 - crop.y0) * rate;
        crop.x1 += (target.x1 - crop.x1) * rate; crop.y1 += (target.y1 - crop.y1) * rate;
        return true;
    }
};

template <class F>
void run(const char* name, F step) {
    std::mt19937 rng(7);
    std::normal_distribution<float> n(0.f, 1.f);
    const float dt = 1.f / 60;
    CropRect crop;
    FaceBox fb;
    double stillPath = 0, maxJump = 0, pvx = 0, pvy = 0;
    float px = 0.5f, py = 0.5f, endErr = 0, reachT = -1, firstZoomT = -1;
    for (int i = 0; i < 60 * 30; i++) {
        float t = i * dt;
        // true head: x 0.45 with a tiny sway, moves to 0.65 between t=12 and 13
        float hx = 0.45f + 0.005f * std::sin(t * 0.7f) + (t < 12 ? 0 : t > 13 ? 0.2f : 0.2f * (t - 12));
        float hy = 0.40f + 0.004f * std::sin(t * 0.5f), hh = 0.16f;
        if (i % 12 == 0) {  // detector at 5 Hz, noisy
            fb.h = hh * (1 + 0.05f * n(rng)); fb.w = fb.h * 0.8f;
            fb.x = hx + 0.012f * n(rng) - fb.w / 2; fb.y = hy + 0.012f * n(rng) - fb.h / 2;
            fb.score = 0.9f; fb.atMs = (int64_t)(t * 1000);
        }
        step(dt, fb, crop);
        float cx = (crop.x0 + crop.x1) / 2, cy = (crop.y0 + crop.y1) / 2;
        double vx = (cx - px) / dt, vy = (cy - py) / dt;
        bool still = (t > 5 && t < 12) || t > 18;
        if (still) stillPath += std::hypot(cx - px, cy - py) + std::fabs((crop.y1 - crop.y0) - 0) * 0;
        if (t > 5) maxJump = std::fmax(maxJump, std::hypot(vx - pvx, vy - pvy));
        pvx = vx, pvy = vy, px = cx, py = cy;
        if (t > 17.9f && t < 18) endErr = std::fabs(cx - 0.65f);
        if (t > 12 && reachT < 0 && std::fabs(cx - 0.65f) < 0.02f) reachT = t - 12;
        if (firstZoomT < 0 && (crop.y1 - crop.y0) < 0.75f) firstZoomT = t;
    }
    printf("%-10s wander while still: %.4f   biggest twitch: %.3f   off target after move: %.3f   caught up %.2fs after the move started   zoomed in at %.2fs\n",
           name, stillPath, maxJump, endErr, reachT, firstZoomT);
}

int main() {
    const int ww = 1920, wh = 1080; const float aspect = 16.f / 9;
    OldFollow old;
    run("old", [&](float dt, const FaceBox& fb, CropRect& c) { old.update(dt, fb, ww, wh, aspect, 0.5f, 0.5f, c); });
    AutoFrame af;
    run("new", [&](float dt, const FaceBox& fb, CropRect& c) { af.update(dt, fb, true, ww, wh, aspect, 0.5f, 0.5f, c); });
}
