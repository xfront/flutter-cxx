#pragma once
/// @file widgets/extended_image.h
/// @brief ExtendedImage —— pub.dev `extended_image` 的 `ExtendedImage` 控件移植。
///
/// 对应上游库的 `ExtendedImage`（base_extended_image.dart + extended_image.dart）：
///   - 三段占位：`loadingBuilder`（带下载进度）、`errorBuilder`、`frameBuilder`；
///   - 三档缓存（解码内存 / 编码内存 / 磁盘）由 `foundation/image_provider.h`
///     负责，本控件只做开关与 key 签名比对；
///   - 动画（GIF/WebP）逐帧播放 + `repeatCount`；
///   - `gaplessPlayback`：换图时先留着旧画面，避免图片墙里一闪而过白块；
///   - `onTap` / `onLongPress`（后者是框架级新增，见 widgets/gestures.h）。
///
/// 绘制完全复用 `cui_core::Image`（RenderImage 已支持 BoxFit + Alignment），本控件
/// **不新增渲染类**，所以是 header-only。
///
/// 三处有意偏离（上游行为与本仓机制对不上时的取舍）：
///   1. 上游的 `ExtendedImage` 通过 `ImageStream` 监听器拿帧；本仓 RenderObject
///      不订阅响应流，走「State 收结果 → setState → 重建 `cui_core::Image`」这条路
///      （与 widgets/transition.h 同一口径）。
///   2. 上游的**不确定**进度条是 Material 的无限旋转动画；本仓
///      `CircularProgressIndicator` 是一段静态弧（`value<0` 画整圈），所以未知
///      进度时默认占位画 30% 的弧，而不是整圈（整圈会被读成「已下载完」）。
///   3. 上游 `wasSynchronouslyLoaded` 指「构建时内存里已有这张图」；本仓同样
///      语义 = 加载结果在 `loadImage()` 返回前就送达（解码缓存命中 /
///      `DecodedImageProvider`），此时**不调 setState**（还在 mount/build 路径
///      上），直接让这一次 build 读到新字段。

#include <functional>
#include <include/core/SkImage.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/color.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/image_cache.h"
#include "cui/foundation/image_provider.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_icon.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/material.h"
#include "cui/widgets/paint.h"
#include "cui/widgets/ticker_mode.h"

namespace cui {

/// 加载状态（对齐上游 `ExtendedImageLoadState`）。
enum class ExtendedImageLoadState {
    None,      ///< 没有来源
    Waiting,   ///< 还没发起（本仓不排队，等于 None）
    Loading,   ///< 在飞
    Completed, ///< 已出图
    Failed,    ///< 失败（有 errorBuilder 就画它）
};

class ExtendedImage;
class ExtendedImageState;

// ── ExtendedImage ───────────────────────────────────────────────────────────

/// 带占位 / 进度 / 失败兜底 / 动画 / 缓存的图像控件。
class ExtendedImage : public StatefulWidget {
public:
    using TapCallback = std::function<void()>;
    /// `progress` 是 0..1；未知（服务端没给 Content-Length）时为 -1。
    using LoadingBuilder = std::function<SharedPtr<Widget>(BuildContext&, double progress)>;
    using ErrorBuilder = std::function<SharedPtr<Widget>(BuildContext&, const std::string& error)>;
    /// `frame` 是当前帧序号（静态图为 0）；`wasSync` = 结果同步送达。
    using FrameBuilder =
        std::function<SharedPtr<Widget>(BuildContext&, const SharedPtr<Widget>& image, int frame,
                                        bool wasSync)>;
    using ReadyCallback = std::function<void(const ImageLoadResult&)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using FrameCallback = std::function<void(int frame)>;

