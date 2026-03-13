#pragma once
/// @file foundation/image_provider.h
/// @brief 图像的来源（Provider）+ 异步加载器 + 可插拔 HTTP 传输。
///
/// 对齐 pub.dev `extended_image` 的 `ImageProvider`/`BaseExtendedImageProvider`
/// 家族与 `ExtendedImage.network/file/asset/memory` 四个入口，但把「谁去拿字节」
/// 拆成三段，方便在桌面 / 移动 / 测试三种环境各接一头：
///
///   - `ImageProvider`：只回答「这张图的字节从哪来」（内存 / 文件 / 网络）。
///   - `HttpGetFn`：网络传输本身。Windows 默认实现是 WinHTTP；其他平台由宿主
///     `setHttpTransport()` 注入（Android/iOS 走原生 http，Web 走 fetch，
///     测试注入 fake）。**cui_foundation 不依赖 cui_platform**，所以通道版
///     传输由宿主安装，不在这里。
///   - `setBackgroundExecutor()` / `setUiDispatcher()`：线程接缝。默认接
///     `Application` 的线程池与 `postTask`；没有 Application（headless 测试、
///     离屏渲染）时同步执行，于是测试完全确定。
///
/// `loadImage()` 的语义：
///   1. 解码缓存命中 ⇒ **同步**回调（`fromCache=true`），不起线程。
///   2. 同一个 key 已在飞 ⇒ 挂到同一次请求上（图片墙里同一张图出现两次不会
///      下两遍）。
///   3. 取消只影响回调：后台任务会尽量提前退出（http 分块读时检查标志），
///      但已解出来的结果仍然进缓存。

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <include/core/SkData.h>

#include "cui/foundation/image_data.h"

namespace cui {

// ── 取消令牌 ────────────────────────────────────────────────────────────────

class ImageCancellation {
public:
    void cancel() { cancelled_.store(true); }
    bool cancelled() const { return cancelled_.load(); }

private:
    std::atomic<bool> cancelled_{false};
};

// ── HTTP 传输 ───────────────────────────────────────────────────────────────

/// 下载进度；`totalBytes < 0` 表示服务端没给 Content-Length。
struct HttpProgress {
    int64_t receivedBytes = 0;
    int64_t totalBytes = -1;

    /// 0..1；未知总长时为 -1（UI 该转成不确定进度条）。
    double fraction() const {
        return totalBytes > 0 ? double(receivedBytes) / double(totalBytes) : -1.0;
    }
};

/// 网络请求头（Content-Type 之外的自定义头，如 Authorization）。
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

/// 传输层签名。返回 true 表示成功（`outBody` 是响应体）；失败时写 `error`。
/// 实现必须能在任意线程被调用，并且周期性检查 `cancel`。
using HttpGetFn =
    std::function<bool(const std::string& url, const HttpHeaders& headers,
                       std::vector<uint8_t>& outBody,
                       const std::function<void(const HttpProgress&)>& onProgress,
                       const std::shared_ptr<ImageCancellation>& cancel, std::string& error)>;

/// 安装传输层；传空函数恢复内置默认（Windows=WinHTTP，其他平台=「没有网络后端」）。
void setHttpTransport(HttpGetFn fn);
/// 当前生效的传输层（可能是内置默认）。返回副本，因此可以从任意线程调用。
/// 空函数表示这个平台拿不到网络图。
HttpGetFn currentHttpTransport();
/// 是否有可用的网络后端（决定 `NetworkImageProvider` 能不能工作）。
bool hasHttpTransport();

// ── Provider ────────────────────────────────────────────────────────────────

/// 图像来源。
class ImageProvider {
public:
    virtual ~ImageProvider() = default;

    /// 缓存标识；不同来源必须不撞（所以都带 scheme 前缀）。
    virtual std::string key() const = 0;

    /// 字节是否值得落盘（网络图 true；本地文件/内存字节 false）。
    virtual bool diskCacheable() const { return false; }

    /// 已经是解码结果时直接返回它（编辑器/画笔的输出回灌给看图控件走这条路，
    /// 省一次「编码再解码」的往返）。返回空表示需要走 `loadBytes()`。
    virtual DecodedImagePtr decoded() const { return nullptr; }

    /// 取字节（在后台线程调用）。返回 false 时写 `error`。
    virtual bool loadBytes(std::vector<uint8_t>& outBytes,
                           const std::function<void(const HttpProgress&)>& onProgress,
                           const std::shared_ptr<ImageCancellation>& cancel,
                           std::string& error) const = 0;

    /// 磁盘缓存的 key（默认 = key()）。
    virtual std::string diskId() const { return key(); }

    /// 调试/日志用的来源名（"network"/"file"/"memory"）。
    virtual const char* sourceName() const = 0;
};

/// 内存字节（例如相机帧、剪贴板图片、单元测试注入的假响应）。
class MemoryImageProvider : public ImageProvider {
public:
    MemoryImageProvider(std::vector<uint8_t> bytes, std::string name = "memory");
    explicit MemoryImageProvider(const sk_sp<SkData>& data, std::string name = "memory");

