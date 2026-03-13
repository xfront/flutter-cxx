/// @file video/audio_sink_darwin.cpp
/// @brief macOS / iOS 出声设备（AudioQueue Services）+ 主时钟来源。
///
/// 为什么是 AudioQueue 而不是 AudioUnit：`AudioSink` 契约只需要「有一条实时线程
/// 按固定长度来拉采样」+「位置能问出来」。AudioUnit（RemoteIO）当然更快，但它的
/// render callback 跑在实时优先级线程上，且必须自己处理 AudioSession 中断
/// （iOS 上来电/其他 App 抢占会打断输出），代价与这条路的收益不匹配。AudioQueue
/// 的 `AudioQueueOutputCallback` 就是「一个缓冲播完了，再给我填一个」，与
/// Android 那份 AAudio 回调模式同形，代码量也小一档。倍速仍由播放引擎决定
/// （见 video_decoder.h 的口径），不在这里改采样率。
///
/// **这个文件是 `.cpp` 而不是 `.mm`**：AudioQueue / CoreAudio 全是纯 C API，
/// 一行 Objective-C 消息发送都不需要（对比解码那侧的 `video_decoder_darwin.mm`）。
/// 于是它与 `audio_sink_alsa.cpp`／`audio_sink_android.cpp` 走同一套编译规则，
/// 也不用给工具链开 `-fobjc-arc`。
///
/// 采样格式：环形缓冲内部恒 F32 交织，设备侧要 S16 交织
/// （`kAudioFormatLinearPCM` + `IsSignedInteger|IsPacked` + 16bit）。AudioQueue
/// 也接受 packed float32，但那要求输出设备本身支持 32 位浮点，共享模式下的
/// 转换行为在不同机器上不一致 ⇒ 一律按 S16 交出去，定点化只发生一次
/// （`AudioRing::readS16`，含音量）。
///
/// 位置口径：`AudioRing` 的「已消耗帧」在**交给 AudioQueue 那一刻**就前进了，而
/// AudioQueue 手里还压着另外两个缓冲没播 ⇒ 直接用 `positionUs()` 会让主时钟超前
/// 约 `缓冲个数 × 每缓冲时长`。所以按 `positionUsMinus(inFlight)` 扣掉在途的那部分
/// （`audio_ring.h` 里这个钩子就是为 AudioQueue / OHOS 回调模式留的）。
///
/// 线程：`push()` 在解码工作线程，`start()`/`stop()`/`flush()`/`close()` 在 GUI
/// 线程，`onComplete` 在 AudioQueue 自己的线程。三者只通过 `AudioRing`（自带
/// mutex）打交道；`onComplete` **绝不取 `lifecycle_`** —— `close()` 是持着这把锁
/// 等 `AudioQueueDispose` 返回的，回调反向拿锁就是死锁（与 AAudio 那侧同一条纪律）。
///
/// 已知边界（本机没有 macOS/iOS SDK ⇒ 这个文件没在任何机器上编过）：
///   - `AudioQueueReset` 之后各缓冲会以「完成」状态回到回调里 ⇒ 这里顺势重新填充
///     并入队，因此 seek 后最多播出 `kBufferMs` 量级的静音，不会播出旧位置的采样。
///     若某台机器上 Reset 不回调（文档未承诺细节），后果退化为「旧采样最多多播
///     60ms」，不影响位置正确性（基准已经搬到目标）。
///   - 设备格式不匹配时不做适配：AudioQueue 是「按你给的 ASBD 收数据」的软件队列，
///     底层混音器负责重采样，所以不像 ALSA 那样有「拿不到期望格式」的失败路径。

#include "cui/video/audio_sink.h"

#include "audio_ring.h"

#if defined(__APPLE__) && __has_include(<AudioToolbox/AudioToolbox.h>)

