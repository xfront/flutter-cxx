#pragma once
/// @file widgets/video_player.h
/// @brief 视频播放控件族，对齐 上游 `video_player` 包的公开能力面。
///
/// 对应 上游源码：
///   packages/video_player/video_player/lib/video_player.dart
///
/// 分层与本仓的三层架构一致：
///   - 解码与播放时钟在 `foundation/video_decoder.h`（VideoPlayback）；
///   - `VideoPlayerController` 是本文件里的状态机（VideoPlayerValue + 每帧轮询）；
///   - `VideoPlayer` / `VideoProgressIndicator` / `VideoScrubber` 只画。
///
/// 与 上游 的分工差异（有意，写在这里免得被当 bug）：
///   1. 上游把 `play/pause/seekTo/getPosition` 交给平台插件；本仓的
///      `VideoPlayback` 自己走时钟，所以 controller 只做「转发 + 值同步」。
///   2. 上游的每一帧是 Texture/PlatformView；本仓的帧就是一张 `SkImage`，
///      `VideoPlayer` 直接交给 `Image` 控件绘制。
///   3. **音频按平台能力出声**：系统端与 FFmpeg 后端都会交出 PCM，出声设施在
///      `cui/video/audio_sink.h`（Windows WASAPI / Android AAudio / Apple
///      AudioQueue / Linux ALSA / OHOS OH_AudioRenderer），浏览器那一路自己出声；
///      这些路上 `setVolume` 会真的下发。设备打不开（没声卡 / 服务没跑）、或播的
///      是动图（恒无音轨）时 `volume` 仍然只是数值。
///   4. `buffered` 恒为空（本地解码没有「已缓冲区间」这一概念；
///      网络来源由原生后端内部缓冲，未上报）。
///   5. 上游用 `Timer.periodic(100ms)` 轮询平台位置；本仓用 `Ticker`
///      每帧轮询（取帧 + 位置一起同步），暂停且没有待补帧时自动停表。
///
/// 线程约定：controller 与控件只在 GUI 线程使用；`initialize()` 的后台
/// 打开走 `Application::runAsync`（没有 Application 时同步执行）。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <include/core/SkImage.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cui/framework/framework.h"
#include "cui/foundation/change_notifier.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/video/video_decoder.h"

namespace cui {

// ── VideoPlayerValue ────────────────────────────────────────────────────────

/// 一段已缓冲的时间区间（上游 `platform_interface.DurationRange`）。
struct DurationRange {
    std::chrono::microseconds start{0};
    std::chrono::microseconds end{0};

    bool operator==(const DurationRange&) const = default;
};

/// 播放器的完整状态。上游的 `VideoPlayerValue`。
///
/// 字段名与 上游 一一对应（Dart 的 `Duration` → `std::chrono::microseconds`，
/// `Size` → `Size`，可空的 `errorDescription` → `std::optional<std::string>`）。
struct VideoPlayerValue {
    std::chrono::microseconds duration{0};
    std::chrono::microseconds position{0};
    Size size{};
    std::vector<DurationRange> buffered;

    bool isInitialized = false;
    bool isPlaying = false;
    bool isLooping = false;
    bool isBuffering = false;
    bool isCompleted = false;
    double volume = 1.0;
    double playbackSpeed = 1.0;
    /// 顺时针补偿角 0/90/180/270（上游 `rotationCorrection`）。
    int rotationCorrection = 0;
    std::optional<std::string> errorDescription;
    bool preventsDisplaySleepDuringVideoPlayback = true;

    bool hasError() const { return errorDescription.has_value(); }

    /// 上游口径：未初始化 / 尺寸为 0 / 比例非正 ⇒ 1.0。
    double aspectRatio() const {
        if (!isInitialized || size.width <= 0.0 || size.height <= 0.0)
            return 1.0;
        const double ratio = size.width / size.height;
        return ratio > 0.0 ? ratio : 1.0;
    }

    bool operator==(const VideoPlayerValue& o) const {
        return duration == o.duration && position == o.position && size == o.size &&
               buffered == o.buffered && isInitialized == o.isInitialized &&
               isPlaying == o.isPlaying && isLooping == o.isLooping &&
               isBuffering == o.isBuffering && isCompleted == o.isCompleted &&
               volume == o.volume && playbackSpeed == o.playbackSpeed &&
               rotationCorrection == o.rotationCorrection &&
               errorDescription == o.errorDescription &&
               preventsDisplaySleepDuringVideoPlayback ==
                       o.preventsDisplaySleepDuringVideoPlayback;
    }
    bool operator!=(const VideoPlayerValue& o) const { return !(*this == o); }
};

// ── VideoPlayerController ───────────────────────────────────────────────────

/// 播放器状态机 + 解码引擎的门面。上游的 `VideoPlayerController`。
class VideoPlayerController : public ValueNotifier<VideoPlayerValue> {
public:
    /// 直接用解码核心的来源类型构造（最通用）。
    explicit VideoPlayerController(VideoSource source);