    /// 位置参数按「来源 → 尺寸 → 缓存 → 播放 → 占位 → 回调」分组，
    /// 新增参数只能追加在 `key` 之前（宿主都按位传参）。
    explicit ExtendedImage(std::shared_ptr<ImageProvider> provider = nullptr,
                           BoxFit fit = BoxFit::Contain,
                           const Alignment& alignment = Alignment::center(),
                           std::optional<double> width = std::nullopt,
                           std::optional<double> height = std::nullopt,
                           std::optional<int> cacheWidth = std::nullopt,
                           std::optional<int> cacheHeight = std::nullopt,
                           bool decodeAllFrames = true,
                           bool useMemoryCache = true,
                           bool useDiskCache = true,
                           bool gaplessPlayback = false,
                           bool animate = true,
                           int repeatCount = -1,
                           LoadingBuilder loadingBuilder = nullptr,
                           ErrorBuilder errorBuilder = nullptr,
                           FrameBuilder frameBuilder = nullptr,
                           TapCallback onTap = nullptr,
                           TapCallback onLongPress = nullptr,
                           ReadyCallback onReady = nullptr,
                           ErrorCallback onError = nullptr,
                           FrameCallback onFrameChanged = nullptr,
                           KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)),
          provider_(std::move(provider)),
          fit_(fit),
          alignment_(alignment),
          width_(width),
          height_(height),
          cacheWidth_(cacheWidth),
          cacheHeight_(cacheHeight),
          decodeAllFrames_(decodeAllFrames),
          useMemoryCache_(useMemoryCache),
          useDiskCache_(useDiskCache),
          gaplessPlayback_(gaplessPlayback),
          animate_(animate),
          repeatCount_(repeatCount),
          loadingBuilder_(std::move(loadingBuilder)),
          errorBuilder_(std::move(errorBuilder)),
          frameBuilder_(std::move(frameBuilder)),
          onTap_(std::move(onTap)),
          onLongPress_(std::move(onLongPress)),
          onReady_(std::move(onReady)),
          onError_(std::move(onError)),
          onFrameChanged_(std::move(onFrameChanged)) {}

    // ── 来源 ────────────────────────────────────────────────────────────────

    const std::shared_ptr<ImageProvider>& provider() const { return provider_; }
    void setProvider(std::shared_ptr<ImageProvider> p) { provider_ = std::move(p); }

    BoxFit fit() const { return fit_; }
    void setFit(BoxFit f) { fit_ = f; }

    const Alignment& alignment() const { return alignment_; }
    void setAlignment(const Alignment& a) { alignment_ = a; }

    const std::optional<double>& width() const { return width_; }
    void setWidth(std::optional<double> w) { width_ = w; }

    const std::optional<double>& height() const { return height_; }
    void setHeight(std::optional<double> h) { height_ = h; }

    /// 解码期降采样目标宽（像素）；同时是缓存 key 的一部分。
    const std::optional<int>& cacheWidth() const { return cacheWidth_; }
    void setCacheWidth(std::optional<int> w) { cacheWidth_ = w; }

    const std::optional<int>& cacheHeight() const { return cacheHeight_; }
    void setCacheHeight(std::optional<int> h) { cacheHeight_ = h; }

    /// false ⇒ 动图只解首帧（省内存，缩略图常用）。
    bool decodeAllFrames() const { return decodeAllFrames_; }
    void setDecodeAllFrames(bool v) { decodeAllFrames_ = v; }

    bool useMemoryCache() const { return useMemoryCache_; }
    void setUseMemoryCache(bool v) { useMemoryCache_ = v; }

    bool useDiskCache() const { return useDiskCache_; }
    void setUseDiskCache(bool v) { useDiskCache_ = v; }

    /// 换源时先保留旧画面（对齐上游 `gaplessPlayback`）。
    bool gaplessPlayback() const { return gaplessPlayback_; }
    void setGaplessPlayback(bool v) { gaplessPlayback_ = v; }

    /// 动图是否播放（false ⇒ 恒停在首帧）。
    bool animate() const { return animate_; }
    void setAnimate(bool v) { animate_ = v; }

