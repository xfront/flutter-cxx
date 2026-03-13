/// @file foundation/image_provider.cpp — Provider / 加载器 / WinHTTP 传输实现。
#include "cui/foundation/image_provider.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "cui/foundation/application.h"
#include "cui/foundation/image_cache.h"
#include "cui/utils/thread_pool.h"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define CUI_HAS_WINHTTP 1
#include <windows.h>
#include <winhttp.h>
#endif

namespace cui {

namespace {

// ── 可注入的接缝 ────────────────────────────────────────────────────────────

std::mutex& seamMutex() {
    static std::mutex m;
    return m;
}

HttpGetFn& transportSlot() {
    static HttpGetFn fn;
    return fn;
}

std::function<void(ImageTask)>& backgroundSlot() {
    static std::function<void(ImageTask)> fn;
    return fn;
}

std::function<void(ImageTask)>& dispatcherSlot() {
    static std::function<void(ImageTask)> fn;
    return fn;
}

#if defined(CUI_HAS_WINHTTP)
/// 定义在本文件末尾的 WinHTTP 传输（同一个匿名命名空间，前置声明即可）。
bool winHttpGet(const std::string& url, const HttpHeaders& headers, std::vector<uint8_t>& out,
                const std::function<void(const HttpProgress&)>& onProgress,
                const std::shared_ptr<ImageCancellation>& cancel, std::string& error);
#endif

/// 内置默认传输层（没有内置后端的平台返回空函数 ⇒ 网络图不可用，
/// 由宿主自己 `setHttpTransport()`）。
const HttpGetFn& builtinTransport() {
    static const HttpGetFn fn = [] {
#if defined(CUI_HAS_WINHTTP)
        return [](const std::string& url, const HttpHeaders& headers, std::vector<uint8_t>& out,
                  const std::function<void(const HttpProgress&)>& onProgress,
                  const std::shared_ptr<ImageCancellation>& cancel, std::string& error) {
            return winHttpGet(url, headers, out, onProgress, cancel, error);
        };
#else
        return HttpGetFn{};
#endif
    }();
    return fn;
}

void dispatchBackground(ImageTask task) {
    std::function<void(ImageTask)> custom;
    {
        std::lock_guard<std::mutex> lock(seamMutex());
        custom = backgroundSlot();
    }
    if (custom) {
        custom(std::move(task));
        return;
    }
    if (Application* app = Application::instance()) {
        try {
            app->threadPool().submit(std::move(task));
            return;
        } catch (const std::exception&) {
            // 池已停（进程退出中）⇒ 退回同步执行，至少不会丢结果。
        }
    }
    task();
}

void dispatchUi(ImageTask task) {
    std::function<void(ImageTask)> custom;
    {
        std::lock_guard<std::mutex> lock(seamMutex());
        custom = dispatcherSlot();
    }
    if (custom) {
        custom(std::move(task));
        return;
    }
    if (Application* app = Application::instance()) {
        app->postTask(std::move(task));
        return;
    }
    task();
}

// ── 在飞请求去重 ────────────────────────────────────────────────────────────

struct Waiter {
    std::shared_ptr<ImageRequest> request;
    ImageLoadListener onDone;
    /// 进度监听按等待者各自保存：某一个等待者取消后它的闭包就再不执行
    /// （分组级共享的话，先析构的那半会留着悬垂 `this` 被下一次进度打到）。
    ImageProgressListener onProgress;
};

struct RequestGroup {
    std::string key;
    std::shared_ptr<ImageCancellation> cancellation = std::make_shared<ImageCancellation>();
    std::vector<Waiter> waiters;
    /// 进度合并：至少隔 32KB 才发一次，避免大图把 UI 线程刷爆。
    int64_t lastReportedBytes = 0;
};

std::mutex& inflightMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::shared_ptr<RequestGroup>>& inflightSlot() {
    static std::unordered_map<std::string, std::shared_ptr<RequestGroup>> map;
    return map;
}

/// 缓存 key：来源 + 目标尺寸。不同尺寸是不同条目（缩略图不会顶掉原图）。
std::string makeCacheKey(const ImageProvider& provider, const ImageRequestOptions& options) {
    std::string key = provider.key();
    if (options.cacheWidth)
        key += "|w" + std::to_string(*options.cacheWidth);
    if (options.cacheHeight)
        key += "|h" + std::to_string(*options.cacheHeight);
    if (!options.decodeAllFrames)
        key += "|still";
    return key;
}

/// 把结果派发给这一组的所有等待者（GUI 线程）。已取消的静默丢弃。
void deliverResults(const std::string& key, const ImageLoadResult& result) {
    std::vector<Waiter> waiters;
    {
        std::lock_guard<std::mutex> lock(inflightMutex());
        auto it = inflightSlot().find(key);
        if (it != inflightSlot().end()) {
            waiters = std::move(it->second->waiters);
            inflightSlot().erase(it);
        }
    }
    for (Waiter& w : waiters) {
        if (!w.onDone || w.request->cancelled())
            continue;
        w.request->markDone();
        w.onDone(result);
    }
}

void reportProgress(const std::shared_ptr<RequestGroup>& group, const HttpProgress& p) {
    if (p.totalBytes > 0 && p.receivedBytes - group->lastReportedBytes < 32768 &&
        p.receivedBytes < p.totalBytes)
        return;
    group->lastReportedBytes = p.receivedBytes;
    // 只把这一条进度交给**仍存活**的等待者；已取消的静默丢弃。
    std::vector<Waiter> snapshot;
    {
        std::lock_guard<std::mutex> lock(inflightMutex());
        auto it = inflightSlot().find(group->key);
        if (it == inflightSlot().end())
            return;
        snapshot = it->second->waiters;
    }
    HttpProgress copy = p;
    dispatchUi([snapshot, copy] {
        for (const Waiter& w : snapshot) {
            if (w.request->cancelled() || !w.onProgress)
                continue;
            w.onProgress(copy);
        }
    });
}

/// 分组里所有等待者都取消了 ⇒ 中止取字节（在飞表已被别处持有锁时不要再调）。
void installCancelHook(const std::shared_ptr<ImageRequest>& request,
                       const std::shared_ptr<RequestGroup>& group) {
    std::weak_ptr<RequestGroup> weak = group;
    request->setCancelHook([weak] {
        std::shared_ptr<RequestGroup> g = weak.lock();
        if (!g)
            return;
        std::lock_guard<std::mutex> lock(inflightMutex());
        bool anyAlive = false;
        for (const Waiter& w : g->waiters) {
            if (!w.request->cancelled()) {
                anyAlive = true;
                break;
            }
        }
        if (!anyAlive)
            g->cancellation->cancel();
    });
}

/// 后台真正干活的部分。
void runImageJob(std::shared_ptr<ImageProvider> provider, ImageRequestOptions options,
                 std::string cacheKey, std::shared_ptr<RequestGroup> group) {
    ImageLoadResult result;
    result.source = provider->sourceName();

    const bool preDecoded = provider->decoded() != nullptr;
    std::vector<uint8_t> bytes;
    if (!preDecoded) {
        // 1) 编码字节的内存缓存
        if (options.useMemoryCache)
            if (sk_sp<SkData> mem = EncodedDataCache::instance().get(provider->diskId())) {
                const uint8_t* p = static_cast<const uint8_t*>(mem->data());
                bytes.assign(p, p + mem->size());
            }
        // 2) 磁盘缓存
        if (bytes.empty() && provider->diskCacheable() && options.useDiskCache)
            bytes = readDiskCache(provider->diskId());
    }
    // 3) 真正去取
    if (!preDecoded && bytes.empty()) {
        std::string error;
        const bool ok = provider->loadBytes(
            bytes, [&](const HttpProgress& p) { reportProgress(group, p); }, group->cancellation,
            error);
        if (!ok || bytes.empty()) {
            result.error = error.empty() ? "no image data" : error;
            dispatchUi([cacheKey, result] { deliverResults(cacheKey, result); });
            return;
        }
        if (provider->diskCacheable() && options.useDiskCache)
            writeDiskCache(provider->diskId(), bytes.data(), bytes.size());
        if (options.useMemoryCache)
            EncodedDataCache::instance().put(provider->diskId(),
                                             SkData::MakeWithCopy(bytes.data(), bytes.size()));
    }

    if (group->cancellation->cancelled()) {
        // 全部等待者都跑了：结果照样进缓存，但不再派发给 UI。
        dispatchUi([cacheKey] {
            std::lock_guard<std::mutex> lock(inflightMutex());
            inflightSlot().erase(cacheKey);
        });
        return;
    }

    // 4) 解码
    if (preDecoded) {
        result.image = provider->decoded();
    } else {
        DecodeOptions d;
        d.targetWidth = options.cacheWidth;
        d.targetHeight = options.cacheHeight;
        d.decodeAllFrames = options.decodeAllFrames;
        result.image = decodeImage(bytes, d);
    }
    result.bytes = int64_t(bytes.size());
    if (!result.image || !result.image->valid()) {
        result.error = result.image && !result.image->error.empty() ? result.image->error
                                                                   : "image decode failed";
        result.image.reset();
        dispatchUi([cacheKey, result] { deliverResults(cacheKey, result); });
        return;
    }
    DecodedImageCache::instance().put(cacheKey, result.image);
    dispatchUi([cacheKey, result] { deliverResults(cacheKey, result); });
}

}  // namespace

// ── 传输层接缝 ──────────────────────────────────────────────────────────────

void setHttpTransport(HttpGetFn fn) {
    std::lock_guard<std::mutex> lock(seamMutex());
    transportSlot() = std::move(fn);
}

HttpGetFn currentHttpTransport() {
    std::lock_guard<std::mutex> lock(seamMutex());
    if (transportSlot())
        return transportSlot();
    return builtinTransport();
}

bool hasHttpTransport() { return bool(currentHttpTransport()); }

void setBackgroundExecutor(std::function<void(ImageTask)> executor) {
    std::lock_guard<std::mutex> lock(seamMutex());
    backgroundSlot() = std::move(executor);
}

void setUiDispatcher(std::function<void(ImageTask)> dispatcher) {
    std::lock_guard<std::mutex> lock(seamMutex());
    dispatcherSlot() = std::move(dispatcher);
}

// ── Provider ────────────────────────────────────────────────────────────────

MemoryImageProvider::MemoryImageProvider(std::vector<uint8_t> bytes, std::string name)
    : bytes_(std::move(bytes)), name_(std::move(name)) {}

MemoryImageProvider::MemoryImageProvider(const sk_sp<SkData>& data, std::string name)
    : name_(std::move(name)) {
    if (data) {
        const uint8_t* p = static_cast<const uint8_t*>(data->data());
        bytes_.assign(p, p + data->size());
    }
}

bool MemoryImageProvider::loadBytes(std::vector<uint8_t>& out,
                                    const std::function<void(const HttpProgress&)>& onProgress,
                                    const std::shared_ptr<ImageCancellation>&,
                                    std::string& error) const {
    if (bytes_.empty()) {
        error = "memory image is empty";
        return false;
    }
    out = bytes_;
    if (onProgress)
        onProgress(HttpProgress{int64_t(bytes_.size()), int64_t(bytes_.size())});
    return true;
}

DecodedImageProvider::DecodedImageProvider(DecodedImagePtr image, std::string name)
    : image_(std::move(image)), name_(std::move(name)) {}

bool DecodedImageProvider::loadBytes(std::vector<uint8_t>&,
                                     const std::function<void(const HttpProgress&)>&,
                                     const std::shared_ptr<ImageCancellation>&,
                                     std::string& error) const {
    error = "decoded provider has no bytes";
    return false;
}

FileImageProvider::FileImageProvider(std::string path) : path_(std::move(path)) {}

bool FileImageProvider::loadBytes(std::vector<uint8_t>& out,
                                  const std::function<void(const HttpProgress&)>& onProgress,
                                  const std::shared_ptr<ImageCancellation>&,
                                  std::string& error) const {
    sk_sp<SkData> data = SkData::MakeFromFileName(path_.c_str());
    if (!data) {
        error = "cannot read file: " + path_;
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data->data());
    out.assign(p, p + data->size());
    if (onProgress)
        onProgress(HttpProgress{int64_t(out.size()), int64_t(out.size())});
    return true;
}

NetworkImageProvider::NetworkImageProvider(std::string url, HttpHeaders headers)
    : url_(std::move(url)), headers_(std::move(headers)) {}

bool NetworkImageProvider::loadBytes(std::vector<uint8_t>& out,
                                     const std::function<void(const HttpProgress&)>& onProgress,
                                     const std::shared_ptr<ImageCancellation>& cancel,
                                     std::string& error) const {
    HttpGetFn get = currentHttpTransport();
    if (!get) {
        error = "no http backend installed for this platform";
        return false;
    }
    return get(url_, headers_, out, onProgress, cancel, error);
}

// ── 加载器 ──────────────────────────────────────────────────────────────────

std::shared_ptr<ImageRequest> loadImage(const std::shared_ptr<ImageProvider>& provider,
                                        const ImageRequestOptions& options, ImageLoadListener onDone,
                                        ImageProgressListener onProgress) {
    if (!provider) {
        auto req = std::make_shared<ImageRequest>("");
        ImageLoadResult result;
        result.error = "null image provider";
        if (onDone)
            onDone(result);
        req->markDone();
        return req;
    }

    const std::string cacheKey = makeCacheKey(*provider, options);
    auto request = std::make_shared<ImageRequest>(cacheKey);

    // 1) 已是解码结果 / 解码缓存命中 ⇒ 同步返回，不起线程。
    DecodedImagePtr cached;
    if (provider->decoded())
        cached = provider->decoded();
    else if (options.useMemoryCache)
        cached = DecodedImageCache::instance().get(cacheKey);
    if (cached && cached->valid()) {
        ImageLoadResult result;
        result.image = std::move(cached);
        result.source = provider->sourceName();
        result.fromCache = !provider->decoded();
        request->markDone();
        if (onDone)
            onDone(result);
        return request;
    }

    // 2) 同 key 在飞 ⇒ 挂上去。
    std::shared_ptr<RequestGroup> group;
    bool startJob = false;
    {
        std::lock_guard<std::mutex> lock(inflightMutex());
        auto it = inflightSlot().find(cacheKey);
        if (it != inflightSlot().end()) {
            group = it->second;
        } else {
            group = std::make_shared<RequestGroup>();
            group->key = cacheKey;
            inflightSlot().emplace(cacheKey, group);
            startJob = true;
        }
        group->waiters.push_back(Waiter{request, std::move(onDone), std::move(onProgress)});
    }
    installCancelHook(request, group);

    if (startJob) {
        auto providerCopy = provider;
        dispatchBackground([providerCopy, options, cacheKey, group] {
            runImageJob(providerCopy, options, cacheKey, group);
        });
    }
    return request;
}

size_t inflightImageRequests() {
    std::lock_guard<std::mutex> lock(inflightMutex());
    return inflightSlot().size();
}

void cancelAllImageRequests() {
    // cancel() 会触发分组的取消钩子，钩子要再拿一次在飞表锁 ⇒ 先把等待者
    // 搬出表再解锁，锁外调用，避免自死锁。
    std::vector<std::shared_ptr<RequestGroup>> groups;
    {
        std::lock_guard<std::mutex> lock(inflightMutex());
        groups.reserve(inflightSlot().size());
        for (auto& kv : inflightSlot())
            groups.push_back(std::move(kv.second));
        inflightSlot().clear();
    }
    for (std::shared_ptr<RequestGroup>& g : groups) {
        for (Waiter& w : g->waiters)
            w.request->cancel();
        g->cancellation->cancel();
    }
}

// ── WinHTTP 内置传输 ────────────────────────────────────────────────────────

#if defined(CUI_HAS_WINHTTP)
namespace {

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 443;
    bool https = false;
};

std::wstring toWide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

bool parseUrl(const std::string& url, ParsedUrl& out) {
    std::string rest = url;
    if (rest.rfind("https://", 0) == 0) {
        out.https = true;
        out.port = INTERNET_DEFAULT_HTTPS_PORT;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        out.https = false;
        out.port = INTERNET_DEFAULT_HTTP_PORT;
        rest = rest.substr(7);
    } else {
        return false;
    }
    const size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? std::wstring(L"/") : toWide(rest.substr(slash));
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.port = INTERNET_PORT(std::atoi(authority.c_str() + colon + 1));
        authority = authority.substr(0, colon);
    }
    if (authority.empty())
        return false;
    out.host = toWide(authority);
    return true;
}

