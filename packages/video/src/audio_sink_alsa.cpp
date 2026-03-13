/// @file audio_sink_alsa.cpp
/// @brief Linux 出声设备（ALSA pcm，S16 + 搬运线程）+ 主时钟来源。
///
/// 为什么值得先做这条：桌面 Linux 上原本只有 FFmpeg 解码，而 `createPlatformAudioSink()`
/// 返回空 ⇒ 画面照走、**全程无声**，主时钟也只能用墙钟。ALSA 是这端唯一「必然存在」
/// 的音频层（PulseAudio / PipeWire 都在它下面接，设备名 `default` 还会按 asoundrc
/// 自动走到 pulse/dmix 插件），所以它是比 GStreamer 更划算的第一块平台能力。
///
/// 为什么要自己起一条搬运线程：AAudio 有数据回调，ALSA 的拉取模型只有
/// `snd_pcm_avail_update()` + `snd_pcm_writei()`。而环形缓冲的消耗速率必须等于设备
/// 的消耗速率，`positionUs()` 才是媒体时钟 —— 于是取多少帧由「现在写得进去多少」
/// 决定（见 `feedLoop`），不是定时器说了算。
///
/// 格式：`AudioRing` 内部恒 F32 交织，设备侧一律申请 **S16_LE 交织**（几乎所有声卡
/// 的原生格式，省一次软件转换）。采样率/声道数照音轨给；`snd_pcm_set_params` 的
/// `allow_resample=1` 让 ALSA 的 rate/plug 插件去迁就硬件，本仓位置全部按媒体帧算
/// （不读设备时钟），插件层重采样不影响同步。
///
/// 线程：`push()` 在解码工作线程；`start()`/`stop()`/`flush()`/`positionUs()` 在 GUI
/// 线程；搬运在自建线程。两条纪律：
///   1. `pcm_` 上的每一个 ALSA 调用都持 `io_` —— 句柄本身不是线程安全的，
///      并发 writei/drop 是未定义行为（这是本文件唯一真正的坑位）；
///   2. 启停状态 `running_` 只由 `state_` + `wake_` 管，搬运线程睡在条件变量上，
///      绝不持 `io_` 睡觉（`snd_pcm_wait` 的 10ms 除外，那一步不碰状态）。

#include "cui/video/audio_sink.h"

#include "audio_ring.h"

#if defined(__linux__) && __has_include(<alsa/asoundlib.h>)

#include <algorithm>
#include <alsa/asoundlib.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cui {
namespace {

/// 一次 `writei` 的帧数上限（≈21ms @48k）。太小系统调用频繁，太大则暂停/seek 之后
/// 还会多播出一截 —— flush 清得掉环形缓冲，清不掉已经交给设备的。
constexpr snd_pcm_uframes_t kMaxChunkFrames = 1024;

/// `snd_pcm_set_params` 的目标延迟。50ms 是「欠载不爆音、seek 不拖泥」的常见折中，
/// 缓冲区与 period 大小都由 ALSA 按它反推。
constexpr unsigned kLatencyUs = 50 * 1000;

std::string sndError(const std::string& what, int err) {
    return what + " 失败: " + snd_strerror(err);
}

class AlsaSink final : public AudioSink {
public:
    AlsaSink() = default;
    ~AlsaSink() override { close(); }

    AlsaSink(const AlsaSink&) = delete;
    AlsaSink& operator=(const AlsaSink&) = delete;

    bool open(const AudioFormat& format, std::string& error) override {
        if (!format.isValid()) {
            error = "AudioFormat 无效";
            return false;
        }
        ring_.reset(format);
        channels_ = std::max(1, format.channels);

        // 设备名：`CUI_AUDIO_DEVICE` 覆盖（多卡机器 / CI 用），否则 ALSA 的 `default`。
        const char* env = std::getenv("CUI_AUDIO_DEVICE");
        const std::string device = (env && *env) ? env : "default";

        snd_pcm_t* pcm = nullptr;
        int err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            error = sndError("snd_pcm_open(" + device + ")", err);
            return false;
        }
        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                 unsigned(channels_), unsigned(format.sampleRate),
                                 /*allow_resample=*/1, kLatencyUs);
        if (err < 0) {
            error = sndError("snd_pcm_set_params", err);
            snd_pcm_close(pcm);
            return false;
        }
        // 设备报给自己的缓冲尺寸：两者只用来算「能领先多少帧」这个上限。
        snd_pcm_uframes_t boundary = 0;
        snd_pcm_uframes_t bufferFrames = 0;
        snd_pcm_get_params(pcm, &boundary, &bufferFrames);
        // 「已交给设备但还没响」的帧数上限。不能只信 `bufferFrames`/`boundary` 此中的一个：
        // PipeWire 插件报的 buffer=600、boundary=2400，而 `snd_pcm_delay` 能回到 2429
        // ⇒ 拿 600 当容量则永远算不出空位（搬运线程饿死、全程静音）。
        capacityFrames_ = std::max<size_t>(size_t(bufferFrames), size_t(boundary));