    ~VideoPlayerController() override;

    // 上游的四个具名构造器，这里用工厂表达（同样返回 shared_ptr，
    // 方便控件与后台任务共享生命周期）。
    static std::shared_ptr<VideoPlayerController> file(std::string path);
    static std::shared_ptr<VideoPlayerController> networkUrl(std::string url);
    /// 本仓没有资源包，asset 按当前工作目录的相对路径解析（与 File 同义）。
    static std::shared_ptr<VideoPlayerController> asset(std::string name);
    /// 内存来源（相机帧 / 测试注入的字节）。
    static std::shared_ptr<VideoPlayerController> memory(std::vector<uint8_t> bytes,
                                                         std::string formatHint = {});

    const std::string& dataSource() const { return source_.uri; }
    VideoSource::Type dataSourceType() const { return source_.type; }

    /// 打开数据源并读取元信息。异步（有 Application 时走线程池），
    /// 完成后通过 `value().isInitialized` / `errorDescription` 反映结果。
    void initialize();
    bool isInitialized() const { return value().isInitialized; }

    // ── 播放控制（对齐 上游 的方法语义） ──────────────────────────────────
    /// 播到末尾时再次播放会从 0 开始（上游行为）。
    void play();
    void pause();
    /// 夹到 `[0, duration]`。
    void seekTo(std::chrono::microseconds position);
    /// 夹到 `[0, 1]`，并下发给解码引擎（无声后端仍只记录数值）。
    void setVolume(double volume);
    /// speed <= 0 抛 `std::invalid_argument`（上游 ArgumentError）。
    void setPlaybackSpeed(double speed);
    void setLooping(bool looping);
    void setPreventsDisplaySleepDuringVideoPlayback(bool prevents);

    std::chrono::microseconds position() const { return value().position; }
    std::chrono::microseconds duration() const { return value().duration; }

    /// 最新一帧（没取到过帧时为空）。生命周期到下一次取帧/析构。
    const sk_sp<SkImage>& frame() const { return currentFrame_; }

    /// 停表、释放解码引擎。幂等；之后所有调用都是空操作。
    /// （与基类同名：基类版本只清监听器，本类版本额外收掉后台资源。）
    void dispose();

private:
    void poll();
    void ensureTicker();
    void stopTickerIfIdle();
    void applyInitialized(std::shared_ptr<VideoPlayback> playback, std::string error);

    VideoSource source_;
    std::shared_ptr<VideoPlayback> playback_;
    std::unique_ptr<Ticker> ticker_;
    sk_sp<SkImage> currentFrame_;
    uint64_t frameSerial_ = 0;
    bool initializing_ = false;
    bool disposed_ = false;
    /// 后台初始化回来时，controller 可能已经没了。
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

// ── VideoPlayer ─────────────────────────────────────────────────────────────

/// 把 controller 的当前帧画出来（含旋转补偿）。上游的 `VideoPlayer`。
///
/// 与上游一致：本控件只负责「画帧」，尺寸/比例由调用方包 `AspectRatio`。
class VideoPlayer : public StatefulWidget {
public:
    explicit VideoPlayer(std::shared_ptr<VideoPlayerController> controller,
                         KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), controller_(std::move(controller)) {}

    const std::shared_ptr<VideoPlayerController>& controller() const { return controller_; }
    void setController(std::shared_ptr<VideoPlayerController> controller) {
        controller_ = std::move(controller);
    }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "VideoPlayer"; }

private:
    std::shared_ptr<VideoPlayerController> controller_;
};

// ── VideoProgressColors ─────────────────────────────────────────────────────

/// 进度条配色。默认值与上游一致。
struct VideoProgressColors {
    Color playedColor = Color::fromRGBA(1.0, 0.0, 0.0, 0.7);
    Color bufferedColor = Color::fromRGBA(50.0 / 255.0, 50.0 / 255.0, 200.0 / 255.0, 0.2);
    Color backgroundColor = Color::fromRGBA(200.0 / 255.0, 200.0 / 255.0, 200.0 / 255.0, 0.5);
};

// ── VideoScrubber ───────────────────────────────────────────────────────────

/// 横向拖动/点击 seek 的包裹层。上游的 `VideoScrubber`。
///
/// 拖动开始时若正在播放则先暂停，松手后恢复（上游行为）。
class VideoScrubber : public StatefulWidget {
public:
    VideoScrubber(std::shared_ptr<VideoPlayerController> controller, SharedPtr<Widget> child,
                  KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), controller_(std::move(controller)),
          child_(std::move(child)) {}