#include <AudioToolbox/AudioToolbox.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace cui {
namespace {

/// 同时在途的缓冲个数。3 个 × 20ms = 60ms 的音频深度：够扛住一次 GUI 卡顿，
/// 又不至于让 seek 后要丢的旧采样太多（见上面 Reset 那条）。
constexpr UInt32 kNumBuffers = 3;
/// 单个缓冲的长度（毫秒）。`inFlight` 的估算、`positionUs` 的精度都直接受它影响。
constexpr int kBufferMs = 20;

/// `OSStatus` → 可读文本。fourcc 型的错误码（'tmfo' 之类）打成字符比打数字有用，
/// 负数（系统级错误）打十进制。
std::string statusText(OSStatus status) {
    char buf[64];
    if (status >= 0x20 && status <= 0x7e) {
        // 常见的都是 fourcc：把它按 4 个字符拆开
        const char text[5] = {char((status >> 24) & 0xff), char((status >> 16) & 0xff),
                              char((status >> 8) & 0xff), char(status & 0xff), 0};
        std::snprintf(buf, sizeof(buf), "AudioQueue: '%s' (%" PRId32 ")", text, int32_t(status));
    } else {
        std::snprintf(buf, sizeof(buf), "AudioQueue: %" PRId32, int32_t(status));
    }
    return buf;
}

class AudioQueueSink final : public AudioSink {
public:
    AudioQueueSink() = default;
    ~AudioQueueSink() override { close(); }

    AudioQueueSink(const AudioQueueSink&) = delete;
    AudioQueueSink& operator=(const AudioQueueSink&) = delete;

    bool open(const AudioFormat& format, std::string& error) override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (!format.isValid()) {
            error = "AudioFormat 无效";
            return false;
        }
        ring_.reset(format);  // 容量 2s（默认），内部恒 F32 交织
        framesPerBuffer_ = UInt32(int64_t(format.sampleRate) * kBufferMs / 1000);
        if (framesPerBuffer_ == 0)
            framesPerBuffer_ = UInt32(format.sampleRate / 100);  // 亚 1kHz 的极端采样率
        bytesPerFrame_ = UInt32(format.channels) * 2u;            // S16 交织
        const UInt32 bufferBytes = framesPerBuffer_ * bytesPerFrame_;

        AudioStreamBasicDescription asbd {};
        asbd.mSampleRate = Float64(format.sampleRate);
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = AudioFormatFlags(kAudioFormatFlagIsSignedInteger |
                                             kAudioFormatFlagIsPacked);
        asbd.mBytesPerPacket = bytesPerFrame_;
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = bytesPerFrame_;
        asbd.mChannelsPerFrame = UInt32(format.channels);
        asbd.mBitsPerChannel = 16;

        // 回调 runloop 给 nullptr/nullptr ⇒ AudioQueue 自己起一条线程投递回调。
        // 本仓没有主 runloop 的约定（macOS 上窗口后端有自己的循环，iOS 上
        // UIApplicationMain 的循环不归这里管），自己起线程最省心。
        OSStatus res = AudioQueueNewOutput(&asbd, &AudioQueueSink::onComplete, this, nullptr,
                                           nullptr, 0, &queue_);
        if (res != noErr || queue_ == nullptr) {
            queue_ = nullptr;
            error = statusText(res);
            return false;
        }

        for (UInt32 i = 0; i < kNumBuffers; ++i) {
            AudioQueueBufferRef buffer = nullptr;
            res = AudioQueueAllocateBuffer(queue_, bufferBytes, &buffer);
            if (res != noErr || buffer == nullptr)
                break;
            buffers_.push_back(buffer);
            // 先按静音入队：环形缓冲此时是空的，`readS16` 本来就补零。启动后第一
            // 个回调会带着真正的采样回来。
            prime(buffer, bufferBytes);
        }
        if (buffers_.size() < kNumBuffers) {
            error = "AudioQueueAllocateBuffer 失败";
            releaseLocked();
            return false;
        }
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
        if (AudioQueueStart(queue_, nullptr) == noErr)
            running_ = true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (!opened_ || !running_)
            return;
        // Pause 保留已入队的缓冲，且回调不再被敲 ⇒ 位置就此冻结，正是
        // `AudioSink::stop()` 的语义（与 AAudio 侧 requestPause 同一条选择）。
        AudioQueuePause(queue_);
        running_ = false;
    }