        {
            std::lock_guard<std::mutex> lock(io_);
            pcm_ = pcm;
        }
        opened_ = true;
        stopped_ = false;
        feeder_ = std::thread([this] { feedLoop(); });
        error.clear();
        return true;
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(state_);
            stopped_ = true;
            running_ = false;
        }
        wake_.notify_all();
        if (feeder_.joinable())
            feeder_.join();
        std::lock_guard<std::mutex> lock(io_);
        if (pcm_ == nullptr)
            return;
        // 先解除暂停再 drop：暂停态直接 close，ALSA 会报「unable to suspend」。
        snd_pcm_pause(pcm_, 0);
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        opened_ = false;
    }

    void start() override {
        if (!opened_)
            return;
        {
            std::lock_guard<std::mutex> lock(state_);
            if (running_)
                return;
            running_ = true;
        }
        wake_.notify_all();
        std::lock_guard<std::mutex> lock(io_);
        if (pcm_ != nullptr && devicePaused_) {
            snd_pcm_pause(pcm_, 0);
            devicePaused_ = false;
        }
    }

    void stop() override {
        if (!opened_)
            return;
        {
            std::lock_guard<std::mutex> lock(state_);
            if (!running_)
                return;
            running_ = false;
        }
        std::lock_guard<std::mutex> lock(io_);
        if (pcm_ == nullptr)
            return;
        // 首选 pause：已交给设备的采样保留着，恢复后继续播，位置就此冻住
        // （`snd_pcm_delay` 在暂停期间不再缩小）。dmix/pulse 那类插件设备常常不支持
        // ⇒ 退化成「只是不再喂数据」，等设备把剩下的播完，位置最多比真值超前
        // 一个缓冲区（≈50ms），仍比硬 drop 把尾巴切掉好听。
        const int err = snd_pcm_pause(pcm_, 1);
        // 失败（插件不支持 pause）⇒ `devicePaused_` 保持 false，后面的 `start()`
        // 不会去叫一句注定失败的 `snd_pcm_pause(0)`。行为退化成「不再喂数据」。
        devicePaused_ = err == 0;
    }

    void flush(int64_t ptsUs) override {
        ring_.clear(ptsUs);
        // 让搬运线程的「墙钟预算」重新起算：位置基准刚刚跳变（seek / 暂停后重新
        // 开播），而真实时间不会跳 ⇒ 不清 `pulled`/`t0` 的话预算会变成负数，搬运
        // 线程从此饿死。
        ++flushEpoch_;
        if (!opened_)
            return;
        std::lock_guard<std::mutex> lock(io_);
        if (pcm_ == nullptr)
            return;
        // drop = 停 + 丢弃硬件里的残留，随后 prepare 回到可写状态。搬运线程此刻可能
        // 正卡在同一句柄的某个调用上 —— 它全程持 `io_`，所以这里必然排在它后面。
        snd_pcm_drop(pcm_);
        if (snd_pcm_prepare(pcm_) < 0) {
            // 回不到 PREPARED（句柄被服务端收走那类）：搬运线程下一轮的
            // `avail_update` 会拿到负值并走 recover，这里不必自己重试。
            return;
        }
        bool running = false;
        {
            std::lock_guard<std::mutex> lock(state_);
            running = running_;
        }
        if (running) {
            // prepare 之后是 PREPARED，必须显式 start（writei 只在 PLAYING 状态下推进）。
            devicePaused_ = false;
            snd_pcm_start(pcm_);
        }
    }

    size_t push(const AudioChunk& chunk) override { return ring_.push(chunk); }

    void setVolume(float volume) override { ring_.setVolume(volume); }

    int64_t positionUs() const override {
        // 环形缓冲的「消耗」= 已交给设备，但 `delay` 里那些还没响 ⇒ 扣掉才是听众
        // 眼里的位置（`audio_ring.h` 里 `positionUsMinus` 就是为这个留的）。
        return ring_.positionUsMinus(delayFrames());
    }
    int64_t queuedUs() const override { return ring_.queuedUs(); }

