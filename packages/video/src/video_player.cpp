/// @file widgets/video_player.cpp
/// @brief VideoPlayerController 的状态机实现 + 三个控件。
///
/// 解码与播放时钟在 foundation/video_decoder.{h,cpp}；本文件只把
/// 「引擎状态」翻译成上游 `VideoPlayerValue`，并把帧交给 `Image` 绘制。

#include "cui/video/video_player.h"

#include "cui/foundation/application.h"
#include "cui/foundation/image_data.h"
#include "cui/platform/services/file_dialog.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/icons.h"
#include "cui/widgets/interactive.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/material.h"
#include "cui/widgets/paint.h"
#include "cui/widgets/text.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace cui {
namespace {

std::chrono::microseconds clampToRange(std::chrono::microseconds value,
                                       std::chrono::microseconds duration) {
    if (value.count() < 0)
        return std::chrono::microseconds(0);
    if (duration.count() > 0 && value > duration)
        return duration;
    return value;
}

}  // namespace

// ── VideoPlayerController ───────────────────────────────────────────────────

VideoPlayerController::VideoPlayerController(VideoSource source) : source_(std::move(source)) {}

VideoPlayerController::~VideoPlayerController() { dispose(); }

std::shared_ptr<VideoPlayerController> VideoPlayerController::file(std::string path) {
    return std::make_shared<VideoPlayerController>(VideoSource::file(std::move(path)));
}

std::shared_ptr<VideoPlayerController> VideoPlayerController::networkUrl(std::string url) {
    return std::make_shared<VideoPlayerController>(VideoSource::network(std::move(url)));
}

std::shared_ptr<VideoPlayerController> VideoPlayerController::asset(std::string name) {
    return std::make_shared<VideoPlayerController>(VideoSource::asset(std::move(name)));
}

std::shared_ptr<VideoPlayerController> VideoPlayerController::memory(std::vector<uint8_t> bytes,
                                                                     std::string formatHint) {
    return std::make_shared<VideoPlayerController>(
            VideoSource::memory(std::move(bytes), std::move(formatHint)));
}

void VideoPlayerController::initialize() {
    if (initializing_ || disposed_ || playback_ || value().isInitialized)
        return;
    initializing_ = true;

    // 后台线程只碰 VideoSource（值类型）；回到 GUI 线程再动 value。
    auto done = [this, alive = alive_](std::shared_ptr<VideoPlayback> playback,
                                       std::string error) {
        if (!alive->load()) {
            if (playback)
                playback->dispose();
            return;
        }
        applyInitialized(std::move(playback), std::move(error));
    };

    const VideoSource source = source_;
    if (Application* app = Application::instance()) {
        app->runAsync(
                [source] {
                    std::string error;
                    auto playback = VideoPlayback::create(source, error);
                    return std::make_pair(std::move(playback), std::move(error));
                },
                [done](std::pair<std::shared_ptr<VideoPlayback>, std::string> result) {
                    done(std::move(result.first), std::move(result.second));
                });
    } else {
        // headless 测试 / 无事件循环：同步打开。
        std::string error;
        auto playback = VideoPlayback::create(source, error);
        done(std::move(playback), std::move(error));
    }
}

void VideoPlayerController::applyInitialized(std::shared_ptr<VideoPlayback> playback,
                                             std::string error) {
    initializing_ = false;
    if (disposed_) {
        if (playback)
            playback->dispose();
        return;
    }
    if (!playback) {
        VideoPlayerValue v = value();
        v.errorDescription = error.empty() ? "视频打开失败" : std::move(error);
        setValue(std::move(v));
        return;
    }

    playback_ = std::move(playback);
    const VideoInfo& info = playback_->info();

    VideoPlayerValue v = value();
    v.duration = info.duration;
    v.size = Size(double(info.width), double(info.height));
    v.rotationCorrection = info.rotation;
    v.isInitialized = true;
    v.isCompleted = false;
    v.errorDescription.reset();
    setValue(std::move(v));

    playback_->setLooping(value().isLooping);
    playback_->setPlaybackSpeed(value().playbackSpeed);
    playback_->setVolume(value().volume);
    // 上游 initialize 之后画面停在首帧：seek 到 0，引擎在暂停态补出一帧。
    playback_->seekTo(std::chrono::microseconds(0));
    ensureTicker();
}

