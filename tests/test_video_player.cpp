/// @file test_video_player.cpp
/// @brief video_player 控件族回归：controller 状态机 + 三个控件。
///
/// 素材是运行时合成的 MJPEG-in-AVI（160×90 / 10 fps / 5 帧），走真实原生
/// 后端（Windows Media Foundation / Linux FFmpeg），和 gallery 页同一条路径。
/// 无 Application 时 `initialize()` 同步打开，Ticker 由测试手动泵。
///
/// 因为全份都依赖一个真能解容器的后端，两条编译开关都关时（`-DCUI_VIDEO_SYS=OFF
/// -DCUI_VIDEO_FFMPEG=OFF`，或运行期探测没过）整份跳过 —— 那时候打不开是正确
/// 行为，不该把 ctest 染红。
///
/// 覆盖：
///   1. 初始化元信息：尺寸、时长、比例、首帧发布（视频帧不是透明底 ——
///      回归 MF RGB32 的 X 字节被当成 alpha 的缺陷，见 readbackAlpha）。
///   2. seekTo 夹取 [0, duration]；停在末尾时 isCompleted。
///   3. 参数校验：音量夹到 [0,1]；setPlaybackSpeed(0) 抛 invalid_argument。
///   4. play/pause 时钟：位置前进 / 暂停后冻结。
///   5. 播完再 play 从 0 开始（上游行为）。
///   6. 循环：越过末尾后回卷、不置 isCompleted、仍在播放。
///   7. 监听器句柄 + dispose 幂等（dispose 之后所有调用都是空操作）。
///   8. VideoPlayer 画 controller 当前帧；换帧后树里 RenderImage 跟着换。
///   9. VideoProgressIndicator：已初始化时两条进度条（缓冲 + 已播）；
///      allowScrubbing 包裹后点击进度条按比例 seek。
///  10. 拆树后 controller 上的订阅全部摘除。
///  11. VideoControls 每个传输键真的能点（钉住「tight 0 高度穿透 ⇒ 整条点不动」）。
///  12. 叠放模式（overlay）贴在画面最底部：Stack + Positioned 下仍拿得到真实
///     尺寸、仍贴在底边、仍能被点着。
///  13. 画面层包 GestureDetector 做 tap-to-toggle 时的命中优先级：落在控制条
///     上的点击必须只被条吃掉（不能同时触发画面的 onTap），收起时又必须
///     落回画面（才能点画面播放 / 暂停）。
///  14. 真后端（FFmpeg / Media Foundation）解带音轨的容器 ⇒ 交出 PCM 给设备：
///     采样格式、分段连续性、样本非静音、seek 后基准搬迁与 PCM 重定位。
///     设备是注册进来的假件（不碰真声卡），解码用的仍是表里那条真后端。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <include/core/SkImage.h>
#include <include/encode/SkJpegEncoder.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../examples/video_demo.h"
#include "cui/framework/build_owner.h"
#include "cui/framework/element.h"
#include "cui/framework/framework.h"
#include "cui/foundation/events.h"
#include "cui/foundation/shared_ptr.h"
#include "cui/foundation/ticker.h"
#include "cui/rendering/render_box_widgets.h"
#include "cui/rendering/render_flex.h"
#include "cui/rendering/render_icon.h"
#include "cui/rendering/render_material.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/video/video_decoder.h"
#include "cui/video/video_player.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/gestures.h"
#include "cui/widgets/implicit_animation.h"

using namespace cui;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

double usToMs(std::chrono::microseconds v) { return double(v.count()) / 1000.0; }

// ── 素材：运行时合成的 MJPEG-in-AVI ─────────────────────────────────────────

constexpr int kWidth = 160;
constexpr int kHeight = 90;
constexpr int kFps = 10;
constexpr int kFrameUs = 1000000 / kFps;

std::vector<uint8_t> makeTestVideo(int frames) {
    std::vector<std::vector<uint8_t>> jpegs;
    jpegs.reserve(size_t(frames));
    for (int i = 0; i < frames; ++i) {
        auto image = gallery::video_demo::renderDemoFrame(i, frames, kWidth, kHeight);
        if (!image)
            return {};
        SkJpegEncoder::Options options;
        options.fQuality = 80;
        auto data = SkJpegEncoder::Encode(nullptr, image.get(), options);
        if (!data)
            return {};
        const auto* raw = static_cast<const uint8_t*>(data->data());
        jpegs.emplace_back(raw, raw + data->size());
    }
    return gallery::video_demo::buildMjpegAvi(jpegs, kWidth, kHeight, kFps);
}

/// 与 `makeTestVideo` 同一套画面，但容器里多一条 `seconds` 秒的立体声 PCM
/// 音轨（44100Hz）。只给第 14 节用 —— 前面十三节的素材逐字节不变。
std::vector<uint8_t> makeTestVideoWithAudio(int frames, int seconds) {
    constexpr int kAudioRate = 44100;
    constexpr int kAudioChannels = 2;
    std::vector<std::vector<uint8_t>> jpegs;
    jpegs.reserve(size_t(frames));
    for (int i = 0; i < frames; ++i) {
        auto image = gallery::video_demo::renderDemoFrame(i, frames, kWidth, kHeight);
        if (!image)
            return {};
        SkJpegEncoder::Options options;
        options.fQuality = 80;
        auto data = SkJpegEncoder::Encode(nullptr, image.get(), options);
        if (!data)
            return {};
        const auto* raw = static_cast<const uint8_t*>(data->data());
        jpegs.emplace_back(raw, raw + data->size());
    }
    return gallery::video_demo::buildMjpegAvi(
            jpegs, kWidth, kHeight, kFps,
            gallery::video_demo::buildDemoAudio(kAudioRate, kAudioChannels, seconds));
}

/// 手动泵帧：无 SchedulerBinding 时 Ticker 只靠这个入口推进。
void pumpOnce() { Ticker::tickAllActive(Duration(0)); }

/// 在真实时间内轮询，直到谓词成立（视频位置来自墙上时钟）。
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        pumpOnce();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pumpOnce();
    return pred();
}

std::shared_ptr<VideoPlayerController> openController(int frames, std::string& error) {
    auto controller = VideoPlayerController::memory(makeTestVideo(frames), "avi");
    controller->initialize();
    const VideoPlayerValue v = controller->value();
    if (!v.isInitialized)
        error = v.errorDescription.value_or("未初始化");
    return controller;
}

