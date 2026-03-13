#pragma once
/// @file cui/video/audio_sink.h
/// @brief 视频包的音频输出：解码出的 PCM → 平台音频设备 + 主时钟。
///
/// 为什么音频住在这一层（而不是每个后端各自出声）：
///   - 后端只负责「解复用 + 解码 + 交出一段 PCM」，出声设备各端一套
///     （Windows WASAPI、Android AAudio、iOS AudioQueue、OHOS OH_AudioRenderer、
///     Linux ALSA / GStreamer 自己带），共同点只有「拉走若干采样帧」这一件事
///     —— 所以环形缓冲与音量/位置计算抽成一份 `AudioRing`（src/audio_ring.h），
///     平台侧只写搬运。
///   - **主时钟来自这里**：`VideoPlayback::positionLocked()` 在有 sink 时读
///     `positionUs()` 而不是墙钟推算（对齐 ffplay 的「音频做主时钟」口径），
///     音视频同步才有意义。
///   - 打不开设备 ⇒ `createPlatformAudioSink()` 返回 nullptr，播放引擎自动
///     退回原来的墙钟节拍（有画面无声音，不算错误）。
///
/// 采样格式不做统一假设（这是系统解码器与 FFmpeg 的关键差别）：
///   - FFmpeg 侧用 swresample 一次转成 S16 交织；
///   - Media Foundation / AudioToolbox / GStreamer 的解码器直接交出的多是
///     **[-1,1] 浮点**（MF 的 IEEE float、AudioQueue 的 packed float32），
///     有的还是平面（planar）。
///   所以 `AudioChunk` 带格式标记，`AudioRing` 内部一律归一成 **F32 交织**：
///   音量是浮点乘法（不会像定点那样二次量化），平台侧再按设备偏好转成
///   S16（AAudio/ALSA）或直接给 F32（WASAPI 共享模式 / AudioQueue / OHOS）。
///
/// 线程约定：sink 对象跨线程（GUI 线程 start/stop/setVolume，播放线程
/// push/queuedUs，音频回调线程读环形缓冲），实现内部自带同步；对外
/// 只有 `push()` 与 `read()` 两条路径真的并发。

#include <memory>
#include <string>

namespace cui {

/// PCM 的内存布局。`S16Interleaved` 是定点族，其余两个是浮点族。
enum class AudioSampleFormat {
    S16Interleaved,  ///< int16 交织：L R L R …（FFmpeg swresample 的输出）
    F32Interleaved,  ///< float 交织，取值 [-1,1]（MF / AudioToolbox / GStreamer）
    F32Planar,       ///< float 平面：每声道一个缓冲（部分系统解码器）
};

/// 音频轨的采样描述。`sampleRate`/`channels` 为 0 表示无效。
struct AudioFormat {
    int sampleRate = 0;
    int channels = 0;
    AudioSampleFormat format = AudioSampleFormat::S16Interleaved;

    bool isValid() const { return sampleRate > 0 && channels > 0; }
    /// 交织布局下一帧的字节数（平面布局按 `sizeof(float)` 单声道算）。
    int64_t bytesPerFrame() const {
        const int64_t sampleBytes = format == AudioSampleFormat::S16Interleaved ? 2 : 4;
        return int64_t(channels) * sampleBytes;
    }
    bool isFloat() const { return format != AudioSampleFormat::S16Interleaved; }

    bool operator==(const AudioFormat&) const = default;
};

/// `AudioChunk::plane` 支持的声道上限（8 = 7.1，超出只在前几路出声）。
constexpr int kAudioMaxPlanes = 8;

/// 一段已解码的 PCM。所有权在解码后端，`push()` 拷进环形缓冲后即可失效。
/// 按 `format` 只填对应的一个指针组，其余留空。
struct AudioChunk {
    size_t frames = 0;  ///< 采样帧数（每帧 `AudioFormat::channels` 个样本）
    int64_t ptsUs = 0;  ///< 这一段第一个采样帧的呈现时间戳
    AudioSampleFormat format = AudioSampleFormat::S16Interleaved;

    const int16_t* s16 = nullptr;  ///< `S16Interleaved`
    const float* f32 = nullptr;    ///< `F32Interleaved`
    const float* plane[kAudioMaxPlanes] = {};  ///< `F32Planar`：逐声道
};

/// 平台音频设备。
class AudioSink {
public:
    virtual ~AudioSink() = default;

    /// 打开设备。失败时返回 false 并填 `error`（调用方退回无声播放）。
    /// `format` 是**后端交出的**格式；设备侧格式由实现自己选（能直接收同格式
    /// 就同格式，否则在 `read()` 时转换）。
    virtual bool open(const AudioFormat& format, std::string& error) = 0;
    /// 幂等；析构里也必须完成（停回调 + 释放设备）。
    virtual void close() = 0;

    virtual void start() = 0;
    /// 暂停：位置就此冻结（`positionUs()` 不再前进）。
    virtual void stop() = 0;
    /// 丢弃未播出的采样，并把位置基准设为 `ptsUs`（seek / 循环回卷）。
    virtual void flush(int64_t ptsUs) = 0;

    /// 非阻塞。返回真正收下的帧数 —— 队列满时小于 `chunk.frames`，调用方据此
    /// 节流（正常用法不会碰到：`queuedUs()` 的阈值远小于缓冲容量）。
    virtual size_t push(const AudioChunk& chunk) = 0;

    /// 软件音量 [0,1]，在环形缓冲读出时施加（因此对三个平台都零成本）。
    virtual void setVolume(float volume) = 0;

    /// 已经播出的媒体位置（微秒）= `flush()` 基准 + 已消耗帧 / 采样率。
    virtual int64_t positionUs() const = 0;
    /// 队列里还没播出的时长（微秒），给调用方做背压。
    virtual int64_t queuedUs() const = 0;
};

/// 平台工厂。没有可用后端（平台不支持 / 设备打不开）时返回 nullptr，
/// 播放引擎退回「无声 + 墙钟节拍」。
std::unique_ptr<AudioSink> createPlatformAudioSink();

}  // namespace cui
