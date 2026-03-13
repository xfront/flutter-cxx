/// @file video/audio_sink_android.cpp
/// @brief Android 出声设备（AAudio，回调模式）+ 主时钟来源。
///
/// 为什么是 AAudio 而不是 AudioTrack：`AudioSink` 契约里最硬的一条是
/// `positionUs()` —— 播放引擎拿它当主时钟（见 audio_ring.h 的位置口径）。AudioTrack
/// 也能问出已播帧数（`getPlaybackHeadPosition`），但那个值在低延迟模式下更新粒度很粗，
/// 而 AAudio 的回调本身就是「每 `framesPerDataCallback` 帧敲一次」，位置推进的分辨率
/// 直接等于一次回调的长度，音视频同步的抖动就小一档。
///
/// 为什么走 dlopen 而不是直接 `-laaudio`：以前是硬的（下限 `android-24` 时
/// `libaaudio.so` 的符号从 API 26 才有，直接链会被 NDK 的 ABI 检查拦下）；
/// 现在本仓下限已抬到 `android-28`（原因见 `scripts/package_apk.sh` 的注释：
/// vcpkg 的 ffmpeg@arm64-android 需要 bionic 自 API 28 才有的 `glob`），所以
/// 直接链已经能过。dlopen 保留下来是因为它还有用：
///   - 某些 ROM / 定制 HAL 不带 `libaaudio.so` ⇒ `dlopen` 失败 ⇒ `open()` 返回
///     false ⇒ 引擎退回「无声 + 墙钟」，与设备打不开时的既有行为同一条路，
///     而直接链接会让整个 `.so` 在 `dlopen` 阶段就加载失败；
///   - 这套函数指针声明已经过实测，换成 `-laaudio` + 直接调用不是本轮的事。
///
/// 线程：`push()`/`queuedUs()` 在解码工作线程，`start()`/`stop()`/`setVolume()` 在
/// GUI 线程，`audioCallback` 在 AAudio 自己的实时线程。三者只通过 `AudioRing`（自带
/// mutex）和下面两个原子量交互；`stream_` 的创建/销毁只发生在 GUI 线程的
/// `open()`/`close()` 里，而 `close()` 会等回调返回，所以不会出现回调打在已释放对象上。
///
/// 采样格式：`AudioRing` 内部恒 F32 交织，AAudio 直接要 `PCM_FLOAT` —— 省掉一次
/// 定点化。设备不接受 float 时（少数老 HAL）退回 `PCM_I16`，用 `readS16()` 取数。

#include "cui/video/audio_sink.h"

#include "audio_ring.h"

#if defined(__ANDROID__)

#include <aaudio/AAudio.h>
#include <cstdio>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>