void VideoPlayerController::play() {
    if (!playback_ || disposed_)
        return;
    // 上游：播到末尾再 play 会从 0 开始。
    if (value().duration.count() > 0 && value().position >= value().duration) {
        seekTo(std::chrono::microseconds(0));
        if (disposed_ || !playback_)
            return;
    }

    VideoPlayerValue v = value();
    v.isPlaying = true;
    v.isCompleted = false;
    setValue(std::move(v));

    playback_->play();
    ensureTicker();
}

void VideoPlayerController::pause() {
    if (!playback_ || disposed_)
        return;
    playback_->pause();

    VideoPlayerValue v = value();
    v.isPlaying = false;
    setValue(std::move(v));
    stopTickerIfIdle();
}

void VideoPlayerController::seekTo(std::chrono::microseconds position) {
    if (!playback_ || disposed_)
        return;
    position = clampToRange(position, value().duration);
    playback_->seekTo(position);

    VideoPlayerValue v = value();
    v.position = position;
    v.isCompleted = !v.isLooping && v.duration.count() > 0 && position >= v.duration;
    setValue(std::move(v));
    ensureTicker();
}

void VideoPlayerController::setVolume(double volume) {
    if (disposed_)
        return;
    volume = std::clamp(volume, 0.0, 1.0);
    VideoPlayerValue v = value();
    v.volume = volume;
    setValue(std::move(v));
    // 上游只把数值交给平台插件；本仓直接下发给解码引擎（没引擎时等于只记录）。
    if (playback_)
        playback_->setVolume(volume);
}

void VideoPlayerController::setPlaybackSpeed(double speed) {
    if (disposed_)
        return;
    if (speed <= 0.0)
        throw std::invalid_argument("VideoPlayerController::setPlaybackSpeed: speed 必须大于 0");
    VideoPlayerValue v = value();
    v.playbackSpeed = speed;
    setValue(std::move(v));
    if (playback_)
        playback_->setPlaybackSpeed(speed);
}

void VideoPlayerController::setLooping(bool looping) {
    if (disposed_)
        return;
    VideoPlayerValue v = value();
    v.isLooping = looping;
    if (looping)
        v.isCompleted = false;
    setValue(std::move(v));
    if (playback_)
        playback_->setLooping(looping);
}

void VideoPlayerController::setPreventsDisplaySleepDuringVideoPlayback(bool prevents) {
    if (disposed_)
        return;
    VideoPlayerValue v = value();
    v.preventsDisplaySleepDuringVideoPlayback = prevents;
    setValue(std::move(v));
}

void VideoPlayerController::dispose() {
    if (disposed_)
        return;
    disposed_ = true;
    alive_->store(false);
    if (ticker_) {
        ticker_->stop();
        ticker_.reset();
    }
    if (playback_) {
        playback_->dispose();
        playback_.reset();
    }
    currentFrame_.reset();
    ChangeNotifier::dispose();
}

void VideoPlayerController::ensureTicker() {
    if (disposed_ || !playback_)
        return;
    if (!ticker_)
        ticker_ = std::make_unique<Ticker>([this](Duration) { poll(); });
    if (!ticker_->isActive())
        ticker_->start();
}

void VideoPlayerController::stopTickerIfIdle() {
    if (!ticker_ || !ticker_->isActive())
        return;
    if (value().isPlaying)
        return;
    if (playback_ && playback_->hasPendingFrame())
        return;
    ticker_->stop();
}

