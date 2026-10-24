// Test-bench app for the Windows virtual camera: does what Zoom/OBS do through DirectShow.
// Lists video capture devices, opens "rng-lens", pulls a frame through a Sample Grabber and saves it.
//   dshow-probe.exe [out.bmp] [seconds]
#include <dshow.h>
#include <initguid.h>
#include <qedit.h>
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

DEFINE_GUID(CLSID_SampleGrabberRL, 0xC1F400A0, 0x3F08, 0x11d3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(CLSID_NullRendererRL, 0xC1F400A4, 0x3F08, 0x11d3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);

// Copies every delivered sample; works even where SetBufferSamples is flaky.
struct Grab : public ISampleGrabberCB {
    std::vector<char> last;
    long count = 0;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
            *ppv = this;
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE* buf, long len) override {
        last.assign((char*)buf, (char*)buf + len);
        count++;
        return S_OK;
    }
};

#define CHECK(x, what)                                             \
    do {                                                           \
        HRESULT hr_ = (x);                                         \
        if (FAILED(hr_)) {                                         \
            printf("FAIL %s: 0x%08lx\n", what, (unsigned long)hr_); \
            return 1;                                              \
        }                                                          \
    } while (0)

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "probe.bmp";
    int seconds = argc > 2 ? atoi(argv[2]) : 3;
    CoInitialize(nullptr);

    ICreateDevEnum* devEnum = nullptr;
    CHECK(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&devEnum), "device enum");
    IEnumMoniker* monikers = nullptr;
    HRESULT hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0);
    if (hr != S_OK) {
        printf("FAIL no video devices at all\n");
        return 1;
    }
    IBaseFilter* src = nullptr;
    IMoniker* m = nullptr;
    while (monikers->Next(1, &m, nullptr) == S_OK) {
        IPropertyBag* bag = nullptr;
        if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&bag))) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr))) {
                char name[256];
                WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1, name, sizeof(name), nullptr, nullptr);
                printf("device: %s\n", name);
                if (!src && std::string(name) == "rng-lens") m->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&src);
                VariantClear(&v);
            }
            bag->Release();
        }
        m->Release();
    }
    if (!src) {
        printf("FAIL rng-lens camera not found\n");
        return 1;
    }

    IGraphBuilder* graph = nullptr;
    CHECK(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&graph), "graph");
    ICaptureGraphBuilder2* cgb = nullptr;
    CHECK(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&cgb), "capture builder");
    cgb->SetFiltergraph(graph);
    CHECK(graph->AddFilter(src, L"rng-lens"), "add source");

    IAMStreamConfig* cfg = nullptr;
    if (SUCCEEDED(cgb->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, src, IID_IAMStreamConfig, (void**)&cfg))) {
        AM_MEDIA_TYPE* mt = nullptr;
        if (SUCCEEDED(cfg->GetFormat(&mt)) && mt->formattype == FORMAT_VideoInfo) {
            VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)mt->pbFormat;
            printf("format: %ldx%ld %d bpp, %.1f fps\n", vih->bmiHeader.biWidth, vih->bmiHeader.biHeight,
                   vih->bmiHeader.biBitCount, vih->AvgTimePerFrame ? 1e7 / vih->AvgTimePerFrame : 0.0);
        }
        cfg->Release();
    }

    IBaseFilter* grabberF = nullptr;
    CHECK(CoCreateInstance(CLSID_SampleGrabberRL, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&grabberF), "sample grabber");
    ISampleGrabber* grabber = nullptr;
    CHECK(grabberF->QueryInterface(IID_ISampleGrabber, (void**)&grabber), "grabber iface");
    AM_MEDIA_TYPE want{};
    want.majortype = MEDIATYPE_Video;
    want.subtype = MEDIASUBTYPE_RGB24;
    grabber->SetMediaType(&want);
    grabber->SetBufferSamples(FALSE);
    static Grab cb;
    grabber->SetCallback(&cb, 1);
    CHECK(graph->AddFilter(grabberF, L"grabber"), "add grabber");
    IBaseFilter* nullR = nullptr;
    CHECK(CoCreateInstance(CLSID_NullRendererRL, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&nullR), "null renderer");
    CHECK(graph->AddFilter(nullR, L"null"), "add null");
    // Connect pins directly (like Chrome/Zoom do) instead of letting the capture builder add a Smart Tee.
    auto pinOf = [](IBaseFilter* f, PIN_DIRECTION want) -> IPin* {
        IEnumPins* e = nullptr;
        IPin* p = nullptr;
        f->EnumPins(&e);
        while (e && e->Next(1, &p, nullptr) == S_OK) {
            PIN_DIRECTION d;
            p->QueryDirection(&d);
            if (d == want) break;
            p->Release();
            p = nullptr;
        }
        if (e) e->Release();
        return p;
    };
    IPin* srcOut = pinOf(src, PINDIR_OUTPUT);
    if (!srcOut) {
        printf("FAIL camera has no output pin\n");
        return 1;
    }
    CHECK(graph->ConnectDirect(srcOut, pinOf(grabberF, PINDIR_INPUT), nullptr), "connect camera -> grabber");
    CHECK(graph->ConnectDirect(pinOf(grabberF, PINDIR_OUTPUT), pinOf(nullR, PINDIR_INPUT), nullptr), "connect grabber -> null");

    AM_MEDIA_TYPE got{};
    CHECK(grabber->GetConnectedMediaType(&got), "connected type");
    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)got.pbFormat;
    int w = vih->bmiHeader.biWidth, h = vih->bmiHeader.biHeight;

    IMediaControl* ctl = nullptr;
    CHECK(graph->QueryInterface(IID_IMediaControl, (void**)&ctl), "media control");
    HRESULT runHr = ctl->Run();
    printf("run: 0x%08lx\n", (unsigned long)runHr);
    fflush(stdout);
    for (int i = 0; i < seconds * 2; i++) {
        Sleep(500);
        OAFilterState fs = 0;
        ctl->GetState(0, &fs);
        printf("t=%.1fs state=%ld samples=%ld\n", (i + 1) * 0.5, (long)fs, cb.count);
        fflush(stdout);
    }
    printf("samples delivered: %ld\n", cb.count);
    fflush(stdout);
    ctl->Stop();
    if (cb.last.empty()) {
        printf("FAIL no samples\n");
        return 1;
    }
    std::vector<char> buf = cb.last;
    long size = (long)buf.size();

    // Mean colour as a quick "is there a picture" signal.
    double sum[3] = {0, 0, 0};
    for (long i = 0; i + 2 < size; i += 3)
        for (int c = 0; c < 3; c++) sum[c] += (unsigned char)buf[i + c];
    long px = size / 3;
    printf("frame: %dx%d, %ld bytes, mean BGR %.0f %.0f %.0f\n", w, h, size, sum[0] / px, sum[1] / px, sum[2] / px);

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER bi = vih->bmiHeader;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + size;
    FILE* f = fopen(out, "wb");
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(buf.data(), 1, size, f);
    fclose(f);
    printf("OK saved %s\n", out);
    return 0;
}
