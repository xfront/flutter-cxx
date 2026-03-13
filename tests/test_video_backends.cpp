/// @file test_video_backends.cpp
/// @brief 后端表（系统端 / FFmpeg 端）的选表、降级、优先级与探测契约。
///
/// 钉的是「两条路线并存之后怎么选」这一层，全部用注册进来的假件，
/// 不依赖任何真解码器（真后端的口径由 `test_video_player` 守）：
///
///   1. 内置表形状：`videoBackendCatalog()` 与编译期宏一致，
///      `videoCapabilities().nativeName` == 表里第一个运行期可用项。
///      （两条编译开关都关时表应为空，这一节的口径随之翻转。）
///   2. preference：`SystemFirst`/`FfmpegFirst`/`SystemOnly`/`FfmpegOnly`
///      对**整张表**（含注册项）生效，同组内保持注册顺序。
///   3. 同名顶掉：注册一条与内置同名的假件 ⇒ 内置那条不再进表。
///   4. 降级链：第一个后端拒了这个容器 ⇒ 继续试下一个；全都失败 ⇒
///      `error` 留下第一个的具体原因（不是含糊的「没有可用后端」）。
///   5. 出声设备：注册项的 `sinkFactory` 优先于 `createPlatformAudioSink()`。
///   6. 清空注册表 + 复位 preference 后回到内置表（不留全局污染）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cui/video/audio_sink.h"
#include "cui/video/video_decoder.h"

// 编译期有没有任何一条「真容器」路线进来。两个开关都关时（`-DCUI_VIDEO_SYS=OFF
// -DCUI_VIDEO_FFMPEG=OFF`）表应为空，这不是错 —— 下面的口径随之翻转。
#if defined(CUI_HAVE_FFMPEG) || defined(CUI_HAVE_MEDIA_FOUNDATION) || defined(CUI_HAVE_AVFOUNDATION) || \
    defined(CUI_HAVE_MEDIACODEC) || defined(CUI_HAVE_BROWSER_VIDEO) || defined(CUI_HAVE_GSTREAMER) ||    \
    defined(CUI_HAVE_OH_AV_CODEC)
#define CUI_VIDEO_ANY_BACKEND 1
#else
#define CUI_VIDEO_ANY_BACKEND 0
#endif

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

/// 一个「不是任何真格式」的字节流：文件头嗅探认不出图片，因此一定会走原生
/// 后端那一条（正好用来观察选表结果）。
VideoSource fakeSource() {
    return VideoSource::memory(std::vector<uint8_t>(256, 0x00), "mp4");
}

// ── 假后端 ─────────────────────────────────────────────────────────────────

struct FactoryState {
    int calls = 0;                ///< 工厂被调了几次（谁真的接走了解码）
    bool succeed = true;          ///< false ⇒ 拒了这个容器
    std::string rejectReason;     ///< 拒绝时填进 `error` 的具体原因
    bool hasAudio = false;        ///< 申报有音轨（决定引擎要不要起设备）
    int width = 64;               ///< 用来区分「到底是哪一条接走的」
};

class FakeBackend final : public VideoDecoderBackend {
public:
    explicit FakeBackend(int width) {
        info_.width = width;
        info_.height = 36;
        info_.duration = std::chrono::milliseconds(1000);
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = "fake";
    }

    const VideoInfo& info() const override { return info_; }
    /// 只用来证明「选中的是哪一条」，不参与取帧节拍（测试里不 play）。
    bool readFrame(VideoFrame& /*out*/) override {
        atEnd_ = true;
        return false;
    }
    bool seekTo(int64_t) override { return true; }
    bool atEnd() const override { return atEnd_; }
    const std::string& error() const override { return error_; }
    bool hasAudioStream() const override { return info_.hasAudio; }
    AudioFormat audioFormat() const override { return AudioFormat{1000, 1}; }

    void setHasAudio(bool hasAudio) {
        info_.hasAudio = hasAudio;
    }

private:
    VideoInfo info_;
    std::string error_;
    bool atEnd_ = false;
};

VideoBackendRegistration fakeRegistration(std::string name,
                                          bool systemDecoder,
                                          std::shared_ptr<FactoryState> state,
                                          AudioSinkFactory sinkFactory = nullptr) {
    VideoBackendRegistration reg;
    reg.name = std::move(name);
    reg.audioName = systemDecoder ? std::string("FakeSysSink") : std::string();
    reg.sinkFactory = std::move(sinkFactory);
    reg.platformDecoder = systemDecoder;
    reg.factory = [state = std::move(state)](const VideoSource&, std::string& error)
            -> std::unique_ptr<VideoDecoderBackend> {
        ++state->calls;
        if (!state->succeed) {
            error = state->rejectReason;
            return nullptr;
        }
        auto backend = std::make_unique<FakeBackend>(state->width);
        backend->setHasAudio(state->hasAudio);
        return backend;
    };
    return reg;
}