    /// 循环次数：-1 = 无限（对齐 SkCodec 口径），0 = 不放，n = 播 n 轮后停在末帧。
    int repeatCount() const { return repeatCount_; }
    void setRepeatCount(int n) { repeatCount_ = n; }

    // ── 占位与回调 ──────────────────────────────────────────────────────────

    const LoadingBuilder& loadingBuilder() const { return loadingBuilder_; }
    void setLoadingBuilder(LoadingBuilder b) { loadingBuilder_ = std::move(b); }

    const ErrorBuilder& errorBuilder() const { return errorBuilder_; }
    void setErrorBuilder(ErrorBuilder b) { errorBuilder_ = std::move(b); }

    const FrameBuilder& frameBuilder() const { return frameBuilder_; }
    void setFrameBuilder(FrameBuilder b) { frameBuilder_ = std::move(b); }

    const TapCallback& onTap() const { return onTap_; }
    void setOnTap(TapCallback cb) { onTap_ = std::move(cb); }

    const TapCallback& onLongPress() const { return onLongPress_; }
    void setOnLongPress(TapCallback cb) { onLongPress_ = std::move(cb); }

    const ReadyCallback& onReady() const { return onReady_; }
    void setOnReady(ReadyCallback cb) { onReady_ = std::move(cb); }

    const ErrorCallback& onError() const { return onError_; }
    void setOnError(ErrorCallback cb) { onError_ = std::move(cb); }

    const FrameCallback& onFrameChanged() const { return onFrameChanged_; }
    void setOnFrameChanged(FrameCallback cb) { onFrameChanged_ = std::move(cb); }

    /// 决定「要不要重新发起加载」的签名：只有影响取字节/解码的字段进这里。
    /// 宿主常原地改同一个 Widget 对象，所以 `didUpdateWidget` 只能比这份快照。
    std::string loadSignature() const {
        std::string s = provider_ ? provider_->key() : std::string("<none>");
        s += '|';
        s += cacheWidth_ ? std::to_string(*cacheWidth_) : "-";
        s += '|';
        s += cacheHeight_ ? std::to_string(*cacheHeight_) : "-";
        s += '|';
        s += decodeAllFrames_ ? "all" : "first";
        s += '|';
        s += useMemoryCache_ ? "m" : "-";
        s += useDiskCache_ ? "d" : "-";
        return s;
    }

    // ── 常用入口（对齐上游 `ExtendedImage.network/file/memory`）──────────────

    /// 四个入口工厂共用的构造：把「来源 + 尺寸 + 缓存 + key」之外的参数留在
    /// 默认值上，需要占位/回调的调用方直接用主构造函数。
    static SharedPtr<ExtendedImage> make(std::shared_ptr<ImageProvider> provider, BoxFit fit,
                                         const Alignment& alignment, std::optional<double> width,
                                         std::optional<double> height, std::optional<int> cacheWidth,
                                         std::optional<int> cacheHeight, KeyPtr key) {
        return cui::make_shared<ExtendedImage>(std::move(provider), fit, alignment, width, height,
                                               cacheWidth, cacheHeight,
                                               /*decodeAllFrames=*/true, /*useMemoryCache=*/true,
                                               /*useDiskCache=*/true, /*gaplessPlayback=*/false,
                                               /*animate=*/true, /*repeatCount=*/-1,
                                               /*loadingBuilder=*/nullptr, /*errorBuilder=*/nullptr,
                                               /*frameBuilder=*/nullptr, /*onTap=*/nullptr,
                                               /*onLongPress=*/nullptr, /*onReady=*/nullptr,
                                               /*onError=*/nullptr, /*onFrameChanged=*/nullptr,
                                               std::move(key));
    }

    static SharedPtr<ExtendedImage> network(const std::string& url,
                                            BoxFit fit = BoxFit::Contain,
                                            const Alignment& alignment = Alignment::center(),
                                            std::optional<double> width = std::nullopt,
                                            std::optional<double> height = std::nullopt,
                                            std::optional<int> cacheWidth = std::nullopt,
                                            std::optional<int> cacheHeight = std::nullopt,
                                            HttpHeaders headers = {}, KeyPtr key = nullptr) {
        return make(std::make_shared<NetworkImageProvider>(url, std::move(headers)), fit, alignment,
                    width, height, cacheWidth, cacheHeight, std::move(key));
    }

