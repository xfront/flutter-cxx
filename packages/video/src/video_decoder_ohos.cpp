/// @file video/video_decoder_ohos.cpp
/// @brief OHOS 系统媒体栈解码后端（OH_AVSource + OH_AVDemuxer + OH_VideoDecoder / OH_AudioCodec）。
///
/// 路线一（各端系统 API）在 OpenHarmony 上的落点，角色与 `video_decoder_android.cpp`
/// 完全对齐：**解复用 + 解码交给操作系统**，本仓只把解出来的 YUV 转成 BGRA、把 PCM
/// 转交给 `AudioSink`。
///
/// 关键实现口径：
///   - 走 **buffer 模式**（不调 `OH_VideoDecoder_SetSurface`）。本仓渲染路径是 Skia
///     位图，没有 `OHNativeWindow` 可给；而且 surface 模式拿不到 stride/slice-height
///     （官方文档明写：surface 模式走 HEBC 压缩，`widthStride`/`heightStride` 取不到），
///     像素也就无从解析。buffer 模式是唯一能读到 CPU 像素的路。
///   - **异步回调 + 槽队列**，不是同步模式。同步模式要 `OH_MD_KEY_ENABLE_SYNC_MODE`
///     （API 20）配 `OH_VideoDecoder_QueryInputBuffer/QueryOutputBuffer`，覆盖面太窄；
///     回调形态下 `OnNeedInputBuffer`/`OnNewOutputBuffer` 把 `{index, OH_AVBuffer*}`
///     投进队列，工作线程按条件变量取（对齐官方示例的 `CodecBufferQueue`）。
///   - **解复用游标是「按轨」的**：`OH_AVDemuxer_ReadSampleBuffer(demuxer, trackId, buf)`
///     自己推进那一条轨的游标，所以不像 Android 那样「一条 extractor 游标 +
///     `getSampleTrackIndex()` 判归属」，而是视频/音频各读各的。代价：两条轨的
///     交错顺序由各自的解码进度决定，不再有「按呈现顺序取一个 sample 再派发」这一步。
///   - 几何（stride / slice-height / pic 尺寸 / 像素格式）只信
///     `OH_VideoDecoder_GetOutputDescription()` 与 `OnStreamChanged` 交出的
///     `OH_AVFormat`（`OH_MD_KEY_VIDEO_STRIDE`/`VIDEO_SLICE_HEIGHT`/`VIDEO_PIC_WIDTH`/
///     `VIDEO_PIC_HEIGHT`，均 API 12）。容器轨上的 `WIDTH`/`HEIGHT` 只用来 Configure 与
///     `info()`，转换时以输出侧为准。
///   - 请求 `AV_PIXEL_FORMAT_NV12`（半平面、一份色度块，转换最省），但**以解码器实际
///     交出的格式为准**：I420 / NV12 / NV21 / RGBA 都认，其余（10bit、厂商私有）判
///     结构性故障 ⇒ 放弃整条系统端，让引擎退到 FFmpeg（与 Android 那份同口径）。
///   - pts 取输出侧 attr 的 `pts`（呈现顺序），不取喂进去时的值 —— 有 B 帧时两者不同。
///   - **seek 与 Android 最大的不同**：`OH_AVDemuxer_SeekToTime(SEEK_MODE_CLOSEST_SYNC)`
///     之后必须 `OH_VideoDecoder_Flush` → 作废两个队列里已投出来的槽（官方约束：
///     flush/reset/stop 之后不得再操作此前回调交出的 `OH_AVBuffer`）→ `Start` →
///     **重发参数集**（文档约束 1：flush/reset/stop 后 start 必须再传一次 PPS/SPS）。
///     参数集用打开时从 `OH_MD_KEY_CODEC_CONFIG` 拷来的那份 CSD，flags=
///     `AVCODEC_BUFFER_FLAGS_CODEC_DATA`，作为一条普通 pending 走同一条投递路径。
///   - 输入侧同样先 `ReadSampleBuffer` 到自有暂存 `OH_AVBuffer`、再 memcpy 进 codec 的
///     input buffer：直接读进 input buffer 要依赖「两边容量恰好合适」，而
///     `ReadSampleBuffer` 装不下时只打 `INCOMPLETE_FRAME` 标记并**截断**（半个 NAL 交给
///     解码器的后果随设备而变）。这里遇到截断就把暂存缓冲翻倍重建再读一次。
///   - 音频积压超阈值时**丢输出不丢输入**（`FreeOutputBuffer` 照做，只是不拷进队列）。
///     反过来「不收输出」会让 audio input 槽永久补不上，那条轨的游标就卡死了。
///   - 音频解码交出的采样格式不做统一假设（`OH_BitsPerSample` 有 S16LE / F32LE / S32LE
///     / 各种 planar）：`appendAudio()` 一律归一成**交织**布局再进 `pcmBuf_`
///     （`readAudio()` 只按字节切片，平面布局必须在进缓冲前摊平）。
///   - 旋转只报 `info().rotation`（容器轨 `OH_MD_KEY_ROTATION`，API 12），不自己翻像素。
///     注意：`OH_MD_KEY_ROTATION` 在 Configure/SetParameter 里只对 surface 模式生效，
///     buffer 模式下它是「告诉你该转多少」的元数据，转像素是上层的事（同 FFmpeg/MF）。
///   - 出声仍然走本仓 `AudioSink`（`audio_sink_ohos.cpp` 的 OH_AudioRenderer）：主时钟
///     要问「已经播到第几微秒」，只有 PCM 经过环形缓冲才拿得到这个数。
///
/// 有意未覆盖：
///   - **编过 ≠ 跑过**：本机装了 OHOS NDK 26.0.0，这个文件已用它的**真头**在
///     `--target=aarch64-linux-ohos --sysroot=<native>/sysroot -D__MUSL__`
///     （口径照 `ohos.toolchain.cmake`）下编过，`-Wall -Wextra -Wpedantic` 干净；
///     引用的 83 个 `OH_*` 符号逐个对过 sysroot 里 `libnative_media_*` /
///     `libohaudio` 的导出表，零缺失。真头这一轮还改掉了两处 stub 时代抄错的
///     字段：`OH_AVCodecBufferAttr`（`native_avbuffer_info.h`）只有
///     `pts/size/offset/flags`，**没有** `index`，也没有 `userData` —— buffer 的
///     身份靠 `OH_VideoDecoder_PushInputBuffer`/`*_FreeOutputBuffer` 的参数传递。
///     但 arm64-ohos 的 skia 依赖本机没有、也没有设备 ⇒ 没链过真库、没跑过真机，
///     **真机行为与 SDK 版本差异仍未验证**。
///   - 整条路线要求 API 12+（`OH_AVDataSource`、`VIDEO_STRIDE` 一族键、
///     `SetFrameSizeInCallback` 都是 12）。低于 12 的 SDK 上这些符号根本不存在 ⇒
///     编译期就报缺失（本仓不去猜 `__OHOS_API__` 这类宏名，免得门控自己先写错）。
///   - DRM（`OH_VideoDecoder_SetDecryptionConfig` + `libnative_drm`）、`RenderOutputBufferAtTime`
///     那类系统侧 AV 同步（本仓的同步留在 `AudioRing` 位置口径上）、多音轨切换、
///     硬解失败重试软解、`OH_AVSource_CreateWithDataSourceExt`（API 20 的 userData 形态）。
///   - 反向播放、倍速（引擎层用墙钟做倍速，后端不参与）。

#include "cui/video/video_decoder.h"

#if defined(CUI_HAVE_OH_AV_CODEC)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dlfcn.h>

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avdemuxer.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avsource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>

namespace cui {
namespace {

// ── 小工具 ──────────────────────────────────────────────────────────────────

std::string errText(const char* what, OH_AVErrCode code) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s (%d)", what, static_cast<int>(code));
    return buf;
}

std::string hexText(int32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(v));
    return buf;
}