// ── 假出声设备 ─────────────────────────────────────────────────────────────

struct SinkState {
    int openCalls = 0;
    bool opened = false;
    AudioFormat seenFormat;
    std::string seenName;  ///< 哪一个工厂起的设备
};

class FakeSink final : public AudioSink {
public:
    FakeSink(std::shared_ptr<SinkState> state, std::string name)
        : st_(std::move(state)), name_(std::move(name)) {}

    bool open(const AudioFormat& format, std::string&) override {
        ++st_->openCalls;
        st_->seenFormat = format;
        st_->seenName = name_;
        st_->opened = true;
        return true;
    }
    void close() override { st_->opened = false; }
    void start() override {}
    void stop() override {}
    void flush(int64_t) override {}
    size_t push(const AudioChunk& chunk) override { return chunk.frames; }
    void setVolume(float) override {}
    int64_t positionUs() const override { return 0; }
    int64_t queuedUs() const override { return 0; }

private:
    std::shared_ptr<SinkState> st_;
    std::string name_;
};

std::vector<std::string> catalogNames() {
    std::vector<std::string> names;
    for (const auto& st : videoBackendCatalog())
        names.push_back(st.name);
    return names;
}

bool hasName(const std::vector<std::string>& names, const std::string& wanted) {
    for (const auto& n : names)
        if (n == wanted)
            return true;
    return false;
}

int indexOf(const std::vector<std::string>& names, const std::string& wanted) {
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == wanted)
            return int(i);
    return -1;
}

// ── 1：内置表形状 ──────────────────────────────────────────────────────────

void testBuiltinShape() {
    std::printf("--- 1: 内置表与能力申报一致 ---\n");
    clearRegisteredVideoBackends();
    setVideoBackendPreference(VideoBackendPreference::Auto);

    const auto catalog = videoBackendCatalog();
#if CUI_VIDEO_ANY_BACKEND
    check(!catalog.empty(), "至少编进了一条真容器后端");
#else
    check(catalog.empty(), "两条路线都没编（CUI_VIDEO_SYS/CUI_VIDEO_FFMPEG）⇒ 表为空");
#endif

    const VideoCapabilities caps = videoCapabilities();
    int firstUsable = -1;
    for (size_t i = 0; i < catalog.size(); ++i) {
        check(catalog[i].compiled, "表里出现的就是编进来了的：" + catalog[i].name);
        if (catalog[i].available && firstUsable < 0)
            firstUsable = int(i);
    }
    check(caps.nativeName == (firstUsable >= 0 ? catalog[size_t(firstUsable)].name : std::string{}),
          "nativeName 就是第一个运行期可用的那一个");
    check(caps.systemDecoder == (firstUsable >= 0 && catalog[size_t(firstUsable)].kind ==
                                 VideoBackendKind::System),
          "systemDecoder 跟着「选中的是不是系统端」");
#if defined(CUI_HAVE_FFMPEG)
    check(hasName(catalogNames(), "FFmpeg"), "编进 FFmpeg 时表里有 FFmpeg");
    check(caps.ffmpeg, "ffmpeg 字段报的是编译期事实（与能不能用无关）");
    const bool ffmpegUsable = [&] {
        for (const auto& st : catalog)
            if (st.name == "FFmpeg")
                return st.available;
        return false;
    }();
    check(ffmpegUsable, "本机 FFmpeg 的 ABI 探测通过");
    // FFmpeg 这一份现在也交 PCM（`video_decoder_ffmpeg.cpp` 覆写了 `readAudio`：
    // swresample 直接出 S16 交织）⇒ 申报口径跟着「这台机器上编进了哪个 sink」走：
    // 有 sink 时表里那一项必须带设备名，`videoCapabilities()` 在选中 FFmpeg 那一路
    // 也要如实报「含音频」；反过来，一个 sink 都没编进来的端（web 除外，它自带
    // 出声）必须两项都空 —— 名字一旦填上而设备起不来，报表就是谎报。
    for (const auto& st : catalog) {
        if (st.name != "FFmpeg")
            continue;
        check(!st.selfAudio, "FFmpeg 的 PCM 要经本仓 AudioSink，不是后端自带出声");
#if defined(CUI_HAVE_AUDIO_SINK)
        check(!st.audioName.empty(), "FFmpeg 交 PCM ⇒ 申报带上平台 sink 名");
        if (caps.nativeName == "FFmpeg")
            check(caps.nativeAudio, "选中 FFmpeg 且编进了 sink ⇒ nativeAudio 为真");
#else
        check(st.audioName.empty(), "这一端没编进 sink ⇒ FFmpeg 那一项不能带设备名");
        check(!caps.nativeAudio || caps.nativeName != "FFmpeg",
              "选中 FFmpeg 但没 sink 时 nativeAudio 必须为 false");
#endif
    }
#endif

    // 默认（Auto）系统端在前：本机没有系统端时 FFmpeg 排第一，但
    // 「系统端优先」这条要能测到 —— 由第 2 节用假件钉。
#if defined(CUI_HAVE_FFMPEG)
    const auto names = catalogNames();
    check(indexOf(names, "FFmpeg") == 0 || caps.systemDecoder,
          "默认顺序：没有可用系统端时 FFmpeg 就是第一个");
#endif
}