    static SharedPtr<ExtendedImage> file(const std::string& path,
                                         BoxFit fit = BoxFit::Contain,
                                         const Alignment& alignment = Alignment::center(),
                                         std::optional<double> width = std::nullopt,
                                         std::optional<double> height = std::nullopt,
                                         std::optional<int> cacheWidth = std::nullopt,
                                         std::optional<int> cacheHeight = std::nullopt,
                                         KeyPtr key = nullptr) {
        return make(std::make_shared<FileImageProvider>(path), fit, alignment, width, height,
                    cacheWidth, cacheHeight, std::move(key));
    }

    static SharedPtr<ExtendedImage> memory(std::vector<uint8_t> bytes,
                                            std::string name = "memory",
                                            BoxFit fit = BoxFit::Contain,
                                            const Alignment& alignment = Alignment::center(),
                                            std::optional<double> width = std::nullopt,
                                            std::optional<double> height = std::nullopt,
                                            std::optional<int> cacheWidth = std::nullopt,
                                            std::optional<int> cacheHeight = std::nullopt,
                                            KeyPtr key = nullptr) {
        return make(std::make_shared<MemoryImageProvider>(std::move(bytes), std::move(name)), fit,
                    alignment, width, height, cacheWidth, cacheHeight, std::move(key));
    }

    /// 已解码结果直接回灌（编辑器 / 画笔的输出接回看图控件，省一次编解码往返）。
    static SharedPtr<ExtendedImage> decoded(DecodedImagePtr image,
                                            std::string name = "decoded",
                                            BoxFit fit = BoxFit::Contain,
                                            const Alignment& alignment = Alignment::center(),
                                            std::optional<double> width = std::nullopt,
                                            std::optional<double> height = std::nullopt,
                                            KeyPtr key = nullptr) {
        return make(std::make_shared<DecodedImageProvider>(std::move(image), std::move(name)), fit,
                    alignment, width, height, std::nullopt, std::nullopt, std::move(key));
    }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "ExtendedImage"; }

private:
    std::shared_ptr<ImageProvider> provider_;
    BoxFit fit_ = BoxFit::Contain;
    Alignment alignment_ = Alignment::center();
    std::optional<double> width_, height_;
    std::optional<int> cacheWidth_, cacheHeight_;
    bool decodeAllFrames_ = true;
    bool useMemoryCache_ = true;
    bool useDiskCache_ = true;
    bool gaplessPlayback_ = false;
    bool animate_ = true;
    int repeatCount_ = -1;
    LoadingBuilder loadingBuilder_;
    ErrorBuilder errorBuilder_;
    FrameBuilder frameBuilder_;
    TapCallback onTap_, onLongPress_;
    ReadyCallback onReady_;
    ErrorCallback onError_;
    FrameCallback onFrameChanged_;
};

// ── ExtendedImageState ──────────────────────────────────────────────────────

class ExtendedImageState : public State {
public:
    void initState() override {
        auto* w = widget_();
        signature_ = w ? w->loadSignature() : std::string();
        ticker_ = std::make_unique<Ticker>([this](Duration elapsed) { onPlayTick(elapsed); });
        if (w && w->provider())
            startLoad();
    }

    void didChangeDependencies() override {
        if (context() && ticker_)
            tickerMode_.bind(*context(), *ticker_);
    }

    void didUpdateWidget(cui::Widget* oldWidget) override {
        State::didUpdateWidget(oldWidget);
        auto* w = widget_();
        if (!w)
            return;
        const std::string sig = w->loadSignature();
        if (sig != signature_) {
            signature_ = sig;
            startLoad();
        }
    }