void VideoPlayerController::poll() {
    if (disposed_)
        return;
    if (!playback_) {
        if (ticker_)
            ticker_->stop();
        return;
    }

    bool frameChanged = false;
    VideoFrame frame;
    uint64_t serial = frameSerial_;
    // 泵驱动后端（Web）没有工作线程，取帧就在这一拍里做；其他平台 no-op。
    playback_->pumpStep();
    if (playback_->takeLatestFrame(frame, serial)) {
        frameSerial_ = serial;
        currentFrame_ = std::move(frame.image);
        frameChanged = true;
    }

    VideoPlayerValue v = value();
    const VideoPlayerValue before = v;
    // 泵驱动后端（Web）打开时元数据还没到（尺寸/长度要等 `loadedmetadata`），
    // 所以这里得跟着 `info()` 补一次；其他后端的 info 打开后恒定 ⇒ 不改变。
    const VideoInfo& info = playback_->info();
    if (v.duration != info.duration)
        v.duration = info.duration;
    const Size natural(double(info.width), double(info.height));
    if (v.size != natural)
        v.size = natural;
    if (v.rotationCorrection != info.rotation)
        v.rotationCorrection = info.rotation;
    v.position = clampToRange(playback_->position(), v.duration);
    v.isPlaying = playback_->isPlaying();
    // 上游 `_updatePosition`：isCompleted = position == duration；循环中不置位。
    v.isCompleted = !v.isLooping && v.duration.count() > 0 && v.position >= v.duration;

    const std::string error = playback_->error();
    if (!error.empty()) {
        v.errorDescription = error;
        v.isPlaying = false;
    }

    if (v != before)
        setValue(std::move(v));
    else if (frameChanged)
        notifyListeners();  // 值没变但画面换了（暂停中 seek 补帧）

    stopTickerIfIdle();
}

// ── VideoPlayer ─────────────────────────────────────────────────────────────

namespace {

class VideoPlayerState : public State {
public:
    void initState() override {
        controller_ = static_cast<VideoPlayer*>(widget())->controller();
        attach();
    }

    void didUpdateWidget(Widget* oldWidget) override {
        (void)oldWidget;
        auto* w = static_cast<VideoPlayer*>(widget());
        if (w->controller() != controller_) {
            detach();
            controller_ = w->controller();
            attach();
        }
    }

    void dispose() override { detach(); }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        if (!controller_)
            return cui::make_shared<SizedBox>(0.0, 0.0);

        const VideoPlayerValue value = controller_->value();
        if (!value.isInitialized || !controller_->frame())
            return cui::make_shared<SizedBox>(0.0, 0.0);

        auto image = cui::make_shared<Image>(controller_->frame());
        image->setFit(BoxFit::Contain);

        SharedPtr<Widget> child = image;
        const int quarters = (value.rotationCorrection % 360) / 90;
        if (quarters != 0)
            child = cui::make_shared<RotatedBox>(quarters, child);
        return child;
    }

private:
    void attach() {
        if (!controller_ || controller_->disposed())
            return;
        handle_ = controller_->addListenerWithHandle([this] {
            if (mounted())
                setState();
        });
    }

    void detach() {
        if (controller_ && handle_ != 0)
            controller_->removeListenerByHandle(handle_);
        handle_ = 0;
    }

    std::shared_ptr<VideoPlayerController> controller_;
    int handle_ = 0;
};

}  // namespace

std::unique_ptr<State> VideoPlayer::createState() { return std::make_unique<VideoPlayerState>(); }

// ── VideoScrubber ───────────────────────────────────────────────────────────

namespace {

class VideoScrubberState : public State {
public:
    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto* w = static_cast<VideoScrubber*>(widget());
        auto controller = w->controller();

        auto onTapDown = [this, controller](const PointerEvent& e) {
            seekFromLocalX(controller, e.position.dx);
        };
        auto onPanUpdate = [this, controller](const PointerEvent& e) {
            if (!scrubbing_) {
                // 上游在「拖动真正开始」时暂停；本仓 pan 的 slop 判定在第一
                // 个 update 之前完成，因此这里等价于 onHorizontalDragStart。
                scrubbing_ = true;
                wasPlaying_ = controller && controller->value().isPlaying;
                if (wasPlaying_)
                    controller->pause();
            }
            seekFromLocalX(controller, e.position.dx);
        };
        auto onPanEnd = [this, controller](const PointerEvent&) {
            if (!scrubbing_)
                return;
            scrubbing_ = false;
            if (!controller)
                return;
            const auto v = controller->value();
            if (wasPlaying_ && v.position < v.duration)
                controller->play();
            wasPlaying_ = false;
        };

