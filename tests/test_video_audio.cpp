/// @file test_video_audio.cpp
/// @brief 播放引擎的音频链路与主时钟：假后端 + 假设备的契约测试。
///
/// 这里**不碰任何真解码器**（真解码器的口径由 `test_video_player` 守），钉的是
/// 「后端申报了什么 → 引擎做了什么」这一层跨端约定，四端共用同一份实现：
///
///   1. 线程模型：`createWithBackend()` 不起工作线程，节拍只能由 `pumpStep()`
///      驱动；非泵驱动后端调用 `pumpStep()` 是空操作。
///   2. 主时钟优先级：有 sink ⇒ 位置来自 `AudioSink::positionUs()`（故意让
///      墙钟快于音频，位置必须跟音频走）；倍速非 1.0 ⇒ 退回墙钟。
///   3. 无 sink（没音轨 / 设备起不来）⇒ 维持原来的墙钟推算，桌面与
///      Media Foundation 的行为不能被这次改造改动。
///   4. 控制指令下发：play/pause/seek/倍速/音量/dispose 各自都要同步到设备
///      （start/stop/flush 的次数与基准都钉住）。
///   5. 泵驱动语义：`readFrame` 返回 false 且非 `atEnd` 非错误 ⇒ 只是「这一轮
///      还没有新帧」，不置 completed、不写 error；到点判定用后端媒体时间，
///      未到点的帧先扣着（`staged_`），下一轮不再重复解码。
///   6. 喂数据与背压：`queuedUs()` 达到阈值就不再 `push`（100 毫秒的阈值远小于
///      环形缓冲 2 秒容量，所以正常路径永远写得下）。
///   7. 能力文案：`summary()` 的「含音频」与 `nativeAudio` 一致，
///      有/无设备时 `hasAudioOutput()` 的取值。
///
/// 采样率取 1000、视频取 10 帧 × 100 毫秒，微秒↔帧数换算都是整数，断言里
/// 不需要容差。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <include/core/SkBitmap.h>
#include <include/core/SkColor.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

#include "cui/video/audio_sink.h"
#include "cui/video/video_decoder.h"

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

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int64_t us(std::chrono::microseconds v) { return v.count(); }

// ── 素材参数（换算全部取整） ────────────────────────────────────────────────

constexpr int kSampleRate = 1000;          ///< 1 帧采样 = 1000 微秒
constexpr int kChannels = 1;
constexpr int64_t kFrameUs = 100000;       ///< 视频 10 fps
constexpr int kVideoFrames = 10;           ///< 时长 1 秒
constexpr int64_t kDurationUs = kFrameUs * kVideoFrames;
constexpr size_t kChunkFrames = 100;       ///< 每段 PCM = 100 毫秒

sk_sp<SkImage> makeFrameImage(int index) {
    constexpr int kW = 4;
    constexpr int kH = 4;
    const SkImageInfo info = SkImageInfo::MakeN32Premul(kW, kH);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info))
        return nullptr;
    bitmap.eraseColor(SkColorSetARGB(255, 20, 40, 60 + index));
    auto data = SkData::MakeWithCopy(bitmap.getPixels(), bitmap.rowBytes() * size_t(kH));
    return SkImages::RasterFromData(info, std::move(data), bitmap.rowBytes());
}

// ── 假后端 ─────────────────────────────────────────────────────────────────
//
// 状态放在 `BackendState` 里（shared_ptr），这样后端随播放引擎析构之后
// 测试仍然读得到「引擎到底调了它几次」。

struct BackendState {
    int seekCalls = 0;
    int64_t lastSeekUs = -1;
    int flushAudioCalls = 0;
    int readFrameCalls = 0;
    int readAudioCalls = 0;
    int pumpSetPlayingCalls = 0;
    int pumpSetSpeedCalls = 0;
    int pumpSetVolumeCalls = 0;
    bool pumpPlaying = false;
    double pumpSpeed = 1.0;
    double pumpVolume = 1.0;
    /// 泵驱动后端是否报得出媒体时间（false ⇒ 返回 -1，引擎退回墙钟）。
    bool reportMaster = false;
    int64_t masterUs = 0;
};

class FakeBackend final : public VideoDecoderBackend {
public:
    FakeBackend(std::shared_ptr<BackendState> state, bool hasAudio, bool pumpDriven)
        : st_(std::move(state)), hasAudio_(hasAudio), pumpDriven_(pumpDriven) {
        info_.width = 64;
        info_.height = 36;
        info_.duration = std::chrono::microseconds(kDurationUs);
        info_.frameRate = 10.0;
        info_.hasAudio = hasAudio;
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = "fake";
    }