bool getInt(OH_AVFormat* fmt, const char* key, int32_t& out) {
    return fmt != nullptr && OH_AVFormat_GetIntValue(fmt, key, &out);
}
bool getInt64(OH_AVFormat* fmt, const char* key, int64_t& out) {
    return fmt != nullptr && OH_AVFormat_GetLongValue(fmt, key, &out);
}
bool getDouble(OH_AVFormat* fmt, const char* key, double& out) {
    return fmt != nullptr && OH_AVFormat_GetDoubleValue(fmt, key, &out);
}
/// `OH_AVFormat_GetStringValue` 交出来的指针由 format 持有，必须在 format 销毁前拷走。
std::string getString(OH_AVFormat* fmt, const char* key) {
    const char* raw = nullptr;
    if (fmt == nullptr || !OH_AVFormat_GetStringValue(fmt, key, &raw) || raw == nullptr)
        return {};
    return raw;
}
/// `OH_AVFormat_GetBuffer` 的指针同样挂在 format 上 ⇒ 直接拷进 out。返回是否读到非空。
bool getBuffer(OH_AVFormat* fmt, const char* key, std::vector<uint8_t>& out) {
    uint8_t* addr = nullptr;
    size_t size = 0;
    if (fmt == nullptr || !OH_AVFormat_GetBuffer(fmt, key, &addr, &size) || addr == nullptr ||
        size == 0) {
        out.clear();
        return false;
    }
    out.assign(addr, addr + size);
    return true;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string stripFileScheme(const std::string& uri) {
    if (startsWith(uri, "file://"))
        return uri.substr(7);
    return uri;
}

inline uint8_t clamp8(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/// BT.601 有限范围 → BGRA（与 Android 那份同一套系数）。`chromaStride` 是色度**行**
/// 的字节距（半平面时一行含 UV 两路 ⇒ 通常等于 luma stride）；semi-planar 下 `v`
/// 不用，靠 `vFirst` 区分 NV12（UV 顺序）/ NV21（VU 顺序）。
void convertYuv(const uint8_t* y, int32_t lumaStride, const uint8_t* u, const uint8_t* v,
                int32_t chromaStride, bool semiPlanar, bool vFirst, int width, int height,
                uint8_t* dst, int32_t dstStride) {
    for (int row = 0; row < height; ++row) {
        const uint8_t* yRow = y + size_t(row) * lumaStride;
        uint8_t* outRow = dst + size_t(row) * dstStride;
        for (int col = 0; col < width; ++col) {
            const int cx = col >> 1;
            const int cy = row >> 1;
            int32_t cb = 0;
            int32_t cr = 0;
            if (semiPlanar) {
                const uint8_t* pair = u + size_t(cy) * chromaStride + cx * 2;
                cb = vFirst ? pair[1] : pair[0];
                cr = vFirst ? pair[0] : pair[1];
            } else {
                cb = u[size_t(cy) * chromaStride + cx];
                cr = v[size_t(cy) * chromaStride + cx];
            }
            const int32_t yy = int32_t(yRow[col]) - 16;
            cb -= 128;
            cr -= 128;
            outRow[col * 4 + 0] = clamp8((298 * yy + 516 * cb + 128) >> 8);
            outRow[col * 4 + 1] = clamp8((298 * yy - 100 * cb - 208 * cr + 128) >> 8);
            outRow[col * 4 + 2] = clamp8((298 * yy + 409 * cr + 128) >> 8);
            // Graphite 上传纹理后仍按原始通道取值，alpha 留 0 就成了一块透明洞 ⇒ 显式写满。
            outRow[col * 4 + 3] = 0xFF;
        }
    }
}

/// RGBA8888 → BGRA（解码器坚持交 RGB 族时用；本仓后端表统一是 BGRA）。
/// 按「内存里依次是 R,G,B,A」处理 —— OHOS 文档只写了 `AV_PIXEL_FORMAT_RGBA = RGBA8888`，
/// 没有写清字节序，真机上若与预期相反表现是红蓝互换（可一眼看出），不是隐蔽的坏帧。
void convertRgba(const uint8_t* src, int32_t srcStride, int width, int height, uint8_t* dst,
                 int32_t dstStride) {
    for (int row = 0; row < height; ++row) {
        const uint8_t* srcRow = src + size_t(row) * srcStride;
        uint8_t* outRow = dst + size_t(row) * dstStride;
        for (int col = 0; col < width; ++col) {
            outRow[col * 4 + 0] = srcRow[col * 4 + 2];
            outRow[col * 4 + 1] = srcRow[col * 4 + 1];
            outRow[col * 4 + 2] = srcRow[col * 4 + 0];
            outRow[col * 4 + 3] = 0xFF;
        }
    }
}

// ── RAII：这些句柄都「不可重复销毁」，所以禁拷贝 ─────────────────────────────
struct SourceHolder {
    OH_AVSource* src = nullptr;
    ~SourceHolder() { reset(); }
    void reset() {
        if (src != nullptr) {
            OH_AVSource_Destroy(src);
            src = nullptr;
        }
    }
    SourceHolder() = default;
    SourceHolder(const SourceHolder&) = delete;
    SourceHolder& operator=(const SourceHolder&) = delete;
};

struct DemuxerHolder {
    OH_AVDemuxer* dx = nullptr;
    ~DemuxerHolder() { reset(); }
    void reset() {
        if (dx != nullptr) {
            OH_AVDemuxer_Destroy(dx);
            dx = nullptr;
        }
    }
    DemuxerHolder() = default;
    DemuxerHolder(const DemuxerHolder&) = delete;
    DemuxerHolder& operator=(const DemuxerHolder&) = delete;
};

/// 解码器句柄。`OH_VideoDecoder_Destroy` / `OH_AudioCodec_Destroy` 是两个函数，
/// 且都**不能在回调里调**（本类只在 open/close/seek 的工作线程路径上用）。
struct CodecHolder {
    OH_AVCodec* codec = nullptr;
    bool audio = false;
    ~CodecHolder() { reset(); }
    void reset() {
        if (codec == nullptr)
            return;
        if (audio)
            OH_AudioCodec_Destroy(codec);
        else
            OH_VideoDecoder_Destroy(codec);
        codec = nullptr;
    }
    CodecHolder() = default;
    CodecHolder(const CodecHolder&) = delete;
    CodecHolder& operator=(const CodecHolder&) = delete;
};

struct FormatHolder {
    explicit FormatHolder(OH_AVFormat* p = nullptr) : fmt(p) {}
    ~FormatHolder() {
        if (fmt != nullptr)
            OH_AVFormat_Destroy(fmt);
    }
    FormatHolder(const FormatHolder&) = delete;
    FormatHolder& operator=(const FormatHolder&) = delete;
    OH_AVFormat* get() const { return fmt; }
    OH_AVFormat* fmt = nullptr;
};

/// demuxer 侧的取样缓冲（每条轨一份，容量不够时 `grow()` 重建）。
struct SampleHolder {
    OH_AVBuffer* buf = nullptr;
    int32_t capacity = 0;
    ~SampleHolder() { reset(); }
    void reset() {
        if (buf != nullptr) {
            OH_AVBuffer_Destroy(buf);
            buf = nullptr;
            capacity = 0;
        }
    }
    bool create(int32_t cap) {
        reset();
        buf = OH_AVBuffer_Create(cap);
        capacity = buf != nullptr ? cap : 0;
        return buf != nullptr;
    }
    void grow() {
        const int32_t next = capacity > 0 ? capacity * 2 : 1024 * 1024;
        create(next);
    }
    SampleHolder() = default;
    SampleHolder(const SampleHolder&) = delete;
    SampleHolder& operator=(const SampleHolder&) = delete;
};

/// 一台 codec 的回调槽队列（官方示例 `CodecBufferQueue` 的形状）。
/// 回调线程只投递；工作线程要么「这一轮先看看有没有」（`tryPop`），要么
/// 「最多等这么久」（`waitPop`）。`invalidateAll()` 对应 flush/stop/reset：
/// 队列里已投出来但还没用的槽从此作废，谁都不许再拿它去 Push/Free。
class SlotQueue {
public:
    struct Slot {
        uint32_t index = 0;
        OH_AVBuffer* buffer = nullptr;
        bool valid = true;
    };
    using Item = std::shared_ptr<Slot>;

    void enqueue(uint32_t index, OH_AVBuffer* buffer) {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.push(std::make_shared<Slot>(Slot{index, buffer, true}));
        cond_.notify_all();
    }

    Item tryPop() { return popFor(0); }
    Item waitPop(int64_t timeoutUs) { return popFor(timeoutUs); }

    void invalidateAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!items_.empty()) {
            items_.front()->valid = false;
            items_.pop();
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    Item popFor(int64_t timeoutUs) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (timeoutUs > 0) {
            cond_.wait_for(lock, std::chrono::microseconds(timeoutUs),
                           [this] { return !items_.empty(); });
        }
        if (items_.empty())
            return nullptr;
        Item out = items_.front();
        items_.pop();
        return out;
    }

    std::mutex mutable mutex_;
    std::condition_variable cond_;
    std::queue<Item> items_;
};

/// 一条轨的全部状态：demuxer 的轨号、自己的 codec、两个回调队列、待投递的 sample。
/// 带着 mutex/atomic/RAII 句柄 ⇒ 不可拷贝，只能作为成员原地构造。
struct Stream {
    CodecHolder codec;              ///< .audio 已按轨种设好
    SlotQueue in;
    SlotQueue out;
    SampleHolder sample;            ///< 仅视频/音频各自的 demuxer 缓冲
    int track = -1;
    std::string mime;
    std::vector<uint8_t> csd;       ///< OH_MD_KEY_CODEC_CONFIG，flush 后重发要用

    std::vector<uint8_t> pendingBytes;
    int64_t pendingPts = 0;
    uint32_t pendingFlags = 0;
    bool hasPending = false;
    bool pendingEos = false;
    bool eosQueued = false;         ///< EOS 已经塞进 codec（不再需要 input 槽）

    /// 回调线程写、工作线程读：几何/格式随时可能被流变更顶掉。
    std::atomic<bool> geomDirty{false};
    std::atomic<int32_t> errorCode{0};
    std::atomic<bool> failed{false};

    void reset() {
        pendingBytes.clear();
        hasPending = false;
        pendingEos = false;
        pendingFlags = 0;
        pendingPts = 0;
    }
    bool alive() const { return codec.codec != nullptr; }
};

// 回调把 userData 还原成 Stream*。注意：这些指针的有效性由后端持有 —— 后端析构
// 前一定先 Destroy 掉 codec（Destroy 会等已排队的回调跑完），所以不会打到悬空对象。

void onStreamError(OH_AVCodec* codec, int32_t errorCode, void* userData) {
    (void)codec;
    auto* s = static_cast<Stream*>(userData);
    if (s == nullptr)
        return;
    s->errorCode.store(errorCode);
    s->failed.store(true);
    s->geomDirty.store(true);
}

void onStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData) {
    (void)codec;
    auto* s = static_cast<Stream*>(userData);
    if (s == nullptr)
        return;
    // `format` 的生命周期**只到本次回调返回**（文档明写），所以这里不存指针，
    // 只置脏标记：几何由工作线程去 GetOutputDescription 重新拿一份。
    (void)format;
    s->geomDirty.store(true);
}

void onNeedInputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData) {
    (void)codec;
    auto* s = static_cast<Stream*>(userData);
    if (s != nullptr)
        s->in.enqueue(index, buffer);
}

void onNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData) {
    (void)codec;
    auto* s = static_cast<Stream*>(userData);
    if (s != nullptr)
        s->out.enqueue(index, buffer);
}

/// `OH_AVDataSource`（API 12）没有 userData 槽（带 userData 的
/// `OH_AVDataSourceExt` 要 API 20），而 `readAt` 回调必须认出「这是哪一个播放器的
/// 内存来源」。这里用一组**模板实例**当带编号的跳板：每个实例是独立函数、独立地址，
/// 下标就是槽号。代价是并发内存来源有上限，满了就如实报不支持（引擎因此退到
/// FFmpeg，那条对内存来源本来就没有限制）。
constexpr int kMemorySlots = 8;

struct MemorySlot {
    std::mutex mutex;
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    bool busy = false;
};
MemorySlot g_memorySlots[kMemorySlots];

enum MemoryError : int32_t {
    kSourceErrorIo = -2,
    kSourceErrorEof = -1,
};

int32_t readAtImpl(int slot, OH_AVBuffer* data, int32_t length, int64_t pos) {
    if (slot < 0 || slot >= kMemorySlots || data == nullptr || length < 0)
        return kSourceErrorIo;
    MemorySlot& m = g_memorySlots[slot];
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.bytes.empty())
        return kSourceErrorIo;
    if (pos < 0 || size_t(pos) >= m.bytes.size())
        return kSourceErrorEof;
    const uint8_t* addr = OH_AVBuffer_GetAddr(data);
    const int32_t cap = OH_AVBuffer_GetCapacity(data);
    if (addr == nullptr || cap <= 0)
        return kSourceErrorIo;
    const size_t left = m.bytes.size() - size_t(pos);
    const size_t n = std::min(left, size_t(std::min(length, cap)));
    if (n == 0)
        return kSourceErrorEof;
    std::memcpy(const_cast<uint8_t*>(addr), m.bytes.data() + size_t(pos), n);
    m.pos = size_t(pos) + n;
    return int32_t(n);
}

template <int N>
int32_t readAtTrampoline(OH_AVBuffer* data, int32_t length, int64_t pos) {
    return readAtImpl(N, data, length, pos);
}

using ReadAtFn = int32_t (*)(OH_AVBuffer*, int32_t, int64_t);
const ReadAtFn kReadAtTrampolines[kMemorySlots] = {
        &readAtTrampoline<0>, &readAtTrampoline<1>, &readAtTrampoline<2>, &readAtTrampoline<3>,
        &readAtTrampoline<4>, &readAtTrampoline<5>, &readAtTrampoline<6>, &readAtTrampoline<7>};

class OhosAvCodecBackend : public VideoDecoderBackend {
public:
    OhosAvCodecBackend() = default;
    ~OhosAvCodecBackend() override { closeAll(); }

    OhosAvCodecBackend(const OhosAvCodecBackend&) = delete;
    OhosAvCodecBackend& operator=(const OhosAvCodecBackend&) = delete;

    bool open(const VideoSource& source, std::string& error) {
        if (!prepare(source, error))
            return false;
        if (!startStream(video_, error))
            return false;
        refreshVideoGeometry();
        if (audio_.alive()) {
            std::string audioError;
            if (!startStream(audio_, audioError))
                discardAudio();  // 起不来的音频解码器不该拖死画面
            else if (!applyAudioGeometry())
                discardAudio();  // 交不出认识的采样格式：只关声音
        }
        return true;
    }

    const VideoInfo& info() const override { return info_; }
    bool atEnd() const override { return eof_; }
    const std::string& error() const override { return error_; }

    bool hasAudioStream() const override { return audio_.alive(); }
    AudioFormat audioFormat() const override { return audioFormat_; }

    bool readFrame(VideoFrame& out) override {
        if (!video_.alive() || eof_)
            return false;
        // 一帧可能要喂若干 sample 才吐出来（B 帧重排、解码器预热），中间还会连续
        // 「这一轮什么都没发生」。给一个宽松但有上限的步数，超了就认「这一路卡住了」。
        for (int step = 0; step < kMaxStepsPerFrame; ++step) {
            if (video_.failed.load()) {
                error_ = "视频解码器报错 " + std::to_string(video_.errorCode.load());
                fatalError_ = true;
                return false;
            }
            if (!pump()) {
                if (error_.empty())
                    error_ = "demuxer 读取失败";
                eof_ = true;
                return false;
            }
            if (frameReady_) {
                out = std::move(readyFrame_);
                frameReady_ = false;
                return true;
            }
            if (eof_)
                return false;
        }
        error_ = "连续 " + std::to_string(kMaxStepsPerFrame) + " 步没有解出画面";
        eof_ = true;
        return false;
    }