        return cui::make_shared<GestureDetector>(
                w->child(), GestureDetector::TapCallback(), onTapDown,
                GestureDetector::PointerCallback(), GestureDetector::PointerCallback(),
                onPanUpdate, onPanEnd);
    }

private:
    void seekFromLocalX(const std::shared_ptr<VideoPlayerController>& controller, double dx) {
        if (!controller || controller->disposed())
            return;
        RenderObject* ro = element() ? element()->findRenderObject() : nullptr;
        if (!ro)
            return;
        const double width = ro->size().width;
        const double durationUs = double(controller->value().duration.count());
        if (width <= 0.0 || durationUs <= 0.0)
            return;
        const double relative = std::clamp(dx / width, 0.0, 1.0);
        controller->seekTo(std::chrono::microseconds(int64_t(durationUs * relative)));
    }

    bool scrubbing_ = false;
    bool wasPlaying_ = false;
};

}  // namespace

std::unique_ptr<State> VideoScrubber::createState() {
    return std::make_unique<VideoScrubberState>();
}

// ── VideoProgressIndicator ──────────────────────────────────────────────────

namespace {

class VideoProgressIndicatorState : public State {
public:
    void initState() override {
        controller_ = static_cast<VideoProgressIndicator*>(widget())->controller();
        attach();
    }

    void didUpdateWidget(Widget* oldWidget) override {
        (void)oldWidget;
        auto* w = static_cast<VideoProgressIndicator*>(widget());
        if (w->controller() != controller_) {
            detach();
            controller_ = w->controller();
            attach();
        }
    }

    void dispose() override { detach(); }

    SharedPtr<Widget> build(BuildContext& context) override {
        (void)context;
        auto* w = static_cast<VideoProgressIndicator*>(widget());
        const VideoProgressColors colors = w->colors();
        const VideoPlayerValue value = controller_ ? controller_->value() : VideoPlayerValue();

        SharedPtr<Widget> indicator;
        if (value.isInitialized) {
            const double durationUs = double(value.duration.count());
            double maxBuffering = 0.0;
            if (durationUs > 0.0) {
                for (const auto& range : value.buffered)
                    maxBuffering = std::max(maxBuffering,
                                            double(range.end.count()) / durationUs);
            }
            const double played =
                    durationUs <= 0.0 ? 0.0 : double(value.position.count()) / durationUs;

            std::vector<SharedPtr<Widget>> bars;
            // 缓冲在下、已播在上（上游两条 LinearProgressIndicator 的叠法）。
            bars.push_back(cui::make_shared<LinearProgressIndicator>(
                    maxBuffering, colors.backgroundColor, colors.bufferedColor));
            bars.push_back(cui::make_shared<LinearProgressIndicator>(
                    played, Color::fromRGBA(0.0, 0.0, 0.0, 0.0), colors.playedColor));
            indicator = cui::make_shared<Stack>(std::move(bars), nullptr,
                                                RenderStack::Alignment::TopStart,
                                                RenderStack::Fit::Passthrough);
        } else {
            indicator = cui::make_shared<LinearProgressIndicator>(
                    std::nullopt, colors.backgroundColor, colors.playedColor);
        }

        auto padded = cui::make_shared<Padding>(w->padding(), indicator);
        if (w->allowScrubbing() && controller_)
            return cui::make_shared<VideoScrubber>(controller_, padded);
        return padded;
    }

private:
    void attach() {
        if (!controller_ || controller_->disposed())
            return;
        handle_ = controller_->addListenerWithHandle([this] {
            if (mounted())
                setState();
        });
    }

    void detach() {
        if (controller_ && handle_ != 0)
            controller_->removeListenerByHandle(handle_);
        handle_ = 0;
    }

    std::shared_ptr<VideoPlayerController> controller_;
    int handle_ = 0;
};

}  // namespace

std::unique_ptr<State> VideoProgressIndicator::createState() {
    return std::make_unique<VideoProgressIndicatorState>();
}

// ── VideoControls ───────────────────────────────────────────────────────────