    void setWithholdFrames(bool withhold) { withhold_ = withhold; }
    void setErrorText(std::string text) { error_ = std::move(text); }
    /// 让后端提前跑到第 `index` 帧（用来快速构造「快播完」的场景）。
    void forceIndex(int index) { index_ = index; }

    const VideoInfo& info() const override { return info_; }

    bool readFrame(VideoFrame& out) override {
        ++st_->readFrameCalls;
        if (withhold_)
            return false;  // 「这一轮还没有新帧」：既不是结束也不是错误
        if (index_ >= kVideoFrames) {
            atEnd_ = true;
            return false;
        }
        out.image = makeFrameImage(index_);
        out.ptsUs = int64_t(index_) * kFrameUs;
        ++index_;
        atEnd_ = index_ >= kVideoFrames;
        return true;
    }

    bool seekTo(int64_t ptsUs) override {
        ++st_->seekCalls;
        st_->lastSeekUs = ptsUs;
        const int64_t clamped = std::clamp<int64_t>(ptsUs, 0, kDurationUs);
        index_ = int(clamped / kFrameUs);
        atEnd_ = index_ >= kVideoFrames;
        audioCursor_ = framesAtUs(int64_t(index_) * kFrameUs);
        return true;
    }

    bool atEnd() const override { return atEnd_; }
    const std::string& error() const override { return error_; }

    // ── 音频 ────────────────────────────────────────────────────────────
    bool hasAudioStream() const override { return hasAudio_; }
    AudioFormat audioFormat() const override { return AudioFormat{kSampleRate, kChannels}; }

    bool readAudio(AudioChunk& out) override {
        ++st_->readAudioCalls;
        const size_t total = framesAtUs(kDurationUs);
        if (!hasAudio_ || audioCursor_ >= total)
            return false;
        const size_t n = std::min(kChunkFrames, total - audioCursor_);
        pcm_.assign(n * size_t(kChannels), int16_t(audioCursor_ % 7));
        out.s16 = pcm_.data();
        out.format = AudioSampleFormat::S16Interleaved;
        out.frames = n;
        out.ptsUs = usAtFrame(audioCursor_);
        audioCursor_ += n;
        return true;
    }

    void flushAudio() override {
        ++st_->flushAudioCalls;
        audioCursor_ = framesAtUs(int64_t(index_) * kFrameUs);
    }

    // ── 泵驱动 ──────────────────────────────────────────────────────────
    bool pumpDriven() const override { return pumpDriven_; }
    void pumpSetPlaying(bool playing) override {
        ++st_->pumpSetPlayingCalls;
        st_->pumpPlaying = playing;
    }
    void pumpSetSpeed(double speed) override {
        ++st_->pumpSetSpeedCalls;
        st_->pumpSpeed = speed;
    }
    void pumpSetVolume(double volume) override {
        ++st_->pumpSetVolumeCalls;
        st_->pumpVolume = volume;
    }
    int64_t pumpPositionUs() const override { return st_->reportMaster ? st_->masterUs : -1; }

private:
    static size_t framesAtUs(int64_t usValue) {
        return size_t(usValue * int64_t(kSampleRate) / 1000000);
    }
    static int64_t usAtFrame(size_t frame) { return int64_t(frame) * 1000000 / kSampleRate; }

    std::shared_ptr<BackendState> st_;
    VideoInfo info_;
    bool hasAudio_ = false;
    bool pumpDriven_ = false;
    int index_ = 0;
    bool atEnd_ = false;
    bool withhold_ = false;
    std::string error_;
    std::vector<int16_t> pcm_;
    size_t audioCursor_ = 0;
};

// ── 假音频设备 ─────────────────────────────────────────────────────────────

struct SinkState {
    AudioFormat format;
    int openCalls = 0;
    int closeCalls = 0;
    int startCalls = 0;
    int stopCalls = 0;
    int flushCalls = 0;
    int64_t lastFlushPtsUs = -1;
    int pushCalls = 0;
    int64_t pushedFrames = 0;
    int64_t queuedFrames = 0;
    float volume = 1.0f;
    bool opened = false;
    bool running = false;
    /// 位置由测试手工推进（真设备是靠回调消耗的，这里要的是可控）。
    int64_t posUs = 0;
};