// ── 2：preference 决定顺序与参与 ──────────────────────────────────────────

void testPreference() {
    std::printf("--- 2: preference 的先后与开关 ---\n");
    clearRegisteredVideoBackends();

    auto sysState = std::make_shared<FactoryState>();
    auto ffmpegState = std::make_shared<FactoryState>();
    sysState->width = 111;
    ffmpegState->width = 222;
    registerVideoBackend(fakeRegistration("FakeSys", true, sysState));
    registerVideoBackend(fakeRegistration("FakeFfmpeg", false, ffmpegState));

    setVideoBackendPreference(VideoBackendPreference::Auto);
    auto names = catalogNames();
    check(indexOf(names, "FakeSys") < indexOf(names, "FakeFfmpeg"),
          "Auto：系统端排在 FFmpeg 前");

    setVideoBackendPreference(VideoBackendPreference::FfmpegFirst);
    names = catalogNames();
    check(indexOf(names, "FakeFfmpeg") < indexOf(names, "FakeSys"),
          "FfmpegFirst：FFmpeg 端排到前面");

    setVideoBackendPreference(VideoBackendPreference::SystemOnly);
    names = catalogNames();
    check(hasName(names, "FakeSys") && !hasName(names, "FakeFfmpeg"),
          "SystemOnly：FFmpeg 端整组不参与");

    setVideoBackendPreference(VideoBackendPreference::FfmpegOnly);
    names = catalogNames();
    check(!hasName(names, "FakeSys") && hasName(names, "FakeFfmpeg"),
          "FfmpegOnly：系统端整组不参与");

    // 「谁接走解码」必须与表顺序一致：系统端优先时只有系统端的工厂被调。
    setVideoBackendPreference(VideoBackendPreference::Auto);
    sysState->calls = ffmpegState->calls = 0;
    std::string error;
    auto playback = VideoPlayback::create(fakeSource(), error);
    check(playback != nullptr, "Auto：第一个就打得开");
    check(sysState->calls == 1 && ffmpegState->calls == 0, "Auto：系统端接走，FFmpeg 没被问");
    if (playback)
        check(playback->info().width == 111, "选中的后端信息确实来自系统端假件");
    if (playback)
        playback->dispose();

    setVideoBackendPreference(VideoBackendPreference::FfmpegFirst);
    sysState->calls = ffmpegState->calls = 0;
    playback = VideoPlayback::create(fakeSource(), error);
    check(playback != nullptr, "FfmpegFirst：照样打得开");
    check(ffmpegState->calls == 1 && sysState->calls == 0, "FfmpegFirst：FFmpeg 先被问");
    if (playback)
        check(playback->info().width == 222, "选中的后端信息来自 FFmpeg 假件");
    if (playback)
        playback->dispose();

    setVideoBackendPreference(VideoBackendPreference::Auto);
    clearRegisteredVideoBackends();
}

// ── 3：同名顶掉内置项 ──────────────────────────────────────────────────────

void testOverrideByName() {
    std::printf("--- 3: 注册项顶掉同名内置后端 ---\n");
    clearRegisteredVideoBackends();
    setVideoBackendPreference(VideoBackendPreference::Auto);

#if defined(CUI_HAVE_FFMPEG)
    auto state = std::make_shared<FactoryState>();
    state->width = 333;
    registerVideoBackend(fakeRegistration("FFmpeg", false, state));

    int ffmpegEntries = 0;
    for (const auto& n : catalogNames())
        if (n == "FFmpeg")
            ++ffmpegEntries;
    check(ffmpegEntries == 1, "同名只留一条（内置的那条被顶掉）");

    std::string error;
    auto playback = VideoPlayback::create(fakeSource(), error);
    check(playback != nullptr, "假件接走了本来会失败的字节流");
    check(state->calls == 1, "假件工厂被调一次");
    if (playback)
        check(playback->info().width == 333, "解码由同名注册项完成");
    if (playback)
        playback->dispose();
#else
    check(true, "没编 FFmpeg：跳过同名顶掉（本机应有 FFmpeg）");
#endif
    clearRegisteredVideoBackends();
}

// ── 4：降级链与错误文案 ────────────────────────────────────────────────────