namespace {

/// VCL 风格媒体控制条。扁平、跟随主题；两行：① seek 轨道（含快退/快进），
/// ② 传输键（播放/上一/停止/下一/快照）+ 音量组（静音/滑条/百分比）。
class VideoControlsState : public State {
public:
    void initState() override {
        controller_ = static_cast<VideoControls*>(widget())->controller();
        attach();
    }

    void didUpdateWidget(Widget* oldWidget) override {
        (void)oldWidget;
        auto* w = static_cast<VideoControls*>(widget());
        if (w->controller() != controller_) {
            detach();
            controller_ = w->controller();
            attach();
        }
    }

    void dispose() override { detach(); }

    SharedPtr<Widget> build(BuildContext&) override {
        // 叠放模式每次构建都从 widget 取（换 setOverlay 不需要重新挂监听）。
        if (auto* w = static_cast<VideoControls*>(widget()))
            overlay_ = w->overlay();
        const VideoPlayerValue v = controller_ ? controller_->value() : VideoPlayerValue();
        const bool ready = v.isInitialized && !v.hasError();

        // ① seek 轨道行：[⏪] [可拖动进度条] [⏩]
        auto seekRow = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                iconButton(IconCode::Rewind, ready ? [this] { seekBy(std::chrono::seconds(-5)); }
                                                             : std::function<void()>(),
                           "后退 5 秒"),
                cui::make_shared<Expanded>(1, seekTrack(ready)),
                iconButton(IconCode::FastForward,
                           ready ? [this] { seekBy(std::chrono::seconds(5)); }
                                 : std::function<void()>(),
                           "前进 5 秒"),
        });

        // ② 传输行 + 音量组
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%d%%", int(v.volume * 100.0 + 0.5));
        auto transport = cui::make_shared<Row>(std::vector<SharedPtr<Widget>>{
                iconButton(v.isPlaying ? IconCode::Pause : IconCode::Play,
                           ready ? [this] { togglePlay(); } : std::function<void()>(),
                           v.isPlaying ? "暂停" : "播放"),
                iconButton(IconCode::SkipPrevious,
                           ready ? [this] { seekToStart(); } : std::function<void()>(),
                           "回到开头"),
                iconButton(IconCode::Stop,
                           ready ? [this] { stop(); } : std::function<void()>(),
                           "停止"),
                iconButton(IconCode::SkipNext,
                           ready ? [this] { seekToEnd(); } : std::function<void()>(),
                           "跳到结尾"),
                iconButton(IconCode::PhotoCamera,
                           ready ? [this] { takeSnapshot(); } : std::function<void()>(),
                           "保存当前帧快照"),
                cui::make_shared<Spacer>(),
                iconButton(v.volume <= 0.0 ? IconCode::VolumeMute : IconCode::VolumeUp,
                           ready ? [this] { toggleMute(); } : std::function<void()>(),
                           "静音 / 取消静音"),
                // 同上面控制条的道理：只给宽度。SizedBox(140, 0) 的紧高度 0 会
                // 穿透到 Slider，滑条画得出来但拖不到、点不动。
                cui::make_shared<ConstrainedBox>(
                        BoxConstraints(140.0, 140.0, 0.0, 1e18),
                        volumeSlider(ready, v.volume)),
                cui::make_shared<SizedBox>(8.0, 0.0),
                cui::make_shared<Text>(std::string(pct),
                                       TextStyle().size(12.0).fgColor(fg())),
        });

        auto bar = cui::make_shared<Container>(cui::make_shared<Column>(
                std::vector<SharedPtr<Widget>>{
                        seekRow, cui::make_shared<SizedBox>(0.0, 2.0), transport}));
        bar->padding(EdgeInsets::symmetric(overlay_ ? 10.0 : 8.0, 6.0));
        // 叠在画面上 ⇒ 半透明深底、不做圆角（四周由画面自己裁）；
        // 独立摆放 ⇒ 跟随主题。
        bar->decoration(overlay_ ? BoxDecoration{Color::fromRGBA(0.02, 0.03, 0.05, 0.66)}
                                 : BoxDecoration{theme().surfaceVariant, 6.0});
        return bar;
    }