class FakeSink final : public AudioSink {
public:
    explicit FakeSink(std::shared_ptr<SinkState> state) : st_(std::move(state)) {}
    ~FakeSink() override { close(); }

    bool open(const AudioFormat& format, std::string& error) override {
        ++st_->openCalls;
        st_->format = format;
        st_->opened = true;
        if (!format.isValid()) {
            error = "采样格式无效";
            st_->opened = false;
            return false;
        }
        return true;
    }
    void close() override {
        if (!st_->opened)
            return;
        ++st_->closeCalls;
        st_->opened = false;
        st_->running = false;
    }
    void start() override {
        ++st_->startCalls;
        st_->running = true;
    }
    void stop() override {
        ++st_->stopCalls;
        st_->running = false;
    }
    void flush(int64_t ptsUs) override {
        ++st_->flushCalls;
        st_->lastFlushPtsUs = ptsUs;
        st_->posUs = ptsUs;
        st_->queuedFrames = 0;
    }
    size_t push(const AudioChunk& chunk) override {
        ++st_->pushCalls;
        st_->pushedFrames += int64_t(chunk.frames);
        st_->queuedFrames += int64_t(chunk.frames);
        return chunk.frames;
    }
    void setVolume(float volume) override { st_->volume = volume; }
    int64_t positionUs() const override { return st_->posUs; }
    int64_t queuedUs() const override {
        return st_->format.sampleRate > 0
                       ? st_->queuedFrames * int64_t(1000000) / int64_t(st_->format.sampleRate)
                       : 0;
    }

private:
    std::shared_ptr<SinkState> st_;
};

/// 造一套「已 open 的假设备 + 假后端」并注进播放引擎。
/// `pumpDriven` 决定引擎走泵路径还是线程路径（注入时都不起线程）。
std::shared_ptr<VideoPlayback> makePlayback(bool hasAudio, bool pumpDriven,
                                            std::shared_ptr<BackendState>& backendState,
                                            std::shared_ptr<SinkState>& sinkState,
                                            FakeBackend** backendOut = nullptr) {
    backendState = std::make_shared<BackendState>();
    sinkState = std::make_shared<SinkState>();
    auto backend = std::make_unique<FakeBackend>(backendState, hasAudio, pumpDriven);
    if (backendOut)
        *backendOut = backend.get();
    auto sink = std::make_unique<FakeSink>(sinkState);
    std::string error;
    if (!sink->open(AudioFormat{kSampleRate, kChannels}, error))
        check(false, "假设备 open 失败: " + error);
    return VideoPlayback::createWithBackend(std::move(backend), std::move(sink));
}

std::shared_ptr<VideoPlayback> makeVideoOnlyPlayback(bool pumpDriven,
                                                    std::shared_ptr<BackendState>& backendState,
                                                    FakeBackend** backendOut = nullptr) {
    backendState = std::make_shared<BackendState>();
    std::shared_ptr<SinkState> unusedSinkState;
    (void)unusedSinkState;
    auto backend = std::make_unique<FakeBackend>(backendState, false, pumpDriven);
    if (backendOut)
        *backendOut = backend.get();
    return VideoPlayback::createWithBackend(std::move(backend));
}

bool publishOnce(VideoPlayback& playback) {
    VideoFrame frame;
    uint64_t serial = 0;
    const bool got = playback.takeLatestFrame(frame, serial);
    return got && frame.image != nullptr;
}

// ── 1：注入口与线程模型 ────────────────────────────────────────────────────

