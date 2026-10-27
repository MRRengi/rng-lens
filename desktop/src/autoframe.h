#pragma once
// Auto-frame camera motion. The face detector reports a few boxes a second and each one is a little off
// (position and especially size wobble), so following the raw boxes makes the frame twitch.
// 1. The detections are filtered (the size much harder than the position: a breathing zoom is the worst).
// 2. A dead zone with hysteresis: small head movements don't move the frame at all; once the head
//    really moves, the frame follows until it has caught up, then holds still again.
// 3. The frame moves on a critically damped spring (SmoothDamp), so every move starts and ends with
//    zero speed instead of jumping into motion.

#include <cmath>
#include <cstdint>

#include "renderer.h"
#include "vision.h"

class AutoFrame {
public:
    // ww, wh: the picture size; aspect: output width / height; tightness, speed: 0..1 from the UI.
    // Returns true while the crop moves.
    bool update(float dt, const FaceBox& fb, bool faceValid, int ww, int wh, float aspect, float tightness,
                float speed, CropRect& crop) {
        if (!init_) {
            cx_ = (crop.x0 + crop.x1) / 2;
            cy_ = (crop.y0 + crop.y1) / 2;
            ch_ = crop.y1 - crop.y0;
            init_ = true;
        }
        // 1. Filter each new detection.
        if (faceValid && fb.atMs != lastDetMs_) {
            float fx = fb.x + fb.w / 2, fy = fb.y + fb.h / 2;
            if (!haveFace_) {
                fx_ = fx, fy_ = fy, fh_ = fb.h;
                haveFace_ = true;
            } else {
                fx_ += (fx - fx_) * 0.6f;
                fy_ += (fy - fy_) * 0.6f;
                fh_ += (fb.h - fh_) * 0.3f;
            }
            lastDetMs_ = fb.atMs;
        }
        if (!faceValid) haveFace_ = false;

        // Where the frame wants to be.
        float tx = 0.5f, ty = 0.5f, th = 1.f;
        float maxH = fitHeight(1.f, ww, wh, aspect);
        if (haveFace_) {
            float k = 4.2f - 2.6f * tightness; // crop height in face heights
            th = std::fmin(maxH, std::fmax(0.35f, fh_ * k));
            tx = fx_;
            ty = fy_ + th * 0.12f; // eyes in the upper third
        } else {
            th = maxH;
        }

        // 2. Dead zone with hysteresis, measured against the current frame size.
        float dx = tx - gx_, dy = ty - gy_, dh = th - gh_;
        if (!goalInit_) {
            gx_ = cx_, gy_ = cy_, gh_ = ch_;
            goalInit_ = true;
            dx = tx - gx_, dy = ty - gy_, dh = th - gh_;
        }
        float startMove = ch_ * 0.065f, startZoom = ch_ * 0.10f;
        if (!tracking_ && (std::fabs(dx) > startMove || std::fabs(dy) > startMove || std::fabs(dh) > startZoom))
            tracking_ = true;
        if (tracking_) {
            gx_ = tx, gy_ = ty, gh_ = th;
            // Caught up and the head has stopped: hold still again.
            bool settled = std::fabs(cx_ - gx_) < ch_ * 0.01f && std::fabs(cy_ - gy_) < ch_ * 0.01f &&
                           std::fabs(ch_ - gh_) < ch_ * 0.01f && std::fabs(vx_) + std::fabs(vy_) + std::fabs(vh_) < 0.01f;
            if (settled) tracking_ = false;
        }

        // 3. Spring towards the goal. Speed slider: ~1 s (slow) .. ~0.25 s (fast) to settle.
        float smooth = 0.45f - 0.34f * speed;
        float ox = cx_, oy = cy_, oh = ch_;
        cx_ = smoothDamp(cx_, gx_, vx_, smooth, dt);
        cy_ = smoothDamp(cy_, gy_, vy_, smooth, dt);
        ch_ = smoothDamp(ch_, gh_, vh_, smooth * 1.15f, dt); // zoom a bit lazier than pan

        // Crop rectangle, kept inside the picture (clamped here, not in the spring, so edges don't fight it).
        float h = std::fmin(ch_, maxH);
        float w = h * wh * aspect / ww;
        if (w > 1.f) {
            h /= w;
            w = 1.f;
        }
        crop.x0 = std::fmin(1.f - w, std::fmax(0.f, cx_ - w / 2));
        crop.y0 = std::fmin(1.f - h, std::fmax(0.f, cy_ - h / 2));
        crop.x1 = crop.x0 + w;
        crop.y1 = crop.y0 + h;
        return std::fabs(cx_ - ox) + std::fabs(cy_ - oy) + std::fabs(ch_ - oh) > 1e-5f;
    }

private:
    // Tallest crop (0..1 of the picture height) with the output aspect that fits the picture.
    static float fitHeight(float h, int ww, int wh, float aspect) {
        float w = h * wh * aspect / ww;
        return w > 1.f ? h / w : h;
    }
    // Critically damped spring, stable for any dt (Game Programming Gems 4, 1.10).
    static float smoothDamp(float cur, float target, float& vel, float smoothTime, float dt) {
        float omega = 2.f / smoothTime, x = omega * dt;
        float e = 1.f / (1.f + x + 0.48f * x * x + 0.235f * x * x * x);
        float change = cur - target, temp = (vel + omega * change) * dt;
        vel = (vel - omega * temp) * e;
        return target + (change + temp) * e;
    }

    bool init_ = false, goalInit_ = false, haveFace_ = false, tracking_ = false;
    float cx_ = 0.5f, cy_ = 0.5f, ch_ = 1.f, vx_ = 0, vy_ = 0, vh_ = 0;
    float gx_ = 0.5f, gy_ = 0.5f, gh_ = 1.f;
    float fx_ = 0.5f, fy_ = 0.5f, fh_ = 0.2f;
    int64_t lastDetMs_ = -1;
};
