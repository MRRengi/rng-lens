#include "updater.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "json.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

namespace {

const char* kApi = "https://api.github.com/repos/MRRengi/rng-lens/releases/latest";

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// GET `url` (follows redirects). Writes into `out` or, with `file`, to that file. `onProgress(done, total)`.
template <class F>
bool httpGet(const std::string& url, std::string* out, FILE* file, std::string& err, F onProgress) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[1024];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        err = "bad update address";
        return false;
    }
    std::wstring fullPath = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"rng-lens-updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0) : nullptr;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags)
                    : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        DWORD total = 0;
        len = sizeof(total);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            err = "server answered " + std::to_string(status);
        } else {
            std::vector<char> buf(64 * 1024);
            uint64_t done = 0;
            ok = true;
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got)) {
                    ok = false;
                    err = "download interrupted";
                    break;
                }
                if (got == 0) break;
                if (out) out->append(buf.data(), got);
                if (file && fwrite(buf.data(), 1, got, file) != got) {
                    ok = false;
                    err = "could not write the file";
                    break;
                }
                done += got;
                onProgress(done, (uint64_t)total);
            }
        }
    } else if (!err.size()) {
        err = "no connection to GitHub";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}
#endif

}  // namespace

Updater::~Updater() { join(); }

void Updater::join() {
    if (worker_.joinable()) worker_.join();
}

void Updater::set(State s, const std::string& err) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!err.empty()) error_ = err;
    }
    state_ = s;
}

std::string Updater::latest() const {
    std::lock_guard<std::mutex> lock(m_);
    return latest_;
}

std::string Updater::error() const {
    std::lock_guard<std::mutex> lock(m_);
    return error_;
}

bool Updater::newer(const std::string& a, const std::string& b) {
    int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
    sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

void Updater::check(const std::string& current) {
#ifdef _WIN32
    State st = state_.load();
    if (st == State::Checking || st == State::Downloading) return;
    join();
    state_ = State::Checking;
    worker_ = std::thread([this, current] {
        const char* env = getenv("RNGLENS_UPDATE_URL");
        std::string body, err;
        if (!httpGet(env && *env ? env : kApi, &body, nullptr, err, [](uint64_t, uint64_t) {})) {
            set(State::Failed, err);
            return;
        }
        auto j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) {
            set(State::Failed, "unexpected answer from GitHub");
            return;
        }
        std::string tag = j["tag_name"].get<std::string>();
        if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);
        std::string url;
        if (j.contains("assets") && j["assets"].is_array())
            for (const auto& a : j["assets"]) {
                if (!a.is_object() || !a.contains("name") || !a["name"].is_string()) continue;
                std::string n = a["name"].get<std::string>();
                if (n.size() > 4 && n.rfind("rng-lens-setup", 0) == 0 && n.substr(n.size() - 4) == ".exe" &&
                    a.contains("browser_download_url") && a["browser_download_url"].is_string()) {
                    url = a["browser_download_url"].get<std::string>();
                    if (n == "rng-lens-setup.exe") break;
                }
            }
        {
            std::lock_guard<std::mutex> lock(m_);
            latest_ = tag;
            assetUrl_ = url;
        }
        set(newer(tag, current) && !url.empty() ? State::Available : State::UpToDate);
    });
#else
    (void)current;
    state_ = State::Unsupported;
#endif
}

void Updater::download() {
#ifdef _WIN32
    if (state_.load() != State::Available) return;
    join();
    state_ = State::Downloading;
    progress_ = 0.f;
    worker_ = std::thread([this] {
        std::string url, tag;
        {
            std::lock_guard<std::mutex> lock(m_);
            url = assetUrl_;
            tag = latest_;
        }
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        std::string path = std::string(tmp) + "rng-lens-setup-" + tag + ".exe";
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            set(State::Failed, "could not save the installer");
            return;
        }
        std::string err;
        bool ok = httpGet(url, nullptr, f, err, [this](uint64_t done, uint64_t total) {
            if (total) progress_ = (float)((double)done / (double)total);
        });
        fclose(f);
        if (!ok) {
            DeleteFileA(path.c_str());
            set(State::Failed, err);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            file_ = path;
        }
        progress_ = 1.f;
        set(State::Ready);
    });
#endif
}

bool Updater::fetch(const std::string& url, const std::string& path, std::string& err) {
#ifdef _WIN32
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        err = "could not save the file";
        return false;
    }
    bool ok = httpGet(url, nullptr, f, err, [](uint64_t, uint64_t) {});
    fclose(f);
    if (!ok) DeleteFileA(path.c_str());
    return ok;
#else
    (void)url;
    (void)path;
    err = "Windows only";
    return false;
#endif
}

bool Updater::launchInstaller() {
#ifdef _WIN32
    std::string f;
    {
        std::lock_guard<std::mutex> lock(m_);
        f = file_;
    }
    if (f.empty()) return false;
    // The installer asks for admin itself (UAC), closes the running rng-lens and offers to start the new one.
    HINSTANCE h = ShellExecuteW(nullptr, L"open", widen(f).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
#else
    return false;
#endif
}