namespace cui {
namespace {

// ── AAudio 符号（只用得到这些；API 26 起才有） ───────────────────────────────
// 这里**不写** `decltype(&AAudio_createStreamBuilder)`：下限还是 android-24 时那样写
// 必错（NDK 的头给每个函数都挂了 `__INTRODUCED_IN(26)`，光是「取这个函数的类型」就报
// unavailable，而该标注来自工具链，`#define __INTRODUCED_IN(x)` 压不掉）。现在下限是
// android-28 ⇒ 那样写已经合法，但函数本来就是运行期 dlsym 来的，编译期不需要它的声明
// —— 于是函数指针类型继续自己写（签名逐个对着 `aaudio/AAudio.h` 抄），而枚举/typedef
// 不受 availability 影响，依旧用头里的。
using CreateStreamBuilderFn = aaudio_result_t (*)(AAudioStreamBuilder**);
using BuilderDeleteFn = aaudio_result_t (*)(AAudioStreamBuilder*);
using BuilderSetDirectionFn = void (*)(AAudioStreamBuilder*, aaudio_direction_t);
using BuilderSetSampleRateFn = void (*)(AAudioStreamBuilder*, int32_t);
using BuilderSetChannelCountFn = void (*)(AAudioStreamBuilder*, int32_t);
using BuilderSetFormatFn = void (*)(AAudioStreamBuilder*, aaudio_format_t);
using BuilderSetPerformanceModeFn = void (*)(AAudioStreamBuilder*, aaudio_performance_mode_t);
using BuilderSetDataCallbackFn =
        void (*)(AAudioStreamBuilder*, AAudioStream_dataCallback, void*);
using BuilderSetFramesPerDataCallbackFn = void (*)(AAudioStreamBuilder*, int32_t);
using BuilderOpenStreamFn = aaudio_result_t (*)(AAudioStreamBuilder*, AAudioStream**);
using StreamRequestFn = aaudio_result_t (*)(AAudioStream*);
using StreamCloseFn = aaudio_result_t (*)(AAudioStream*);
using StreamGetInt32Fn = int32_t (*)(AAudioStream*);
using StreamGetFormatFn = aaudio_format_t (*)(AAudioStream*);
using ConvertResultToTextFn = const char* (*)(aaudio_result_t);

struct AAudioApi {
    CreateStreamBuilderFn createStreamBuilder = nullptr;
    BuilderDeleteFn builderDelete = nullptr;
    BuilderSetDirectionFn setDirection = nullptr;
    BuilderSetSampleRateFn setSampleRate = nullptr;
    BuilderSetChannelCountFn setChannelCount = nullptr;
    BuilderSetFormatFn setFormat = nullptr;
    BuilderSetPerformanceModeFn setPerformanceMode = nullptr;
    BuilderSetDataCallbackFn setDataCallback = nullptr;
    BuilderSetFramesPerDataCallbackFn setFramesPerDataCallback = nullptr;
    BuilderOpenStreamFn openStream = nullptr;
    StreamRequestFn requestStart = nullptr;
    StreamRequestFn requestPause = nullptr;
    StreamRequestFn requestStop = nullptr;
    StreamCloseFn closeStream = nullptr;
    StreamGetInt32Fn getSampleRate = nullptr;
    StreamGetInt32Fn getChannelCount = nullptr;
    StreamGetFormatFn getFormat = nullptr;
    ConvertResultToTextFn convertResultToText = nullptr;

    /// 只 dlopen 一次：`libaaudio.so` 在 API 26 以下不存在，此时整体返回 false。
    static const AAudioApi* instance() {
        static const AAudioApi api = load();
        static const bool ok = api.requestStart != nullptr;
        return ok ? &api : nullptr;
    }

    bool available() const { return requestStart != nullptr; }

private:
    /// 取一个符号并按上面的类型固定下来。缺符号 ⇒ nullptr（不崩），由调用方逐个判。
    template <typename Fn>
    static Fn sym(void* handle, const char* name) {
        return reinterpret_cast<Fn>(dlsym(handle, name));
    }