    bool readAudio(AudioChunk& out) override {
        if (!audio_.alive() || !audioFormat_.isValid())
            return false;
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        size_t avail = 0;
        if (pcmPos_ < pcmBuf_.size())
            avail = (pcmBuf_.size() - pcmPos_) / size_t(frameBytes);
        if (avail == 0)
            return false;

        const size_t want = std::min<size_t>(avail, kPcmChunkFrames);
        if (pcmView_.size() != want * size_t(frameBytes))
            pcmView_.resize(want * size_t(frameBytes));
        std::memcpy(pcmView_.data(), pcmBuf_.data() + pcmPos_, want * size_t(frameBytes));
        pcmPos_ += want * size_t(frameBytes);
        if (pcmPos_ == pcmBuf_.size()) {
            pcmBuf_.clear();
            pcmPos_ = 0;
        }

        out.frames = want;
        out.ptsUs = pcmPtsUs_;
        out.format = audioFormat_.format;
        if (audioFormat_.format == AudioSampleFormat::S16Interleaved)
            out.s16 = reinterpret_cast<const int16_t*>(pcmView_.data());
        else
            out.f32 = reinterpret_cast<const float*>(pcmView_.data());
        pcmPtsUs_ += int64_t(want) * 1000000 / audioFormat_.sampleRate;
        return true;
    }

    void flushAudio() override {
        pcmBuf_.clear();
        pcmPos_ = 0;
        pcmPtsUs_ = 0;
    }

    bool seekTo(int64_t ptsUs) override {
        if (demuxer_.dx == nullptr || !video_.alive())
            return false;
        const OH_AVErrCode rc =
                OH_AVDemuxer_SeekToTime(demuxer_.dx, ptsUs, OH_AVSeekMode::SEEK_MODE_CLOSEST_SYNC);
        if (rc != AV_ERR_OK) {
            error_ = errText("OH_AVDemuxer_SeekToTime 失败", rc);
            return false;
        }
        // flush 之后必须 Start，且第一份输入得是参数集（文档约束 1）。见文件头。
        if (!restartStream(video_)) {
            error_ = error_.empty() ? "视频解码器 seek 后重启失败" : error_;
            return false;
        }
        if (audio_.alive() && !restartStream(audio_))
            discardAudio();
        readyFrame_ = VideoFrame{};
        frameReady_ = false;
        fatalError_ = false;
        eof_ = false;
        error_.clear();
        return true;
    }

private:
    static constexpr int kMaxStepsPerFrame = 2000;
    /// 等一个回调槽的上限。这里不是在等「数据」而是在等「缓冲位」，超时不是错误：
    /// 下一轮先收输出、给解码器消化。2000 步 × 10ms = 最坏 20 秒，够长但不会挂死。
    static constexpr int64_t kWaitTimeoutUs = 10 * 1000;
    /// demuxer 暂存缓冲的起始容量（H.264 一个 sample 通常远小于此）。
    static constexpr int32_t kSampleCapacity = 1024 * 1024;
    /// 一次 `readAudio` 最多交出的采样帧数（≈42ms @48k，够 sink 一次填一条缓冲）。
    static constexpr size_t kPcmChunkFrames = 2048;
    /// PCM 队列的高水位（秒）。超过就丢音频输出、不丢输入，理由见文件头。
    static constexpr int kPcmBacklogSeconds = 2;

    // ── 打开 ────────────────────────────────────────────────────────────────

    bool prepare(const VideoSource& source, std::string& error) {
        if (!bindSource(source, error))
            return false;
        demuxer_.dx = OH_AVDemuxer_CreateWithSource(source_.src);
        if (demuxer_.dx == nullptr) {
            error = "OH_AVDemuxer_CreateWithSource 失败";
            return false;
        }
        if (!pickTracks(error))
            return false;
        if (!openStream(video_, error))
            return false;
        if (audio_.track >= 0) {
            std::string audioError;
            if (!openStream(audio_, audioError))
                discardAudio();  // 只关声音，画面继续
        }
        return true;
    }

