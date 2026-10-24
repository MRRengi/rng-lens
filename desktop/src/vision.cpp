#include "vision.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

// mingw's sal.h lacks some SAL annotations the ONNX Runtime header uses on Windows; they are no-ops.
#if defined(_WIN32) && defined(__MINGW32__)
#include <sal.h>
#ifndef _In_
#define _In_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _In_reads_
#define _In_reads_(X)
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(X)
#endif
#ifndef _Out_writes_
#define _Out_writes_(X)
#endif
#ifndef _Inout_updates_all_
#define _Inout_updates_all_(X)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(X)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(X)
#endif
#ifndef _Success_
#define _Success_(X)
#endif
#endif
#include "onnxruntime/onnxruntime_c_api.h"
#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// One ONNX Runtime environment per process, shared by both phones' workers.
OrtEnv* g_env = nullptr;
std::mutex g_envM;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Bilinear sample of an RGB8 image at (x, y) in pixels.
inline void sample(const uint8_t* rgb, int w, int h, float x, float y, float out[3]) {
    x = std::max(0.f, std::min(x, w - 1.001f));
    y = std::max(0.f, std::min(y, h - 1.001f));
    int x0 = (int)x, y0 = (int)y;
    float fx = x - x0, fy = y - y0;
    const uint8_t* p00 = rgb + ((size_t)y0 * w + x0) * 3;
    const uint8_t* p01 = p00 + 3;
    const uint8_t* p10 = p00 + (size_t)w * 3;
    const uint8_t* p11 = p10 + 3;
    for (int c = 0; c < 3; c++)
        out[c] = (p00[c] * (1 - fx) + p01[c] * fx) * (1 - fy) + (p10[c] * (1 - fx) + p11[c] * fx) * fy;
}

}  // namespace

Vision::~Vision() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        if (!ready_ && !failed_) { // still loading (or stuck in the runtime): don't hang the exit on it
            thread_.detach();
            return;
        }
        thread_.join();
    }
    if (api_) {
        if (seg_) api_->ReleaseSession(seg_);
        if (faceNet_) api_->ReleaseSession(faceNet_);
        if (mem_) api_->ReleaseMemoryInfo(mem_);
    }
}

void Vision::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread(&Vision::run, this);
}

std::string Vision::error() const {
    std::lock_guard<std::mutex> lock(m_);
    return error_;
}

bool Vision::load(std::string& why) {
    std::string dir = plat::exeDir();
    modelsDir_ = dir + "models/";
    typedef const OrtApiBase*(ORT_API_CALL * GetBase)();
    GetBase getBase = nullptr;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA((dir + "onnxruntime.dll").c_str());
    if (!lib) {
        why = "Background and auto-frame need the Microsoft Visual C++ Runtime (x64): "
              "https://aka.ms/vs/17/release/vc_redist.x64.exe";
        return false;
    }
    getBase = (GetBase)GetProcAddress(lib, "OrtGetApiBase");
#else
    void* lib = dlopen((dir + "libonnxruntime.so.1").c_str(), RTLD_NOW);
    if (!lib) lib = dlopen("libonnxruntime.so.1", RTLD_NOW);
    if (!lib) {
        why = "ONNX Runtime (libonnxruntime.so.1) not found next to rng-lens";
        return false;
    }
    getBase = (GetBase)dlsym(lib, "OrtGetApiBase");
#endif
    lib_ = (void*)lib;
    const OrtApi* api = getBase ? getBase()->GetApi(ORT_API_VERSION) : nullptr;
    if (!api) {
        why = "ONNX Runtime is too old for rng-lens";
        return false;
    }
    if (!plat::fileExists(modelsDir_ + "human_segmentation_pphumanseg_2023mar.onnx") ||
        !plat::fileExists(modelsDir_ + "face_detection_yunet_2023mar.onnx")) {
        why = "Models missing in " + modelsDir_;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_envM);
        if (!g_env && api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "rng-lens", &g_env)) g_env = nullptr;
        env_ = g_env;
    }
    if (!env_ || api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_)) {
        why = "ONNX Runtime failed to start";
        return false;
    }
    api_ = api;
    return true;
}