private:
    /// 设备里「已写入但尚未播出」的帧数。查询失败（状态不对）回 0 = 不扣。
    int64_t delayFrames() const {
        std::lock_guard<std::mutex> lock(io_);
        if (pcm_ == nullptr)
            return 0;
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(pcm_, &delay) < 0)
            return 0;
        return std::max<int64_t>(0, int64_t(delay));
    }

    /// 搬运线程：按「设备真播出去了多少」从环形缓冲取帧，再 `writei`。
    ///
    /// 为什么不用 `snd_pcm_avail_update()` 当节拍（这是本文件踩过的最大的坑）：
    /// PipeWire 的那个 alsa 插件会虚报空位 —— 写进去的帧立刻被算成「已播完」，
    /// `avail` 长期停在半空，于是「取 avail 那么多」的循环能以数倍速抽干环形
    /// 缓冲，而环形缓冲的消耗计数就是主时钟（见 audio_ring.h）⇒ 实测 0.35 秒墙钟
    /// 里位置冲到 12 秒。所以这里换两条独立的约束，取交集：
    ///   1. `delay`（设备里还没播出去的帧）：只能领先设备一个缓冲区，取帧数 =
    ///      `capacityFrames_ - delay`；
    ///   2. 墙钟：媒体位置不许快过真实时间（同样只允许一个缓冲区的领先），防止
    ///      `delay` 也虚报（返回 0）时再次飞起来。倍速 1.0 才走音频主时钟
    ///      （`audioMasterLocked`），所以这个上限就是 `elapsed * rate`。
    void feedLoop() {
        std::vector<int16_t> chunk;
        const size_t blockFrames =
                capacityFrames_ > 0 ? std::min<size_t>(size_t(kMaxChunkFrames), capacityFrames_)
                                    : size_t(kMaxChunkFrames);
        const int64_t capacity = int64_t(capacityFrames_);
        const int rate = ring_.format().sampleRate;
        chunk.assign(blockFrames * size_t(channels_), 0);
        // 首次写入前置基准：之后「墙钟允许播出的帧数」从这里起算（flush 会重置）。
        auto t0 = std::chrono::steady_clock::now();
        int64_t pulled = 0;  ///< 本轮（自上次 flush 起）从环形缓冲取走的总帧数
        uint64_t epoch = flushEpoch_.load();
        for (;;) {
            bool running = false;
            {
                std::unique_lock<std::mutex> lock(state_);
                wake_.wait(lock, [this] { return stopped_ || running_; });
                if (stopped_)
                    return;
                running = running_;
            }
            if (!running)
                continue;

            std::unique_lock<std::mutex> lock(io_);
            if (pcm_ == nullptr || stopped_)
                return;
            if (const uint64_t nowEpoch = flushEpoch_.load(); nowEpoch != epoch) {
                epoch = nowEpoch;
                t0 = std::chrono::steady_clock::now();
                pulled = 0;
            }
            // 先做一次状态同步兼 xrun 探测：负值 = 欠载/挂起，交给 recover。
            const snd_pcm_sframes_t probe = snd_pcm_avail_update(pcm_);
            if (probe < 0) {
                recoverLocked(int(probe));
                lock.unlock();
                continue;
            }
            snd_pcm_sframes_t rawDelay = 0;
            if (snd_pcm_delay(pcm_, &rawDelay) < 0)
                rawDelay = 0;
            const int64_t inFlight = std::clamp<int64_t>(rawDelay, 0, capacity);
            int64_t quota = std::min<int64_t>(capacity - inFlight, int64_t(blockFrames));  // 约束 1
            if (rate > 0) {
                const int64_t elapsedUs =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
                const int64_t wallBudget =
                        elapsedUs * int64_t(rate) / 1000000 + capacity - pulled;  // 约束 2
                quota = std::min(quota, wallBudget);
            }
            if (quota <= 0) {
                // 设备那边没有空位：等它消费。带超时，免得设备被抢占时永久睡在这里
                // （`stopped_` 那时没人能叫醒搬运线程）。
                snd_pcm_wait(pcm_, 10);
                lock.unlock();
                continue;
            }
            const size_t frames = size_t(std::min<int64_t>(quota, int64_t(blockFrames)));
            // 欠载时 `readS16` 补零**也计进消耗**（见 audio_ring.h 的 `takeInto`）——
            // 这正是要的：主时钟继续按真实速率走，不因欠载停住。
            ring_.readS16(chunk.data(), frames);
            pulled += int64_t(frames);
            if (pcm_ == nullptr || stopped_)
                return;  // 上面读环形缓冲期间 close() 可能已经插手（它拿不到 io_，但可能已置标志）
            const snd_pcm_sframes_t written = snd_pcm_writei(pcm_, chunk.data(), snd_pcm_uframes_t(frames));
            if (written < 0) {
                // 写入失败：这些帧已经算进消耗了，但设备没拿到 ⇒ 预算往后挪，否则
                // 位置会永远领先真实声音一个 chunk。
                pulled -= int64_t(frames);
                recoverLocked(int(written));
            }
            lock.unlock();
        }
    }

    /// 经典恢复：-EPIPE = 欠载（prepare 后继续），-ESTRPIPE = 系统挂起（等 resume，
    /// 等不动再 prepare）。调用者持 `io_`。
    void recoverLocked(int err) {
        if (err == -EPIPE || err == -ESTRPIPE) {
            if (err == -ESTRPIPE) {
                while (snd_pcm_resume(pcm_) == -EAGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (stopped_)
                        return;
                }
            } else {
                snd_pcm_prepare(pcm_);
            }
            std::lock_guard<std::mutex> lock(state_);
            if (running_) {
                devicePaused_ = false;
                snd_pcm_start(pcm_);
            }
            return;
        }
        // 其余错误（-EBADFD / -ENODEV 那类）：睡一下再看，下一轮的
        // `avail_update` 会给回一个负值 ⇒ 这里不必自己判断句柄死透了。
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    AudioRing ring_;
    snd_pcm_t* pcm_ = nullptr;
    int channels_ = 2;
    /// 设备侧「写入后还能存着没响」的帧数上限（见 `open()` 里为什么要取大）。
    size_t capacityFrames_ = 0;
    std::atomic<bool> opened_{false};
    std::atomic<bool> stopped_{true};
    /// `flush()` 的次数，只用来让搬运线程发现「基准跳了」（见 `feedLoop`）。
    std::atomic<uint64_t> flushEpoch_{0};
    /// 只在 `state_` 下读写（搬运线程靠它判断「现在该不该取数据」）。
    bool running_ = false;
    /// 只有 `stop()` 真把设备按下去时才是 true；不支持 pause 的设备保持 false，
    /// 于是 `start()` 不会去叫一句注定失败的 `snd_pcm_pause(0)`。
    bool devicePaused_ = false;

    /// 保护 `pcm_` 上的所有 ALSA 调用与句柄生命周期（可重入点见注释）。
    mutable std::mutex io_;
    /// 保护 `running_`/`stopped_`，并与 `wake_` 配对叫醒搬运线程。
    std::mutex state_;
    std::condition_variable wake_;
    std::thread feeder_;
};

}  // namespace

/// ALSA 的可用性只有真去 `snd_pcm_open` 才知道（没声卡 ⇒ -ENOENT），而格式要等拿到
/// 音轨才定 —— 所以这里无条件给一个未开的设备，`open()` 失败由播放引擎退回
/// 「无声 + 墙钟」，其余平台那几份 sink（AAudio / AudioQueue / WASAPI /
/// OH_AudioRenderer）也是同一条口径。
std::unique_ptr<AudioSink> createPlatformAudioSink() { return std::make_unique<AlsaSink>(); }

}  // namespace cui_core

#endif  // __linux__ && <alsa/asoundlib.h>