    static AAudioApi load() {
        AAudioApi api;
        // RTLD_NOW：一次性把缺符号暴露在 dlopen 的返回值上，而不是等到第一次调用
        // 时才崩。句柄故意不关 —— 它和进程同寿命，关掉反而让已解析的指针悬空。
        void* handle = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
            return api;
        api.createStreamBuilder = sym<CreateStreamBuilderFn>(handle, "AAudio_createStreamBuilder");
        api.builderDelete = sym<BuilderDeleteFn>(handle, "AAudioStreamBuilder_delete");
        api.setDirection = sym<BuilderSetDirectionFn>(handle, "AAudioStreamBuilder_setDirection");
        api.setSampleRate = sym<BuilderSetSampleRateFn>(handle, "AAudioStreamBuilder_setSampleRate");
        api.setChannelCount = sym<BuilderSetChannelCountFn>(handle, "AAudioStreamBuilder_setChannelCount");
        api.setFormat = sym<BuilderSetFormatFn>(handle, "AAudioStreamBuilder_setFormat");
        api.setPerformanceMode =
                sym<BuilderSetPerformanceModeFn>(handle, "AAudioStreamBuilder_setPerformanceMode");
        api.setDataCallback =
                sym<BuilderSetDataCallbackFn>(handle, "AAudioStreamBuilder_setDataCallback");
        api.setFramesPerDataCallback = sym<BuilderSetFramesPerDataCallbackFn>(
                handle, "AAudioStreamBuilder_setFramesPerDataCallback");
        api.openStream = sym<BuilderOpenStreamFn>(handle, "AAudioStreamBuilder_openStream");
        api.requestStart = sym<StreamRequestFn>(handle, "AAudioStream_requestStart");
        api.requestPause = sym<StreamRequestFn>(handle, "AAudioStream_requestPause");
        api.requestStop = sym<StreamRequestFn>(handle, "AAudioStream_requestStop");
        api.closeStream = sym<StreamCloseFn>(handle, "AAudioStream_close");
        api.getSampleRate = sym<StreamGetInt32Fn>(handle, "AAudioStream_getSampleRate");
        api.getChannelCount = sym<StreamGetInt32Fn>(handle, "AAudioStream_getChannelCount");
        api.getFormat = sym<StreamGetFormatFn>(handle, "AAudioStream_getFormat");
        api.convertResultToText = sym<ConvertResultToTextFn>(handle, "AAudio_convertResultToText");
        // requestStart 是「一个都没有」的哨兵；其余若缺，open() 里再逐个判。
        return api;
    }
};

std::string resultText(const AAudioApi* api, aaudio_result_t result) {
    char buf[96];
    const char* text = api->convertResultToText ? api->convertResultToText(result) : nullptr;
    std::snprintf(buf, sizeof(buf), "AAudio: %s (%d)", text ? text : "未知", int(result));
    return buf;
}

class AAudioSink final : public AudioSink {
public:
    explicit AAudioSink(const AAudioApi* api) : api_(api) {}

    ~AAudioSink() override { close(); }

    AAudioSink(const AAudioSink&) = delete;
    AAudioSink& operator=(const AAudioSink&) = delete;

    bool open(const AudioFormat& format, std::string& error) override {
        if (!format.isValid()) {
            error = "AudioFormat 无效";
            return false;
        }
        // 环形缓冲按来格式建（内部恒 F32 交织）；设备侧格式在下面二选一。
        ring_.reset(format);

        AAudioStreamBuilder* builder = nullptr;
        if (api_->createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
            error = "AAudio_createStreamBuilder 失败";
            return false;
        }
        api_->setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        api_->setSampleRate(builder, format.sampleRate);
        api_->setChannelCount(builder, format.channels);
        // NONE 是「允许数据回调」的最保守档位：EXCLUSIVE 之类的低延迟模式在部分
        // 设备上会拒回调，而这里必须靠回调拉数据才不会欠载。
        api_->setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
        api_->setDataCallback(builder, &AAudioSink::audioCallback, this);
        api_->setFramesPerDataCallback(builder, kFramesPerCallback);

        // 先试 float（省一次定点化），失败再试 S16。
        api_->setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        useFloat_ = true;
        aaudio_result_t res = api_->openStream(builder, &stream_);
        if (res != AAUDIO_OK || stream_ == nullptr) {
            api_->setFormat(builder, AAUDIO_FORMAT_PCM_I16);
            useFloat_ = false;
            stream_ = nullptr;
            res = api_->openStream(builder, &stream_);
        }
        api_->builderDelete(builder);
        if (res != AAUDIO_OK || stream_ == nullptr) {
            error = resultText(api_, res);
            stream_ = nullptr;
            return false;
        }

        // 设备可能给了我们别的 rate/声道数（共享模式下的少数妥协）。这里**不做适
        // 配**：环形缓冲里的 PCM 是按来格式排布的，改分母只会让位置算错、把立体
        // 声按四声道读成噪声。起不来就退回无声，比出声出坏东西好。
        const int deviceRate = api_->getSampleRate ? api_->getSampleRate(stream_) : format.sampleRate;
        const int deviceChannels =
                api_->getChannelCount ? api_->getChannelCount(stream_) : format.channels;
        if (deviceRate != format.sampleRate || deviceChannels != format.channels) {
            error = "AAudio 给的设备格式与音轨不一致（" + std::to_string(deviceRate) + "Hz/" +
                    std::to_string(deviceChannels) + "ch，期望 " +
                    std::to_string(format.sampleRate) + "Hz/" + std::to_string(format.channels) +
                    "ch）";
            api_->closeStream(stream_);
            stream_ = nullptr;
            return false;
        }
        // 实际生效的格式决定回调里读哪一条：设备给 float 就走 F32，否则 S16。
        const aaudio_format_t actual =
                api_->getFormat ? api_->getFormat(stream_) : AAUDIO_FORMAT_PCM_FLOAT;
        useFloat_ = actual == AAUDIO_FORMAT_PCM_FLOAT;
        opened_ = true;
        error.clear();
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        AAudioStream* stream = stream_;
        stream_ = nullptr;
        opened_ = false;
        running_ = false;
        if (stream == nullptr)
            return;
        // close() 内部会等回调返回，所以之后可以确定「没有线程再碰 ring_ / this」。
        api_->closeStream(stream);
    }