/// `CUI_VIDEO_BACKEND=auto|ffmpeg-first|system|ffmpeg` 把整份钉到某一条路线上，
/// 好在一台同时编进两条后端的机器（Windows = MF + FFmpeg）上各跑一遍。
/// 不设就是默认的 auto（系统端优先）。`videoBackendCatalog()` 走的是
/// `orderedVideoBackends()`，所以选了没编进来的那条 ⇒ 表为空 ⇒ 整份 [SKIP]；
/// 把表打出来才不会把「配置错了」误读成「这台机器没后端」。
void applyBackendPreferenceFromEnv() {
    const char* raw = std::getenv("CUI_VIDEO_BACKEND");
    if (!raw || !*raw)
        return;
    const std::string name = raw;
    if (name == "system")
        setVideoBackendPreference(VideoBackendPreference::SystemOnly);
    else if (name == "ffmpeg")
        setVideoBackendPreference(VideoBackendPreference::FfmpegOnly);
    else if (name == "ffmpeg-first")
        setVideoBackendPreference(VideoBackendPreference::FfmpegFirst);
    else
        setVideoBackendPreference(VideoBackendPreference::Auto);
    std::printf("[backend] CUI_VIDEO_BACKEND=%s\n", raw);
    for (const auto& st : videoBackendCatalog())
        std::printf("[backend]   %s: %s%s\n", st.name.c_str(),
                    st.available ? "可用" : "不可用",
                    st.note.empty() ? "" : ("（" + st.note + "）").c_str());
}

/// 这台机器上有没有「编进来且运行期可用」的真容器后端。
bool hasContainerBackend() {
    for (const auto& st : videoBackendCatalog())
        if (st.available)
            return true;
    return false;
}

// ── 1：素材与初始化元信息 ───────────────────────────────────────────────────

void testInitialize() {
    std::printf("--- 1: 初始化元信息 ---\n");
    const std::vector<uint8_t> bytes = makeTestVideo(5);
    check(bytes.size() > 1000, "合成素材非空");
    check(bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
                  std::memcmp(bytes.data() + 8, "AVI ", 4) == 0,
          "素材是 RIFF/AVI 容器");

    std::string error;
    auto c = openController(5, error);
    if (!error.empty())
        std::printf("      打开失败: %s\n", error.c_str());
    check(error.empty() && c && c->isInitialized(), "initialize() 打开成功");
    if (!c || !c->isInitialized()) {
        check(false, "跳过后续断言（打不开）");
        return;
    }

    const VideoPlayerValue v = c->value();
    check(int(v.size.width) == kWidth && int(v.size.height) == kHeight, "尺寸 160×90");
    check(std::fabs(v.aspectRatio() - double(kWidth) / kHeight) < 1e-9, "aspectRatio ≈ 16:9");
    // 5 帧 @10fps ⇒ 500ms；容器/后端对时长口径略有出入，给区间。
    const double durationMs = usToMs(v.duration);
    check(durationMs >= 350.0 && durationMs <= 650.0, "时长 ≈ 500ms");
    check(v.position.count() == 0, "初始化后位置在 0");
    check(!v.isCompleted && !v.isPlaying, "初始化后暂停、未完成");
    check(!v.hasError(), "没有错误描述");

    check(waitFor([&] { return c->frame() != nullptr; }), "首帧已发布");
    const auto& frame = c->frame();
    check(frame && frame->width() == kWidth && frame->height() == kHeight, "首帧尺寸与视频一致");
    if (frame) {
        // RGB32 的 X 字节在 MF 里未定义（常见 0），若原样交给 Graphite 上传
        // 会整帧 alpha=0。回归点：读回的像素必须不透明。
        uint32_t px = 0;
        check(frame->readPixels(SkImageInfo::MakeN32Premul(1, 1), &px, sizeof(px), 0, 0),
              "首帧可读回像素");
        check(((px >> 24) & 0xFF) == 0xFF, "首帧像素不透明（X 字节已归一成 0xFF）");
    }
    c->dispose();
}

// ── 2：seek 夹取与完成态 ────────────────────────────────────────────────────

void testSeekAndComplete() {
    std::printf("--- 2: seekTo 夹取 ---\n");
    std::string error;
    auto c = openController(5, error);
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    const auto duration = c->duration();

    c->seekTo(std::chrono::microseconds(-5 * 1000000));
    check(c->position().count() == 0, "负位置夹到 0");

    c->seekTo(std::chrono::microseconds(10 * 1000000));
    check(c->position() == duration, "超过时长夹到 duration");
    check(c->value().isCompleted, "停在末尾 ⇒ isCompleted");

    c->seekTo(duration / 2);
    check(c->position() == duration / 2, "中间位置精确保留");
    check(!c->value().isCompleted, "离开末尾 ⇒ isCompleted 清除");

    c->setLooping(true);
    c->seekTo(duration);
    check(!c->value().isCompleted, "循环开启时末尾不置 isCompleted");
    c->setLooping(false);
    c->dispose();
}

// ── 3：参数校验 ─────────────────────────────────────────────────────────────

