#pragma once
/// @file audio_ring.h
/// @brief 各平台 sink 共用的 PCM 环形缓冲（归一成 F32 交织 + 软件音量 + 播出位置）。
///
/// 只解决四件事：
///   1. `push()` 非阻塞、按整帧写入，任何来格式（S16 交织 / F32 交织 / F32 平面）
///      都先归一成 F32 交织再入队；写不满就只写一部分（容量给到 2 秒，而背压
///      阈值只有 100 毫秒，所以正常情况下永远写得完）；
///   2. `readF32()` / `readS16()` 给音频回调取固定帧数，**不够就补零** —— 静音
///      照样计入已消耗帧，位置才会跟着真实时间走（否则欠载时主时钟会停住，
///      画面被拖到后面）；
///   3. 音量在读取时施加（浮点域相乘，不做二次量化），`setVolume()` 因此
///      立即生效且不需要重开设备；
///   4. 位置 = `flush()` 基准 + **自那次 flush 起**消耗的帧 / 采样率。
///
/// 锁：一把 mutex 够了。音频回调里的临界区只有一次 memcpy 加一轮换算，
/// 微秒级，不会影响实时性（真要无锁 SPSC 得等平台侧实测出瓶颈再说）。

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "cui/video/audio_sink.h"

namespace cui {

class AudioRing {
public:
    /// 容量按毫秒给，默认 2 秒（足够覆盖「喂数据线程被长时间抢占」）。
    void reset(const AudioFormat& format, size_t capacityMs = 2000) {
        std::lock_guard<std::mutex> lock(mutex_);
        sampleRate_ = format.sampleRate;
        channels_ = std::max(1, format.channels);
        const size_t capacityFrames =
                std::max<size_t>(1024, size_t(int64_t(format.sampleRate) * capacityMs / 1000));
        capacityFrames_ = capacityFrames;
        buf_.assign(capacityFrames * size_t(channels_), 0.0f);
        head_ = 0;
        count_ = 0;
        consumedFrames_ = 0;
        basePtsUs_ = 0;
    }

    /// 丢弃未播出的采样并把位置基准搬到 `basePtsUs`（seek / 循环回卷）。
    /// 消耗计数必须一起清零：位置 = 基准 + 消耗，不清零就等于「新基准 + 上一段
    /// 已经播过的时长」—— 每 seek 一次多出一截，位置从此永远对不上声音。
    void clear(int64_t basePtsUs) {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        count_ = 0;
        consumedFrames_ = 0;
        basePtsUs_ = basePtsUs;
    }

    /// 归一后入队；返回真正收下的帧数（队列满时小于 `chunk.frames`）。
    /// 声明的格式对应的那个指针为空 ⇒ 一个也不收（返回 0）：`convertInto()` 会直接
    /// 跳过空指针，但 `count_` 照样涨 ⇒ 那就等于往队列里塞一段未定义内容。
    size_t push(const AudioChunk& chunk) {
        if (chunk.frames == 0 || channels_ <= 0 || !chunkHasSource(chunk))
            return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t space = capacityFrames_ - count_;
        const size_t written = std::min(space, chunk.frames);
        if (written == 0)
            return 0;
        size_t tail = (head_ + count_) % capacityFrames_;
        // 一次写入只能到物理末尾，跨过环形尾界就拆成两段。
        const size_t first = std::min(written, capacityFrames_ - tail);
        convertInto(buf_.data() + tail * size_t(channels_), chunk, 0, first);
        if (first < written)
            convertInto(buf_.data(), chunk, first, written - first);
        count_ += written;
        return written;
    }

    /// 取 `frames` 帧到 F32 交织缓冲（施加音量）；不足部分补零。返回取走的帧数。
    size_t readF32(float* out, size_t frames) {
        if (!out || frames == 0 || channels_ <= 0)
            return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        return takeInto(out, frames);
    }

    /// 同上，落点是 S16 交织（AAudio / ALSA 那类定点设备）。
    size_t readS16(int16_t* out, size_t frames) {
        if (!out || frames == 0 || channels_ <= 0)
            return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        return takeInto(out, frames);
    }

    void setVolume(float volume) {
        std::lock_guard<std::mutex> lock(mutex_);
        volume_ = std::clamp(volume, 0.0f, 1.0f);
    }
    float volume() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return volume_;
    }