/// WinHTTP 的宽字符缓冲区不能是空串，用 L" " 顶位（对齐官方示例）。
const wchar_t* safeWide(const std::wstring& s) { return s.empty() ? L" " : s.c_str(); }

bool winHttpGet(const std::string& url, const HttpHeaders& headers, std::vector<uint8_t>& out,
                const std::function<void(const HttpProgress&)>& onProgress,
                const std::shared_ptr<ImageCancellation>& cancel, std::string& error) {
    ParsedUrl parsed;
    if (!parseUrl(url, parsed)) {
        error = "unsupported url (only http/https): " + url;
        return false;
    }
    HINTERNET session = WinHttpOpen(L"cui_core/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "WinHttpOpen failed";
        return false;
    }
    HINTERNET connection = WinHttpConnect(session, safeWide(parsed.host), parsed.port, 0);
    if (!connection) {
        error = "WinHttpConnect failed";
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", safeWide(parsed.path), nullptr,
                                          nullptr, nullptr, parsed.https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        error = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    // 自签名证书在本地测试服务器上很常见：接受任何证书（图片下载不承担
    // 认证职责；要更严的宿主自己安装传输层）。
    if (parsed.https) {
        DWORD security = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &security, sizeof(security));
    }
    WinHttpSetTimeouts(request, 15000, 15000, 30000, 60000);

    std::wstring headerBlob;
    for (const auto& h : headers) {
        headerBlob += toWide(h.first);
        headerBlob += L": ";
        headerBlob += toWide(h.second);
        headerBlob += L"\r\n";
    }
    if (!headerBlob.empty() &&
        !WinHttpAddRequestHeaders(request, headerBlob.c_str(), DWORD(headerBlob.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD)) {
        error = "WinHttpAddRequestHeaders failed";
    }

    bool ok = false;
    std::vector<uint8_t> body;
    int64_t total = -1;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                           0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        wchar_t lenBuf[32] = {0};
        DWORD lenSize = sizeof(lenBuf);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX))
            total = int64_t(_wtoi64(lenBuf));

        if (status >= 200 && status < 300) {
            ok = true;
            for (;;) {
                if (cancel && cancel->cancelled()) {
                    ok = false;
                    error = "cancelled";
                    break;
                }
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    ok = false;
                    error = "WinHttpQueryDataAvailable failed";
                    break;
                }
                if (available == 0)
                    break;
                const size_t before = body.size();
                body.resize(before + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, body.data() + before, available, &read)) {
                    ok = false;
                    error = "WinHttpReadData failed";
                    break;
                }
                body.resize(before + read);
                if (onProgress)
                    onProgress(HttpProgress{int64_t(body.size()), total});
            }
        } else {
            char msg[64];
            std::snprintf(msg, sizeof(msg), "http status %lu", unsigned(status));
            error = msg;
        }
    } else {
        const DWORD e = GetLastError();
        char msg[64];
        std::snprintf(msg, sizeof(msg), "winhttp error %lu", unsigned(e));
        error = msg;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (ok)
        out = std::move(body);
    return ok && !out.empty();
}

}  // namespace

#endif

}  // namespace cui_core
