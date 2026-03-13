/// @file video/audio_sink_ohos.cpp
/// @brief OHOS 出声设备（OHAudio 的 OH_AudioRenderer，回调模式）+ 主时钟来源。
///
/// 为什么是 OHAudio 而不是 OpenSL ES：OHOS 上 OpenSL ES 只覆盖到「播放 PCM」这一层，
/// 而 `AudioSink` 契约里最硬的一条是 `positionUs()` —— 播放引擎拿它当主时钟
/// （见 audio_ring.h 的位置口径）。OHAudio 是 OHOS 主推的 C API 音频框架，回调模式
/// 与 Android 那份 AAudio 同形：实时线程按固定帧数来拉一次采样，位置推进的分辨率
/// 就等于一次回调的长度。倍速仍由播放引擎决定（见 video_decoder.h 的口径），
/// 这里不碰 `OH_AudioRenderer_SetSpeed`。
///
/// 采样格式：`AudioRing` 内部恒 F32 交织。设备侧先要 `AUDIOSTREAM_SAMPLE_F32LE`
/// （省掉一次定点化），这个枚举值是 **API 17** 起的 ⇒ 老 SDK 上 `GenerateRenderer`
/// 会失败，于是退回 `AUDIOSTREAM_SAMPLE_S16LE` 用 `readS16()` 取数。两条都试一遍，
/// 不把「这个 SDK 支持 float」写死在编译期。
///
/// 数据回调选 `OH_AudioStreamBuilder_SetRendererWriteDataCallback`（API 12）而不是
/// `SetRendererCallback`（API 10 起、但**整个 `OH_AudioRenderer_Callbacks` 自 API 20
/// 废弃**，且四个成员必须逐个填）。这一族里 `SetRendererInterruptCallback` /
/// `SetRendererErrorCallback` 都是 API 20 ⇒ 中断（来电/其它 App 抢占）与错误事件
/// 在这里不做处理：没有中断回调时系统仍会把流置为 stopped，欠载补零让位置继续推进，
/// 表现是「被抢占期间静音」而不是卡死。真要接中断，得按 SDK 版本分两条编译路径。
///
/// 线程：`push()`/`queuedUs()` 在解码工作线程，`start()`/`stop()`/`flush()`/
/// `setVolume()` 在 GUI 线程，`writeData` 在 OHAudio 自己的实时线程。三者只通过
/// `AudioRing`（自带 mutex）打交道。`close()` 里先 `Stop` 再 `Release`：
/// `OH_AudioRenderer_Release` **不允许在回调线程里调用**（文档约束），而本函数只会
/// 从 GUI 线程/析构进来；`Release` 返回后不会再有回调 ⇒ 之后 `this` 才可以销毁。
///
/// 位置口径：一次回调把 `framesPerCallback_` 帧交给框架，这些帧**还没播出**，而
/// `AudioRing` 的「已消耗帧」在读出那一刻就前进了 ⇒ 直接用 `positionUs()` 会让主
/// 时钟超前约「框架内部缓冲深度」。这里按 `kInFlightCallbacks` 个回调的长度扣掉
/// （`audio_ring.h` 的 `positionUsMinus()` 就是为这类回调模式留的钩子）。这个常数是
/// **估算**：扣少了画面比声音快一点点、扣多了慢一点点，都是固定偏移，不会累积漂移
/// （偏移量 ≈ 一次回调的几毫秒，远小于人眼阈）。真机上要精调，改用
/// `OH_AudioRenderer_GetTimestamp` —— 但文档要求两次调用的间隔不小于 200ms，
/// 放在每帧都问一次的主时钟上并不合适。
///
/// 已知边界（本机 NDK 26.0.0 的真头已编过这个文件、`OH_AudioStreamBuilder_*` /
/// `OH_AudioRenderer_*` 也在 sysroot 的 `libohaudio` 导出表里逐个对过 ⇒ API 名字与
/// 签名不再是抄写；但没链过真库、没上设备）：
///   - `AUDIOSTREAM_SAMPLE_F32LE`（API 17）与 `AUDIOSTREAM_USAGE_MOVIE`（API 10）的
///     可用性差异靠运行期「先试 F32、失败退 S16」覆盖；更低版本的 SDK 上连
///     `SetFrameSizeInCallback`（API 11）都没有 ⇒ 直接编译失败，不会假报已启用。
///   - 设备给回的 rate/声道数与音轨不一致时**不做适配**：环形缓冲里的 PCM 是按来格式
///     排布的，改分母只会让位置算错、把立体声按四声道读成噪声。起不来就退回无声。