void testThreadModel() {
    std::printf("--- 1: 注入口不起工作线程 ---\n");
#if defined(__EMSCRIPTEN__)
    check(VideoPlayback::pumpDrivenPlatform(), "wasm 申报泵驱动平台");
#else
    check(!VideoPlayback::pumpDrivenPlatform(), "桌面/移动申报线程驱动平台");
#endif

    std::shared_ptr<BackendState> bs;
    FakeBackend* backend = nullptr;
    auto playback = makeVideoOnlyPlayback(/*pumpDriven=*/true, bs, &backend);

    playback->play();
    sleepMs(120);
    // 真起了工作线程的话，这两项早就被跑起来了（线程不等人）。
    check(bs->readFrameCalls == 0, "注入后 play() 没有偷偷解码（无工作线程）");
    check(!publishOnce(*playback), "没有 pumpStep 就没有新帧上屏");

    check(playback->pumpStep(), "泵驱动后端 pumpStep 有动作");
    check(bs->readFrameCalls == 1, "泵驱动取帧就发生在调用线程这一拍");

    // 非泵驱动后端（桌面/移动的真后端）调 pumpStep 必须是空操作，
    // 否则控件层那句无条件调用会跟工作线程抢解码。
    std::shared_ptr<BackendState> bs2;
    auto threaded = makeVideoOnlyPlayback(/*pumpDriven=*/false, bs2);
    threaded->play();
    check(!threaded->pumpStep(), "线程驱动后端调用 pumpStep 是空操作");
    check(bs2->readFrameCalls == 0, "线程驱动后端不被 pumpStep 抢帧");
    threaded->pause();
    threaded->dispose();

    // 打开时就把音量交给泵驱动后端（浏览器的 `<video>.volume`）。
    check(bs->pumpSetVolumeCalls >= 1 && bs->pumpVolume == 1.0, "初始音量下发到后端");

    playback->dispose();
}

// ── 2：有 sink ⇒ 主时钟是音频 ──────────────────────────────────────────────

void testAudioMasterClock() {
    std::printf("--- 2: 有音频设备时位置跟音频走 ---\n");
    std::shared_ptr<BackendState> bs;
    std::shared_ptr<SinkState> ss;
    auto playback = makePlayback(/*hasAudio=*/true, /*pumpDriven=*/false, bs, ss);

    check(playback->hasAudioOutput(), "有 sink ⇒ hasAudioOutput() 为真");

    playback->play();
    check(ss->startCalls == 1, "play ⇒ 设备起声");
    // 故意让设备停在 40 毫秒、墙钟跑到 200 毫秒：位置必须以音频为准。
    ss->posUs = 40000;
    sleepMs(200);
    check(us(playback->position()) == 40000, "墙钟更快时位置仍以音频为准");

    ss->posUs = 300000;
    check(us(playback->position()) == 300000, "设备推进 ⇒ 位置跟着推进");

    // 暂停抓取的是「音频位置」，之后设备再消耗也不许动进度条。
    playback->pause();
    check(!ss->running, "pause ⇒ 设备停");
    const int64_t frozen = us(playback->position());
    ss->posUs = 900000;
    sleepMs(30);
    check(us(playback->position()) == frozen, "暂停后位置冻结（不跟设备抖动）");

    // 倍速期间设备不会变速：主时钟退回墙钟，且这一头要保持静音。
    playback->play();
    const int startBefore = ss->startCalls;
    playback->setPlaybackSpeed(2.0);
    check(ss->stopCalls >= 1, "倍速 ⇒ 设备先停");
    check(!ss->running, "倍速期间设备没在跑");
    const int64_t wallFrom = us(playback->position());
    sleepMs(120);
    const int64_t wallTo = us(playback->position());
    check(wallTo > wallFrom, "倍速期间位置由墙钟给出");
    check(wallTo - wallFrom >= 60000, "倍速期间位置按倍速前进");
    check(ss->startCalls == startBefore, "倍速期间没有偷偷重启设备");

    const int64_t resumeFrom = us(playback->position());
    playback->setPlaybackSpeed(1.0);
    check(ss->startCalls == startBefore + 1, "回到 1.0 倍速 ⇒ 设备重新起声");
    check(std::llabs(ss->lastFlushPtsUs - resumeFrom) < 50000,
          "重新起声前把设备位置基准搬到当前点");

    // 夹到时长内：设备报得再远也不能超过 duration。
    ss->posUs = kDurationUs + 5000000;
    check(us(playback->position()) == kDurationUs, "位置夹到 duration");

    playback->pause();
    playback->dispose();
}

// ── 3：没有 sink ⇒ 维持墙钟 ────────────────────────────────────────────────