void testParameterClamp() {
    std::printf("--- 3: 音量 / 倍速参数 ---\n");
    std::string error;
    auto c = openController(2, error);
    if (!c)
        return;

    c->setVolume(2.5);
    check(c->value().volume == 1.0, "音量上夹到 1");
    c->setVolume(-1.0);
    check(c->value().volume == 0.0, "音量下夹到 0");
    c->setVolume(0.25);
    check(c->value().volume == 0.25, "合法音量原样保留");

    bool threw = false;
    try {
        c->setPlaybackSpeed(0.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "setPlaybackSpeed(0) 抛 invalid_argument");
    check(c->value().playbackSpeed == 1.0, "抛异常后倍速不变");

    c->setPlaybackSpeed(2.0);
    check(c->value().playbackSpeed == 2.0, "倍速写入");

    c->setPreventsDisplaySleepDuringVideoPlayback(false);
    check(!c->value().preventsDisplaySleepDuringVideoPlayback, "显示常亮开关可写");
    c->dispose();
}

// ── 4：play/pause 时钟 ──────────────────────────────────────────────────────

void testPlayPauseClock() {
    std::printf("--- 4: play / pause 时钟 ---\n");
    std::string error;
    auto c = openController(5, error);  // 500ms
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    check(waitFor([&] { return c->frame() != nullptr; }), "首帧已发布");

    c->play();
    check(c->value().isPlaying, "play() 后 isPlaying");
    const bool advanced = waitFor([&] { return c->position().count() > 80000; }, 1500);
    check(advanced, "播放中位置前进（>80ms）");
    check(c->position() < c->duration(), "还没到末尾");

    c->pause();
    check(!c->value().isPlaying, "pause() 后 isPlaying 清除");
    const auto frozen = c->position();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    pumpOnce();
    check(c->position() == frozen, "暂停后位置冻结（真实时间流逝也不动）");
    c->dispose();
}

// ── 5：播完再 play 从 0 开始 ────────────────────────────────────────────────

void testPlayFromEnd() {
    std::printf("--- 5: 播完再 play ---\n");
    std::string error;
    auto c = openController(5, error);
    if (!c || !c->isInitialized())
        return;

    c->seekTo(c->duration());
    check(c->value().isCompleted, "先停在末尾");
    c->play();
    check(c->value().isPlaying && !c->value().isCompleted, "play() 清 completed 并开播");
    check(c->position() < c->duration() / 2, "位置回到开头附近");
    c->dispose();
}

// ── 5b：自然播完再 play（gallery「播完后无法再播」回归） ────────────────────

void testPlayAfterNaturalEnd() {
    std::printf("--- 5b: 自然播完再 play ---\n");
    std::string error;
    auto c = openController(5, error);  // 500ms
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    check(waitFor([&] { return c->frame() != nullptr; }), "首帧已发布");

    c->play();
    const bool ended = waitFor([&] { return c->value().isCompleted; }, 3000);
    check(ended, "自然播放到末尾（isCompleted）");
    check(!c->value().isPlaying, "播完后 isPlaying 清除");
    const auto endPosition = c->position();
    check(endPosition == c->duration(), "播完位置停在 duration");

    const auto* frameAtEnd = c->frame().get();
    c->play();
    check(c->value().isPlaying, "再次 play() 后 isPlaying");
    check(!c->value().isCompleted, "再次 play() 清 completed");
    check(c->position() < endPosition, "位置回到开头");

    const bool advanced = waitFor([&] {
        return c->position().count() > 50000 && c->position() < endPosition;
    }, 1500);
    check(advanced, "重新播放后位置继续前进");
    const bool newFrame = waitFor([&] { return c->frame().get() != frameAtEnd; }, 1500);
    check(newFrame, "重新播放后发布新帧");
    c->pause();
    c->dispose();
}

// ── 6：循环 ─────────────────────────────────────────────────────────────────

void testLooping() {
    std::printf("--- 6: 循环 ---\n");
    std::string error;
    auto c = openController(5, error);  // 500ms
    if (!c || !c->isInitialized())
        return;

    check(waitFor([&] { return c->frame() != nullptr; }), "首帧已发布");
    c->setLooping(true);
    c->seekTo(c->duration() - std::chrono::milliseconds(120));
    c->play();

    // 越过末尾：引擎把播放头拉回 0 继续，不置 completed。
    const bool wrapped = waitFor(
            [&] { return c->position() < c->duration() - std::chrono::milliseconds(120); },
            2500);
    check(wrapped, "越过末尾后位置回卷");
    check(c->value().isPlaying, "循环中仍在播放");
    check(!c->value().isCompleted, "循环中不置 isCompleted");

    c->setLooping(false);
    c->pause();
    c->dispose();
}

// ── 7：监听器 + dispose ─────────────────────────────────────────────────────

void testListenersAndDispose() {
    std::printf("--- 7: 监听器句柄与 dispose ---\n");
    std::string error;
    auto c = openController(2, error);
    if (!c)
        return;

    int notifications = 0;
    const size_t before = c->listenerCount();
    const int handle = c->addListenerWithHandle([&] { ++notifications; });
    check(c->listenerCount() == before + 1, "addListenerWithHandle 挂上一条");

    c->setVolume(0.5);
    check(notifications >= 1, "值变化触发通知");

    c->removeListenerByHandle(handle);
    check(c->listenerCount() == before, "removeListenerByHandle 精确摘除");
    const int seen = notifications;
    c->setVolume(0.75);
    check(notifications == seen, "摘除后不再收到通知");

    int afterDispose = 0;
    c->addListenerWithHandle([&] { ++afterDispose; });
    c->dispose();
    check(c->listenerCount() == 0, "dispose 清空监听器");
    c->dispose();  // 幂等

    // dispose 之后所有控制调用都是空操作，不崩、不改值。
    const VideoPlayerValue frozen = c->value();
    c->play();
    c->pause();
    c->seekTo(c->duration());
    c->setVolume(0.1);
    check(c->value() == frozen && afterDispose == 0, "dispose 后调用是空操作");
}

// ── 控件树 harness ──────────────────────────────────────────────────────────

struct Harness {
    PipelineOwner po;
    RenderView rv;
    BuildOwner owner;
    Element* root = nullptr;

    void attach(SharedPtr<Widget> w, double ww, double hh) {
        rv.setWindowSize({ww, hh});
        auto e = w->createElement(w);
        root = e.get();
        owner.adoptRootElement(std::move(e));
        rv.setChild(static_cast<RenderBox*>(root->findRenderObject()));
        po.setRootNode(&rv);
        pump();
    }

    void pump() {
        owner.flushBuildScope();
        po.flushLayout();
    }

    void tap(double x, double y) {
        PointerDownEvent down;
        down.position = {x, y};
        down.buttons = kPrimaryMouseButton;
        po.dispatchPointerEvent(down);
        PointerUpEvent up;
        up.position = {x, y};
        po.dispatchPointerEvent(up);
        pump();
    }
};

template <typename T>
T* findFirst(RenderObject* ro) {
    if (!ro)
        return nullptr;
    if (auto* hit = dynamic_cast<T*>(ro))
        return hit;
    T* found = nullptr;
    ro->visitChildren([&](AbstractNode* node) {
        if (found)
            return;
        if (auto* child = dynamic_cast<RenderObject*>(node))
            found = findFirst<T>(child);
    });
    return found;
}

template <typename T>
int countOf(RenderObject* ro) {
    if (!ro)
        return 0;
    int n = dynamic_cast<T*>(ro) ? 1 : 0;
    ro->visitChildren([&](AbstractNode* node) {
        if (auto* child = dynamic_cast<RenderObject*>(node))
            n += countOf<T>(child);
    });
    return n;
}

/// 钉到左上角并给出松约束（RenderView 的紧约束会顶掉 SizedBox 的指定尺寸）。
SharedPtr<Widget> pinned(SharedPtr<Widget> w) {
    return cui::make_shared<Align>(-1.0, -1.0, std::move(w));
}

// ── 8：VideoPlayer 控件 ─────────────────────────────────────────────────────

void testVideoPlayerWidget() {
    std::printf("--- 8: VideoPlayer 控件 ---\n");
    std::string error;
    auto c = openController(5, error);
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    if (!waitFor([&] { return c->frame() != nullptr; })) {
        check(false, "首帧没出来");
        return;
    }

    Harness h;
    h.attach(pinned(cui::make_shared<SizedBox>(kWidth, kHeight,
                                               cui::make_shared<VideoPlayer>(c))),
             kWidth, kHeight);
    auto* image = findFirst<RenderImage>(&h.rv);
    check(image != nullptr, "树里有 RenderImage");
    check(image && image->image() == c->frame(), "画的正是 controller 当前帧");

    // seek 换帧 ⇒ controller 通知 ⇒ 重建后 RenderImage 拿到新帧。
    const auto oldFrame = c->frame();
    c->seekTo(std::chrono::milliseconds(300));
    // seek 帧由后台线程补出，等它落地再核验。
    check(waitFor([&] { return c->frame() && c->frame() != oldFrame; }),
          "seek 确实产生了另一帧");
    h.pump();
    auto* updated = findFirst<RenderImage>(&h.rv);
    check(updated && updated->image() == c->frame(), "seek 后 RenderImage 换成新帧");
    c->dispose();
}

// ── 9：VideoProgressIndicator / VideoScrubber ───────────────────────────────

void testProgressIndicator() {
    std::printf("--- 9: 进度条与拖动 seek ---\n");
    std::string error;
    auto c = openController(10, error);  // 1s
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    if (!waitFor([&] { return c->frame() != nullptr; })) {
        check(false, "首帧没出来");
        return;
    }

    auto indicator = cui::make_shared<VideoProgressIndicator>(c, true);
    Harness h;
    h.attach(pinned(cui::make_shared<SizedBox>(200.0, 16.0, indicator)), 200.0, 16.0);
    check(countOf<RenderLinearProgressIndicator>(&h.rv) == 2,
          "已初始化时两条进度条（缓冲 + 已播）");

    // 点击进度条 75% 处 ⇒ 按比例 seek（scrubber 用自身宽度换算）。
    h.tap(150.0, 8.0);
    const double durationMs = usToMs(c->duration());
    const double positionMs = usToMs(c->position());
    check(positionMs > durationMs * 0.60 && positionMs < durationMs * 0.90,
          "点击 75% 处 seek 到 60%~90%");

    // 重建后「已播」那条的 value 跟上位置。
    h.pump();
    double played = -1.0;
    std::function<void(RenderObject*)> visit = [&](RenderObject* ro) {
        if (!ro)
            return;
        if (auto* bar = dynamic_cast<RenderLinearProgressIndicator*>(ro))
            played = std::max(played, bar->value());
        ro->visitChildren([&](AbstractNode* node) {
            if (auto* child = dynamic_cast<RenderObject*>(node))
                visit(child);
        });
    };
    visit(&h.rv);
    const double expected = durationMs > 0.0 ? positionMs / durationMs : 0.0;
    check(std::fabs(played - expected) < 0.05, "进度条 value 跟随 seek 位置");
    c->dispose();
}

// ── 10：拆树摘订阅 ──────────────────────────────────────────────────────────

void testSubscriptionTeardown() {
    std::printf("--- 10: 拆树后订阅摘除 ---\n");
    std::string error;
    auto c = openController(2, error);
    if (!c || !c->isInitialized())
        return;
    waitFor([&] { return c->frame() != nullptr; });

    const size_t base = c->listenerCount();
    {
        Harness h;
        h.attach(pinned(cui::make_shared<Column>(std::vector<SharedPtr<Widget>>{
                          cui::make_shared<SizedBox>(kWidth, kHeight,
                                                     cui::make_shared<VideoPlayer>(c)),
                          cui::make_shared<VideoProgressIndicator>(c, true)})),
                 400.0, 400.0);
        check(c->listenerCount() >= base + 2, "VideoPlayer 与进度条各订阅一条");
    }
    check(c->listenerCount() == base, "拆树后订阅全部摘除");
    c->dispose();
}

// ── 11：VideoControls 传输键真的能点 ────────────────────────────────────────

/// 按图标码在树里找渲染对象（控制条上每个键就是一个 RenderIcon）。
RenderIcon* findIconByCode(RenderObject* ro, IconCode code) {
    if (!ro)
        return nullptr;
    if (auto* ic = dynamic_cast<RenderIcon*>(ro))
        return ic->code() == code ? ic : nullptr;
    RenderIcon* found = nullptr;
    ro->visitChildren([&](AbstractNode* node) {
        if (found)
            return;
        if (auto* child = dynamic_cast<RenderObject*>(node))
            found = findIconByCode(child, code);
    });
    return found;
}

/// 点某个传输键：按图标自身中心换算全局坐标（对齐「测试命中点用目标 RO 的
/// localToGlobal(中心)」的口径），图标不在树里则返回 false。
bool tapIcon(RenderObject& root, IconCode code, Harness& h) {
    RenderIcon* ic = findIconByCode(&root, code);
    if (!ic)
        return false;
    const Size s = ic->size();
    const Offset g = ic->localToGlobal({s.width * 0.5, s.height * 0.5});
    h.tap(g.dx, g.dy);
    return true;
}

void testControlsHitTest() {
    std::printf("--- 11: VideoControls 传输键可点击 ---\n");
    std::string error;
    auto c = openController(10, error);  // ≈1s
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    if (!waitFor([&] { return c->frame() != nullptr; })) {
        check(false, "首帧没出来");
        return;
    }

    // 摆在画面外（非叠放）时的包法：只锁宽度，高度留给控制条自己量。
    Harness h;
    h.attach(pinned(cui::make_shared<ConstrainedBox>(
                    BoxConstraints(420.0, 420.0, 0.0, 1e18),
                    cui::make_shared<VideoControls>(c))),
            600.0, 300.0);

    // 命中前提：控制条自身有真实高度。旧写法用 SizedBox(420, 0) 包，那是
    // tight 420×0，紧高度一路穿透到 Column ⇒ 按钮靠溢出照样画得出来，
    // 但命中区是 0 高，整条控制条一个键都点不动。
    auto* flex = findFirst<RenderFlex>(&h.rv);
    check(flex != nullptr, "控制条本体是 Column（树里有 RenderFlex）");
    check(flex && flex->size().height > 30.0, "控制条有真实高度（不是 tight 0）");

    check(tapIcon(h.rv, IconCode::Play, h), "树里有播放键");
    check(c->value().isPlaying, "点播放键 ⇒ 开始播放");
    h.pump();
    check(findIconByCode(&h.rv, IconCode::Pause) != nullptr, "重建后图标换成暂停");

    check(tapIcon(h.rv, IconCode::Pause, h), "树里有暂停键");
    check(!c->value().isPlaying, "点暂停键 ⇒ 停止播放");

    check(tapIcon(h.rv, IconCode::VolumeUp, h), "树里有静音键");
    check(c->value().volume == 0.0, "点静音键 ⇒ 音量归零");

    // 快进 5 秒：素材只有 ≈1s，夹到末尾也证明 seekBy 真的被触发了。
    const auto beforeSeek = c->position();
    check(tapIcon(h.rv, IconCode::FastForward, h), "树里有快进键");
    check(c->position() > beforeSeek, "点快进键 ⇒ 位置前移");

    check(tapIcon(h.rv, IconCode::Stop, h), "树里有停止键");
    check(c->position().count() == 0 && !c->value().isPlaying, "点停止键 ⇒ 回到开头并暂停");

    c->dispose();
}

// ── 12：叠在画面底部的控制条（gallery 页的包法）────────────────────────

void testOverlayControls() {
    std::printf("--- 12: 叠放模式贴画面底部且可点击 ---\n");
    std::string error;
    auto c = openController(10, error);  // ≈1s
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    if (!waitFor([&] { return c->frame() != nullptr; })) {
        check(false, "首帧没出来");
        return;
    }

    // gallery 页同构：`AspectRatio` 定出画面盒子（约束 600×300、比例 2 ⇒ 600×300），
    // `Stack` 用 `Fit::Expand` 把画面撑满盒子，控制条用
    // `Positioned(left+right+bottom = 0)` 贴在底边，外面还套着淡入淡出与
    // IgnorePointer 两层（收起时不吃点击）。
    constexpr double kBoxW = 600.0;
    constexpr double kBoxH = 300.0;
    auto picture = cui::make_shared<SizedBox>(160.0, 90.0);  // 自然尺寸远小于盒子
    auto bar = cui::make_shared<VideoControls>(c, nullptr, /*overlay=*/true);
    auto fade = cui::make_shared<AnimatedOpacity>(1.0, std::move(bar), 220.0);
    auto gate = cui::make_shared<IgnorePointer>(false, std::move(fade));
    auto pos = cui::make_shared<Positioned>(std::move(gate));
    pos->hasLeft = pos->hasRight = pos->hasBottom = true;
    auto stack = cui::make_shared<Stack>(
            std::vector<SharedPtr<Widget>>{picture, pos}, nullptr,
            RenderStack::Alignment::TopStart, RenderStack::Fit::Expand);
    auto stage = cui::make_shared<ConstrainedBox>(
            BoxConstraints(0.0, 720.0, 0.0, 480.0),
            cui::make_shared<ClipRRect>(8.0,
                                        cui::make_shared<AspectRatio>(2.0, std::move(stack))));
    Harness h;
    h.attach(pinned(stage), kBoxW, kBoxH);

    auto* rs = findFirst<RenderStack>(&h.rv);
    check(rs != nullptr, "树里有 RenderStack");
    if (!rs) {
        c->dispose();
        return;
    }
    check(std::fabs(rs->size().width - kBoxW) < 0.5 &&
                  std::fabs(rs->size().height - kBoxH) < 0.5,
          "Stack = AspectRatio 定出的画面盒子（定位子不参与定尺寸）");

    // Fit::Expand 的意义：画面被撑满到盒子尺寸。写成默认的 Loose 时画面会
    // 缩回自己要求的 160×90，挂在左上角一小块（本仓曾经的翻车点）。
    RenderBox* pic = nullptr;
    size_t seen = 0;
    rs->visitChildren([&](AbstractNode* node) {
        if (seen++ == 0)
            pic = dynamic_cast<RenderBox*>(node);
    });
    check(pic && std::fabs(pic->size().width - kBoxW) < 0.5 &&
                std::fabs(pic->size().height - kBoxH) < 0.5,
          "画面撑满盒子（Fit::Expand，不是自然尺寸 160×90）");

    // 量的是控制条本体（带底色的那条盒），不是内层 Column：外面还有 Padding，
    // 内层盒会比画面窄两个内边距。
    auto* barBox = findFirst<RenderDecoratedBox>(&h.rv);
    check(barBox != nullptr, "叠放的控制条有本体盒");
    if (!barBox) {
        c->dispose();
        return;
    }
    // 定位子收到的是「紧宽 + 松高」：宽度拉满画面，高度自己量。
    check(std::fabs(barBox->size().width - kBoxW) < 0.5, "left+right ⇒ 横向拉满画面宽");
    check(barBox->size().height > 30.0, "bottom ⇒ 竖向取自然高（不是 0）");
    const Offset top = barBox->localToGlobal({0.0, 0.0});
    check(std::fabs(top.dx) < 0.5, "贴在画面左缘");
    check(std::fabs(top.dy + barBox->size().height - kBoxH) < 0.5,
          "贴在画面最底部（下缘与画面下缘齐平）");

    auto* flex = findFirst<RenderFlex>(&h.rv);
    check(flex && flex->size().height > 30.0, "控制条内部两行仍是真实高度（不是 tight 0）");

    // 命中链要能穿过 Stack/Positioned/IgnorePointer/Opacity 走到图标上。
    check(tapIcon(h.rv, IconCode::Play, h), "叠放树里也有播放键");
    check(c->value().isPlaying, "点叠放的播放键 ⇒ 开始播放");
    h.pump();
    check(findIconByCode(&h.rv, IconCode::Pause) != nullptr, "重建后图标换成暂停");

    c->dispose();
}

// ── 13：点画面 toggle 与叠放命中优先级 ────────────────────────────

void testStageTapPrecedence() {
    std::printf("--- 13: 点画面 toggle 与控制条命中优先级 ---\n");
    std::string error;
    auto c = openController(10, error);  // ≈1s
    if (!c || !c->isInitialized()) {
        check(false, "打开失败");
        return;
    }
    if (!waitFor([&] { return c->frame() != nullptr; })) {
        check(false, "首帧没出来");
        return;
    }

    constexpr double kBoxW = 600.0;
    constexpr double kBoxH = 300.0;

    // 把 gallery 页 onStageTap() 的形状直接复刻到测试里：计数 + toggle。
    // 本仓没有手势竞技场，「点控制条不会连带触发画面」完全靠 Stack 的
    // 倒序命中 + 「第一个命中就停」，所以这条必须钉住。
    int stageTaps = 0;
    auto toggle = [&c, &stageTaps] {
        ++stageTaps;
        if (c->value().isPlaying)
            c->pause();
        else
            c->play();
    };

    auto makePicture = [&] {
        auto box = cui::make_shared<Container>(cui::make_shared<VideoPlayer>(c));
        box->decoration(BoxDecoration{Color::fromRGB(0.04, 0.05, 0.08)});
        return cui::make_shared<GestureDetector>(std::move(box), toggle);
    };
    auto makeBar = [&c](bool ignoring) {
        auto pos = cui::make_shared<Positioned>(cui::make_shared<IgnorePointer>(
                ignoring, cui::make_shared<VideoControls>(c, nullptr, /*overlay=*/true)));
        pos->hasLeft = pos->hasRight = pos->hasBottom = true;
        return pos;
    };
    auto makeStage = [&](bool ignoring) {
        auto stack = cui::make_shared<Stack>(
                std::vector<SharedPtr<Widget>>{makePicture(), makeBar(ignoring)}, nullptr,
                RenderStack::Alignment::TopStart, RenderStack::Fit::Expand);
        return cui::make_shared<ConstrainedBox>(
                BoxConstraints(0.0, 720.0, 0.0, 480.0),
                cui::make_shared<ClipRRect>(8.0,
                                            cui::make_shared<AspectRatio>(2.0, std::move(stack))));
    };

    // ── A：控制条显形时 ──────────────────────────────────────────────
    {
        Harness h;
        h.attach(pinned(makeStage(false)), kBoxW, kBoxH);
        c->pause();
        stageTaps = 0;
        h.pump();

        // 1) 点画面上方空白（条外）⇒ toggle 生效。
        h.tap(120.0, 20.0);
        check(stageTaps == 1, "点画面空白处 ⇒ 画面 onTap 触发");
        check(c->value().isPlaying, "点画面空白处 ⇒ 开始播放");

        // 2) 重建后条上换成暂停图标，点它 ⇒ 只暂停，不能再多一次画面 toggle。
        check(findIconByCode(&h.rv, IconCode::Pause) != nullptr, "重建后条上是暂停键");
        check(tapIcon(h.rv, IconCode::Pause, h), "点条上的暂停键");
        check(!c->value().isPlaying, "点条上的暂停键 ⇒ 停止播放");
        check(stageTaps == 1, "落在条上的点击被条吃掉，没有二次 toggle");

        // 3) 点在条内但不在按钮上（下缘内边距）⇒ 同样不该 toggle 画面。
        auto* barBox = findFirst<RenderDecoratedBox>(&h.rv);
        check(barBox != nullptr, "条本体盒可量");
        if (barBox) {
            const Offset g = barBox->localToGlobal(
                    {barBox->size().width * 0.5, barBox->size().height - 2.0});
            h.tap(g.dx, g.dy);
            check(stageTaps == 1, "点条的空白背景也不触发画面 onTap");
            check(!c->value().isPlaying, "点条背景也不播放");
        }
    }

    // ── B：收起（IgnorePointer）时 ───────────────────────────────────
    {
        Harness h;
        h.attach(pinned(makeStage(true)), kBoxW, kBoxH);
        c->pause();
        stageTaps = 0;
        h.pump();

        // 收起后整条不吃点击 ⇒ 原本按在传输键上的那个点现在落到画面层，
        // 正好就是「点画面 toggle 播放」这条路径。
        RenderIcon* ic = findIconByCode(&h.rv, IconCode::Play);
        check(ic != nullptr, "收起的树里仍能找到播放键位置");
        if (ic) {
            const Size s = ic->size();
            const Offset g = ic->localToGlobal({s.width * 0.5, s.height * 0.5});
            h.tap(g.dx, g.dy);
            check(stageTaps == 1, "收起后点原按钮位置 ⇒ 落到画面层");
            check(c->value().isPlaying, "收起后点画面也能开始播放");
        }
    }

    c->dispose();
}

// ── 14：真后端解音轨（PCM → 设备）───────────────────────────────────
//
// 设备是注进来的假件，不是真声卡：CI / 无音频设备上也能跑，而且能把
// 「后端究竟交出了什么」逐段钉住。注意只注**设备**不注**后端**：
// `factory` 故意返回 nullptr（引擎因此接着往下试内置真后端），而
// `sinkFactory` 会被 `createRegisteredAudioSink()` 先拿走 —— 于是
// 「真解码器 → AudioChunk → sink」这整一段跑的是真路径。

constexpr int kTapRate = 44100;
constexpr int kTapChannels = 2;

struct AudioTap {
    /// 写者 = 解码工作线程（open/push）与 GUI 线程（start/stop/flush），
    /// 读者 = 测试线程，所以全部访问都在 `mutex` 下。
    std::mutex mutex;
    int openCalls = 0;
    int closeCalls = 0;
    int startCalls = 0;
    int stopCalls = 0;
    int flushCalls = 0;
    int64_t lastFlushPtsUs = -1;
    int pushCalls = 0;
    int64_t pushedFrames = 0;
    /// 自上次 `flush()` 以来收下的帧数（背压就看它与「设备消耗了多少」的差）。
    int64_t pushedSinceFlush = 0;
    AudioFormat format;
    /// 每一段的 (ptsUs, frames)：钉「分段连续」就是钉它。
    std::vector<std::pair<int64_t, size_t>> segments;
    /// 样本绝对值的峰值，归一到 [-1,1]（音阶真的在里面而不是全零）。
    /// 不能钉死成 S16 的整数幅度：真后端交什么格式由它自己谈 —— FFmpeg 恒 S16
    /// 交织，MF 首选 IEEE float（交织或平面），两种都要能验。
    float peakAbs = 0.0f;
    bool opened = false;
    bool running = false;
    /// 位置基准 = `flush()` 给的值 + 「起声以来」的真实时间（真设备就是
    /// 按 1 倍速消耗采样，欠载也只是补零、时间照走）。
    int64_t baseUs = 0;
    std::chrono::steady_clock::time_point anchorWall{};

    int64_t positionLocked() const {
        if (!running)
            return baseUs;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - anchorWall);
        return baseUs + elapsed.count();
    }

    /// 自上次 `flush()` 起设备已经「播出去」的采样帧数：位置差换算，
    /// 与真设备（环形缓冲的消耗计数）同一个口径。
    int64_t consumedFramesLocked() const {
        const int64_t elapsedUs = std::max<int64_t>(0, positionLocked() - lastFlushPtsUs);
        return elapsedUs * int64_t(kTapRate) / 1000000;
    }

    /// 在锁保护下跑一段断言（`check` 只写计数与 stdout，不会回写这里）。
    template <typename Fn>
    void inspect(Fn fn) {
        std::lock_guard<std::mutex> lock(mutex);
        fn(*this);
    }
};