    bool bindSource(const VideoSource& source, std::string& error) {
        if (!source.bytes.empty()) {
            memorySlot_ = claimMemorySlot();
            if (memorySlot_ < 0) {
                error = "内存来源槽已满（" + std::to_string(kMemorySlots) +
                        " 路并发）⇒ 这一路退回 FFmpeg";
                return false;
            }
            MemorySlot& m = g_memorySlots[memorySlot_];
            std::lock_guard<std::mutex> lock(m.mutex);
            m.bytes = source.bytes;
            m.pos = 0;
            dataSource_.size = int64_t(m.bytes.size());
            dataSource_.readAt = kReadAtTrampolines[memorySlot_];
            source_.src = OH_AVSource_CreateWithDataSource(&dataSource_);
            if (source_.src == nullptr) {
                releaseMemorySlot();
                error = "OH_AVSource_CreateWithDataSource 失败";
                return false;
            }
            return true;
        }
        if (source.type == VideoSource::Type::Network) {
            // `OH_AVSource_CreateWithURI` 收的是 `char *`（非 const），且只支持 HTTP
            // 渐进式流（HLS/DASH 要用 AVPlayer）。uri 文本由 uriBuffer_ 持有到创建返回。
            uriBuffer_ = source.uri;
            source_.src = OH_AVSource_CreateWithURI(uriBuffer_.data());
            if (source_.src == nullptr) {
                error = "OH_AVSource_CreateWithURI 失败：" + source.uri;
                return false;
            }
            return true;
        }
        const std::string path = stripFileScheme(source.uri);
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            error = "打不开文件：" + path;
            return false;
        }
        struct stat sb {};
        if (::fstat(fd, &sb) != 0) {
            ::close(fd);
            error = "fstat 失败：" + path;
            return false;
        }
        // source 内部 dup 了 fd，原句柄自己关（官方示例同口径：Create 之后就可以关）。
        source_.src = OH_AVSource_CreateWithFD(fd, 0, int64_t(sb.st_size));
        ::close(fd);
        if (source_.src == nullptr) {
            error = "OH_AVSource_CreateWithFD 失败：" + path;
            return false;
        }
        return true;
    }

    /// 挑第一条视频轨 + 第一条音频轨。OHOS 没有 `OH_AVTrack` 对象：轨号就是 index，
    /// 轨种从 `OH_AVSource_GetTrackFormat()` 的 `OH_MD_KEY_TRACK_TYPE` 里读。
    bool pickTracks(std::string& error) {
        FormatHolder sf(OH_AVSource_GetSourceFormat(source_.src));
        if (sf.get() == nullptr) {
            error = "OH_AVSource_GetSourceFormat 失败";
            return false;
        }
        int32_t trackCount = 0;
        if (!getInt(sf.get(), OH_MD_KEY_TRACK_COUNT, trackCount) || trackCount <= 0) {
            error = "容器里没有轨（trackCount=" + std::to_string(trackCount) + "）";
            return false;
        }
        int64_t durUs = 0;
        if (getInt64(sf.get(), OH_MD_KEY_DURATION, durUs) && durUs > 0)
            info_.duration = std::chrono::microseconds(durUs);

        for (int32_t index = 0; index < trackCount; ++index) {
            FormatHolder tf(OH_AVSource_GetTrackFormat(source_.src, uint32_t(index)));
            if (tf.get() == nullptr)
                continue;
            int32_t type = -1;
            if (!getInt(tf.get(), OH_MD_KEY_TRACK_TYPE, type))
                continue;
            const std::string mime = getString(tf.get(), OH_MD_KEY_CODEC_MIME);
            if (type == OH_MediaType::MEDIA_TYPE_VID && video_.track < 0) {
                video_.track = int(index);
                video_.mime = mime;
                readContainerInfo(tf.get());
            } else if (type == OH_MediaType::MEDIA_TYPE_AUD && audio_.track < 0) {
                audio_.track = int(index);
                audio_.mime = mime;
                readAudioInfo(tf.get());
            }
        }
        if (video_.track < 0) {
            error = "容器里没有视频轨（共 " + std::to_string(trackCount) + " 条轨）";
            return false;
        }
        info_.hasAudio = audio_.track >= 0;
        if (OH_AVDemuxer_SelectTrackByID(demuxer_.dx, uint32_t(video_.track)) != AV_ERR_OK) {
            error = "OH_AVDemuxer_SelectTrackByID(视频) 失败";
            return false;
        }
        if (audio_.track >= 0 &&
            OH_AVDemuxer_SelectTrackByID(demuxer_.dx, uint32_t(audio_.track)) != AV_ERR_OK) {
            // 选不上就退回「只看画面」。`info_.hasAudio` 报的是容器里的存在性
            //（同 win32/ffmpeg/android 口径），不是「能不能出声」，所以不动。
            discardAudio();
        }
        return true;
    }

    void readContainerInfo(OH_AVFormat* tf) {
        int32_t v = 0;
        if (getInt(tf, OH_MD_KEY_WIDTH, v) && v > 0)
            info_.width = v;
        if (getInt(tf, OH_MD_KEY_HEIGHT, v) && v > 0)
            info_.height = v;
        double fps = 0.0;
        if (getDouble(tf, OH_MD_KEY_FRAME_RATE, fps))
            info_.frameRate = fps;
        int64_t durUs = 0;
        if (info_.duration.count() <= 0 && getInt64(tf, OH_MD_KEY_DURATION, durUs) && durUs > 0)
            info_.duration = std::chrono::microseconds(durUs);
        // 容器轨上的旋转角（buffer 模式下解码器不会自己转像素，这里只是如实报出去）。
        int32_t rotation = 0;
        if (getInt(tf, OH_MD_KEY_ROTATION, rotation))
            info_.rotation = ((rotation % 360) + 360) % 360;
        getBuffer(tf, OH_MD_KEY_CODEC_CONFIG, video_.csd);
        int32_t maxInput = 0;
        if (getInt(tf, OH_MD_KEY_MAX_INPUT_SIZE, maxInput) && maxInput > 0)
            videoSampleCapacity_ = std::max(kSampleCapacity, maxInput);
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = video_.mime;
        width_ = info_.width;
        height_ = info_.height;
    }

    void readAudioInfo(OH_AVFormat* tf) {
        int32_t v = 0;
        if (getInt(tf, OH_MD_KEY_AUD_SAMPLE_RATE, v) && v > 0)
            audioFormat_.sampleRate = v;
        if (getInt(tf, OH_MD_KEY_AUD_CHANNEL_COUNT, v) && v > 0)
            audioFormat_.channels = v;
        getBuffer(tf, OH_MD_KEY_CODEC_CONFIG, audio_.csd);
        int32_t maxInput = 0;
        if (getInt(tf, OH_MD_KEY_MAX_INPUT_SIZE, maxInput) && maxInput > 0)
            audioSampleCapacity_ = std::max(256 * 1024, maxInput);
        // ADTS 标志要透传给解码器（容器里没有裸 AAC 的参数集时靠它找帧头）。
        getInt(tf, OH_MD_KEY_AAC_IS_ADTS, aacIsAdts_);
        // 输入侧看起来是 S16，真正的输出格式要等 `GetOutputDescription`（可能是
        // float、也可能是 planar）—— 那时才在 audioFormat_ 里落定。
        audioFormat_.format = AudioSampleFormat::S16Interleaved;
        rawSampleFormat_ = -1;
    }

    /// 建 + RegisterCallback + Configure 一台 codec。
    /// **顺序铁律**：`RegisterCallback` 必须在 `Configure` 之前（同步模式那条
    /// `OH_MD_KEY_ENABLE_SYNC_MODE` 才是反过来的，这里不走同步模式）。
    bool openStream(Stream& s, std::string& error) {
        s.codec.audio = (&s == &audio_);
        if (s.codec.audio) {
            // 解码器：isEncoder=false。音频只有 CreateByMime 拿到 `OH_AVCodec*`。
            s.codec.codec = OH_AudioCodec_CreateByMime(s.mime.c_str(), false);
        } else {
            s.codec.codec = OH_VideoDecoder_CreateByMime(s.mime.c_str());
        }
        if (s.codec.codec == nullptr) {
            error = "这台设备没有 " + s.mime + " 的解码器";
            return false;
        }
        OH_AVCodecCallback cb = {&onStreamError, &onStreamChanged, &onNeedInputBuffer,
                                 &onNewOutputBuffer};
        const OH_AVErrCode reg = s.codec.audio
                                         ? OH_AudioCodec_RegisterCallback(s.codec.codec, cb, &s)
                                         : OH_VideoDecoder_RegisterCallback(s.codec.codec, cb, &s);
        if (reg != AV_ERR_OK) {
            error = errText("RegisterCallback 失败", reg);
            return false;
        }
        FormatHolder fmt(OH_AVFormat_Create());
        if (fmt.get() == nullptr) {
            error = "OH_AVFormat_Create 失败";
            return false;
        }
        if (s.codec.audio) {
            if (audioFormat_.sampleRate <= 0 || audioFormat_.channels <= 0) {
                error = "音频轨没有采样率/声道数";
                return false;
            }
            OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_AUD_SAMPLE_RATE, audioFormat_.sampleRate);
            OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_AUD_CHANNEL_COUNT, audioFormat_.channels);
            if (aacIsAdts_ != 0)
                OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_AAC_IS_ADTS, aacIsAdts_);
            if (audioSampleCapacity_ > 0)
                OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_MAX_INPUT_SIZE, audioSampleCapacity_);
            const OH_AVErrCode cfg = OH_AudioCodec_Configure(s.codec.codec, fmt.get());
            if (cfg != AV_ERR_OK) {
                error = errText("OH_AudioCodec_Configure 失败", cfg);
                return false;
            }
        } else {
            if (info_.width <= 0 || info_.height <= 0) {
                error = "视频轨没有宽高，无法 Configure";
                return false;
            }
            // WIDTH/HEIGHT 是文档明写的必填项；PIXEL_FORMAT 可选，这里主动要 NV12：
            // 半平面、一份色度块，转换最省，且是 OHOS 各例程的默认值。
            OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_WIDTH, info_.width);
            OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_HEIGHT, info_.height);
            OH_AVFormat_SetIntValue(fmt.get(), OH_MD_KEY_PIXEL_FORMAT,
                                    AV_PIXEL_FORMAT_NV12);
            const OH_AVErrCode cfg = OH_VideoDecoder_Configure(s.codec.codec, fmt.get());
            if (cfg != AV_ERR_OK) {
                error = errText("OH_VideoDecoder_Configure 失败", cfg);
                return false;
            }
        }
        const int32_t cap =
                s.codec.audio ? (audioSampleCapacity_ > 0 ? audioSampleCapacity_ : 256 * 1024)
                              : videoSampleCapacity_;
        if (!s.sample.create(cap)) {
            error = "OH_AVBuffer_Create 失败（demuxer 暂存缓冲）";
            return false;
        }
        return true;
    }

    bool startStream(Stream& s, std::string& error) {
        const OH_AVErrCode pre =
                s.codec.audio ? OH_AudioCodec_Prepare(s.codec.codec) : OH_VideoDecoder_Prepare(s.codec.codec);
        if (pre != AV_ERR_OK) {
            error = errText("Prepare 失败", pre);
            return false;
        }
        if (!startExec(s, error))
            return false;
        // 首帧之前把参数集发一次（MP4 的 CSD 不在码流里，得靠 CODEC_CONFIG 补）。
        queueCodecData(s);
        return true;
    }

    bool startExec(Stream& s, std::string& error) {
        const OH_AVErrCode st =
                s.codec.audio ? OH_AudioCodec_Start(s.codec.codec) : OH_VideoDecoder_Start(s.codec.codec);
        if (st != AV_ERR_OK) {
            error = errText("Start 失败", st);
            return false;
        }
        s.failed.store(false);
        s.errorCode.store(0);
        s.eosQueued = false;
        return true;
    }

    /// flush → 作废队列 → Start → 重发参数集。视频失败 = 整条路放弃；
    /// 音频失败 = 只关声音。
    bool restartStream(Stream& s) {
        const OH_AVErrCode fl =
                s.codec.audio ? OH_AudioCodec_Flush(s.codec.codec) : OH_VideoDecoder_Flush(s.codec.codec);
        s.in.invalidateAll();
        s.out.invalidateAll();
        s.reset();
        if (fl != AV_ERR_OK) {
            error_ = errText("Flush 失败", fl);
            return false;
        }
        std::string startError;
        if (!startExec(s, startError)) {
            error_ = startError;
            return false;
        }
        if (&s == &video_)
            refreshVideoGeometry();
        else
            flushAudio();
        queueCodecData(s);
        return true;
    }

    /// 把缓存的 CSD 变成一条 pending（flags=CODEC_DATA），由下一轮 `deliver` 投出去。
    void queueCodecData(Stream& s) {
        if (s.csd.empty())
            return;
        s.pendingBytes = s.csd;
        s.pendingPts = 0;
        s.pendingFlags = AVCODEC_BUFFER_FLAGS_CODEC_DATA;
        s.pendingEos = false;
        s.hasPending = true;
    }

    /// 关掉音频这一路（codec 起不来 / 重排失败 / 设备吐异常格式），画面继续。
    /// 不能写成 `audio_ = Stream{}`：`Stream` 里装着不可拷贝的 mutex/atomic/RAII。
    /// `info_.hasAudio` 故意不动：它报的是容器里有音轨（对齐其它后端）。
    void discardAudio() {
        audio_.codec.reset();
        audio_.track = -1;
        audio_.mime.clear();
        audio_.sample.reset();
        audio_.reset();
        audio_.eosQueued = false;
        audio_.csd.clear();
        audioFormat_ = AudioFormat{};
        rawSampleFormat_ = -1;
        flushAudio();
    }

    void closeAll() {
        // 销毁顺序：codec 先走（Destroy 会等回调返回），再拆 demuxer/source。
        video_.in.invalidateAll();
        video_.out.invalidateAll();
        audio_.in.invalidateAll();
        audio_.out.invalidateAll();
        video_.codec.reset();
        audio_.codec.reset();
        video_.sample.reset();
        audio_.sample.reset();
        demuxer_.reset();
        source_.reset();
        releaseMemorySlot();
    }

    // ── 输出侧几何 ──────────────────────────────────────────────────────────

    /// stride / slice-height / 真实像素尺寸 / 实际像素格式都只有 Configure+Prepare
    /// 之后才可信，所以每次流变更、以及第一帧之前都要重新拿一份。
    /// `OH_VideoDecoder_GetOutputDescription` 交出的 `OH_AVFormat*` 由调用者销毁。
    void refreshVideoGeometry() {
        if (!video_.alive())
            return;
        FormatHolder of(OH_VideoDecoder_GetOutputDescription(video_.codec.codec));
        if (of.get() == nullptr)
            return;
        int32_t v = 0;
        if (getInt(of.get(), OH_MD_KEY_PIXEL_FORMAT, v))
            pixelFormat_ = v;
        if (getInt(of.get(), OH_MD_KEY_VIDEO_STRIDE, v) && v > 0)
            lumaStride_ = v;
        if (getInt(of.get(), OH_MD_KEY_VIDEO_SLICE_HEIGHT, v) && v > 0)
            sliceHeight_ = v;
        if (getInt(of.get(), OH_MD_KEY_VIDEO_PIC_WIDTH, v) && v > 0)
            width_ = v;
        if (getInt(of.get(), OH_MD_KEY_VIDEO_PIC_HEIGHT, v) && v > 0)
            height_ = v;
        if (width_ <= 0)
            width_ = info_.width;
        if (height_ <= 0)
            height_ = info_.height;
        if (lumaStride_ <= 0)
            lumaStride_ = width_;
        if (sliceHeight_ <= 0)
            sliceHeight_ = height_;
        video_.geomDirty.store(false);
    }

    /// 音频输出侧才是采样的真格式。返回 false = 这格式我们不认（关声音，画面继续）。
    bool applyAudioGeometry() {
        if (!audio_.alive())
            return false;
        FormatHolder of(OH_AudioCodec_GetOutputDescription(audio_.codec.codec));
        if (of.get() == nullptr)
            return audioFormat_.isValid();
        int32_t v = 0;
        if (getInt(of.get(), OH_MD_KEY_AUD_SAMPLE_RATE, v) && v > 0)
            audioFormat_.sampleRate = v;
        if (getInt(of.get(), OH_MD_KEY_AUD_CHANNEL_COUNT, v) && v > 0)
            audioFormat_.channels = v;
        int32_t sampleFormat = -1;
        if (!getInt(of.get(), OH_MD_KEY_AUDIO_SAMPLE_FORMAT, sampleFormat))
            sampleFormat = SAMPLE_S16LE;
        if (rawSampleFormat_ >= 0 && rawSampleFormat_ != sampleFormat && !pcmBuf_.empty()) {
            // 中途换采样格式：环形缓冲里已经排着旧布局的字节，混着读只会出噪声。
            // 与其猜不如整条声音关掉（画面继续），比播坏声音好。
            discardAudio();
            return false;
        }
        rawSampleFormat_ = sampleFormat;
        switch (sampleFormat) {
            case SAMPLE_S16LE:
            case SAMPLE_S16P:
                audioFormat_.format = AudioSampleFormat::S16Interleaved;
                break;
            case SAMPLE_F32LE:
            case SAMPLE_F32P:
            case SAMPLE_S32LE:
                audioFormat_.format = AudioSampleFormat::F32Interleaved;
                break;
            default:
                audioFormat_ = AudioFormat{};
                return false;
        }
        audio_.geomDirty.store(false);
        return audioFormat_.isValid();
    }

    // ── 泵 ──────────────────────────────────────────────────────────────────

    /// 一轮：收两条轨的输出 → 把各自 pending 交给空槽 → 从 demuxer 各补一个 sample。
    /// 返回 false 只在 demuxer/解码器真出错时发生。
    bool pump() {
        if (!drainVideoOutput())
            return false;
        if (audio_.alive())
            drainAudioOutput();
        if (!deliver(video_))
            return false;
        if (audio_.alive() && !deliver(audio_))
            return false;
        if (!fetchSample(video_))
            return false;
        if (audio_.alive() && !fetchSample(audio_))
            return false;
        return true;
    }

    /// 有一个空 input 槽就把 `s` 的 pending 交出去。等不到槽不是错误：留着下一轮。
    bool deliver(Stream& s) {
        if (!s.hasPending)
            return true;
        auto slot = s.in.waitPop(kWaitTimeoutUs);
        if (slot == nullptr || !slot->valid)
            return true;  // 超时 / 槽已被 flush 作废：pending 原样留着

        OH_AVCodecBufferAttr attr{};
        attr.pts = s.pendingPts;
        attr.offset = 0;
        attr.size = 0;
        attr.flags = s.pendingEos ? uint32_t(AVCODEC_BUFFER_FLAGS_EOS) : s.pendingFlags;

        if (!s.pendingEos) {
            const uint8_t* addr = OH_AVBuffer_GetAddr(slot->buffer);
            const int32_t cap = OH_AVBuffer_GetCapacity(slot->buffer);
            if (addr == nullptr || cap <= 0) {
                error_ = "拿不到 input buffer 地址";
                return false;
            }
            if (s.pendingBytes.size() > size_t(cap)) {
                // 截断会解出坏帧：视频侧放弃整条（退 FFmpeg），音频侧只关声音。
                const std::string detail = "input buffer 太小（" +
                                           std::to_string(s.pendingBytes.size()) + " > " +
                                           std::to_string(cap) + "）";
                if (&s == &video_) {
                    error_ = detail;
                    return false;
                }
                discardAudio();
                return true;
            }
            std::memcpy(const_cast<uint8_t*>(addr), s.pendingBytes.data(), s.pendingBytes.size());
            attr.size = int32_t(s.pendingBytes.size());
        }
        if (OH_AVBuffer_SetBufferAttr(slot->buffer, &attr) != AV_ERR_OK) {
            error_ = "OH_AVBuffer_SetBufferAttr 失败";
            return false;
        }
        const OH_AVErrCode pushed = s.codec.audio
                                           ? OH_AudioCodec_PushInputBuffer(s.codec.codec, slot->index)
                                           : OH_VideoDecoder_PushInputBuffer(s.codec.codec, slot->index);
        if (pushed != AV_ERR_OK) {
            error_ = errText("PushInputBuffer 失败", pushed);
            return false;
        }
        if (s.pendingEos)
            s.eosQueued = true;
        s.reset();
        return true;
    }

    /// 从 demuxer 给 `s` 的那条轨读一个 sample 到自有暂存缓冲（游标按轨推进）。
    bool fetchSample(Stream& s) {
        if (s.track < 0 || s.hasPending || s.eosQueued || s.sample.buf == nullptr)
            return true;
        const OH_AVErrCode rc =
                OH_AVDemuxer_ReadSampleBuffer(demuxer_.dx, uint32_t(s.track), s.sample.buf);
        if (rc != AV_ERR_OK) {
            error_ = errText("OH_AVDemuxer_ReadSampleBuffer 失败", rc);
            return false;
        }
        OH_AVCodecBufferAttr attr{};
        if (OH_AVBuffer_GetBufferAttr(s.sample.buf, &attr) != AV_ERR_OK) {
            error_ = "OH_AVBuffer_GetBufferAttr 失败";
            return false;
        }
        if ((attr.flags & AVCODEC_BUFFER_FLAGS_INCOMPLETE_FRAME) != 0) {
            // 装不下：demuxer 会静默截断（半个 NAL）。翻倍重建再读一次同一个 sample
            //（游标没推进），还是没有就按截断处理 —— 视频放弃整条，音频关声音。
            const int32_t before = s.sample.capacity;
            s.sample.grow();
            if (s.sample.buf == nullptr || s.sample.capacity <= before) {
                if (&s == &video_) {
                    error_ = "sample 超出暂存缓冲，且扩容失败";
                    return false;
                }
                discardAudio();
                return true;
            }
            return true;  // 下一轮重读这一个 sample
        }
        const uint8_t* addr = OH_AVBuffer_GetAddr(s.sample.buf);
        const bool eos = (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0;
        int32_t n = attr.size;
        if (n < 0)
            n = 0;
        if (!eos) {
            if (addr == nullptr || n > s.sample.capacity) {
                error_ = "demuxer 暂存缓冲越界";
                return false;
            }
            const int32_t off = std::max(0, attr.offset);
            const int32_t take = std::min(n, s.sample.capacity - off);
            s.pendingBytes.assign(addr + off, addr + off + take);
        } else {
            s.pendingBytes.clear();
        }
        s.pendingPts = attr.pts;
        s.pendingFlags = attr.flags;
        s.pendingEos = eos;
        s.hasPending = true;
        return true;
    }

    bool drainVideoOutput() {
        auto slot = video_.out.tryPop();
        if (slot == nullptr)
            return true;
        if (!slot->valid)
            return true;  // flush 之后的旧槽：不碰、不 Free
        if (video_.geomDirty.load())
            refreshVideoGeometry();
        OH_AVCodecBufferAttr attr{};
        const OH_AVErrCode got = OH_AVBuffer_GetBufferAttr(slot->buffer, &attr);
        if (got == AV_ERR_OK) {
            if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0)
                eof_ = true;
            else if (attr.size > 0 && !frameReady_)
                renderVideo(slot->buffer, attr);
        } else {
            error_ = errText("OH_AVBuffer_GetBufferAttr(视频) 失败", got);
        }
        const OH_AVErrCode freed =
                OH_VideoDecoder_FreeOutputBuffer(video_.codec.codec, slot->index);
        if (freed != AV_ERR_OK && error_.empty())
            error_ = errText("OH_VideoDecoder_FreeOutputBuffer 失败", freed);
        if (fatalError_)
            return false;
        return true;
    }

    void drainAudioOutput() {
        auto slot = audio_.out.tryPop();
        if (slot == nullptr)
            return;
        if (!slot->valid)
            return;
        bool alive = true;
        if (audio_.geomDirty.load() && !applyAudioGeometry())
            alive = false;
        else if (audioFormat_.isValid()) {
            OH_AVCodecBufferAttr attr{};
            const OH_AVErrCode got = OH_AVBuffer_GetBufferAttr(slot->buffer, &attr);
            const bool last = got == AV_ERR_OK && (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0;
            if (got != AV_ERR_OK) {
                discardAudio();
                alive = false;
            } else if (attr.size > 0 && !last && !audioBacklogFull()) {
                if (!appendAudio(slot->buffer, attr)) {
                    error_ = "音频解码器交出的采样格式不支持";
                    discardAudio();
                    alive = false;
                }
            }
        }
        // 必须先判 `alive`：`discardAudio()` 已经 Destroy 了这台 codec，再拿旧槽
        // 去 FreeOutputBuffer 就是往已释放的解码器上操作。
        if (alive)
            freeAudioOutput(slot);
    }

    void freeAudioOutput(const SlotQueue::Item& slot) {
        OH_AudioCodec_FreeOutputBuffer(audio_.codec.codec, slot->index);
    }

    bool audioBacklogFull() const {
        if (!audioFormat_.isValid())
            return true;
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        const int64_t queued =
                (int64_t(pcmBuf_.size()) - int64_t(pcmPos_)) / std::max<int64_t>(1, frameBytes);
        return queued >= int64_t(audioFormat_.sampleRate) * kPcmBacklogSeconds;
    }

    /// 解码器交出的 PCM 归一成**交织**布局塞进 pcmBuf_（`readAudio` 只按字节切片）。
    /// 返回 false = 格式认不出（调用方只关声音）。
    bool appendAudio(OH_AVBuffer* buffer, const OH_AVCodecBufferAttr& attr) {
        const uint8_t* base = OH_AVBuffer_GetAddr(buffer);
        if (base == nullptr)
            return false;
        const int32_t off = std::max(0, attr.offset);
        const int32_t cap = OH_AVBuffer_GetCapacity(buffer);
        if (off + attr.size > cap)
            return false;
        const uint8_t* src = base + off;
        const int channels = audioFormat_.channels;
        const size_t bytes = size_t(std::max(0, attr.size));
        if (bytes == 0 || channels <= 0)
            return true;
        if (pcmPos_ == pcmBuf_.size())
            pcmPtsUs_ = attr.pts;

        // 样本宽度与是否平面，都从 `OH_BitsPerSample` 推。目标布局在
        // `applyAudioGeometry()` 里已经定下（S16LE/S16P ⇒ S16，其余 ⇒ F32）。
        size_t sampleBytes = 0;
        bool planar = false;
        switch (rawSampleFormat_) {
            case SAMPLE_S16LE:
                sampleBytes = 2;
                break;
            case SAMPLE_F32LE:
                sampleBytes = 4;
                break;
            case SAMPLE_S32LE:
                sampleBytes = 4;
                break;
            case SAMPLE_S16P:
                sampleBytes = 2;
                planar = true;
                break;
            case SAMPLE_F32P:
                sampleBytes = 4;
                planar = true;
                break;
            default:
                return false;
        }
        // 交织与平面的总长都是 `frames * channels * sampleBytes`，帧数算法共用。
        const size_t frameBytes = size_t(channels) * sampleBytes;
        const size_t frames = bytes / frameBytes;
        if (frames == 0)
            return true;

        const bool toFloat = audioFormat_.format == AudioSampleFormat::F32Interleaved;
        if (!planar && rawSampleFormat_ != SAMPLE_S32LE) {
            // S16LE / F32LE：布局与目标一致 ⇒ 原样搬（不重采样、不换算字长）。
            const size_t old = pcmBuf_.size();
            pcmBuf_.resize(old + frames * frameBytes);
            std::memcpy(pcmBuf_.data() + old, src, frames * frameBytes);
            return true;
        }

        const size_t old = pcmBuf_.size();
        const size_t outSamples = frames * size_t(channels);
        pcmBuf_.resize(old + outSamples * (toFloat ? sizeof(float) : sizeof(int16_t)));
        uint8_t* dst = pcmBuf_.data() + old;

        if (rawSampleFormat_ == SAMPLE_S32LE) {
            // 定点 32 位 ⇒ [-1,1) 浮点（S32 的满幅就是 2^31，除下来恰好不溢出）。
            const int32_t* in = reinterpret_cast<const int32_t*>(src);
            float* out = reinterpret_cast<float*>(dst);
            for (size_t i = 0; i < outSamples; ++i)
                out[i] = float(in[i]) * (1.0f / 2147483648.0f);
            return true;
        }
        // 平面 ⇒ 逐声道摊成交织。
        for (int ch = 0; ch < channels; ++ch) {
            if (toFloat) {
                const float* in = reinterpret_cast<const float*>(src) + size_t(ch) * frames;
                float* out = reinterpret_cast<float*>(dst);
                for (size_t i = 0; i < frames; ++i)
                    out[i * size_t(channels) + size_t(ch)] = in[i];
            } else {
                const int16_t* in = reinterpret_cast<const int16_t*>(src) + size_t(ch) * frames;
                int16_t* out = reinterpret_cast<int16_t*>(dst);
                for (size_t i = 0; i < frames; ++i)
                    out[i * size_t(channels) + size_t(ch)] = in[i];
            }
        }
        return true;
    }

    /// 把输出缓冲里的像素转成 BGRA 位图；成功时填 `readyFrame_`。
    /// 失败分两级：结构性问题（格式不认识）置 `fatalError_` 放弃整条路，
    /// 单帧问题（长度不够/越界）只报错，下一帧照样试。
    bool renderVideo(OH_AVBuffer* buffer, const OH_AVCodecBufferAttr& attr) {
        auto fatal = [&](std::string message) {
            error_ = std::move(message);
            fatalError_ = true;
            return false;
        };
        const uint8_t* p = OH_AVBuffer_GetAddr(buffer);
        if (p == nullptr)
            return fatal("buffer 模式下拿不到输出地址");
        const int32_t cap = OH_AVBuffer_GetCapacity(buffer);
        const int32_t off = std::max(0, attr.offset);
        if (off + attr.size > cap) {
            error_ = "视频输出缓冲越界";
            return false;
        }
        p += off;

        const int w = width_ > 0 ? width_ : info_.width;
        const int h = height_ > 0 ? height_ : info_.height;
        if (w <= 0 || h <= 0) {
            error_ = "还没有拿到输出尺寸";
            return false;
        }
        const int32_t lumaStride = lumaStride_ > 0 ? lumaStride_ : w;
        const int32_t slice = sliceHeight_ > 0 ? sliceHeight_ : h;

        const SkImageInfo imageInfo =
                SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
        sk_sp<SkData> pixels = SkData::MakeUninitialized(size_t(w) * 4 * size_t(h));
        // 本机 Skia 版本的 `writable_data()` 返 `void*`，得显式转。
        uint8_t* dst = static_cast<uint8_t*>(pixels->writable_data());
        const int32_t dstStride = w * 4;

        if (pixelFormat_ == AV_PIXEL_FORMAT_RGBA) {
            const size_t need = size_t(lumaStride) * h;  // RGBA 的行距按像素 stride 给
            if (need > size_t(attr.size)) {
                error_ = "RGBA 输出帧太小（需要 " + std::to_string(need) + "，只有 " +
                         std::to_string(attr.size) + "）";
                return false;
            }
            convertRgba(p, lumaStride, w, h, dst, dstStride);
            readyFrame_.image = SkImages::RasterFromData(imageInfo, std::move(pixels), dstStride);
            readyFrame_.ptsUs = attr.pts;
            frameReady_ = readyFrame_.image != nullptr;
            if (!frameReady_)
                error_ = "SkImages::RasterFromData 失败";
            return frameReady_;
        }

        const bool semi = pixelFormat_ == AV_PIXEL_FORMAT_NV12 ||
                          pixelFormat_ == AV_PIXEL_FORMAT_NV21;
        const bool planarYuv = pixelFormat_ == AV_PIXEL_FORMAT_YUVI420;
        if (!semi && !planarYuv)
            return fatal("不支持的像素格式 " + hexText(pixelFormat_) + "（这一路交回 FFmpeg）");
        const int32_t chromaStride = semi ? lumaStride : lumaStride / 2;
        const int32_t cRows = slice / 2;
        const size_t need = size_t(lumaStride) * slice +
                            size_t(chromaStride) * cRows * (semi ? 1 : 2);
        if (need > size_t(attr.size)) {
            error_ = "输出帧太小（需要 " + std::to_string(need) + " 字节，缓冲只有 " +
                     std::to_string(attr.size) + "），pixel format=" + hexText(pixelFormat_);
            return false;
        }
        const uint8_t* luma = p;
        const uint8_t* chroma = p + size_t(lumaStride) * slice;
        if (semi) {
            convertYuv(luma, lumaStride, chroma, nullptr, chromaStride, /*semiPlanar=*/true,
                       /*vFirst=*/pixelFormat_ == AV_PIXEL_FORMAT_NV21, w, h, dst, dstStride);
        } else {
            convertYuv(luma, lumaStride, chroma, chroma + size_t(chromaStride) * cRows,
                       chromaStride, /*semiPlanar=*/false, /*vFirst=*/false, w, h, dst, dstStride);
        }
        readyFrame_.image = SkImages::RasterFromData(imageInfo, std::move(pixels), dstStride);
        if (!readyFrame_.image) {
            error_ = "SkImages::RasterFromData 失败";
            return false;
        }
        readyFrame_.ptsUs = attr.pts;
        frameReady_ = true;
        return true;
    }

    // ── 内存来源的槽位 ──────────────────────────────────────────────────────

    static int claimMemorySlot() {
        for (int i = 0; i < kMemorySlots; ++i) {
            MemorySlot& m = g_memorySlots[i];
            std::lock_guard<std::mutex> lock(m.mutex);
            if (!m.busy) {
                m.busy = true;
                return i;
            }
        }
        return -1;
    }
    void releaseMemorySlot() {
        if (memorySlot_ < 0)
            return;
        MemorySlot& m = g_memorySlots[memorySlot_];
        std::lock_guard<std::mutex> lock(m.mutex);
        m.bytes.clear();
        m.pos = 0;
        m.busy = false;
        memorySlot_ = -1;
    }

    SourceHolder source_;
    DemuxerHolder demuxer_;
    OH_AVDataSource dataSource_{};
    std::string uriBuffer_;
    int memorySlot_ = -1;

    Stream video_;
    Stream audio_;
    VideoInfo info_;
    std::string error_;

    VideoFrame readyFrame_;
    bool frameReady_ = false;
    bool eof_ = false;
    /// 结构性故障（格式不认识 / 缓冲拿不到）：readFrame 因此直接终止。
    bool fatalError_ = false;

    // 输出侧几何（refreshVideoGeometry 填）。
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t lumaStride_ = 0;
    int32_t sliceHeight_ = 0;
    int32_t pixelFormat_ = 0;
    int32_t videoSampleCapacity_ = kSampleCapacity;
    int32_t audioSampleCapacity_ = 0;

    // 音频：归一成交织后的原始字节，readAudio 从这里切片交给 AudioSink。
    AudioFormat audioFormat_;
    int32_t rawSampleFormat_ = -1;
    int32_t aacIsAdts_ = 0;
    std::vector<uint8_t> pcmBuf_;
    std::vector<uint8_t> pcmView_;
    size_t pcmPos_ = 0;
    int64_t pcmPtsUs_ = 0;
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> createOhosAvCodecBackend(const VideoSource& source,
                                                              std::string& error) {
    auto backend = std::make_unique<OhosAvCodecBackend>();
    if (!backend->open(source, error))
        return nullptr;
    return backend;
}

/// 运行期探测：这六个 `libnative_media_*.so` 是链接期依赖（进程一起来就加载好了），
/// API 9/10 之后系统必带 ⇒ 「编进来了但这台机器没有 OH_AVDemuxer」不成立。真正的不确定性
/// 在**具体解码器**（某设备可能没有 HEVC 硬解），那只能在 `CreateByMime` 里见分晓
/// —— 不在这里预测（与 Android 那份同口径）。
bool ohosAvCodecVideoAvailable(std::string& note) {
    static const bool ok = dlsym(RTLD_DEFAULT, "OH_VideoDecoder_CreateByMime") != nullptr;
    if (!ok)
        note = "libnative_media_vdec 里没有 OH_VideoDecoder_CreateByMime 符号";
    return ok;
}

}  // namespace cui

#endif  // CUI_HAVE_OH_AV_CODEC