void testWallClockFallback() {
    std::printf("--- 3: 无音轨 / 设备起不来时退回墙钟 ---\n");
    std::shared_ptr<BackendState> bs;
    auto playback = makeVideoOnlyPlayback(/*pumpDriven=*/false, bs);

    check(!playback->hasAudioOutput(), "无音轨 ⇒ 无声");
    playback->play();
    const int64_t from = us(playback->position());
    sleepMs(120);
    const int64_t to = us(playback->position());
    check(to > from, "墙钟路线：位置照常前进");
    check(to - from >= 60000, "墙钟路线：前进量对得上真实时间");

    playback->pause();
    const int64_t paused = us(playback->position());
    sleepMs(80);
    check(us(playback->position()) == paused, "墙钟路线：暂停后冻结");
    playback->dispose();

    // 有音轨但平台没有出声设备（桌面 / Windows Media Foundation 一直如此）。
    const bool platformHasAudio = createPlatformAudioSink() != nullptr;
    if (platformHasAudio) {
        std::printf("      本机有音频设备，跳过「设备起不来」这一支\n");
        return;
    }
    auto audioBackend = std::make_unique<FakeBackend>(std::make_shared<BackendState>(),
                                                      /*hasAudio=*/true, /*pumpDriven=*/false);
    auto fallback = VideoPlayback::createWithBackend(std::move(audioBackend));
    check(!fallback->hasAudioOutput(), "设备起不来 ⇒ 无声（不算错误）");
    check(fallback->error().empty(), "设备起不来不写 error");
    fallback->play();
    sleepMs(120);
    check(us(fallback->position()) >= 60000, "设备起不来 ⇒ 位置走墙钟");
    fallback->pause();
    fallback->dispose();
}

// ── 4：控制指令都同步到设备 ────────────────────────────────────────────────

void testControlForwarding() {
    std::printf("--- 4: play/pause/seek/音量/dispose 与设备同步 ---\n");
    std::shared_ptr<BackendState> bs;
    std::shared_ptr<SinkState> ss;
    auto playback = makePlayback(/*hasAudio=*/true, /*pumpDriven=*/true, bs, ss);

    const int bindFlushes = ss->flushCalls;
    check(bindFlushes == 1 && ss->lastFlushPtsUs == 0, "接线时先把设备基准对齐 0");
    check(ss->startCalls == 0, "接线阶段不起声（没 play 就不该出声）");

    playback->play();
    check(ss->startCalls == 1 && ss->flushCalls == bindFlushes + 1, "play ⇒ flush + start");
    check(bs->pumpPlaying, "play 同时下发给泵驱动后端");

    playback->pause();
    check(ss->stopCalls == 1 && !ss->running, "pause ⇒ 设备 stop");
    check(!bs->pumpPlaying, "pause 下发给泵驱动后端");

    playback->play();
    check(ss->startCalls == 2, "再 play ⇒ 重新 start（位置从冻结点续）");

    playback->seekTo(std::chrono::microseconds(200000));
    check(ss->stopCalls == 2, "seek 期间先静音（等后端真的跳过去）");
    check(bs->seekCalls == 0, "seek 只是挂起，交给下一拍下发");
    check(!playback->pumpStep(), "处理 seek 的那一拍不出帧");
    check(bs->seekCalls == 1 && bs->lastSeekUs == 200000, "下一拍把 seek 下发给后端");
    check(bs->flushAudioCalls == 1, "seek 后清掉后端手里没交出的 PCM");
    check(ss->startCalls == 3, "seek 完成后重新起声");
    check(ss->lastFlushPtsUs == 200000, "seek 后设备位置基准 = 目标");

    playback->setVolume(0.3);
    check(std::fabs(ss->volume - 0.3f) < 1e-6, "setVolume 下发到设备");
    check(std::fabs(bs->pumpVolume - 0.3) < 1e-6, "setVolume 也下发到泵驱动后端");
    check(std::fabs(playback->volume() - 0.3) < 1e-9, "volume() 读回 0.3");
    playback->setVolume(5.0);
    check(std::fabs(playback->volume() - 1.0) < 1e-9 && std::fabs(ss->volume - 1.0f) < 1e-6,
          "音量夹到 1.0");

    playback->play();
    playback->dispose();
    check(!ss->running, "dispose ⇒ 设备已停");
    check(ss->closeCalls == 1, "dispose ⇒ 设备被释放一次");
    check(!bs->pumpPlaying, "dispose ⇒ 泵驱动后端收到暂停");
    playback->dispose();
    check(ss->closeCalls == 1, "dispose 幂等");

    // 泵驱动后端的倍速交给它自己（浏览器有 playbackRate），不许动设备。
    std::shared_ptr<BackendState> bs2;
    std::shared_ptr<SinkState> ss2;
    auto pump = makePlayback(true, true, bs2, ss2);
    pump->play();
    const int stops = ss2->stopCalls;
    pump->setPlaybackSpeed(2.0);
    check(std::fabs(bs2->pumpSpeed - 2.0) < 1e-9, "倍速下发给泵驱动后端");
    check(ss2->stopCalls == stops, "泵驱动路线不碰音频设备（那条路归浏览器）");
    pump->dispose();
}