    std::string key() const override { return "memory:" + name_; }
    bool loadBytes(std::vector<uint8_t>& out, const std::function<void(const HttpProgress&)>&,
                   const std::shared_ptr<ImageCancellation>&, std::string&) const override;
    const char* sourceName() const override { return "memory"; }

    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    std::string name_;
};

/// 已解码图像的来源：编辑器 / 画笔的输出直接回灌给看图控件，
/// 省掉「编码成 PNG 再解码」这一趟往返。
class DecodedImageProvider : public ImageProvider {
public:
    DecodedImageProvider(DecodedImagePtr image, std::string name = "decoded");

    std::string key() const override { return "decoded:" + name_; }
    DecodedImagePtr decoded() const override { return image_; }
    bool loadBytes(std::vector<uint8_t>&, const std::function<void(const HttpProgress&)>&,
                   const std::shared_ptr<ImageCancellation>&, std::string& error) const override;
    const char* sourceName() const override { return "decoded"; }

private:
    DecodedImagePtr image_;
    std::string name_;
};

/// 本地文件（绝对/相对路径）。
class FileImageProvider : public ImageProvider {
public:
    explicit FileImageProvider(std::string path);

    std::string key() const override { return "file:" + path_; }
    bool loadBytes(std::vector<uint8_t>& out, const std::function<void(const HttpProgress&)>&,
                   const std::shared_ptr<ImageCancellation>&, std::string&) const override;
    const char* sourceName() const override { return "file"; }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

/// 网络图像（http/https）。落盘缓存默认开，TTL 由 `setDiskCacheMaxAgeDays` 决定。
class NetworkImageProvider : public ImageProvider {
public:
    explicit NetworkImageProvider(std::string url, HttpHeaders headers = {});

    std::string key() const override { return "net:" + url_; }
    bool diskCacheable() const override { return true; }
    bool loadBytes(std::vector<uint8_t>& out, const std::function<void(const HttpProgress&)>& onProgress,
                   const std::shared_ptr<ImageCancellation>& cancel, std::string&) const override;
    const char* sourceName() const override { return "network"; }

    const std::string& url() const { return url_; }
    const HttpHeaders& headers() const { return headers_; }

private:
    std::string url_;
    HttpHeaders headers_;
};

// ── 加载器 ──────────────────────────────────────────────────────────────────

struct ImageRequestOptions {
    /// 解码目标尺寸（等比缩小）；命中缓存时也是缓存 key 的一部分。
    std::optional<int> cacheWidth;
    std::optional<int> cacheHeight;
    /// false ⇒ 动画只解首帧。
    bool decodeAllFrames = true;
    bool useMemoryCache = true;
    bool useDiskCache = true;
};

struct ImageLoadResult {
    DecodedImagePtr image;   ///< 失败时为空
    std::string error;       ///< 非空表示失败
    bool fromCache = false;  ///< 命中解码缓存（没有起后台任务）
    std::string source;      ///< "memory" / "file" / "network"
    int64_t bytes = 0;       ///< 编码字节数（缓存命中时为 0）

    bool ok() const { return error.empty() && image && image->valid(); }
};

using ImageLoadListener = std::function<void(const ImageLoadResult&)>;
using ImageProgressListener = std::function<void(const HttpProgress&)>;

/// 一次进行中的加载。句柄本身是 `std::shared_ptr`，可安全跨线程弱引用。
class ImageRequest {
public:
    const std::string& cacheKey() const { return cacheKey_; }
    bool done() const { return done_.load(); }
    bool cancelled() const { return cancel_->cancelled(); }
    /// 取消：之后的回调不再送达（已在 GUI 队列里的会被丢弃）。
    /// 同一 key 的等待者全取消时，加载器会连带中止这次取字节（见 cancelHook）。
    void cancel() {
        cancel_->cancel();
        if (cancelHook_)
            cancelHook_();
    }

    /// 由加载器安装（弱引用在飞分组，避免 request→group→request 成环）。
    void setCancelHook(std::function<void()> hook) { cancelHook_ = std::move(hook); }

    /// 由加载器调用（做成公开方法而不是 friend，省一处耦合）。
    void markDone() { done_.store(true); }

    explicit ImageRequest(std::string cacheKey) : cacheKey_(std::move(cacheKey)) {}

private:
    std::string cacheKey_;
    std::shared_ptr<ImageCancellation> cancel_ = std::make_shared<ImageCancellation>();
    std::function<void()> cancelHook_;
    std::atomic<bool> done_{false};
};

/// 发起加载。回调一律在 GUI 线程执行（无 Application 时同步执行在调用线程）。
/// 返回的句柄可以扔掉——扔掉就等于不取消。
std::shared_ptr<ImageRequest> loadImage(const std::shared_ptr<ImageProvider>& provider,
                                        const ImageRequestOptions& options,
                                        ImageLoadListener onDone,
                                        ImageProgressListener onProgress = {});

/// 同 key 的在飞请求数（测试用）。
size_t inflightImageRequests();
/// 丢弃所有在飞请求的监听者（不取消后台任务本身），用于宿主整体拆卸。
void cancelAllImageRequests();

// ── 线程接缝 ────────────────────────────────────────────────────────────────

using ImageTask = std::function<void()>;

/// 后台执行器；默认用 `Application` 的线程池，没有 Application 时同步执行。
/// 传空函数恢复默认。测试里安装「同步执行器」即可确定性地跑完整条链路。
void setBackgroundExecutor(std::function<void(ImageTask)> executor);
/// GUI 投递器；默认 `Application::postTask`，没有 Application 时同步执行。
void setUiDispatcher(std::function<void(ImageTask)> dispatcher);

}  // namespace cui_core