class RecordingSink final : public AudioSink {
public:
    explicit RecordingSink(std::shared_ptr<AudioTap> tap) : tap_(std::move(tap)) {}
    ~RecordingSink() override { close(); }

    bool open(const AudioFormat& format, std::string& error) override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        ++tap_->openCalls;
        tap_->format = format;
        tap_->opened = format.isValid();
        if (!tap_->opened)
            error = "采样格式无效";
        return tap_->opened;
    }
    void close() override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        if (!tap_->opened)
            return;
        ++tap_->closeCalls;
        tap_->opened = false;
        tap_->running = false;
    }
    void start() override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        ++tap_->startCalls;
        tap_->anchorWall = std::chrono::steady_clock::now();
        tap_->running = true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        ++tap_->stopCalls;
        if (tap_->running) {
            tap_->baseUs = tap_->positionLocked();
            tap_->running = false;
        }
    }
    void flush(int64_t ptsUs) override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        ++tap_->flushCalls;
        tap_->lastFlushPtsUs = ptsUs;
        tap_->baseUs = ptsUs;
        tap_->pushedSinceFlush = 0;
        tap_->anchorWall = std::chrono::steady_clock::now();
    }
    size_t push(const AudioChunk& chunk) override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        ++tap_->pushCalls;
        tap_->pushedFrames += int64_t(chunk.frames);
        tap_->pushedSinceFlush += int64_t(chunk.frames);
        tap_->segments.emplace_back(chunk.ptsUs, chunk.frames);
        const size_t ch = size_t(std::max(1, tap_->format.channels));
        auto take = [&](float v) { tap_->peakAbs = std::max(tap_->peakAbs, std::abs(v)); };
        if (chunk.s16) {
            for (size_t i = 0; i < chunk.frames * ch; ++i)
                take(float(chunk.s16[i]) / 32768.0f);
        } else if (chunk.f32) {
            for (size_t i = 0; i < chunk.frames * ch; ++i)
                take(chunk.f32[i]);
        } else {
            for (size_t c = 0; c < ch && c < kAudioMaxPlanes; ++c)
                if (chunk.plane[c])
                    for (size_t i = 0; i < chunk.frames; ++i)
                        take(chunk.plane[c][i]);
        }
        return chunk.frames;
    }
    void setVolume(float) override {}
    int64_t positionUs() const override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        return tap_->positionLocked();
    }
    int64_t queuedUs() const override {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        // 不能拿「累计收下多少帧」当积压：那样第一个 100 毫秒之后引擎就
        // 再也不会喂数据（真设备一直在消耗，这里得把消耗的那部分减掉）。
        const int64_t queued =
                std::max<int64_t>(0, tap_->pushedSinceFlush - tap_->consumedFramesLocked());
        return queued * int64_t(1000000) / int64_t(kTapRate);
    }