// ── 5：泵驱动语义 ──────────────────────────────────────────────────────────

void testPumpSemantics() {
    std::printf("--- 5: 泵驱动的「没有新帧」不是结束也不是错误 ---\n");
    std::shared_ptr<BackendState> bs;
    FakeBackend* backend = nullptr;
    auto playback = makeVideoOnlyPlayback(true, bs, &backend);
    backend->setWithholdFrames(true);

    playback->play();
    check(!playback->pumpStep(), "这一轮没有新帧");
    check(!playback->isCompleted(), "「没有新帧」不置 completed");
    check(playback->error().empty(), "「没有新帧」不写 error");
    check(playback->isPlaying(), "「没有新帧」不影响播放态");

    backend->setWithholdFrames(false);
    check(playback->pumpStep(), "下一轮取到帧就上屏");
    check(publishOnce(*playback), "帧真的发布了");

    // 到点判定用后端的媒体时间；未到点的帧要扣着，别重复解码。
    bs->reportMaster = true;
    bs->masterUs = 0;
    const int readsBefore = bs->readFrameCalls;
    check(!playback->pumpStep(), "下一帧还没到点 ⇒ 不上屏");
    check(bs->readFrameCalls == readsBefore + 1, "未到点的帧已经解出来了（扣在 staged）");
    check(!playback->pumpStep(), "同一帧不会重复解（第二拍只看时间）");
    check(bs->readFrameCalls == readsBefore + 1, "第二拍确实没有再解码");
    bs->masterUs = kFrameUs;
    check(playback->pumpStep(), "后端时间到点 ⇒ 扣着的那帧上屏");
    check(us(playback->position()) == kFrameUs, "泵驱动位置 = 后端媒体时间");

    // 后端报不出时间（元信息还没回来）⇒ 退回墙钟，不能卡住。
    bs->reportMaster = false;
    playback->seekTo(std::chrono::microseconds(2 * kFrameUs));
    check(!playback->pumpStep(), "这一拍先把挂起的 seek 下发掉");
    const int64_t fallbackFrom = us(playback->position());
    sleepMs(60);
    check(us(playback->position()) >= fallbackFrom, "后端时间不可用时位置不倒退");
    check(us(playback->position()) >= 2 * kFrameUs, "后端时间不可用时以 seek 点为基准");
    check(playback->pumpStep(), "后端时间不可用时照常出帧");

    // 流尾优先于错误：到点就是「播完」，不是失败。
    bs->reportMaster = true;
    bs->masterUs = kDurationUs;
    backend->setErrorText("假后端解码失败");
    backend->forceIndex(kVideoFrames);
    for (int i = 0; i < kVideoFrames + 4 && !playback->isCompleted(); ++i)
        playback->pumpStep();
    check(playback->isCompleted(), "解到流尾 ⇒ completed");
    check(!playback->isPlaying(), "流尾后自动暂停");
    check(playback->error().empty(), "到流尾不算错误");
    playback->dispose();

    // 循环：越过末尾只挂一次回卷，不置 completed。
    std::shared_ptr<BackendState> bsLoop;
    FakeBackend* loopBackend = nullptr;
    auto looping = makeVideoOnlyPlayback(true, bsLoop, &loopBackend);
    looping->setLooping(true);
    loopBackend->forceIndex(kVideoFrames - 1);
    looping->play();
    looping->pumpStep();               // 最后一帧
    check(!looping->pumpStep(), "越过末尾那一拍只挂起回卷");
    check(!looping->isCompleted(), "循环开启时不置 completed");
    check(looping->isPlaying(), "循环开启时仍在播放");
    looping->pumpStep();               // 这一拍真的 seek 回 0
    check(bsLoop->seekCalls == 1 && bsLoop->lastSeekUs == 0, "循环回卷下发 seek(0)");
    looping->dispose();

    // 到不了流尾但后端报了错 ⇒ 这才算失败。
    std::shared_ptr<BackendState> bsErr;
    FakeBackend* errBackend = nullptr;
    auto failing = makeVideoOnlyPlayback(true, bsErr, &errBackend);
    failing->play();
    errBackend->setWithholdFrames(true);  // 没到流尾、也取不到帧
    errBackend->setErrorText("假后端解码失败");
    check(!failing->pumpStep(), "出错那一拍不上屏");
    check(!failing->error().empty(), "后端错误写进 error()");
    check(failing->isCompleted(), "出错后收摊（不再空转）");
    check(!failing->isPlaying(), "出错后自动暂停");
    failing->dispose();
}