private:
    // ── 配色：叠放模式整体换浅色字形 ─────────────────────────
    bool overlay_ = false;

    Color fg() const {
        return overlay_ ? Color::fromRGBA(0.95, 0.96, 0.98, 1.0) : theme().textPrimary;
    }
    Color disabled() const {
        return overlay_ ? Color::fromRGBA(0.95, 0.96, 0.98, 0.35) : theme().textDisabled;
    }

    // ── 小工具：包一层 IconButton（禁用态由空回调给出）──────────────────
    SharedPtr<Widget> iconButton(IconCode code, std::function<void()> onPressed,
                                 const std::string& tooltip) {
        return cui::make_shared<IconButton>(code, std::move(onPressed), 22.0, fg(),
                                            disabled(), tooltip);
    }

    SharedPtr<Widget> seekTrack(bool ready) {
        if (!ready || !controller_)
            return cui::make_shared<SizedBox>(0.0, 20.0);
        // VideoProgressIndicator(allowScrubbing=true) 内部已包 VideoScrubber：
        // 拖动开始自动暂停、松手恢复。
        return cui::make_shared<VideoProgressIndicator>(controller_, true);
    }

    SharedPtr<Widget> volumeSlider(bool ready, double volume) {
        ValueChanged<double> onChanged;
        if (ready)
            onChanged = [this](double val) {
                if (controller_) {
                    lastVolume_ = val;
                    controller_->setVolume(val);
                }
            };
        return cui::make_shared<Slider>(volume, std::move(onChanged), 0.0, 1.0);
    }

    // ── 传输动作 ──────────────────────────────────────────────────────────
    void togglePlay() {
        if (!controller_)
            return;
        if (controller_->value().isPlaying)
            controller_->pause();
        else
            controller_->play();
    }

    void stop() {
        if (!controller_)
            return;
        controller_->pause();
        controller_->seekTo(std::chrono::microseconds(0));
    }

    void seekToStart() {
        if (controller_)
            controller_->seekTo(std::chrono::microseconds(0));
    }

    void seekToEnd() {
        if (!controller_)
            return;
        controller_->seekTo(controller_->value().duration);
    }

    void seekBy(std::chrono::microseconds delta) {
        if (!controller_)
            return;
        controller_->seekTo(controller_->value().position + delta);
    }

    void toggleMute() {
        if (!controller_)
            return;
        const double volume = controller_->value().volume;
        if (volume > 0.0) {
            lastVolume_ = volume;
            controller_->setVolume(0.0);
        } else {
            controller_->setVolume(lastVolume_ > 0.0 ? lastVolume_ : 1.0);
        }
    }

    /// 把当前帧编码为 PNG，弹原生保存对话框写盘。平台不支持 / 取消时静默返回。
    void takeSnapshot() {
        if (!controller_)
            return;
        const sk_sp<SkImage> image = controller_->frame();
        if (!image)
            return;
        const sk_sp<SkData> data = encodePng(image);
        if (!data)
            return;
        services::FileDialogOptions options;
        options.title = "保存快照";
        options.suggestedName = "snapshot.png";
        options.filters = {{"PNG", {"png"}}};
        const std::optional<std::string> path = services::saveFileDialog(options);
        if (!path || path->empty())
            return;
        std::ofstream out(*path, std::ios::binary);
        if (out)
            out.write(reinterpret_cast<const char*>(data->bytes()),
                      std::streamsize(data->size()));
    }

    void attach() {
        if (!controller_ || controller_->disposed())
            return;
        handle_ = controller_->addListenerWithHandle([this] {
            if (mounted())
                setState();
        });
    }

    void detach() {
        if (controller_ && handle_ != 0)
            controller_->removeListenerByHandle(handle_);
        handle_ = 0;
    }

    std::shared_ptr<VideoPlayerController> controller_;
    int handle_ = 0;
    double lastVolume_ = 1.0;
};

}  // namespace

std::unique_ptr<State> VideoControls::createState() {
    return std::make_unique<VideoControlsState>();
}

}  // namespace cui_core