private:
    std::shared_ptr<AudioTap> tap_;
};

void testBackendAudioTrack() {
    std::printf("--- 14: 真后端解音轨 ---\n");
    auto tap = std::make_shared<AudioTap>();
    VideoBackendRegistration reg;
    reg.name = "BackendAudioTap";
    reg.factory = [](const VideoSource&, std::string& error) {
        error = "这一条只带设备工厂，不解码";
        return std::unique_ptr<VideoDecoderBackend>{nullptr};
    };
    reg.sinkFactory = [tap] { return std::make_unique<RecordingSink>(tap); };
    reg.platformDecoder = false;
    check(registerVideoBackend(std::move(reg)), "注进只带设备工厂的条目");

    const std::vector<uint8_t> bytes = makeTestVideoWithAudio(10, 1);
    check(bytes.size() > 100000, "带音轨的素材非空（PCM 比画面大得多）");

    std::string error;
    auto playback = VideoPlayback::create(VideoSource::memory(bytes, "avi"), error);
    if (!playback) {
        check(false, "打开失败: " + error);
        clearRegisteredVideoBackends();
        return;
    }
    check(playback->info().hasAudio, "容器里有音轨 ⇒ info().hasAudio");
    check(playback->hasAudioOutput(), "后端交出 PCM + 设备起得来 ⇒ hasAudioOutput()");
    check(playback->error().empty(), "有声路径不写错误");
    tap->inspect([](const AudioTap& t) {
        check(t.openCalls == 1, "设备只 open 一次");
        // 采样率/声道数照素材，但**样本格式由后端自己谈**：FFmpeg 恒 S16 交织，
        // MF 首选 IEEE float（`MF_MT_ALL_SAMPLES_INDEPENDENT` 决定交织还是平面）。
        // 钉死 S16 会让 Windows 的系统端这一路假红。
        check(t.format.sampleRate == kTapRate && t.format.channels == kTapChannels &&
                      t.format.isValid(),
              "设备收到 44100Hz / 2 声道");
        std::printf("    后端交出的样本格式：%s\n",
                    t.format.format == AudioSampleFormat::S16Interleaved ? "S16 交织"
                    : t.format.format == AudioSampleFormat::F32Interleaved ? "F32 交织"
                                                                           : "F32 平面");
    });

    playback->play();
    tap->inspect([](const AudioTap& t) { check(t.startCalls >= 1, "play ⇒ 设备起声"); });
    const bool fed = waitFor([&] {
        std::lock_guard<std::mutex> lock(tap->mutex);
        return tap->pushCalls >= 4;
    });
    check(fed, "真后端一段一段把 PCM 交了出来");
    tap->inspect([](const AudioTap& t) {
        check(t.pushedFrames > 0, "交出的采样帧数 > 0");
        // 「首段从 0 开始」只能按**近似**钉：两条后端切 PCM 的形状本来就不同
        // （Windows 实测，素材 = 10 帧视频 + 1 秒 44100Hz 立体声，即每帧一片 4410 采样）：
        //   - FFmpeg：`avcodec` 固定 1024 采样一片 ⇒ seg[0] pts=0 frames=1024、…、
        //     seg[4] pts=92880 frames=314（最后拿余数）。
        //   - Media Foundation：一个 AVI 音频块 = 一片 4410 采样，但 reader 为了自己做
        //     音视频同步会把**最前面一小截**吃掉 ⇒ seg[0] pts=1995 frames=4322
        //     （少 88 个采样 ≈ 2ms），之后 pts 就按 100ms 整片对齐。
        // 2ms 是系统解码器自己的口径，不是本仓丢的样本（后面每片都完整连续，见下一条
        // 断言），所以在 Windows 上「必须恰好等于 0」是假红。留 5ms 容差：既兜得住这条
        // 尾差，也能抓住真的读偏了半个文件那一类错。
        const int64_t firstPts = t.segments.empty() ? -1 : t.segments.front().first;
        check(!t.segments.empty() && firstPts >= 0 && firstPts <= 5000,
              "首段 PCM 从 0 附近开始（实际 " + std::to_string(firstPts) + "us）");
        for (size_t i = 0; i < t.segments.size() && i < 6; ++i)
            std::printf("    seg[%zu] pts=%lldus frames=%zu\n", i,
                        static_cast<long long>(t.segments[i].first), t.segments[i].second);
        // 连续性按「累计帧数 → 时间」的绝对公式算，不是逐段相加：44100Hz 下
        // `帧数 * 1e6 / 44100` 每段都余一点，逐段累加会漂出整微秒。段内的 pts
        // 是整数除法，段开头则是 `av_rescale_q`（就近取整），两者最多差 1 微秒。
        bool contiguous = !t.segments.empty();
        int64_t cumFrames = 0;
        for (const auto& s : t.segments) {
            const int64_t expect =
                    t.segments.front().first + cumFrames * int64_t(1000000) / int64_t(kTapRate);
            contiguous = contiguous && std::llabs(s.first - expect) <= 1;
            cumFrames += int64_t(s.second);
        }
        check(contiguous, "逐段 pts 连续（切片换算不多不少一个采样）");
        check(t.peakAbs > 0.03f, "样本不静音（演示音阶真的解出来了）");
    });

    // 主时钟已经换到音频：位置不能再是 0。
    const bool advanced = waitFor([&] { return playback->position().count() > 80000; });
    check(advanced, "有设备时位置由音频给出（而不是停在 0）");
    check(playback->position().count() < 1500000, "位置不超过素材时长");

    // seek：设备基准搬到目标，重定位后的 PCM 也必须落在目标附近。
    const int flushBefore = [&] {
        std::lock_guard<std::mutex> lock(tap->mutex);
        return tap->flushCalls;
    }();
    const size_t segmentsBefore = [&] {
        std::lock_guard<std::mutex> lock(tap->mutex);
        return tap->segments.size();
    }();
    playback->seekTo(std::chrono::microseconds(500000));
    const bool rejoiced = waitFor([&] {
        std::lock_guard<std::mutex> lock(tap->mutex);
        return tap->flushCalls > flushBefore && tap->segments.size() > segmentsBefore;
    });
    check(rejoiced, "seek 后设备被 flush 且重新交出新位置的 PCM");
    tap->inspect([](const AudioTap& t) {
        check(t.lastFlushPtsUs == 500000, "设备 flush 基准 = seek 目标");
        check(!t.segments.empty() && t.segments.back().first >= 400000,
              "重定位后的 PCM 落在目标附近（旧的不再重交）");
    });

    playback->pause();
    tap->inspect([](const AudioTap& t) { check(t.stopCalls >= 1, "pause ⇒ 设备停"); });
    const int64_t frozen = playback->position().count();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    check(playback->position().count() == frozen, "暂停后位置冻结");

    playback->dispose();
    tap->inspect([](const AudioTap& t) { check(t.closeCalls == 1, "dispose ⇒ 设备关闭"); });
    clearRegisteredVideoBackends();
}