// ── 6：喂数据与背压 ────────────────────────────────────────────────────────

void testAudioFeed() {
    std::printf("--- 6: PCM 喂给与背压 ---\n");
    std::shared_ptr<BackendState> bs;
    std::shared_ptr<SinkState> ss;
    auto playback = makePlayback(/*hasAudio=*/true, /*pumpDriven=*/true, bs, ss);

    playback->play();
    check(playback->pumpStep(), "第一拍出帧");
    check(ss->pushCalls == 1, "一拍把队列填到阈值就停（100 毫秒）");
    check(ss->pushedFrames == int64_t(kChunkFrames), "push 的是一整段 100 毫秒");
    check(ss->queuedFrames * 1000000 / kSampleRate >= 100000, "队列里已有 100 毫秒垫底");

    // 队列满（积压 5 秒）时不许再塞。
    ss->queuedFrames = 5000;
    const int pushes = ss->pushCalls;
    const int reads = bs->readFrameCalls;
    playback->pumpStep();
    check(ss->pushCalls == pushes, "积压超过阈值 ⇒ 不再 push");
    check(bs->readFrameCalls == reads + 1, "不 push 也照常解码取帧");

    // 排空后继续喂。
    ss->queuedFrames = 0;
    bs->masterUs = 2 * kFrameUs;
    playback->pumpStep();
    check(ss->pushCalls > pushes, "队列排空后接着喂（否则要欠载）");

    // 没音轨的后端问不出 PCM，也就不会往队列里塞东西。
    std::shared_ptr<BackendState> bsSilent;
    std::shared_ptr<SinkState> ssSilent;
    auto silent = makePlayback(/*hasAudio=*/false, /*pumpDriven=*/true, bsSilent, ssSilent);
    silent->play();
    silent->pumpStep();
    check(bsSilent->readAudioCalls <= 1, "无音轨后端一问就断");
    check(ssSilent->pushCalls == 0, "无音轨 ⇒ 没有 PCM 进队列");
    silent->dispose();

    playback->pause();
    check(ss->running == false, "pause ⇒ 设备停，位置不再前进");
    playback->dispose();
}

// ── 7：能力与文案 ──────────────────────────────────────────────────────────

void testCapabilities() {
    std::printf("--- 7: 能力申报与文案 ---\n");
    const VideoCapabilities caps = videoCapabilities();
    check(caps.animatedImage, "动图后端恒可用");
    check((caps.summary().find("（含音频）") != std::string::npos) == caps.nativeAudio,
          "summary 的「含音频」与 nativeAudio 一致");
    // `nativeName` = 这台机器实际会用的那一个（运行期探测过的）；
    // 「只有动图后端」必须与「一个原生后端都不可用」同真同假。
    const bool usable = !caps.nativeName.empty();
    check(usable == (caps.summary().find("只有动图后端") == std::string::npos),
          "没有可用的原生后端时文案如实说明只有动图");
    if (usable) {
        check(caps.summary().find(caps.nativeName) != std::string::npos,
              "summary 里带上了实际生效的后端名");
        // 系统端标记必须跟「选中的是不是系统端」一致，不能各自为政。
        check(caps.systemDecoder == (caps.systemName == caps.nativeName),
              "systemDecoder 标记与实际选中的后端一致");
    }
    // 「自带声音」只有后端自己放声那一路（浏览器 / 系统播放器）；
    // MF 这类「只解码不出声」是合法组合，所以只验反向：报了出声设备
    // 就必须承认可有声音。
    if (!caps.systemAudioName.empty())
        check(caps.nativeAudio, "报了出声设备名 ⇒ nativeAudio 必须为真");

    // 运行期：有/无设备两条路的 hasAudioOutput 都在前面钉过了（2、3 节），
    // 这里再确认「无声」不会被误报成错误。
    std::shared_ptr<BackendState> bs;
    std::shared_ptr<SinkState> ss;
    auto withSink = makePlayback(true, false, bs, ss);
    check(withSink->hasAudioOutput(), "有设备 ⇒ 有声");
    check(withSink->error().empty(), "有声路径没有错误");
    withSink->dispose();
}