void* Vision::session(const std::string& file) {
    OrtSessionOptions* opt = nullptr;
    api_->CreateSessionOptions(&opt);
    api_->SetIntraOpNumThreads(opt, 2); // small models: more threads only cost wake-ups
    api_->SetSessionGraphOptimizationLevel(opt, ORT_ENABLE_ALL);
    OrtSession* s = nullptr;
    std::string path = modelsDir_ + file;
    OrtStatus* st;
#ifdef _WIN32
    std::wstring wpath(path.begin(), path.end());
    st = api_->CreateSession(env_, wpath.c_str(), opt, &s);
#else
    st = api_->CreateSession(env_, path.c_str(), opt, &s);
#endif
    if (st) {
        fprintf(stderr, "vision: %s: %s\n", file.c_str(), api_->GetErrorMessage(st));
        api_->ReleaseStatus(st);
        s = nullptr;
    }
    api_->ReleaseSessionOptions(opt);
    return s;
}

void Vision::submit(const uint8_t* rgb, int w, int h, bool wantMask, bool wantFace) {
    if (!ready_) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        input_.assign(rgb, rgb + (size_t)w * h * 3);
        inW_ = w;
        inH_ = h;
        wantMask_ = wantMask;
        wantFace_ = wantFace;
        hasInput_ = true;
    }
    cv_.notify_one();
}

bool Vision::takeMask(std::vector<uint8_t>& out, int& w, int& h) {
    std::lock_guard<std::mutex> lock(m_);
    if (!maskNew_) return false;
    out = mask_;
    w = h = kMaskSize;
    maskNew_ = false;
    return true;
}

FaceBox Vision::face() const {
    std::lock_guard<std::mutex> lock(m_);
    return face_;
}

void Vision::run() {
    std::string why;
    if (!load(why)) {
        std::lock_guard<std::mutex> lock(m_);
        error_ = why;
        failed_ = true;
        return;
    }
    // Load both networks now so the first effect frame doesn't stall.
    seg_ = (OrtSession*)session("human_segmentation_pphumanseg_2023mar.onnx");
    faceNet_ = (OrtSession*)session("face_detection_yunet_2023mar.onnx");
    if (!seg_ || !faceNet_) {
        std::lock_guard<std::mutex> lock(m_);
        error_ = "Couldn't load the vision models";
        failed_ = true;
        return;
    }
    ready_ = true;
    std::vector<uint8_t> rgb;
    while (!stop_) {
        int w, h;
        bool mask, faceWanted;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [&] { return stop_ || hasInput_; });
            if (stop_) break;
            rgb.swap(input_);
            w = inW_;
            h = inH_;
            mask = wantMask_;
            faceWanted = wantFace_;
            hasInput_ = false;
        }
        if (mask && nowMs() - lastSegMs_ >= 45) { // ~20 masks a second is plenty; saves CPU
            runSeg(rgb, w, h);
            lastSegMs_ = nowMs();
        }
        if (faceWanted && nowMs() - lastFaceMs_ >= 200) {
            runFace(rgb, w, h);
            lastFaceMs_ = nowMs();
        }
    }
}