// ── 15：真平台设备端到端（不注入任何假件）────────────────────────────────

/// 第 14 节的设备是 `RecordingSink`，它把「设备消耗」按墙钟算出来 —— 那条路钉的是
/// 引擎与后端的接线，钉不到真声卡。这一节什么都不注入：真后端（本机 = MF 或
/// FFmpeg，跟着 `CUI_VIDEO_BACKEND` 走）+ `createPlatformAudioSink()` 的真身
/// （Windows WASAPI 共享模式 / Linux ALSA / Android AAudio / OHOS OH_AudioRenderer），
/// 位置完全由设备自己报（WASAPI 是 `GetCurrentPadding`）。
/// 「读出来 ≠ 播出去」那类账算错（`audio_sink_win32.cpp` 的 scratch 预读就是这种）
/// 在这里会以「位置比墙钟快一大截」的形状暴露，所以前进量与目标偏移都按同量级夹。
/// 起不来真设备的机器（无默认端点、VM 无声卡）不算失败：无声回退那条由
/// test_video_audio 第 3 节钉着。
void testRealDeviceEndToEnd() {
    std::printf("--- 15: 真平台设备端到端 ---\n");
    constexpr int64_t kTargetUs = 1500000;
    const std::vector<uint8_t> bytes = makeTestVideoWithAudio(30, 3);
    check(bytes.size() > 100000, "素材非空（3 秒画面 + 3 秒 PCM）");

    const auto wall0 = std::chrono::steady_clock::now();
    std::string error;
    auto playback = VideoPlayback::create(VideoSource::memory(bytes, "avi"), error);
    if (!playback) {
        check(false, "打开失败: " + error);
        return;
    }
    if (!playback->hasAudioOutput()) {
        std::printf("      本机起不来真设备，跳过（无声回退由 test_video_audio §3 钉）\n");
        playback->dispose();
        return;
    }
    check(playback->error().empty(), "真设备路径不写错误");

    playback->play();
    const bool moved = waitFor([&] { return playback->position().count() > 200000; }, 4000);
    check(moved, "真设备做主时钟：位置随真实播放前进");
    const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - wall0)
                                      .count();
    const int64_t pos = playback->position().count();
    // 位置不能跑在墙钟前面：素材从 0 起播，同一时刻的位置最多是「已过的真实时间」
    // 再加设备缓冲那点余量（一个 scratch/一个 period，实测都在 100ms 量级）。
    check(pos <= elapsedUs + 400000,
          "真设备位置与墙钟同量级（不虚报快进：位置 " + std::to_string(pos) +
                  "us / 墙钟 " + std::to_string(elapsedUs) + "us）");

    playback->seekTo(std::chrono::microseconds(kTargetUs));
    const bool seeked = waitFor(
            [&] { return std::llabs(playback->position().count() - kTargetUs) < 600000; }, 3000);
    check(seeked, "seek 后真设备被 flush，位置搬到目标附近（旧 PCM 不重交）");

    playback->pause();
    const int64_t frozen = playback->position().count();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    check(playback->position().count() == frozen, "暂停后真设备位置冻结（stop 不碰环形缓冲）");

    playback->dispose();
    check(playback->error().empty(), "真设备 dispose 后没有留下错误");
}

}  // namespace