#include "cui/video/audio_sink.h"

#include "audio_ring.h"

#if defined(__OHOS__) && __has_include(<ohaudio/native_audiostreambuilder.h>)

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace cui {
namespace {

std::string resultText(const char* what, OH_AudioStream_Result result) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "OHAudio: %s (%d)", what, int(result));
    return buf;
}

class OhAudioSink final : public AudioSink {
public:
    OhAudioSink() = default;
    ~OhAudioSink() override { close(); }

    OhAudioSink(const OhAudioSink&) = delete;
    OhAudioSink& operator=(const OhAudioSink&) = delete;

    bool open(const AudioFormat& format, std::string& error) override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (!format.isValid()) {
            error = "AudioFormat 无效";
            return false;
        }
        // 环形缓冲按来格式建（内部恒 F32 交织）；设备侧格式在下面二选一。
        ring_.reset(format);
        channels_ = format.channels;
        sampleRate_ = format.sampleRate;
        // 一次回调 ≈10ms：太小会让实时线程频繁上锁，太大则暂停/seek 后要丢的旧采样
        // 太多（flush 只能清环形缓冲，清不掉已经交给框架的那一段）。
        framesPerCallback_ = std::max<int32_t>(64, int32_t(int64_t(format.sampleRate) / 100));

        OH_AudioStreamBuilder* builder = nullptr;
        OH_AudioStream_Result ret =
                OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
        if (ret != AUDIOSTREAM_SUCCESS || builder == nullptr) {
            error = resultText("OH_AudioStreamBuilder_Create 失败", ret);
            return false;
        }
        OH_AudioStreamBuilder_SetSamplingRate(builder, format.sampleRate);
        OH_AudioStreamBuilder_SetChannelCount(builder, format.channels);
        OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
        // NORMAL：回调模式要的是「不会欠载」，LOW 那档在部分设备上会拒回调。
        OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
        // MOVIE = 影音播放场景（音量曲线与路由按媒体走；MUSIC 是更通用的一档）。
        OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MOVIE);
        OH_AudioStreamBuilder_SetFrameSizeInCallback(builder, framesPerCallback_);
        OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, &OhAudioSink::writeData, this);

        // 先试 float（省一次定点化，API 17 起才有这个枚举值），失败再试 S16。
        useFloat_ = true;
        ret = OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_F32LE);
        if (ret == AUDIOSTREAM_SUCCESS)
            ret = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer_);
        if (ret != AUDIOSTREAM_SUCCESS || renderer_ == nullptr) {
            useFloat_ = false;
            renderer_ = nullptr;
            ret = OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
            if (ret == AUDIOSTREAM_SUCCESS)
                ret = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer_);
        }
        OH_AudioStreamBuilder_Destroy(builder);
        if (ret != AUDIOSTREAM_SUCCESS || renderer_ == nullptr) {
            renderer_ = nullptr;
            error = resultText("GenerateRenderer 失败（F32 与 S16 都试过）", ret);
            return false;
        }

        // 框架可能给了我们别的 rate/声道数。这里不做适配，理由见文件头。
        int32_t deviceRate = format.sampleRate;
        int32_t deviceChannels = format.channels;
        OH_AudioStream_SampleFormat deviceFormat =
                useFloat_ ? AUDIOSTREAM_SAMPLE_F32LE : AUDIOSTREAM_SAMPLE_S16LE;
        OH_AudioRenderer_GetSamplingRate(renderer_, &deviceRate);
        OH_AudioRenderer_GetChannelCount(renderer_, &deviceChannels);
        OH_AudioRenderer_GetSampleFormat(renderer_, &deviceFormat);
        if (deviceRate != format.sampleRate || deviceChannels != format.channels) {
            error = "OHAudio 给的设备格式与音轨不一致（" + std::to_string(deviceRate) + "Hz/" +
                    std::to_string(deviceChannels) + "ch，期望 " +
                    std::to_string(format.sampleRate) + "Hz/" + std::to_string(format.channels) +
                    "ch）";
            releaseLocked();
            return false;
        }
        // 实际生效的格式决定回调里读哪一条：设备给 float 就走 F32，否则 S16。
        useFloat_ = deviceFormat == AUDIOSTREAM_SAMPLE_F32LE;
        sampleBytes_ = useFloat_ ? 4 : 2;
        opened_ = true;
        error.clear();
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        releaseLocked();
    }

    void start() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (!opened_ || running_)
            return;
        if (OH_AudioRenderer_Start(renderer_) == AUDIOSTREAM_SUCCESS)
            running_ = true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (!opened_ || !running_)
            return;
        // Pause 不丢已交给框架的采样，而回调从此不再被敲 ⇒ 位置就此冻结，
        // 正是 `AudioSink::stop()` 的语义（与 AAudio 的 requestPause 同一条选择）。
        OH_AudioRenderer_Pause(renderer_);
        running_ = false;
    }

    void flush(int64_t ptsUs) override {
        ring_.clear(ptsUs);
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (renderer_ == nullptr)
            return;
        // 丢掉框架内部缓冲里未播出的采样（否则 seek 后还要播出旧位置的音频）。
        // 环形基准已经搬到 ptsUs，所以这里清得干净不干净都不影响位置的正确性，
        // 只影响「最多多响 10ms 旧声音」。
        OH_AudioRenderer_Flush(renderer_);
    }

    size_t push(const AudioChunk& chunk) override { return ring_.push(chunk); }

    // 音量施加在环形缓冲读出时（浮点域，三个平台同一条路），所以不调
    // `OH_AudioRenderer_SetVolume`：那条是系统音量，与这里的软件音量语义不同。
    void setVolume(float volume) override { ring_.setVolume(volume); }

    int64_t positionUs() const override {
        if (!opened_)
            return ring_.positionUs();
        // 已交给框架、但还没播出的那一段要从位置里扣掉（见文件头）。
        const int64_t inFlight = int64_t(framesPerCallback_) * kInFlightCallbacks;
        return ring_.positionUsMinus(inFlight);
    }

    int64_t queuedUs() const override { return ring_.queuedUs(); }