    void flush(int64_t ptsUs) override {
        ring_.clear(ptsUs);
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (queue_ == nullptr)
            return;
        // 丢掉已入队但未播出的采样（否则 seek 后还要播出 60ms 旧位置的音频）。
        // 各缓冲会以「完成」状态回到 `onComplete` ⇒ 在那里按新的环形缓冲重填。
        AudioQueueReset(queue_);
    }

    size_t push(const AudioChunk& chunk) override { return ring_.push(chunk); }

    void setVolume(float volume) override { ring_.setVolume(volume); }

    int64_t positionUs() const override {
        // 在途量：手上压着的那几个缓冲还没播。少一个缓冲就少一份延迟。
        const int64_t inFlight =
                opened_ ? int64_t(framesPerBuffer_) * (int64_t(buffers_.size()) - 1) : 0;
        return ring_.positionUsMinus(inFlight);
    }

    int64_t queuedUs() const override { return ring_.queuedUs(); }

private:
    /// 环形缓冲 → 设备缓冲（S16 交织，含音量），并入队。调用方保证
    /// `buffer->mAudioData` 有 `bytes` 的空间。
    void fillAndEnqueue(AudioQueueBufferRef buffer) {
        const UInt32 bytes = framesPerBuffer_ * bytesPerFrame_;
        ring_.readS16(static_cast<int16_t*>(buffer->mAudioData), size_t(framesPerBuffer_));
        buffer->mAudioDataByteSize = bytes;
        // 回调里入队是 AudioQueue 的标准用法；队列已被 dispose 时不会走到这里
        // （`close()` 持锁等 Dispose 返回，见文件头）。
        AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
    }

    /// `open()` 阶段的首轮填充（与 `fillAndEnqueue` 一样，只是那时还没入队过）。
    void prime(AudioQueueBufferRef buffer, UInt32 bytes) {
        std::memset(buffer->mAudioData, 0, bytes);
        buffer->mAudioDataByteSize = bytes;
        AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr);
    }

    /// AudioQueue 播完一个缓冲后的回调（自己的线程）。
    static void onComplete(void* userData, AudioQueueRef /*queue*/,
                           AudioQueueBufferRef buffer) {
        auto* self = static_cast<AudioQueueSink*>(userData);
        if (self == nullptr || buffer == nullptr)
            return;
        self->fillAndEnqueue(buffer);
    }

    /// 持 `lifecycle_` 时调用：停 + 销毁队列。`inImmediate=true` 保证返回后
    /// 不再有回调；缓冲随队列一起释放（AudioQueue 拥有 AllocateBuffer 的内存）。
    void releaseLocked() {
        AudioQueueRef queue = queue_;
        queue_ = nullptr;
        buffers_.clear();
        opened_ = false;
        running_ = false;
        framesPerBuffer_ = 0;
        if (queue == nullptr)
            return;
        AudioQueueStop(queue, true);
        AudioQueueDispose(queue, true);
    }

    AudioRing ring_;
    /// 与 `onComplete` 之间的唯一交叉是 `ring_`（自带锁），这把锁只保护生命周期。
    std::mutex lifecycle_;
    AudioQueueRef queue_ = nullptr;
    std::vector<AudioQueueBufferRef> buffers_;
    UInt32 framesPerBuffer_ = 0;
    UInt32 bytesPerFrame_ = 0;
    bool opened_ = false;
    bool running_ = false;
};

}  // namespace

std::unique_ptr<AudioSink> createPlatformAudioSink() {
    return std::make_unique<AudioQueueSink>();
}

}  // namespace cui

#endif  // __APPLE__ && <AudioToolbox/AudioToolbox.h>
