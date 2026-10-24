#include "headless.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "connection.h"
#include "decoder.h"
#include "discovery.h"
#include "platform.h"

using nlohmann::json;

int runHeadless(const std::string& host, int port, int frames, const std::string& png, const std::string& control) {
    plat::netInit();
    Decoder decoder;
    Connection conn(decoder);
    conn.connect(host, port, false);

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    bool sentControl = control.empty();
    AVFrame* last = nullptr;
    double firstFrameAt = -1;
    while (elapsed() < 30.0) {
        if (conn.state() == Connection::State::Failed) {
            fprintf(stderr, "FAIL: %s\n", conn.error().c_str());
            return 2;
        }
        if (AVFrame* f = decoder.takeLatest()) {
            if (firstFrameAt < 0) firstFrameAt = elapsed();
            av_frame_free(&last);
            last = f;
        }
        if (!sentControl && conn.helloSerial() > 0) {
            json j = json::parse(control, nullptr, false);
            if (!j.is_discarded()) conn.sendControl(j);
            sentControl = true;
        }
        if ((int)decoder.framesDecoded() >= frames && last) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (AVFrame* f = decoder.takeLatest()) {
        av_frame_free(&last);
        last = f;
    }
    double total = elapsed();
    json hello = conn.hello();
    json state = conn.phoneState();
    uint64_t decoded = decoder.framesDecoded();
    printf("{\"decoded\":%llu,\"received\":%llu,\"stream\":\"%dx%d\",\"decoder\":\"%s\",\"cameras\":%d,"
           "\"device\":\"%s\",\"firstFrameSec\":%.3f,\"fps\":%.1f,\"lastDecodeMs\":%.2f,\"phoneState\":%s}\n",
           (unsigned long long)decoded, (unsigned long long)conn.framesReceived(), conn.streamWidth(),
           conn.streamHeight(), decoder.description().c_str(),
           hello.is_object() && hello.contains("cameras") ? (int)hello["cameras"].size() : 0, (hello.is_object() && hello.contains("device") && hello["device"].is_string() ? hello["device"].get<std::string>() : std::string()).c_str(),
           firstFrameAt, firstFrameAt >= 0 && total > firstFrameAt ? decoded / (total - firstFrameAt) : 0.0,
           decoder.lastDecodeMs(), state.dump().c_str());

    int rc = (int)decoded >= frames && last ? 0 : 3;
    if (last && !png.empty()) {
        int w = last->width, h = last->height;
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        SwsContext* sws = sws_getContext(w, h, (AVPixelFormat)last->format, w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
        uint8_t* dst[1] = {rgb.data()};
        int stride[1] = {w * 3};
        sws_scale(sws, last->data, last->linesize, 0, h, dst, stride);
        sws_freeContext(sws);
        if (!stbi_write_png(png.c_str(), w, h, 3, rgb.data(), w * 3)) rc = 4;
    }
    av_frame_free(&last);
    conn.disconnect();
    if (rc != 0) fprintf(stderr, "FAIL: decoded %llu of %d frames\n", (unsigned long long)decoded, frames);
    return rc;
}

int runDiscover(int seconds) {
    plat::netInit();
    Discovery d;
    d.start();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    auto phones = d.phones();
    for (const auto& p : phones) printf("found %s at %s:%d\n", p.name.c_str(), p.ip.c_str(), p.port);
    d.stop();
    return phones.empty() ? 1 : 0;
}