private:
    /// 估算的「在途回调个数」。框架内部缓冲深度没有对外查询接口
    /// （`GetLatency` 给的是毫秒级估计且含系统链路），这里按 2 次回调取。
    static constexpr int64_t kInFlightCallbacks = 2;

    /// 实时线程拉数据：`audioDataSize` 是**字节数**（文档明写「必须恰好填满」），
    /// 所以帧数要除以声道数与样本字长。欠载时 `AudioRing` 补零 —— 静音照样计入
    /// 已消耗帧，主时钟才会跟着真实时间走。
    static OH_AudioData_Callback_Result writeData(OH_AudioRenderer* renderer, void* userData,
                                                  void* audioData, int32_t audioDataSize) {
        (void)renderer;
        auto* self = static_cast<OhAudioSink*>(userData);
        if (self == nullptr || audioData == nullptr || audioDataSize <= 0 || self->channels_ <= 0)
            return AUDIO_DATA_CALLBACK_RESULT_INVALID;
        const int64_t frameBytes = int64_t(self->channels_) * self->sampleBytes_;
        const size_t frames = size_t(int64_t(audioDataSize) / frameBytes);
        if (frames == 0)
            return AUDIO_DATA_CALLBACK_RESULT_INVALID;
        if (self->useFloat_)
            self->ring_.readF32(static_cast<float*>(audioData), frames);
        else
            self->ring_.readS16(static_cast<int16_t*>(audioData), frames);
        return AUDIO_DATA_CALLBACK_RESULT_VALID;
    }

    /// 调用方持 `lifecycle_`。`Release` 会等回调返回，所以之后可以确定
    /// 「没有线程再碰 ring_ / this」。
    void releaseLocked() {
        OH_AudioRenderer* renderer = renderer_;
        renderer_ = nullptr;
        opened_ = false;
        running_ = false;
        if (renderer == nullptr)
            return;
        OH_AudioRenderer_Stop(renderer);
        OH_AudioRenderer_Release(renderer);
    }

    AudioRing ring_;
    OH_AudioRenderer* renderer_ = nullptr;
    bool opened_ = false;
    bool running_ = false;
    bool useFloat_ = true;
    int sampleRate_ = 0;
    int channels_ = 0;
    int32_t sampleBytes_ = 4;
    int32_t framesPerCallback_ = 480;
    /// 与 `close()` 互斥保护 `renderer_` 的启停；`start()`/`stop()` 来自 GUI 线程，
    /// 而析构可能来自播放引擎释放的路径。
    std::mutex lifecycle_;
};

}  // namespace

std::unique_ptr<AudioSink> createPlatformAudioSink() {
    return std::make_unique<OhAudioSink>();
}

}  // namespace cui

#endif  // __OHOS__ && ohaudio