    const std::shared_ptr<VideoPlayerController>& controller() const { return controller_; }
    const SharedPtr<Widget>& child() const { return child_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "VideoScrubber"; }

private:
    std::shared_ptr<VideoPlayerController> controller_;
    SharedPtr<Widget> child_;
};

// ── VideoProgressIndicator ──────────────────────────────────────────────────

/// 播放/缓冲进度条。上游的 `VideoProgressIndicator`。
///
/// 已初始化时画两条叠起来的线性进度条（缓冲在下、已播在上）；未初始化
/// 时画不确定进度条。`allowScrubbing` 为真时外面包 `VideoScrubber`。
class VideoProgressIndicator : public StatefulWidget {
public:
    explicit VideoProgressIndicator(std::shared_ptr<VideoPlayerController> controller,
                                    bool allowScrubbing = false,
                                    VideoProgressColors colors = VideoProgressColors(),
                                    EdgeInsets padding = EdgeInsets::only(0, 5, 0, 0),
                                    KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), controller_(std::move(controller)),
          allowScrubbing_(allowScrubbing), colors_(colors), padding_(padding) {}

    const std::shared_ptr<VideoPlayerController>& controller() const { return controller_; }
    bool allowScrubbing() const { return allowScrubbing_; }
    const VideoProgressColors& colors() const { return colors_; }
    const EdgeInsets& padding() const { return padding_; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "VideoProgressIndicator"; }

private:
    std::shared_ptr<VideoPlayerController> controller_;
    bool allowScrubbing_;
    VideoProgressColors colors_;
    EdgeInsets padding_;
};

// ── VideoControls ─────────────────────────────────────────────────────────

/// VCL 风格的视频播放控制条（扁平、跟随主题）。本仓自持，上游无对应。
///
/// 两行布局（对齐经典 media player 控制条）：
///   ① [⏪ 快退] [seek 轨道（可拖动）] [⏩ 快进]
///   ② [▶/⏸] [⏮ 上一] [⏹ 停止] [⏭ 下一] [📷 快照] …… [🔊/🔇] [音量滑条] [百分比]
///
/// 传输 / seek / 音量都直接接 `VideoPlayerController`；📷 快照把当前帧
/// 经 `encodePng` 编码后弹原生保存对话框写盘（平台不支持时静默返回）。
/// seek 拖动复用 `VideoScrubber`（拖动开始自动暂停、松手恢复）。
///
/// `overlay=true` 用于**叠在画面上**：半透明深底 + 浅色字形，压在任何一帧上
/// 都读得清；默认（摆在画面外）跟随主题配色。
///
/// 分工：本控件只管「长什么样 + 点了做什么」。「贴在画面底部 / 自动隐藏」是
/// 摆放策略，由调用方用 `Stack` + `Positioned` + `MouseRegion` + `AnimatedOpacity`
/// 拼（对齐上游：video_player 包只给控件，自动隐藏在 example app 里）。
class VideoControls : public StatefulWidget {
public:
    explicit VideoControls(std::shared_ptr<VideoPlayerController> controller,
                           KeyPtr key = nullptr, bool overlay = false)
        : StatefulWidget(std::move(key)), controller_(std::move(controller)),
          overlay_(overlay) {}

    const std::shared_ptr<VideoPlayerController>& controller() const { return controller_; }
    void setController(std::shared_ptr<VideoPlayerController> controller) {
        controller_ = std::move(controller);
    }

    /// 叠放模式（半透明深底 + 浅色字形）。
    bool overlay() const { return overlay_; }
    void setOverlay(bool overlay) { overlay_ = overlay; }

    std::unique_ptr<State> createState() override;

    std::string toStringShort() const override { return "VideoControls"; }

private:
    std::shared_ptr<VideoPlayerController> controller_;
    bool overlay_ = false;
};

}  // namespace cui
