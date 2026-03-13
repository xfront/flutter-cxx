/// @file video/video_decoder_android.cpp
/// @brief Android 系统媒体栈解码后端（AMediaExtractor + AMediaCodec）。
///
/// 路线一（各端系统 API）在 Android 上的落点，角色与 Windows 那份
/// `video_decoder_win32.cpp` 对齐：**解复用 + 解码交给操作系统**，本仓只把解出来
/// 的 YUV 转成 BGRA、把 PCM 转交给 `AudioSink`。与 FFmpeg 那条的差别只在谁在解码。
///
/// 关键实现口径：
///   - 走 **byte-buffer 模式**（`AMediaCodec_configure(..., surface=nullptr, ...)`）。
///     本仓渲染路径是 Skia 位图，没有 Java `Surface`／`ANativeWindow` 可给；给了
///     反而多一次 GPU 往返。代价是 YUV→RGB 自己做（`convertYuv`），收益是帧数据
///     与 FFmpeg／MF 后端同形、上层零改动。
///   - **视频与音频各一个 AMediaCodec**，两条轨一起 select。`AMediaExtractor` 只有
///     一个游标，按呈现顺序把已 select 的轨混在一条序列里吐出来，所以每个 sample
///     必须先看 `AMediaExtractor_getSampleTrackIndex()` 再决定交给哪台 codec ——
///     这就是下面 `Stream` 有两个 pending 槽的原因。
///   - 出声仍然走本仓的 `AudioSink`（`audio_sink_android.cpp` 的 AAudio），不在这里
///     直接碰 AudioTrack：主时钟要问「已经播到第几微秒」，只有 PCM 经过我们的环形
///     缓冲才拿得到这个数（见 audio_ring.h 的位置口径）。
///   - 输入侧先 `AMediaExtractor_readSampleData` 到自有暂存缓冲、再拷进 codec 的
///     input buffer。直接读进 input buffer 是官方示例的写法，但 sample 大于 buffer
///     时会被**静默截断**（解码器收到半个 NAL 之后的表现随设备而变），多一次 memcpy
///     换掉这一类只在某些机型复现的坏帧，划算。
///   - 一个 sample 一旦从 extractor 取出就必须交给对应的 codec：`pending` 兜住
///     「input buffer 还没空出来」的情况，否则丢样本 = 花屏，而 H.264 里丢掉的那一帧
///     会连带污染后面一整段参考链。
///   - pts 一律取输出侧的 `AMediaCodecBufferInfo::presentationTimeUs`，不取 extractor
///     的输入 pts —— 有 B 帧时两者顺序不同，只有输出侧的是呈现顺序。
///   - seek = `AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC` + 两台 codec 各自 `flush()`，并清掉
///     未交出的 PCM（`flushAudio` 的语义由引擎在 seek 路径上另外调用）。
///   - 旋转只报 `info().rotation`，不自己翻像素（与 FFmpeg／MF 后端一致）。
///   - 音频积压超阈值时**丢输出不丢输入**：codec 的 output buffer 照样 release，只是
///     不拷进队列。反过来「不取 output」会让 audio input 永久拿不到空位，整条
///     extractor 游标就卡死了。
///
/// 有意未覆盖：
///   - 内存来源（`VideoSource::memory()`）靠 `AMediaDataSource` +
///     `AMediaExtractor_setDataSourceCustom`，这两个都是 **API 28 起**（NDK 头里
///     整套 `AMediaDataSource_new/_set*/_delete` 与 `setDataSourceCustom` 都挂着
///     `__INTRODUCED_IN(28)`）。本仓交叉下限就是 28 ⇒ 常态会编进来；有人把
///     `ANDROID_PLATFORM` 调回 24 时这一路如实报「不支持」，让引擎退到 FFmpeg。
///     （旧写法用的 `AMediaDataSource_createFromCallbacks` **NDK 里从来没有**，
///     数据源只能 `_new()` 再逐个 `_set*` 填回调；见 `bindSource`。）
///   - 10bit / tiled / 厂商私有 color format：转不了就报错，让引擎退到 FFmpeg（视频）。
///     音频侧解码器缺失/格式不认识时**只关声音**，画面继续（与 FFmpeg 后端同策略）。
///   - 加密（`AMediaCrypto`）、多音轨切换、硬件解码失败后重试软件解码器。

#include "cui/video/video_decoder.h"

#if defined(CUI_HAVE_MEDIACODEC)

#include <algorithm>
#include <dlfcn.h>
#include <fcntl.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cui {
namespace {

std::string statusText(const char* what, media_status_t st) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (%d)", static_cast<int>(st));
    return std::string(what) + buf;
}

std::string hexText(int32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(v));
    return buf;
}