void testFallbackChain() {
    std::printf("--- 4: 打不开时往后退，错误留最具体的 ---\n");
    clearRegisteredVideoBackends();
    setVideoBackendPreference(VideoBackendPreference::Auto);

    // 第一条拒了这个容器，第二条能开 ⇒ 成功，且 error 为空。
    auto bad = std::make_shared<FactoryState>();
    bad->succeed = false;
    bad->rejectReason = "FakeSys 不支持这个容器";
    auto good = std::make_shared<FactoryState>();
    good->width = 444;
    registerVideoBackend(fakeRegistration("FakeBad", true, bad));
    registerVideoBackend(fakeRegistration("FakeGood", true, good));

    std::string error;
    auto playback = VideoPlayback::create(fakeSource(), error);
    check(playback != nullptr, "第一个拒了 ⇒ 第二个接走");
    check(bad->calls == 1 && good->calls == 1, "两条都问过（顺序按注册先后）");
    check(error.empty(), "最终成功 ⇒ 不报错误");
    if (playback)
        playback->dispose();

    // 全都拒了 ⇒ 报第一条的具体原因，而不是含糊的「没有可用后端」。
    auto good2 = std::make_shared<FactoryState>();
    good2->succeed = false;
    good2->rejectReason = "FakeGood 也解不开";
    registerVideoBackend(fakeRegistration("FakeGood", true, good2));
    playback = VideoPlayback::create(fakeSource(), error);
    check(playback == nullptr, "全都打不开 ⇒ create 失败");
    check(error.find("FakeSys 不支持这个容器") != std::string::npos,
          "error 留下第一条的具体原因");
    check(bad->calls == 2 && good2->calls == 1, "降级顺序：第二条也试过");

    clearRegisteredVideoBackends();
}

// ── 5：出声设备的优先权 ────────────────────────────────────────────────────

void testSinkPreference() {
    std::printf("--- 5: 注册项的出声设备优先于平台设备 ---\n");
    clearRegisteredVideoBackends();
    setVideoBackendPreference(VideoBackendPreference::Auto);

    auto sinkState = std::make_shared<SinkState>();
    auto sysState = std::make_shared<FactoryState>();
    sysState->hasAudio = true;
    registerVideoBackend(fakeRegistration(
            "FakeSysAudio", true, sysState,
            [sinkState]() { return std::make_unique<FakeSink>(sinkState, "FakeSysAudio"); }));

    std::string error;
    auto playback = VideoPlayback::create(fakeSource(), error);
    check(playback != nullptr, "有音轨的后端能正常打开");
    if (playback)
        check(playback->hasAudioOutput(), "注册项的 sinkFactory 起了设备");
    check(sinkState->openCalls == 1, "设备只 open 一次");
    check(sinkState->seenName == "FakeSysAudio", "用的就是注册项带来的那一个");
    check(sinkState->seenFormat == AudioFormat{1000, 1}, "open 收到后端的采样格式");
    if (playback)
        playback->dispose();

    // 没有注册设备时退回平台设备（本机没编 ALSA ⇒ 无声，但不算错误）。
    clearRegisteredVideoBackends();
    auto bare = std::make_shared<FactoryState>();
    bare->hasAudio = true;
    registerVideoBackend(fakeRegistration("FakeBare", true, bare));
    playback = VideoPlayback::create(fakeSource(), error);
    check(playback != nullptr, "没有可用设备也照样能播（无声退回墙钟）");
    if (playback)
        check(playback->error().empty(), "设备起不来不写进 error");
    if (playback)
        playback->dispose();
    clearRegisteredVideoBackends();
}

// ── 6：不留全局污染 ────────────────────────────────────────────────────────

void testReset() {
    std::printf("--- 6: 清空注册表后回到内置表 ---\n");
    clearRegisteredVideoBackends();
    setVideoBackendPreference(VideoBackendPreference::FfmpegFirst);
    const auto names = catalogNames();
    check(!hasName(names, "FakeSys") && !hasName(names, "FakeBad"),
          "注册表清空后假件都不在表里");
    setVideoBackendPreference(VideoBackendPreference::Auto);
    check(videoBackendPreference() == VideoBackendPreference::Auto, "preference 能读回");
#if defined(CUI_HAVE_FFMPEG)
    check(hasName(catalogNames(), "FFmpeg"), "内置表还在");
#endif
}

}  // namespace

int main() {
    std::printf("=== test_video_backends ===\n");
    testBuiltinShape();
    testPreference();
    testOverrideByName();
    testFallbackChain();
    testSinkPreference();
    testReset();
    if (g_failures) {
        std::printf("=== 失败 %d 项 ===\n", g_failures);
        return 1;
    }
    std::printf("=== 全部通过 ===\n");
    return 0;
}