bool Vision::runSeg(const std::vector<uint8_t>& rgb, int w, int h) {
    auto t0 = std::chrono::steady_clock::now();
    const int S = kMaskSize;
    std::vector<float> in((size_t)3 * S * S);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float px[3];
            sample(rgb.data(), w, h, (x + 0.5f) * w / S - 0.5f, (y + 0.5f) * h / S - 0.5f, px);
            for (int c = 0; c < 3; c++) in[(size_t)c * S * S + (size_t)y * S + x] = (px[c] / 255.f - 0.5f) / 0.5f;
        }
    int64_t shape[4] = {1, 3, S, S};
    OrtValue* input = nullptr;
    OrtValue* output = nullptr;
    if (api_->CreateTensorWithDataAsOrtValue(mem_, in.data(), in.size() * sizeof(float), shape, 4,
                                             ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input))
        return false;
    const char* inName = "x";
    const char* outName = "save_infer_model/scale_0.tmp_1";
    OrtStatus* st = api_->Run(seg_, nullptr, &inName, &input, 1, &outName, 1, &output);
    api_->ReleaseValue(input);
    if (st) {
        api_->ReleaseStatus(st);
        return false;
    }
    float* p = nullptr;
    api_->GetTensorMutableData(output, (void**)&p);
    const float* person = p + (size_t)S * S; // channel 1 = person probability
    if (maskAcc_.size() != (size_t)S * S) maskAcc_.assign((size_t)S * S, 0.f);
    std::vector<uint8_t> m((size_t)S * S);
    for (size_t i = 0; i < m.size(); i++) {
        maskAcc_[i] = maskAcc_[i] * 0.4f + std::min(1.f, std::max(0.f, person[i])) * 0.6f; // calms edge flicker
        m[i] = (uint8_t)std::lround(maskAcc_[i] * 255.f);
    }
    api_->ReleaseValue(output);
    {
        std::lock_guard<std::mutex> lock(m_);
        mask_.swap(m);
        maskNew_ = true;
    }
    segMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool Vision::runFace(const std::vector<uint8_t>& rgb, int w, int h) {
    auto t0 = std::chrono::steady_clock::now();
    const int S = kFaceSize;
    // Fit the picture into the square input (top-left, black padding), BGR 0..255 as YuNet expects.
    float scale = std::min((float)S / w, (float)S / h);
    int fw = (int)(w * scale), fh = (int)(h * scale);
    std::vector<float> in((size_t)3 * S * S, 0.f);
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) {
            float px[3];
            sample(rgb.data(), w, h, (x + 0.5f) / scale - 0.5f, (y + 0.5f) / scale - 0.5f, px);
            size_t o = (size_t)y * S + x;
            in[o] = px[2];
            in[(size_t)S * S + o] = px[1];
            in[(size_t)2 * S * S + o] = px[0];
        }
    int64_t shape[4] = {1, 3, S, S};
    OrtValue* input = nullptr;
    if (api_->CreateTensorWithDataAsOrtValue(mem_, in.data(), in.size() * sizeof(float), shape, 4,
                                             ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input))
        return false;
    const char* inName = "input";
    const char* outNames[9] = {"cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32", "bbox_8", "bbox_16", "bbox_32"};
    OrtValue* out[9] = {nullptr};
    OrtStatus* st = api_->Run(faceNet_, nullptr, &inName, &input, 1, outNames, 9, out);
    api_->ReleaseValue(input);
    if (st) {
        api_->ReleaseStatus(st);
        return false;
    }
    FaceBox best;
    float bestArea = 0;
    const int strides[3] = {8, 16, 32};
    for (int k = 0; k < 3; k++) {
        float *cls, *obj, *box;
        api_->GetTensorMutableData(out[k], (void**)&cls);
        api_->GetTensorMutableData(out[3 + k], (void**)&obj);
        api_->GetTensorMutableData(out[6 + k], (void**)&box);
        int s = strides[k], cols = S / s, rows = S / s;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                int i = r * cols + c;
                float score = std::sqrt(std::min(1.f, std::max(0.f, cls[i])) * std::min(1.f, std::max(0.f, obj[i])));
                if (score < 0.6f) continue;
                float cx = (c + box[i * 4 + 0]) * s, cy = (r + box[i * 4 + 1]) * s;
                float bw = std::exp(box[i * 4 + 2]) * s, bh = std::exp(box[i * 4 + 3]) * s;
                float area = bw * bh * score; // the main (largest, surest) face wins
                if (area > bestArea) {
                    bestArea = area;
                    best.x = (cx - bw / 2) / fw;
                    best.y = (cy - bh / 2) / fh;
                    best.w = bw / fw;
                    best.h = bh / fh;
                    best.score = score;
                }
            }
    }
    for (OrtValue* v : out) api_->ReleaseValue(v);
    best.atMs = nowMs();
    {
        std::lock_guard<std::mutex> lock(m_);
        if (best.score > 0) face_ = best;
    }
    faceMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}