bool getInt(AMediaFormat* fmt, const char* key, int32_t& out) {
    return fmt != nullptr && AMediaFormat_getInt32(fmt, key, &out);
}
bool getInt64(AMediaFormat* fmt, const char* key, int64_t& out) {
    return fmt != nullptr && AMediaFormat_getInt64(fmt, key, &out);
}
bool getFloat(AMediaFormat* fmt, const char* key, float& out) {
    return fmt != nullptr && AMediaFormat_getFloat(fmt, key, &out);
}
/// `AMediaFormat_getString` 交出来的指针由 format 持有，必须在 format 删除前拷走，
/// 所以这里直接返回 std::string。
std::string getString(AMediaFormat* fmt, const char* key) {
    const char* raw = nullptr;
    if (fmt == nullptr || !AMediaFormat_getString(fmt, key, &raw) || raw == nullptr)
        return {};
    return raw;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string stripFileScheme(const std::string& uri) {
    if (startsWith(uri, "file://"))
        return uri.substr(7);
    return uri;
}

// ── color format（NDK 头里没有这些常量，值按 AOSP MediaFormat 公开定义硬编） ────
constexpr int32_t kColorYuv420Planar = 0x7F000400;            ///< I420：Y / U / V 三平面
constexpr int32_t kColorYuv420SemiPlanar = 0x7FA00A00;        ///< NV12：Y + UV 交织
constexpr int32_t kColorYuv420PackedSemiPlanar = 0x7FC00400;  ///< NV21：Y + VU 交织

// Android AudioFormat 编码常量（同样只存在于 Java 侧）。
constexpr int32_t kPcmEncoding16Bit = 2;
constexpr int32_t kPcmEncodingFloat = 3;

inline uint8_t clamp8(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/// BT.601 有限范围 → BGRA。`chromaStride` 是色度**行**的字节距（semi-planar 时一行
/// 含 UV 两路，所以它通常等于 luma stride）；`u`/`v` 在 semi-planar 下都指向交织块
/// 首字节，靠 `vFirst` 区分 NV12 / NV21。
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
            // 与 MF 后端同口径：Skia 按 kOpaque 读，但 Graphite 上传纹理后仍按原始
            // 通道采样，alpha 留 0 就成了一块透明洞，所以显式写 0xFF。
            outRow[col * 4 + 3] = 0xFF;
        }
    }
}

// ── RAII：句柄不可拷贝，重复 delete 是未定义行为 ────────────────────────────
struct ExtractorHolder {
    ExtractorHolder() = default;
    AMediaExtractor* ex = nullptr;
    ~ExtractorHolder() {
        if (ex)
            AMediaExtractor_delete(ex);
    }
    ExtractorHolder(const ExtractorHolder&) = delete;
    ExtractorHolder& operator=(const ExtractorHolder&) = delete;
};

struct CodecHolder {
    CodecHolder() = default;
    AMediaCodec* codec = nullptr;
    ~CodecHolder() { reset(); }
    /// 停 + 销毁 + 置空。`discardAudio()` 要用它，所以不能只写在析构里。
    void reset() {
        if (codec) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
            codec = nullptr;
        }
    }
    CodecHolder(const CodecHolder&) = delete;
    CodecHolder& operator=(const CodecHolder&) = delete;
};

struct FormatHolder {
    explicit FormatHolder(AMediaFormat* p = nullptr) : fmt(p) {}
    ~FormatHolder() {
        if (fmt)
            AMediaFormat_delete(fmt);
    }
    FormatHolder(const FormatHolder&) = delete;
    FormatHolder& operator=(const FormatHolder&) = delete;
    AMediaFormat* get() const { return fmt; }
    AMediaFormat* fmt = nullptr;
};

#if defined(ANDROID) && __ANDROID_API__ >= 28
/// 内存来源的数据源句柄（`AMediaDataSource_*` 整套自 API 28 起）。
struct DataSourceHolder {
    DataSourceHolder() = default;
    ~DataSourceHolder() { reset(); }
    void reset() {
        if (ds) {
            AMediaDataSource_delete(ds);
            ds = nullptr;
        }
    }
    void set(AMediaDataSource* p) {
        reset();
        ds = p;
    }
    AMediaDataSource* get() const { return ds; }
    DataSourceHolder(const DataSourceHolder&) = delete;
    DataSourceHolder& operator=(const DataSourceHolder&) = delete;
    AMediaDataSource* ds = nullptr;
};
#endif

/// 一条轨的解码状态：extractor 取到但还没交给 codec 的 sample，以及 EOS 是否已排队。
/// 视频与音频各一份（两台 codec 的 buffer 池彼此独立，所以 pending 也必须独立）。
struct Stream {
    CodecHolder codec;
    int track = -1;
    std::string mime;
    std::vector<uint8_t> pendingBytes;
    int64_t pendingTimeUs = 0;
    uint32_t pendingFlags = 0;
    bool hasPending = false;
    bool pendingEos = false;
    bool eosQueued = false;

    void reset() {
        pendingBytes.clear();
        hasPending = false;
        pendingEos = false;
    }
};

class MediaCodecBackend : public VideoDecoderBackend {
public:
    ~MediaCodecBackend() override = default;