    void start() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (!opened_ || running_)
            return;
        if (api_->requestStart(stream_) == AAUDIO_OK)
            running_ = true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (!opened_ || !running_)
            return;
        // requestPause 保留已排队的采样；播放暂停时那些采样会在恢复后继续，而
        // 位置在暂停期间不推进（回调不再被调）—— 正是 `AudioSink::stop()` 的语义。
        api_->requestPause(stream_);
        running_ = false;
    }

    void flush(int64_t ptsUs) override {
        ring_.clear(ptsUs);
    }

    size_t push(const AudioChunk& chunk) override { return ring_.push(chunk); }

    void setVolume(float volume) override { ring_.setVolume(volume); }

    int64_t positionUs() const override { return ring_.positionUs(); }
    int64_t queuedUs() const override { return ring_.queuedUs(); }

private:
    /// 一次回调搬运的采样帧数。太小会让实时线程频繁上锁，太大则暂停/seek 后多播
    /// 出一截（flush 只能清环形缓冲，清不掉已经在设备缓冲里的）。10ms 是这两头的
    /// 平衡点。
    static constexpr int32_t kFramesPerCallback = 480;

    static aaudio_data_callback_result_t audioCallback(AAudioStream* stream, void* userData,
                                                       void* audioData, int32_t audioFrames) {
        (void)stream;
        auto* self = static_cast<AAudioSink*>(userData);
        if (self == nullptr || audioData == nullptr || audioFrames <= 0)
            return AAUDIO_CALLBACK_RESULT_STOP;
        if (self->useFloat_)
            self->ring_.readF32(static_cast<float*>(audioData), size_t(audioFrames));
        else
            self->ring_.readS16(static_cast<int16_t*>(audioData), size_t(audioFrames));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    const AAudioApi* api_ = nullptr;
    AudioRing ring_;
    AAudioStream* stream_ = nullptr;
    bool opened_ = false;
    bool running_ = false;
    bool useFloat_ = true;
    /// 与 `close()` 互斥保护 `stream_` 的启停；`stop()`/`start()` 来自 GUI 线程，
    /// 而析构可能来自播放引擎释放的路径。
    std::mutex lifecycle_;
};

}  // namespace

std::unique_ptr<AudioSink> createPlatformAudioSink() {
    const AAudioApi* api = AAudioApi::instance();
    if (api == nullptr)
        return nullptr;  // 这个 ROM 没带 libaaudio（或被裁剪）：无声 + 墙钟节拍。
    return std::make_unique<AAudioSink>(api);
}

}  // namespace cui_core

#endif  // __ANDROID__