int main() {
    std::printf("=== test_video_player ===\n");
    applyBackendPreferenceFromEnv();
    if (!hasContainerBackend()) {
        std::printf("[SKIP] 没有可用的真容器解码后端（CUI_VIDEO_SYS / CUI_VIDEO_FFMPEG 都没开或探测未通过）\n");
        // 跳过不等于什么都不钉：能力申报必须跟着如实空掉。
        check(videoCapabilities().nativeName.empty(), "没后端时 nativeName 为空（不谎报）");
        if (g_failures) {
            std::printf("=== 失败 %d 项 ===\n", g_failures);
            return 1;
        }
        return 0;
    }
    testInitialize();
    testSeekAndComplete();
    testParameterClamp();
    testPlayPauseClock();
    testPlayFromEnd();
    testPlayAfterNaturalEnd();
    testLooping();
    testListenersAndDispose();
    testVideoPlayerWidget();
    testProgressIndicator();
    testSubscriptionTeardown();
    testControlsHitTest();
    testOverlayControls();
    testStageTapPrecedence();
    testBackendAudioTrack();
    testRealDeviceEndToEnd();
    if (g_failures) {
        std::printf("=== 失败 %d 项 ===\n", g_failures);
        return 1;
    }
    std::printf("=== 全部通过 ===\n");
    return 0;
}