    bool open(const VideoSource& source, std::string& error) {
        if (!prepare(source, error))
            return false;
        if (AMediaCodec_start(video_.codec.codec) != AMEDIA_OK) {
            error = "视频 AMediaCodec_start 失败";
            return false;
        }
        readVideoGeometry();
        if (audio_.codec.codec && AMediaCodec_start(audio_.codec.codec) != AMEDIA_OK) {
            // 起不来的音频解码器不该拖死画面：关掉这一路的出声，视频照走。
            discardAudio();
        }
        return true;
    }

    const VideoInfo& info() const override { return info_; }
    bool atEnd() const override { return eof_; }
    const std::string& error() const override { return error_; }

    bool hasAudioStream() const override { return audio_.codec.codec != nullptr; }
    AudioFormat audioFormat() const override { return audioFormat_; }

    bool readFrame(VideoFrame& out) override {
        if (!video_.codec.codec || eof_)
            return false;

        // 一帧可能要喂若干 sample 才吐出来（B 帧重排、解码器预热），中间还会连续
        // TRY_AGAIN_LATER。给一个宽松但有上限的步数，超了就认「这一路卡住了」——
        // 报错比永久阻塞在工作线程上体面。
        for (int step = 0; step < kMaxStepsPerFrame; ++step) {
            if (!pump()) {
                if (error_.empty())
                    error_ = "extractor 读取失败";
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
        if (!audio_.codec.codec || audioFormat_.channels <= 0)
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
        // `AudioChunk` 的内存由后端持有：view 到下一次 readAudio/flushAudio 有效，
        // 而 `AudioSink::push()` 是立刻拷进环形缓冲的，所以这个生命周期够用。
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
        if (!extractor_.ex || !video_.codec.codec)
            return false;
        const media_status_t st =
                AMediaExtractor_seekTo(extractor_.ex, ptsUs, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
        if (st != AMEDIA_OK) {
            error_ = statusText("AMediaExtractor_seekTo 失败", st);
            return false;
        }
        // flush 丢掉重排窗口里的旧帧；之后 queue 的 sample 全部来自新的关键帧，
        // 否则第一帧会带着旧序列的时间戳。flush 后必须重新 start 才继续收输入。
        if (!restartStream(video_))
            return false;
        if (audio_.codec.codec && !restartStream(audio_))
            discardAudio();
        readyFrame_ = VideoFrame {};
        frameReady_ = false;
        fatalError_ = false;
        eof_ = false;
        error_.clear();
        return true;
    }

private:
    static constexpr int kMaxStepsPerFrame = 2000;
    /// 单次 dequeueOutputBuffer 的等待上限（µs）。太小会在慢机上空转烧 CPU，太大
    /// 则在 EOS 时卡住 —— 2000 步 × 10ms 是最坏 20 秒，够长但不会永久挂死。
    static constexpr int64_t kOutputTimeoutUs = 10 * 1000;
    /// dequeueInputBuffer **必须**用 0：这里只是看一眼「现在有没有空位」，有就交、
    /// 没有就先去收输出。阻塞在这里会在「输入满且输出也满（解码器还没吐出第一帧）」
    /// 时死锁 —— 那个状态在 H.264 预热期是常态。
    static constexpr int64_t kInputTimeoutUs = 0;
    /// 一次 `readAudio` 最多交出的采样帧数（≈42ms @48k，够 sink 一次填一条缓冲）。
    static constexpr size_t kPcmChunkFrames = 2048;
    /// PCM 队列的高水位（秒）。超过就丢音频输出、不丢输入，理由见文件头。
    static constexpr int kPcmBacklogSeconds = 2;

    // ── 打开 ────────────────────────────────────────────────────────────────

    bool prepare(const VideoSource& source, std::string& error) {
        extractor_.ex = AMediaExtractor_new();
        if (!extractor_.ex) {
            error = "AMediaExtractor_new 失败";
            return false;
        }
        if (!bindSource(source, error))
            return false;
        if (!pickTracks(error))
            return false;
        if (!openStream(video_, error))
            return false;
        if (audio_.track >= 0) {
            std::string audioError;
            if (!openStream(audio_, audioError))
                discardAudio();  // 只关声音，画面继续（见文件头）
        }
        return true;
    }

    bool bindSource(const VideoSource& source, std::string& error) {
        media_status_t st = AMEDIA_ERROR_UNKNOWN;
        if (!source.bytes.empty()) {
#if defined(ANDROID) && __ANDROID_API__ >= 28
            sourceBytes_ = source.bytes;
            dataSource_.set(AMediaDataSource_new());
            if (!dataSource_.get()) {
                error = "AMediaDataSource_new 失败";
                return false;
            }
            // 回调集：`readAt` 是随机读（框架给偏移），`getSize` 给总长，`close`
            // 在框架这边释放数据源时敲我们一下。userdata 就是本后端。
            AMediaDataSource_setUserdata(dataSource_.get(), this);
            AMediaDataSource_setGetSize(dataSource_.get(), &MediaCodecBackend::sizeMemory);
            AMediaDataSource_setReadAt(dataSource_.get(), &MediaCodecBackend::readMemory);
            AMediaDataSource_setClose(dataSource_.get(), &MediaCodecBackend::closeMemory);
            st = AMediaExtractor_setDataSourceCustom(extractor_.ex, dataSource_.get());
            if (st != AMEDIA_OK)
                error = statusText("AMediaExtractor_setDataSourceCustom 失败", st);
#else
            (void)source;
            error = "Android 系统端不支持内存来源（要 API 28 的 AMediaDataSource）";
            return false;
#endif
        } else if (source.type == VideoSource::Type::Network) {
            st = AMediaExtractor_setDataSource(extractor_.ex, source.uri.c_str());
            if (st != AMEDIA_OK)
                error = statusText("AMediaExtractor_setDataSource 失败", st);
        } else {
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
            // extractor 内部 dup 了 fd，原句柄自己关（官方示例同口径）。
            st = AMediaExtractor_setDataSourceFd(extractor_.ex, fd, off64_t(0), off64_t(sb.st_size));
            ::close(fd);
            if (st != AMEDIA_OK)
                error = statusText("AMediaExtractor_setDataSourceFd 失败", st);
        }
        return st == AMEDIA_OK;
    }

    /// 挑第一条 video 轨 + 第一条 audio 轨，两条一起 select。
    bool pickTracks(std::string& error) {
        const size_t tracks = AMediaExtractor_getTrackCount(extractor_.ex);
        for (size_t i = 0; i < tracks; ++i) {
            FormatHolder tf(AMediaExtractor_getTrackFormat(extractor_.ex, i));
            if (!tf.get())
                continue;
            const std::string mime = getString(tf.get(), AMEDIAFORMAT_KEY_MIME);
            if (startsWith(mime, "video/") && video_.track < 0) {
                video_.track = int(i);
                video_.mime = mime;
                readContainerInfo(tf.get());
            } else if (startsWith(mime, "audio/") && audio_.track < 0) {
                audio_.track = int(i);
                audio_.mime = mime;
                readAudioInfo(tf.get());
            }
        }
        if (video_.track < 0) {
            error = "容器里没有视频轨（共 " + std::to_string(tracks) + " 条轨）";
            return false;
        }
        info_.hasAudio = audio_.track >= 0;
        if (AMediaExtractor_selectTrack(extractor_.ex, size_t(video_.track)) != AMEDIA_OK) {
            error = "AMediaExtractor_selectTrack(视频) 失败";
            return false;
        }
        if (audio_.track >= 0 &&
            AMediaExtractor_selectTrack(extractor_.ex, size_t(audio_.track)) != AMEDIA_OK) {
            // 选不上就退回「只看画面」，比整条路打不开好。`info_.hasAudio` 不动
            // —— 它报的是容器里的存在性（同 win32/ffmpeg 口径），不是「能不能出声」。
            discardAudio();
        }
        return true;
    }

    void readContainerInfo(AMediaFormat* tf) {
        int32_t w = 0;
        int32_t h = 0;
        if (getInt(tf, AMEDIAFORMAT_KEY_WIDTH, w))
            info_.width = w;
        if (getInt(tf, AMEDIAFORMAT_KEY_HEIGHT, h))
            info_.height = h;
        int64_t durUs = 0;
        if (getInt64(tf, AMEDIAFORMAT_KEY_DURATION, durUs))
            info_.duration = std::chrono::microseconds(durUs);
        float fps = 0.0f;
        if (getFloat(tf, AMEDIAFORMAT_KEY_FRAME_RATE, fps))
            info_.frameRate = fps;
        // `AMEDIAFORMAT_KEY_ROTATION` 这个**符号**是 API 28 才有的（下限抬到 28 后
        // 能直接引用），但它的字符串值（Java 侧 KEY_ROTATION = "rotation-degrees"）
        // 从 API 21 就写在容器里。这里继续按字面量读：少一个运行期符号依赖，
        // 读到读不到与用符号完全等价。
        int32_t rotation = 0;
        if (getInt(tf, "rotation-degrees", rotation))
            info_.rotation = ((rotation % 360) + 360) % 360;
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = video_.mime;
        width_ = info_.width;
        height_ = info_.height;
    }

    void readAudioInfo(AMediaFormat* tf) {
        int32_t v = 0;
        if (getInt(tf, AMEDIAFORMAT_KEY_SAMPLE_RATE, v) && v > 0)
            audioFormat_.sampleRate = v;
        if (getInt(tf, AMEDIAFORMAT_KEY_CHANNEL_COUNT, v) && v > 0)
            audioFormat_.channels = v;
        audioFormat_.format = AudioSampleFormat::S16Interleaved;
    }

    /// 建 + configure 一台 codec。configure 用**容器轨的原始 format**（官方推荐路径：
    /// 解码器需要的 CSD/尺寸都在里面），不再自己拼。
    bool openStream(Stream& s, std::string& error) {
        s.codec.codec = AMediaCodec_createDecoderByType(s.mime.c_str());
        if (!s.codec.codec) {
            error = "这台设备没有 " + s.mime + " 的解码器";
            return false;
        }
        FormatHolder tf(AMediaExtractor_getTrackFormat(extractor_.ex, size_t(s.track)));
        if (!tf.get()) {
            error = "AMediaExtractor_getTrackFormat 失败";
            return false;
        }
        if (s.track == audio_.track) {
            // 尽量把输出钉成 S16：多数设备本来就是这个，写上去省得拿到厂商私有格式。
            // 「pcm-encoding」= Java KEY_PCM_ENCODING，API 21 起可用。
            AMediaFormat_setInt32(tf.get(), "pcm-encoding", kPcmEncoding16Bit);
        }
        const media_status_t st = AMediaCodec_configure(s.codec.codec, tf.get(),
                                                        /*surface=*/nullptr,
                                                        /*crypto=*/nullptr, /*flags=*/0);
        if (st != AMEDIA_OK) {
            error = statusText("AMediaCodec_configure 失败", st);
            return false;
        }
        return true;
    }

    bool restartStream(Stream& s) {
        if (AMediaCodec_flush(s.codec.codec) != AMEDIA_OK)
            return false;
        if (AMediaCodec_start(s.codec.codec) != AMEDIA_OK)
            return false;
        s.reset();
        s.eosQueued = false;
        flushAudio();
        return true;
    }

    /// 关掉音频这一路（codec 起不来 / 重排失败 / 设备吐异常），画面继续。
    /// 不能写成 `audio_ = Stream{}`：`Stream` 里装着不可拷贝的 RAII 句柄，那样既
    /// 编不过，也会把已建好的 codec「换掉」而不是关掉（重复 delete）。
    /// `info_.hasAudio` 故意不动：它报的是容器里有音轨（对齐 win32/ffmpeg）。
    void discardAudio() {
        audio_.codec.reset();
        audio_.track = -1;
        audio_.mime.clear();
        audio_.reset();
        audio_.eosQueued = false;
        audioFormat_ = AudioFormat {};
        flushAudio();
    }

    // ── 输出侧几何 ──────────────────────────────────────────────────────────

    /// stride / slice-height / color format 只有 start 之后（或
    /// INFO_OUTPUT_FORMAT_CHANGED 之后）才可信，像素布局全从这里取。
    void readVideoGeometry() {
        FormatHolder of(AMediaCodec_getOutputFormat(video_.codec.codec));
        if (!of.get())
            return;
        int32_t v = 0;
        if (getInt(of.get(), AMEDIAFORMAT_KEY_COLOR_FORMAT, v))
            colorFormat_ = v;
        if (getInt(of.get(), AMEDIAFORMAT_KEY_STRIDE, v) && v > 0)
            lumaStride_ = v;
        // 同 `rotation-degrees`：`AMEDIAFORMAT_KEY_SLICE_HEIGHT` 这个**符号**是 API 28
        // 才标注可用的（现在下限就是 28，引用它已经合法），而字符串值 "slice-height"
        // 从 API 21 就在容器/输出格式里。仍按字面量读：少一个运行期符号依赖。
        // （读不到就不记行距，下面会回退到 height。）
        if (getInt(of.get(), "slice-height", v) && v > 0)
            sliceHeight_ = v;
        if (getInt(of.get(), AMEDIAFORMAT_KEY_WIDTH, v) && v > 0)
            width_ = v;
        if (getInt(of.get(), AMEDIAFORMAT_KEY_HEIGHT, v) && v > 0)
            height_ = v;
        if (lumaStride_ <= 0)
            lumaStride_ = width_;
        if (sliceHeight_ <= 0)
            sliceHeight_ = height_;
    }

    /// 音频输出侧才是采样的真格式（rate / channels / pcm-encoding）。
    void readAudioGeometry() {
        FormatHolder of(AMediaCodec_getOutputFormat(audio_.codec.codec));
        if (!of.get())
            return;
        int32_t v = 0;
        if (getInt(of.get(), AMEDIAFORMAT_KEY_SAMPLE_RATE, v) && v > 0)
            audioFormat_.sampleRate = v;
        if (getInt(of.get(), AMEDIAFORMAT_KEY_CHANNEL_COUNT, v) && v > 0)
            audioFormat_.channels = v;
        if (getInt(of.get(), "pcm-encoding", v)) {
            if (v == kPcmEncodingFloat)
                audioFormat_.format = AudioSampleFormat::F32Interleaved;
            else if (v == kPcmEncoding16Bit)
                audioFormat_.format = AudioSampleFormat::S16Interleaved;
            else
                audioFormat_ = {};  // 厂商私有编码：这一路不出声，画面继续
        }
    }

    // ── 泵 ──────────────────────────────────────────────────────────────────

    /// 一轮：收两台 codec 的输出 → 交出各自 pending 的 sample → 从 extractor 补一个。
    /// 返回 false 只在 extractor 真出错时发生。
    bool pump() {
        if (!drainVideoOutput())
            return false;
        if (audio_.codec.codec)
            drainAudioOutput();
        if (!deliver(video_))
            return false;
        if (audio_.codec.codec && !deliver(audio_))
            return false;
        return fetchSample();
    }

    /// 有空 input buffer 就把 `s` 的 pending 交出去；没有就原样留着下一轮。
    bool deliver(Stream& s) {
        if (s.eosQueued)
            return true;
        if (!s.hasPending)
            return true;
        const ssize_t idx = AMediaCodec_dequeueInputBuffer(s.codec.codec, kInputTimeoutUs);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER || idx < 0)
            return true;  // 输入满：等输出侧消化，下一轮再交。

        if (s.pendingEos) {
            const media_status_t st = AMediaCodec_queueInputBuffer(
                    s.codec.codec, size_t(idx), off_t(0), 0, 0,
                    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            if (st != AMEDIA_OK) {
                error_ = statusText("排队 EOS 失败", st);
                return false;
            }
            s.eosQueued = true;
            s.reset();
            return true;
        }

        size_t cap = 0;
        uint8_t* in = AMediaCodec_getInputBuffer(s.codec.codec, size_t(idx), &cap);
        if (in == nullptr) {
            error_ = "AMediaCodec_getInputBuffer 返回空";
            return false;
        }
        if (s.pendingBytes.size() > cap) {
            // 交不出去：input buffer 比 sample 小。截断会解出坏帧，所以这在此路是
            // 真故障 —— 视频侧放弃整条（退 FFmpeg），音频侧只关声音。
            const std::string detail = "input buffer 太小（" + std::to_string(s.pendingBytes.size()) +
                                       " > " + std::to_string(cap) + "）";
            if (&s == &video_) {
                error_ = detail;
                return false;
            }
            discardAudio();
            return true;
        }
        std::memcpy(in, s.pendingBytes.data(), s.pendingBytes.size());
        const media_status_t st = AMediaCodec_queueInputBuffer(
                s.codec.codec, size_t(idx), off_t(0), s.pendingBytes.size(),
                uint64_t(s.pendingTimeUs), s.pendingFlags);
        if (st != AMEDIA_OK) {
            error_ = statusText("AMediaCodec_queueInputBuffer 失败", st);
            return false;
        }
        s.reset();
        return true;
    }

    /// 两条流都没有 pending 时，从 extractor 取下一个 sample 并按轨归属派发。
    bool fetchSample() {
        if (video_.hasPending || (audio_.codec.codec && audio_.hasPending))
            return true;
        if (video_.eosQueued && (!audio_.codec.codec || audio_.eosQueued))
            return true;

        const int64_t time = AMediaExtractor_getSampleTime(extractor_.ex);
        if (time < 0) {
            // 没有更多 sample：给还没 EOS 的每条流排一个空 EOS buffer。
            if (!video_.eosQueued) {
                video_.pendingEos = true;
                video_.hasPending = true;
            }
            if (audio_.codec.codec && !audio_.eosQueued) {
                audio_.pendingEos = true;
                audio_.hasPending = true;
            }
            return true;
        }
        const int track = AMediaExtractor_getSampleTrackIndex(extractor_.ex);
        Stream* target = &video_;
        if (audio_.codec.codec && track == audio_.track)
            target = &audio_;

        size_t want = 32u * 1024u;
        for (;;) {
            target->pendingBytes.resize(want);
            const ssize_t n = AMediaExtractor_readSampleData(extractor_.ex,
                                                             target->pendingBytes.data(), want);
            if (n < 0) {
                error_ = "AMediaExtractor_readSampleData 失败";
                return false;
            }
            if (size_t(n) < want) {
                target->pendingBytes.resize(size_t(n));
                target->pendingTimeUs = time;
                target->pendingFlags = AMediaExtractor_getSampleFlags(extractor_.ex);
                target->pendingEos = false;
                target->hasPending = true;
                // readSampleData 不推进游标，必须显式 advance，否则会一直拿到同一个
                // sample（表现是画面冻在第一帧）。
                AMediaExtractor_advance(extractor_.ex);
                return true;
            }
            // 正好填满 ⇒ 可能还有后续：扩容再读。不做这一步就是静默截断。
            want *= 2;
        }
    }

    bool drainVideoOutput() {
        AMediaCodecBufferInfo info {};
        const ssize_t idx =
                AMediaCodec_dequeueOutputBuffer(video_.codec.codec, &info, kOutputTimeoutUs);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            readVideoGeometry();
            return true;
        }
        if (idx < 0) {
            error_ = "视频 dequeueOutputBuffer 返回 " +
                     std::to_string(static_cast<long>(idx));
            return false;
        }
        const bool last = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        if (info.size > 0 && !frameReady_)
            renderVideo(size_t(idx), info);
        AMediaCodec_releaseOutputBuffer(video_.codec.codec, size_t(idx), /*render=*/false);
        if (fatalError_) {
            // 「不支持的像素格式」这类结构性问题：整条路放弃，让引擎退到 FFmpeg。
            // 单帧转换/越界不在此列 —— 下一帧照样试。
            return false;
        }
        if (last)
            eof_ = true;
        return true;
    }

    void drainAudioOutput() {
        AMediaCodecBufferInfo info {};
        const ssize_t idx =
                AMediaCodec_dequeueOutputBuffer(audio_.codec.codec, &info, kOutputTimeoutUs);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
            return;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            readAudioGeometry();
            return;
        }
        if (idx < 0) {
            discardAudio();  // 音频解码器坏了：只关声音
            return;
        }
        const bool last = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        if (info.size > 0 && audioFormat_.isValid() && !audioBacklogFull() && !last) {
            size_t bufSize = 0;
            uint8_t* base = AMediaCodec_getOutputBuffer(audio_.codec.codec, size_t(idx), &bufSize);
            const size_t begin = size_t(std::max<int32_t>(0, info.offset));
            if (base != nullptr && begin + size_t(info.size) <= bufSize) {
                if (pcmPos_ == pcmBuf_.size())
                    pcmPtsUs_ = info.presentationTimeUs;
                const size_t old = pcmBuf_.size();
                pcmBuf_.resize(old + size_t(info.size));
                std::memcpy(pcmBuf_.data() + old, base + begin, size_t(info.size));
            }
        }
        AMediaCodec_releaseOutputBuffer(audio_.codec.codec, size_t(idx), false);
    }

    bool audioBacklogFull() const {
        if (!audioFormat_.isValid())
            return true;
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        const int64_t queued =
                (int64_t(pcmBuf_.size()) - int64_t(pcmPos_)) / std::max<int64_t>(1, frameBytes);
        return queued >= int64_t(audioFormat_.sampleRate) * kPcmBacklogSeconds;
    }

    /// 把 codec 输出缓冲里的 YUV 转成 BGRA 位图；成功时填 `readyFrame_`。
    /// 失败分两级（见 `drainVideoOutput`）：结构性问题置 `fatalError_` 放弃整条路，
    /// 单帧问题只报错、下一帧照常尝试。
    bool renderVideo(size_t idx, const AMediaCodecBufferInfo& info) {
        auto fatal = [&](std::string message) {
            error_ = std::move(message);
            fatalError_ = true;
            return false;
        };
        size_t bufSize = 0;
        uint8_t* base = AMediaCodec_getOutputBuffer(video_.codec.codec, idx, &bufSize);
        if (base == nullptr)
            return fatal("AMediaCodec_getOutputBuffer 返回空");
        const size_t begin = size_t(std::max<int32_t>(0, info.offset));
        if (begin + size_t(std::max<int32_t>(0, info.size)) > bufSize) {
            error_ = "视频输出缓冲越界";
            return false;
        }
        const uint8_t* p = base + begin;

        const int w = width_ > 0 ? width_ : info_.width;
        const int h = height_ > 0 ? height_ : info_.height;
        if (w <= 0 || h <= 0) {
            error_ = "还没有拿到输出尺寸";
            return false;
        }
        const int32_t lumaStride = lumaStride_ > 0 ? lumaStride_ : w;
        const int32_t slice = sliceHeight_ > 0 ? sliceHeight_ : h;
        const bool semi = colorFormat_ == kColorYuv420SemiPlanar ||
                          colorFormat_ == kColorYuv420PackedSemiPlanar;
        // 色度行距两种布局不同算法：semi-planar 一行放 UV 交织（每 2 个 luma 像素占
        // 2 字节 ⇒ 与 luma 行等宽），planar 一行只放 U 或 V（半宽）。
        const int32_t chromaStride = semi ? lumaStride : lumaStride / 2;
        const int32_t cRows = slice / 2;
        const size_t need = size_t(lumaStride) * slice + size_t(chromaStride) * cRows * (semi ? 1 : 2);
        if (need > size_t(info.size)) {
            error_ = "输出帧太小（需要 " + std::to_string(need) + " 字节，缓冲只有 " +
                     std::to_string(info.size) + "），color format=" + hexText(colorFormat_);
            return false;
        }

        const int32_t dstStride = w * 4;
        sk_sp<SkData> pixels = SkData::MakeUninitialized(size_t(dstStride) * size_t(h));
        // 本机 Skia 版本的 `writable_data()` 返 `void*`，得显式转。
        uint8_t* dst = static_cast<uint8_t*>(pixels->writable_data());
        const uint8_t* luma = p;
        const uint8_t* chroma = p + size_t(lumaStride) * slice;
        switch (colorFormat_) {
            case kColorYuv420Planar:
                convertYuv(luma, lumaStride, chroma, chroma + size_t(chromaStride) * cRows,
                           chromaStride, /*semiPlanar=*/false, /*vFirst=*/false, w, h, dst,
                           dstStride);
                break;
            case kColorYuv420SemiPlanar:
                convertYuv(luma, lumaStride, chroma, nullptr, chromaStride,
                           /*semiPlanar=*/true, /*vFirst=*/false, w, h, dst, dstStride);
                break;
            case kColorYuv420PackedSemiPlanar:
                convertYuv(luma, lumaStride, chroma, nullptr, chromaStride,
                           /*semiPlanar=*/true, /*vFirst=*/true, w, h, dst, dstStride);
                break;
            default:
                return fatal("不支持的像素格式 " + hexText(colorFormat_) + "（这一路交回 FFmpeg）");
        }

        const SkImageInfo imageInfo =
                SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
        readyFrame_.image = SkImages::RasterFromData(imageInfo, std::move(pixels), dstStride);
        if (!readyFrame_.image) {
            error_ = "SkImages::RasterFromData 失败";
            return false;
        }
        readyFrame_.ptsUs = info.presentationTimeUs;
        frameReady_ = true;
        return true;
    }

#if defined(ANDROID) && __ANDROID_API__ >= 28
    /// `AMediaDataSource` 的三个回调（签名照 `media/NdkMediaDataSource.h`）。
    /// `readAt` 本身就是**随机读**（偏移由框架给），所以内存来源不需要自己记游标，
    /// 也就没有旧写法里那个 `seekMemory` —— 当时假想的
    /// `AMediaDataSource_createFromCallbacks(userdata, read, seek, close)` 在 NDK 里
    /// 从来不存在（按 API 24 编时这一段根本不参与编译，所以一直没暴露）。
    static ssize_t readMemory(void* self, off64_t offset, void* data, size_t size) {
        const auto* backend = static_cast<MediaCodecBackend*>(self);
        const off64_t total = off64_t(backend->sourceBytes_.size());
        if (offset < 0)
            return -1;
        if (offset >= total)
            return 0;  // 读到尾：框架按 0 字节判 EOF
        // `offset < total` 上面已保证，这里直接按无符号算剩余，免得混符号比较。
        const size_t left = size_t(total) - size_t(offset);
        const size_t n = std::min(left, size);
        std::memcpy(data, backend->sourceBytes_.data() + size_t(offset), n);
        return ssize_t(n);
    }
    static ssize_t sizeMemory(void* self) {
        return ssize_t(static_cast<MediaCodecBackend*>(self)->sourceBytes_.size());
    }
    static void closeMemory(void*) {}
#endif

    /// datasource 声明在 `extractor_` **之前** ⇒ 析构时排在它之后（成员按声明逆序
    /// 销毁）。头文件没说 `setDataSourceCustom` 之后数据源归谁，所以这份跟着后端
    /// 活到 extractor 关掉之后再删 —— 两种所有权口径下都不会 use-after-free。
    DataSourceHolder dataSource_;
    ExtractorHolder extractor_;
    Stream video_;
    Stream audio_;
    VideoInfo info_;
    std::string error_;

    VideoFrame readyFrame_;
    bool frameReady_ = false;
    bool eof_ = false;
    /// 视频侧的结构性故障（格式不认识 / 缓冲拿不到）：readFrame 因此直接终止。
    bool fatalError_ = false;

    // 输出侧几何（readVideoGeometry 填）。
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t lumaStride_ = 0;
    int32_t sliceHeight_ = 0;
    int32_t colorFormat_ = 0;

    // 音频：S16 或 F32 交织的原始字节，readAudio 从这里切片交给 AudioSink。
    AudioFormat audioFormat_;
    std::vector<uint8_t> pcmBuf_;
    std::vector<uint8_t> pcmView_;
    size_t pcmPos_ = 0;
    int64_t pcmPtsUs_ = 0;

#if defined(ANDROID) && __ANDROID_API__ >= 28
    std::vector<uint8_t> sourceBytes_;
#endif
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> createMediaCodecBackend(const VideoSource& source,
                                                             std::string& error) {
    auto backend = std::make_unique<MediaCodecBackend>();
    if (!backend->open(source, error))
        return nullptr;
    return backend;
}

/// 运行期探测：`libmediandk.so` 是链接期依赖（进程一起来就加载好了），API 21 之后
/// 系统必带，所以「编进来了但这台机器没有 AMediaCodec」不成立。真正的不确定性在
/// **具体解码器**（某机型可能没有 HEVC 硬解），那只能在
/// `AMediaCodec_createDecoderByType` 里见分晓 —— 不在这里预测。
bool mediaCodecVideoAvailable(std::string& note) {
    static const bool ok = dlsym(RTLD_DEFAULT, "AMediaCodec_createDecoderByType") != nullptr;
    if (!ok)
        note = "libmediandk 里没有 AMediaCodec 符号";
    return ok;
}

}  // namespace cui_core

#endif  // CUI_HAVE_MEDIACODEC