    void dispose() override {
        // 取消 = 结果闭包再也不执行（image_provider 的 deliverResults 按
        // request->cancelled() 跳过），所以闭包里裸捕 this 是安全的。
        if (request_) {
            request_->cancel();
            request_.reset();
        }
        tickerMode_.unbind();
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        ticker_.reset();
    }

    // ── 对外状态（PhotoView / ImageEditor 从这里读当前帧）───────────────────

    const DecodedImagePtr& decodedImage() const { return image_; }
    int frame() const { return frame_; }
    /// 当前帧的位图；未出图返回空。
    sk_sp<SkImage> frameImage() const {
        if (!image_ || image_->frames.empty())
            return nullptr;
        const size_t i = static_cast<size_t>(frame_) < image_->frames.size()
                             ? static_cast<size_t>(frame_)
                             : image_->frames.size() - 1;
        return image_->frames[i].image;
    }

    double progress() const { return progress_; }
    const std::string& error() const { return error_; }
    bool isAnimating() const { return playing_; }
    /// 报的是「这一次加载 attempt」的状态：`gaplessPlayback` 换源时旧画面还在
    /// `image_` 里（build 会照画），但状态仍是 Loading —— 与上游一致。
    ExtendedImageLoadState loadState() const {
        if (loading_)
            return ExtendedImageLoadState::Loading;
        if (image_ && image_->valid())
            return ExtendedImageLoadState::Completed;
        if (failed_)
            return ExtendedImageLoadState::Failed;
        auto* w = widget_();
        return (w && w->provider()) ? ExtendedImageLoadState::Waiting : ExtendedImageLoadState::None;
    }

    /// 暂停 / 继续动图播放（PhotoView 双击放大时用）。
    void setAnimating(bool animate) {
        if (animate) {
            // 播完 repeatCount 轮之后再次「继续」= 重新来一轮，
            // 所以先解除 finished_ 闩锁（syncPlayback 会把 loops_ 归零）。
            finished_ = false;
            syncPlayback();
        } else {
            stopPlayback();
        }
        if (mounted())
            setState();
    }

    /// 重新发起加载。`clearCache=true` 会先丢掉本图的解码 / 编码内存条目，
    /// 于是网络图会真的重下一遍（磁盘字节仍在，所以不贵）。
    void reload(bool clearCache = false) {
        auto* w = widget_();
        if (!w || !w->provider())
            return;
        if (clearCache) {
            if (!cacheKey_.empty())
                DecodedImageCache::instance().remove(cacheKey_);
            EncodedDataCache::instance().remove(w->provider()->diskId());
        }
        startLoad();
        if (mounted())
            setState();
    }

    SharedPtr<Widget> build(BuildContext& context) override {
        auto* w = widget_();
        if (!w)
            return nullptr;
        synchronousDelivery_ = false;
        syncPlayback();

        // 出图优先于占位：`gaplessPlayback` 换源时旧画面还在 `image_` 里，
        // 这一刻状态是 Loading，但画面上不该闪一下白（上游同款）。
        const bool hasPicture = image_ && image_->valid();
        SharedPtr<Widget> content;
        if (hasPicture) {
            auto img = cui::make_shared<cui::Image>(frameImage());
            img->setFit(w->fit());
            img->setAlignment(w->alignment());
            img->setWidth(w->width());
            img->setHeight(w->height());
            SharedPtr<Widget> inner = img;
            if (const auto& fb = w->frameBuilder())
                inner = fb(context, inner, frame_, wasSync_);
            content = std::move(inner);
        } else if (failed_) {
            content = w->errorBuilder() ? w->errorBuilder()(context, error_)
                                        : defaultError(context);
        } else if (w->provider()) {
            content = w->loadingBuilder() ? w->loadingBuilder()(context, progress_)
                                          : defaultLoading(context);
        } else {
            content = cui::make_shared<SizedBox>(0.0, 0.0);
        }

        // 占位画面也要占住调用方要求的尺寸，否则加载中 ↔ 出图之间会跳版。
        if (!hasPicture && (w->width() || w->height())) {
            content = cui::make_shared<ConstrainedBox>(
                BoxConstraints::tightFor(w->width(), w->height()), std::move(content));
        }

        if (!w->onTap() && !w->onLongPress())
            return content;
        return cui::make_shared<GestureDetector>(std::move(content), w->onTap(), nullptr, nullptr,
                                                 nullptr, nullptr, nullptr, nullptr,
                                                 w->onLongPress());
    }

private:
    ExtendedImage* widget_() const { return static_cast<ExtendedImage*>(widget()); }