    int64_t positionUs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sampleRate_ <= 0)
            return basePtsUs_;
        return basePtsUs_ + consumedFrames_ * int64_t(1000000) / int64_t(sampleRate_);
    }

    int64_t queuedUs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sampleRate_ <= 0)
            return 0;
        return int64_t(count_) * int64_t(1000000) / int64_t(sampleRate_);
    }

    /// 从 `clear()` 基准之后一共消耗了多少帧（测试里核对「补零也算时间」）。
    int64_t consumedFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return consumedFrames_;
    }

    /// 还要多少帧才能填满容量（平台侧用它判断「现在能不能多塞一个缓冲」）。
    size_t freeFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacityFrames_ - count_;
    }

    /// 队列里已有的帧数。
    size_t queuedFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    /// 环形缓冲内部的布局（恒 F32 交织），平台侧 `open()` 时照着申请设备。
    AudioFormat format() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return AudioFormat{sampleRate_, channels_, AudioSampleFormat::F32Interleaved};
    }

    /// 平台侧「已经交给设备但还没播出去」的帧数要从位置里扣掉
    /// （AudioQueue / OH 回调模式一次性握着好几个缓冲）。
    int64_t positionUsMinus(int64_t inFlightFrames) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sampleRate_ <= 0)
            return basePtsUs_;
        const int64_t consumed = std::max<int64_t>(0, consumedFrames_ - inFlightFrames);
        return basePtsUs_ + consumed * int64_t(1000000) / int64_t(sampleRate_);
    }

private:
    /// 按 `chunk.format` 看该填的那个（那几）指针在不在。
    static bool chunkHasSource(const AudioChunk& chunk) {
        switch (chunk.format) {
            case AudioSampleFormat::S16Interleaved:
                return chunk.s16 != nullptr;
            case AudioSampleFormat::F32Interleaved:
                return chunk.f32 != nullptr;
            case AudioSampleFormat::F32Planar:
                return chunk.plane[0] != nullptr;  // 首声道都没有就是没数据
        }
        return false;
    }

    /// `chunk[start, start+frames)` → 环形缓冲（任意来格式归一成 F32 交织）。
    /// 调用方持锁。
    void convertInto(float* dst, const AudioChunk& chunk, size_t start, size_t frames) {
        const size_t stride = size_t(channels_);
        switch (chunk.format) {
            case AudioSampleFormat::S16Interleaved: {
                if (!chunk.s16)
                    return;
                const int16_t* src = chunk.s16 + start * stride;
                for (size_t i = 0; i < frames * stride; ++i)
                    dst[i] = float(src[i]) * (1.0f / 32768.0f);
                break;
            }
            case AudioSampleFormat::F32Interleaved: {
                if (!chunk.f32)
                    return;
                std::memcpy(dst, chunk.f32 + start * stride, frames * stride * sizeof(float));
                break;
            }
            case AudioSampleFormat::F32Planar: {
                for (int c = 0; c < channels_; ++c) {
                    const float* plane = chunk.plane[c];
                    if (!plane)
                        continue;
                    const float* src = plane + start;
                    for (size_t i = 0; i < frames; ++i)
                        dst[i * stride + size_t(c)] = src[i];
                }
                break;
            }
        }
    }

    /// 取 `frames` 帧（欠载补零 + 施加音量）。落点类型只影响一次换算：F32 直接
    /// 乘音量，S16 乘完再放大到定点域并夹住。调用方持锁。
    template <typename T>
    size_t takeInto(T* out, size_t frames) {
        const size_t stride = size_t(channels_);
        const size_t got = std::min(count_, frames);
        size_t done = 0;
        while (done < got) {
            const size_t pos = (head_ + done) % capacityFrames_;
            const size_t run = std::min(got - done, capacityFrames_ - pos);
            emit(out + done * stride, buf_.data() + pos * stride, run * stride);
            done += run;
        }
        head_ = (head_ + got) % capacityFrames_;
        count_ -= got;
        // 欠载补零：静音也占时间，否则主时钟会在欠载期间停住。
        if (got < frames)
            std::memset(out + got * stride, 0, (frames - got) * stride * sizeof(T));
        consumedFrames_ += int64_t(frames);
        return frames;
    }

    void emit(float* dst, const float* src, size_t samples) {
        if (volume_ >= 0.999f) {
            std::memcpy(dst, src, samples * sizeof(float));
            return;
        }
        if (volume_ <= 0.001f) {
            std::memset(dst, 0, samples * sizeof(float));
            return;
        }
        for (size_t i = 0; i < samples; ++i)
            dst[i] = src[i] * volume_;
    }

    void emit(int16_t* dst, const float* src, size_t samples) {
        for (size_t i = 0; i < samples; ++i) {
            const float v = std::clamp(src[i] * volume_, -1.0f, 1.0f);
            dst[i] = int16_t(v >= 0.0f ? v * 32767.0f : v * 32768.0f);
        }
    }

    mutable std::mutex mutex_;
    std::vector<float> buf_;
    size_t capacityFrames_ = 0;
    size_t head_ = 0;   ///< 下一个读位置（帧）
    size_t count_ = 0;  ///< 队列里的帧数
    int64_t consumedFrames_ = 0;
    int64_t basePtsUs_ = 0;
    float volume_ = 1.0f;
    int sampleRate_ = 0;
    int channels_ = 0;
};

}  // namespace cui