// ── 8：真平台设备冒烟（这台机器起不来就整节跳过）──────────────────────────

/// 前七节的设备全是 `FakeSink` —— 「平台 sink 这个类编进来了」不等于「这台机器
/// 的音频端点起得来」。`audio_sink_win32.cpp` 那三条有意偏离（按设备 mix format
/// 开、COM 只活在自起的搬运线程、`GetCurrentPadding` 当引擎真值）以前只有 mingw
/// 语法门兜着，这一节把它们变成实测：开/起声/位置前进/音量/flush/停/析构，
/// 以及整条播放引擎不注入 sink 时走真设备的端到端（环形缓冲 + 主时钟 + seek +
/// 暂停冻结 + dispose 无错误）。
/// 起不来（无默认端点、独占被占、VM 没声卡）不是失败：无声回退那条路由第 3 节
/// 钉着，这里只把「本机没验到」说出来。
void testRealDevice() {
    std::printf("--- 8: 真平台设备冒烟 ---\n");
    std::unique_ptr<AudioSink> probe = createPlatformAudioSink();
    if (!probe) {
        std::printf("      编译期没开出声设备，跳过\n");
        return;
    }
    // 1000Hz 单声道 = 假后端的口径，真设备几乎不可能以此 mix format 打开
    // ⇒ 正好压过 sink 自己的重采样 + 声道映射那段（WASAPI 共享模式必须自己抹平）。
    std::string probeError;
    if (!probe->open(AudioFormat{kSampleRate, kChannels}, probeError)) {
        std::printf("      本机起不来真设备（%s），跳过\n", probeError.c_str());
        return;
    }
    check(probe->positionUs() >= 0, "真设备未起声时位置可读且不为负");
    probe->start();
    sleepMs(120);
    const int64_t played = probe->positionUs();
    check(played > 0, "真设备起声后位置随真实时间前进");
    // 搬运线程与 GUI 线程之间最多晚 20ms 才生效，加上定时器粒度：给到 4 倍余量。
    check(played < 600000,
          "真设备位置没有跑飞（mix format 采样数换算没串位，实测 " +
                  std::to_string(played) + "us / 睡眠 120ms）");
    probe->setVolume(0.5f);
    probe->flush(0);
    check(probe->positionUs() >= 0, "真设备 flush 后仍可读位置");
    probe->stop();
    probe->close();
    check(true, "真设备 stop + close 走通");

    // 接线层：不注入 sink ⇒ `bindBackendLocked()` 只能拿平台真设备。
    // `createWithBackend` 不起工作线程（第 1 节钉的就是这条），所以这里只钉
    // 「设备有没有被真的开起来、音量/位置有没有透传」；带工作线程 + 真后端的
    // 完整链路（seek 落到设备 flush）在 test_video_player 第 15 节。
    std::shared_ptr<BackendState> bs = std::make_shared<BackendState>();
    auto backend = std::make_unique<FakeBackend>(bs, /*hasAudio=*/true, /*pumpDriven=*/false);
    std::shared_ptr<VideoPlayback> playback =
            VideoPlayback::createWithBackend(std::move(backend));
    check(playback->hasAudioOutput(), "真设备接上 ⇒ hasAudioOutput() 为真");
    check(playback->error().empty(), "真设备路径不写错误");
    playback->play();
    sleepMs(150);
    check(us(playback->position()) > 30000, "真设备做主时钟时位置会前进");
    playback->setVolume(0.3);
    check(std::fabs(playback->volume() - 0.3) < 1e-9, "音量透传（真设备不吞 setter）");
    playback->pause();
    const int64_t frozen = us(playback->position());
    sleepMs(80);
    check(us(playback->position()) == frozen, "暂停后位置冻结");
    playback->dispose();
    check(true, "真设备路径 dispose 走通");
}

}  // namespace

int main() {
    std::printf("=== test_video_audio ===\n");
    testThreadModel();
    testAudioMasterClock();
    testWallClockFallback();
    testControlForwarding();
    testPumpSemantics();
    testAudioFeed();
    testCapabilities();
    testRealDevice();
    testAudioMasterClock();
    testWallClockFallback();
    testControlForwarding();
    testPumpSemantics();
    testAudioFeed();
    testCapabilities();
    if (g_failures) {
        std::printf("=== 失败 %d 项 ===\n", g_failures);
        return 1;
    }
    std::printf("=== 全部通过 ===\n");
    return 0;
}