    // ── 加载 ────────────────────────────────────────────────────────────────

    void startLoad() {
        auto* w = widget_();
        if (!w)
            return;
        if (request_) {
            request_->cancel();
            request_.reset();
        }
        if (!w->gaplessPlayback()) {
            image_.reset();
            frame_ = 0;
            stopPlayback();
        }
        failed_ = false;
        error_.clear();
        progress_ = -1.0;
        wasSync_ = false;
        finished_ = false;
        loops_ = 0;
        if (!w->provider()) {
            loading_ = false;
            return;
        }

        ImageRequestOptions opts;
        opts.cacheWidth = w->cacheWidth();
        opts.cacheHeight = w->cacheHeight();
        opts.decodeAllFrames = w->decodeAllFrames();
        opts.useMemoryCache = w->useMemoryCache();
        opts.useDiskCache = w->useDiskCache();

        loading_ = true;
        // 缓存命中/decoded provider 会在返回前就把结果打过来；这段窗口里
        // 不能 setState（还在 mount/didUpdateWidget 路径上），置位后由
        // handleResult 自己识别。
        synchronousDelivery_ = true;
        request_ = loadImage(w->provider(), opts,
                             [this](const ImageLoadResult& r) { handleResult(r); },
                             [this](const HttpProgress& p) { handleProgress(p); });
        synchronousDelivery_ = false;
        // 缓存 key 由加载器拼（含尺寸后缀），reload() 要靠它精确删条目。
        if (request_)
            cacheKey_ = request_->cacheKey();
    }

    void handleResult(const ImageLoadResult& result) {
        auto* w = widget_();
        const bool sync = synchronousDelivery_;
        loading_ = false;
        request_.reset();
        if (result.ok()) {
            image_ = result.image;
            frame_ = 0;
            failed_ = false;
            error_.clear();
            wasSync_ = sync;
            progress_ = 1.0;
        } else {
            image_.reset();
            stopPlayback();
            failed_ = true;
            error_ = result.error.empty() ? std::string("image load failed") : result.error;
        }
        // 同步送达（缓存命中）发生在 mount / didUpdateWidget 路径上，紧接着
        // 就有一次 build 会读到新字段，所以这里不能再 setState。
        if (!sync && mounted())
            setState();
        if (w) {
            if (result.ok()) {
                if (const auto& cb = w->onReady())
                    cb(result);
            } else if (const auto& cb = w->onError()) {
                cb(error_);
            }
        }
        syncPlayback();
    }

    void handleProgress(const HttpProgress& p) {
        const double fraction = p.fraction();
        if (fraction == progress_)
            return;
        progress_ = fraction;
        if (mounted())
            setState();
    }

    // ── 动图播放 ────────────────────────────────────────────────────────────

    double frameDurationMs(size_t i) const {
        if (!image_ || i >= image_->frames.size())
            return 100.0;
        const int d = image_->frames[i].durationMs;
        return d > 0 ? double(d) : 100.0;
    }

    bool playable() const {
        auto* w = widget_();
        return w && w->animate() && w->repeatCount() != 0 && !finished_ && image_ &&
               image_->animated && image_->frames.size() > 1;
    }

    /// 按当前 widget 参数决定是否让 ticker 跑（build 里调，覆盖宿主改参数的情况）。
    void syncPlayback() {
        if (!ticker_)
            return;
        const bool shouldPlay = playable();
        if (shouldPlay && !playing_) {
            playing_ = true;
            frameClockMs_ = 0.0;
            loops_ = 0;
            lastElapsedMs_ = 0.0;
            ticker_->start();
        } else if (!shouldPlay && playing_) {
            stopPlayback();
        }
    }

    void stopPlayback() {
        playing_ = false;
        frameClockMs_ = 0.0;
        if (ticker_ && ticker_->isActive())
            ticker_->stop();
        lastElapsedMs_ = 0.0;
    }

    void onPlayTick(Duration elapsed) {
        auto* w = widget_();
        if (!mounted() || !image_ || image_->frames.size() < 2) {
            stopPlayback();
            return;
        }
        double dt = elapsed.count() - lastElapsedMs_;
        lastElapsedMs_ = elapsed.count();
        if (dt < 0.0)
            dt = 0.0;

        const size_t count = image_->frames.size();
        frameClockMs_ += dt;
        int changed = 0;
        // 一帧里可能吃掉多个 0/短延时帧（GIF 允许 1ms 延时），上限防止死循环。
        while (changed < int(count) + 2) {
            const double dur = frameDurationMs(static_cast<size_t>(frame_));
            if (frameClockMs_ < dur)
                break;
            frameClockMs_ -= dur;
            ++frame_;
            ++changed;
            if (size_t(frame_) >= count) {
                frame_ = 0;
                ++loops_;
                const int repeat = w ? w->repeatCount() : -1;
                if (repeat >= 0 && loops_ >= repeat) {
                    // 播完指定轮数：停在末帧（上游 AnimatedImage 同形）。
                    // finished_ 是闩锁：stopPlayback 会把 playing_ 清掉，
                    // 下一次 build 的 syncPlayback 会当成「暂停后恢复」重起表。
                    frame_ = int(count) - 1;
                    finished_ = true;
                    stopPlayback();
                    break;
                }
            }
        }
        if (changed > 0) {
            setState();
            if (w && w->onFrameChanged())
                w->onFrameChanged()(frame_);
        }
    }

    // ── 默认占位 ────────────────────────────────────────────────────────────

    /// 不确定进度画 30% 的弧（本仓的进度条是静态弧，整圈会被读成「下载完」）。
    SharedPtr<Widget> defaultLoading(BuildContext& context) {
        (void)context;
        const double value = progress_ >= 0.0 ? progress_ : 0.3;
        auto ring = cui::make_shared<CircularProgressIndicator>(
            std::optional<double>(value), Color::fromRGBA(0, 0, 0, 0.12), theme().primary, 3.0);
        return cui::make_shared<Center>(cui::make_shared<SizedBox>(24.0, 24.0, std::move(ring)));
    }

    SharedPtr<Widget> defaultError(BuildContext& context) {
        (void)context;
        return cui::make_shared<Center>(
            cui::make_shared<Icon>(IconCode::Image, 32.0, theme().textDisabled));
    }

    DecodedImagePtr image_;
    std::shared_ptr<ImageRequest> request_;
    std::string cacheKey_;
    std::string signature_;
    std::string error_;
    int frame_ = 0;
    int loops_ = 0;
    double progress_ = -1.0;
    bool loading_ = false;
    bool failed_ = false;
    bool wasSync_ = false;
    /// `loadImage()` 正在同步回调（缓存命中）⇒ 这一次不许 setState。
    bool synchronousDelivery_ = false;

    bool playing_ = false;
    /// 已播完 `repeatCount` 轮的闩锁（否则 build 里的 syncPlayback 会重起表）。
    bool finished_ = false;
    double frameClockMs_ = 0.0;
    double lastElapsedMs_ = 0.0;
    std::unique_ptr<Ticker> ticker_;
    TickerModeBinding tickerMode_;
};

inline std::unique_ptr<State> ExtendedImage::createState() {
    return std::make_unique<ExtendedImageState>();
}

}  // namespace cui_core
