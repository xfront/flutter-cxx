/// @file video/video_decoder_gst.cpp
/// @brief Linux 系统媒体栈解码后端（GStreamer：uridecodebin/decodebin + appsink）。
///
/// 路线一（各端系统 API）在 Linux 上的落点，角色与 Windows 的 Media Foundation、
/// Android 的 AMediaCodec 对齐：**解复用 + 解码交给操作系统的媒体框架**，本仓只把
/// 解出来的像素转成 BGRA、把 PCM 转交给 `AudioSink`（`audio_sink_alsa.cpp`）。
///
/// 关键实现口径：
///   - 两条分支都收在 **appsink 拉模式**（`sync=false` / `async=false` /
///     `emit-signals=false`）。不让 GStreamer 出声（管线里没有任何 audio-sink 元件），
///     PCM 走 `AudioChunk` → 本仓 `AudioSink` ⇒ 主时钟仍是 `AudioRing` 的「已消耗帧
///     / 采样率」（见 audio_ring.h），与 FFmpeg／Android 后端同一条同步链路。让它
///     自己出声就拿不到「播到第几微秒」了，暂停/倍速下的 position 语义反而更绕。
///   - `async=false` **是必须的**：音频分支在「这个文件没有音轨」时永远收不到数据，
///     默认（async=true）它会把流水线吊在 ASYNC_PENDING，而 PAUSED→PLAYING 要等
///     异步协商完成 ⇒ 流线程根本不跑，我们等第一帧只能靠超时退出。
///   - 不用 sample 信号：引擎的线程模型是「`readFrame` 阻塞到下一帧」，与 appsink
///     的 pull 语义天然对齐；信号模式要在 GMainContext 上派发，本仓没有（也不打算
///     只为这一个后端）跑 GMainLoop。
///   - **pts 要从 running-time 换算回媒体时间**（`sampleMediaUs`）：元件之间传的是
///     running-time，样本自带的 segment 才是它到流内时间的映射。不换算的话，seek
///     之后 pts 会连着「真实时间已经过了多久」一起跳，音画同步当场坏掉。
///     拿不到可信换算时退成「上一帧 + 这一帧时长」的累加值（可能差一个 GOP，但单调，
///     不会让引擎的节拍器疯掉）。
///   - 音频格式在 `pad-added` 里读裸 pad 的 `rate`/`channels`（`audioconvert` 不改
///     这两样），capsfilter 只钉 `format=F32LE` ⇒ `audioFormat()` 在打开阶段就能给定
///     值，ALSA 设备因此能在第一帧之前就 open（引擎 `bindBackendLocked` 的时序要求）。
///   - 视频像素：capsfilter 钉 `format=BGRx`（内存序 B,G,R,x，与 Skia 的
///     `kBGRA_8888` 同序；GStreamer 对那第 4 个字节的内容不作承诺），搬到像素缓冲时
///     逐像素或上 `0xFF000000` —— 与 MF／Android 同口径。行距取 `GstVideoMeta`，
///     不假设紧排。
///   - seek：`gst_element_seek(..., GST_SEEK_FLAG_FLUSH)` ⇒ 各 pad 上排队的旧数据被
///     flush 掉，appsink 队列跟着清；再按 `dropBeforeUs_` 丢弃落在目标之前的关键帧
///     序列（与 FFmpeg 后端同一条）。
///   - 内存来源（`VideoSource::memory()`）用 `appsrc`（`GST_FORMAT_BYTES` +
///     `SEEKABLE`）喂 `decodebin`：`need-data` 从我们自己持有的字节里按块拷出去
///     （绝不让 GStreamer 指向 `std::vector` 的内存 —— 它会跨 realloc 悬掉），
///     `seek-data` 移游标。
///   - 背压：PCM 自持队列超过 2 秒就不再从音频 appsink 拉，让解码分支自己堵着
///     （堵 = 解慢一点，不丢数据；比 Android 那侧「丢输出」更省一次重解）。
///
/// 有意未覆盖（本机没有 gst 开发包 ⇒ 整个文件是「按公开 API 写、没在真机编过」，
/// 下面几条更是明确不做）：
///   - **容器旋转**：qtdemux/matroskademux 把旋转矩阵随流以 `GstVideoOrientationMeta`
///     / tag 的形式送下来，取它要在 pad 上加探针，而各 demuxer 的投递口径并不一致 ⇒
///     这里恒报 `info().rotation = 0`。竖拍视频在本端会横着显示（FFmpeg 后端会给）。
///   - 倍速：不把 rate 交给 GStreamer（`gst_element_seek` 带倍速那条路），倍速仍由
///     引擎的墙钟节拍决定 —— 与线程模型下其它后端一致。
///   - 多音轨/多字幕切换、live source（`duration` 未知 ⇒ 进度条不可用）、gapless、
///     DRM（要 `playbin` + `GstURIHandler` 那套）、`shrinkWrap`/缩放（不接 videoscale，
///     帧一律按编码尺寸交出，缩放交给控件层）。
///   - `GstVideoInfo` / `GstVideoFrame` / `GstVideoConverter` 一概不用，只要
///     `GstVideoMeta` 的偏移与行距 —— 少一个随版本变的面积。

#include "cui/video/video_decoder.h"

#if defined(CUI_HAVE_GSTREAMER)

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

namespace cui {
namespace {

constexpr int64_t kUsPerSecond = 1000000;

/// `readFrame` 的总容忍时长：拉不到帧就分片重试，超这个时长才认「这一路卡住了」。
/// 引擎的工作线程只在 `readFrame` 返回后才看 `stop_`，所以不能无限阻塞。
constexpr int64_t kFrameStallMs = 15000;
/// 单片等待（µs 级足够，这里按 ns 给 appsink）。分片是为了能在中途查总线错误/EOS。
constexpr guint64 kPullSliceNs = 200 * guint64(1000000);
/// 第一帧的等待上限（typefind + 建解码器 + 首帧，慢机器/网络流要留够）。
constexpr guint64 kFirstFrameNs = 5 * guint64(1000000000);
/// 等视频 pad 挂上（= 发现视频流）的上限。
constexpr int64_t kPadWaitMs = 5000;
/// 第一帧之后再看音频 pad：解码器建链通常早于首帧，这里只是给一点余量。
constexpr int64_t kAudioWaitMs = 200;

constexpr guint kVideoMaxBuffers = 3;
constexpr guint kAudioMaxBuffers = 8;
/// 单次交给 sink 的采样帧数（≈42ms @48k，与 Android 后端同口径）。
constexpr size_t kPcmChunkFrames = 2048;
/// PCM 自持队列的高水位（微秒）；超了就停止拉音频 appsink（见文件头「背压」）。
constexpr int64_t kPcmBacklogUs = 2 * kUsPerSecond;
/// appsrc 每次 `need-data` 交出的字节数。
constexpr size_t kAppBlockBytes = 256 * 1024;

int64_t nsToUs(gint64 ns) { return ns < 0 ? -1 : int64_t(ns / 1000); }

/// 微秒 → 纳秒（GStreamer 的时间基是 ns）。夹在 int64 上限，免得 seek 值溢出成负数。
gint64 usToNs(int64_t us) {
    if (us <= 0)
        return 0;
    constexpr int64_t kMaxUs = std::numeric_limits<int64_t>::max() / 1000;
    return gint64(us > kMaxUs ? kMaxUs : us) * 1000;
}

bool startsWith(const char* text, const char* prefix) {
    return text != nullptr && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

/// GStreamer 的初始化（进程内一次）。`gst_init_check` 不吃调用方的 argc/argv，
/// 也不会装 main 的参数 —— 本仓不去动宿主程序的命令行，插件全凭环境变量与
/// 系统注册表找。失败原因留一份给探测与打开两处复用。
struct GstInit {
    bool ok = false;
    std::string note;
};

const GstInit& gstInit() {
    static const GstInit init = [] {
        GstInit out;
        GError* gerr = nullptr;
        out.ok = gst_init_check(nullptr, nullptr, &gerr) == TRUE;
        if (gerr != nullptr) {
            if (gerr->message != nullptr)
                out.note = gerr->message;
            g_error_free(gerr);
        }
        return out;
    }();
    return init;
}

/// caps 的第一个结构（我们只关心媒体类型名与几个固定字段，不处理多结构的范围 caps）。
const GstStructure* firstStructure(const GstCaps* caps) {
    if (caps == nullptr || gst_caps_get_size(caps) < 1)
        return nullptr;
    return gst_caps_get_structure(caps, 0);
}

bool capsInt(const GstStructure* structure, const char* field, int& out) {
    return structure != nullptr && gst_structure_get_int(structure, field, &out);
}

/// 这条 caps 是不是我们要的音频布局（`audioconvert` 之后被 capsfilter 钉死的 F32LE
/// 交织）。不是 ⇒ 这段 PCM 一律不接（宁可这段没声，也不猜布局）。
bool capsIsF32Interleaved(const GstCaps* caps) {
    const GstStructure* structure = firstStructure(caps);
    if (structure == nullptr || !startsWith(gst_structure_get_name(structure), "audio/"))
        return false;
    const gchar* format = gst_structure_get_string(structure, "format");
    return format != nullptr && std::strcmp(format, "F32LE") == 0;
}

/// 来源 → 给 `uridecodebin` 的 URI。裸路径转 `file://`：`g_filename_to_uri` 只吃
/// 绝对路径（先 realpath），而它顺带把空格/`#`/非 ASCII 按规范百分号编码 —— 手拼
/// 字符串在带空格的路径上必坏。
std::string uriForSource(const VideoSource& source) {
    if (source.uri.empty())
        return {};
    if (source.uri.find("://") != std::string::npos)
        return source.uri;
    char resolved[PATH_MAX];
    if (::realpath(source.uri.c_str(), resolved) == nullptr)
        return {};
    gchar* uri = g_filename_to_uri(resolved, nullptr, nullptr);
    if (uri == nullptr)
        return {};
    std::string out = uri;
    g_free(uri);
    return out;
}

/// 样本 → 媒体时间（微秒）。返回 -1 = 这条样本给不出可信时间，调用方走累加兜底。
/// `durationUs` 顺带给出这一帧（这一段）的时长，兜底要用。
int64_t sampleMediaUs(GstSample* sample, int64_t& durationUs) {
    durationUs = 0;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer == nullptr)
        return -1;
    const gint64 duration = gint64(GST_BUFFER_DURATION(buffer));
    if (duration > 0)
        durationUs = nsToUs(duration);
    const gint64 pts = gint64(GST_BUFFER_PTS(buffer));
    GstSegment* segment = gst_sample_get_segment(sample);
    if (segment != nullptr && pts >= 0) {
        const gint64 stream = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, pts);
        if (stream >= 0)
            return nsToUs(stream);
    }
    return -1;
}

/// GStreamer 后端。
///
/// 线程：`readFrame`/`readAudio`/`seekTo`/`flushAudio`/析构都只在播放引擎的工作
/// 线程；`pad-added` 与 appsrc 的 `need-data`/`seek-data` 在 GStreamer 自己的流
/// 线程。**纪律：绝不持 `padMutex_`/`srcMutex_` 跨任何 GStreamer 调用**（pad-added
/// 就是在状态变更/流线程里发的，反向拿锁会死锁）。
class GStreamerBackend final : public VideoDecoderBackend {
public:
    GStreamerBackend() = default;
    ~GStreamerBackend() override { teardown(); }
    GStreamerBackend(const GStreamerBackend&) = delete;
    GStreamerBackend& operator=(const GStreamerBackend&) = delete;

    bool open(const VideoSource& source, std::string& error) {
        const GstInit& init = gstInit();
        if (!init.ok) {
            error = init.note.empty() ? "GStreamer 起不来（插件注册表读不到）" : init.note;
            return false;
        }
        if (!buildPipeline(source, error))
            return false;

        info_ = VideoInfo{};
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = "gstreamer";
        info_.rotation = 0;  // 见文件头「有意未覆盖：容器旋转」

        // 起流水线：不指望它「同步完成」（async=false ⇒ 流水线直接往 PLAYING 走），
        // 第一帧到手才是「typefind 完成 + 真的有视频流」的可靠信号。
        const GstStateChangeReturn change = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (change == GST_STATE_CHANGE_FAILURE) {
            checkBus();
            error = error_.empty() ? "GStreamer 无法进入 PLAYING" : error_;
            return false;
        }

        std::chrono::milliseconds padWait(kPadWaitMs), audioWait(kAudioWaitMs);
        {
            std::unique_lock<std::mutex> lock(padMutex_);
            padCv_.wait_for(lock, padWait, [this] { return videoLinked_; });
        }
        if (!videoLinked_) {
            checkBus();
            error = error_.empty()
                            ? "GStreamer 没有发现可解码的视频流（缺对应的解码器插件？）"
                            : error_;
            return false;
        }

        // 第一帧：只为拿到尺寸/帧率，样本本身留在缓存里交给第一次 `readFrame`
        //（别白扔一帧 —— 引擎紧接着会 seek 到 0，那一帧本来还要再解一遍）。
        {
            // `gst_app_sink_try_pull_sample` 没有 GError 出参：拿不到帧只有两种原因
            //（超时无数据 / 流已结束），错误一律走总线，见 `checkBus()`。
            GstSample* first = gst_app_sink_try_pull_sample(GST_APP_SINK(vsink_), kFirstFrameNs);
            if (first == nullptr) {
                checkBus();
                error = error_.empty() ? "GStreamer 在 5 秒内没有解出第一帧" : error_;
                return false;
            }
            const GstCaps* caps = gst_sample_get_caps(first);
            const GstStructure* structure = firstStructure(caps);
            int width = 0;
            int height = 0;
            capsInt(structure, "width", width);
            capsInt(structure, "height", height);
            GstBuffer* buffer = gst_sample_get_buffer(first);
            const GstVideoMeta* meta = buffer != nullptr ? gst_buffer_get_video_meta(buffer) : nullptr;
            if (meta != nullptr && meta->width > 0) {
                width = int(meta->width);
                height = int(meta->height);
            }
            if (width <= 0 || height <= 0) {
                gst_sample_unref(first);
                error = "GStreamer 交出来的第一帧没有尺寸";
                return false;
            }
            int num = 0;
            int den = 0;
            if (structure != nullptr && gst_structure_get_fraction(structure, "framerate", &num,
                                                                   &den) &&
                den > 0)
                info_.frameRate = double(num) / double(den);
            info_.width = width;
            info_.height = height;
            cachedVideo_ = first;
        }

        // 有没有音轨：pad-added 之后才有结论，再给它一点余量（见上）。
        int rate = 0;
        int channels = 0;
        {
            std::unique_lock<std::mutex> lock(padMutex_);
            padCv_.wait_for(lock, audioWait, [this] { return audioLinked_; });
            if (audioLinked_) {
                rate = audioRate_;
                channels = audioChannels_;
            }
        }
        audioFormat_ = AudioFormat{rate, channels, AudioSampleFormat::F32Interleaved};
        audioOut_ = audioLinked_ && audioFormat_.isValid();
        info_.hasAudio = audioOut_;
        // 挂上了但读不出 rate/channels（异常 caps）⇒ 只关声音，分支留着不动：
        // 它的队列顶多堵死自己的解码线程，不影响视频那一路。

        gint64 durationNs = 0;
        if (gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &durationNs) &&
            durationNs > 0)
            info_.duration = std::chrono::microseconds(nsToUs(durationNs));

        lastPtsUs_ = 0;
        dropBeforeUs_ = 0;
        lastSeekTargetUs_ = 0;
        pcmPtsUs_ = 0;
        error_.clear();
        return true;
    }

    const VideoInfo& info() const override { return info_; }
    bool atEnd() const override { return eof_; }
    const std::string& error() const override { return error_; }

    bool hasAudioStream() const override { return audioOut_; }
    AudioFormat audioFormat() const override { return audioFormat_; }

    bool readFrame(VideoFrame& out) override {
        if (vsink_ == nullptr || eof_)
            return false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kFrameStallMs);
        while (std::chrono::steady_clock::now() < deadline) {
            GstSample* sample = takeVideoSample();
            if (sample == nullptr) {
                checkBus();
                if (gst_app_sink_is_eos(GST_APP_SINK(vsink_))) {
                    eof_ = true;
                    return false;
                }
                if (!error_.empty())
                    return false;  // 不算结束：引擎会把这条报成解码错误
                continue;          // 只是这一片还没到
            }
            int64_t durationUs = 0;
            int64_t ptsUs = sampleMediaUs(sample, durationUs);
            if (ptsUs < 0)
                ptsUs = lastPtsUs_ + durationUs;  // 兜底：上一帧 + 这一帧时长
            lastPtsUs_ = ptsUs;
            if (ptsUs + 1 < dropBeforeUs_) {
                // seek 落在关键帧上：目标之前的那些继续解，但不呈现（对齐 FFmpeg 后端）。
                gst_sample_unref(sample);
                continue;
            }
            const bool ok = frameFromSample(sample, ptsUs, out);
            gst_sample_unref(sample);
            // 转不出来不是流尾：`eof_` 保持假 ⇒ 引擎把 `error_` 如实报成解码失败，
            // 而不是悄悄当成「播完了」（与 FFmpeg 后端那条的区别是有意为之）。
            return ok;
        }
        error_ = "GStreamer 在 " + std::to_string(kFrameStallMs / 1000) +
                 " 秒内没有交出新帧";
        return false;
    }

    bool readAudio(AudioChunk& out) override {
        if (!audioOut_ || asink_ == nullptr)
            return false;
        fillPcmQueue();
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        if (frameBytes <= 0 || pcmPos_ >= pcmBuf_.size())
            return false;
        const size_t avail = (pcmBuf_.size() - pcmPos_) / size_t(frameBytes);
        const size_t want = std::min<size_t>(avail, kPcmChunkFrames);
        const size_t bytes = want * size_t(frameBytes);
        // `AudioChunk` 的内存由后端持有，到下一次 readAudio/flushAudio 有效；
        // `AudioSink::push()` 立刻拷进环形缓冲，所以这个生命周期够用。
        pcmView_.assign(pcmBuf_.begin() + pcmPos_, pcmBuf_.begin() + pcmPos_ + bytes);
        pcmPos_ += bytes;
        if (pcmPos_ == pcmBuf_.size()) {
            pcmBuf_.clear();
            pcmPos_ = 0;
        }
        out.frames = want;
        out.ptsUs = pcmPtsUs_;
        out.format = AudioSampleFormat::F32Interleaved;
        out.f32 = reinterpret_cast<const float*>(pcmView_.data());
        pcmPtsUs_ += int64_t(want) * kUsPerSecond / int64_t(audioFormat_.sampleRate);
        return true;
    }

    /// seek 之后：未交出的 PCM 全丢，时间基准搬到目标（引擎随后 `sink->flush(target)`）。
    void flushAudio() override {
        pcmBuf_.clear();
        pcmPos_ = 0;
        pcmPtsUs_ = lastSeekTargetUs_;
    }

    bool seekTo(int64_t ptsUs) override {
        if (pipeline_ == nullptr)
            return false;
        clearCachedVideo();
        dropBeforeUs_ = std::max<int64_t>(0, ptsUs);
        lastSeekTargetUs_ = dropBeforeUs_;
        lastPtsUs_ = dropBeforeUs_;
        eof_ = false;
        error_.clear();
        const bool ok = gst_element_seek(pipeline_, 1.0, GST_FORMAT_TIME,
                                         GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                         GST_SEEK_TYPE_SET, usToNs(ptsUs), GST_SEEK_TYPE_NONE,
                                         GST_CLOCK_TIME_NONE);
        if (!ok) {
            checkBus();
            error_ = error_.empty() ? "GStreamer seek 失败" : error_;
            return false;
        }
        return true;
    }

private:
    // ── 建链 ────────────────────────────────────────────────────────────────

    bool buildPipeline(const VideoSource& source, std::string& error) {
        pipeline_ = gst_pipeline_new("cui-video");
        if (pipeline_ == nullptr) {
            error = "gst_pipeline_new 失败";
            return false;
        }
        // 总线自己持一份 ref（析构里 unref）：`gst_pipeline_get_bus` 给的是新增引用。
        bus_ = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));

        const bool useBytes = !source.bytes.empty();
        decoder_ = gst_element_factory_make(useBytes ? "decodebin" : "uridecodebin", "cui-dec");
        vconv_ = gst_element_factory_make("videoconvert", nullptr);
        vcaps_ = gst_element_factory_make("capsfilter", nullptr);
        vsink_ = gst_element_factory_make("appsink", nullptr);
        aconv_ = gst_element_factory_make("audioconvert", nullptr);
        acaps_ = gst_element_factory_make("capsfilter", nullptr);
        asink_ = gst_element_factory_make("appsink", nullptr);
        if (decoder_ == nullptr || vconv_ == nullptr || vcaps_ == nullptr || vsink_ == nullptr ||
            aconv_ == nullptr || acaps_ == nullptr || asink_ == nullptr) {
            error = "GStreamer 元件缺一个：videoconvert/audioconvert/appsink 都属于 "
                    "gst-plugins-base，uridecodebin/decodebin 属于 gst-core";
            return false;
        }

        GstCaps* vfix = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRx",
                                           nullptr);
        g_object_set(vcaps_, "caps", vfix, nullptr);
        gst_caps_unref(vfix);
        GstCaps* afix = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE",
                                           nullptr);
        g_object_set(acaps_, "caps", afix, nullptr);
        gst_caps_unref(afix);

        // `sync=false`：本仓自己按引擎的节拍取帧；`async=false`：见文件头（必须）；
        // `drop=false`：一帧都不许扔，宁可堵住（引擎要按 pts 对齐主时钟）。
        g_object_set(vsink_, "sync", FALSE, "async", FALSE, "emit-signals", FALSE, "drop", FALSE,
                     "max-buffers", kVideoMaxBuffers, nullptr);
        g_object_set(asink_, "sync", FALSE, "async", FALSE, "emit-signals", FALSE, "drop", FALSE,
                     "max-buffers", kAudioMaxBuffers, nullptr);

        if (useBytes) {
            appsrc_ = gst_element_factory_make("appsrc", nullptr);
            if (appsrc_ == nullptr) {
                error = "没有 appsrc 元件（gst-plugins-base）⇒ 内存来源不可用";
                return false;
            }
            sourceBytes_ = source.bytes;  // 游标指向我们自己的一份拷贝
            srcPos_ = 0;
            GstCaps* raw = gst_caps_new_empty_simple("application/octet-stream");
            g_object_set(appsrc_, "caps", raw, "format", GST_FORMAT_BYTES, "stream-type",
                         GST_APP_STREAM_TYPE_SEEKABLE, "size",
                         guint64(sourceBytes_.size()), nullptr);
            gst_caps_unref(raw);
        } else {
            const std::string uri = uriForSource(source);
            if (uri.empty()) {
                error = "无法把来源交给 uridecodebin（URI 不合法或路径不存在）";
                return false;
            }
            g_object_set(decoder_, "uri", uri.c_str(), nullptr);
        }

        GstElement* const parts[] = {appsrc_, decoder_, vconv_, vcaps_, vsink_, aconv_, acaps_,
                                     asink_};
        for (GstElement* part : parts) {
            if (part == nullptr)
                continue;
            if (gst_bin_add(GST_BIN(pipeline_), part) == FALSE) {
                error = "把元件加进流水线失败";
                gst_object_unref(part);  // 加失败的元件不归 bin 持有
                return false;
            }
        }
        if (gst_element_link_many(vconv_, vcaps_, vsink_, nullptr) == FALSE ||
            gst_element_link_many(aconv_, acaps_, asink_, nullptr) == FALSE) {
            error = "GStreamer 分支内部链接失败";
            return false;
        }
        if (appsrc_ != nullptr &&
            gst_element_link_pads(appsrc_, "src", decoder_, "sink") == FALSE) {
            error = "appsrc→decodebin 链接失败";
            return false;
        }
        // 动态 pad：decodebin/uridecodebin 在 typefind 之后按流发 pad-added。
        g_signal_connect(decoder_, "pad-added", G_CALLBACK(&GStreamerBackend::onPadAdded), this);
        if (appsrc_ != nullptr) {
            g_signal_connect(appsrc_, "need-data", G_CALLBACK(&GStreamerBackend::needData), this);
            g_signal_connect(appsrc_, "seek-data", G_CALLBACK(&GStreamerBackend::seekData), this);
        }
        return true;
    }

    void teardown() {
        if (pipeline_ != nullptr)
            // 先断流：之后不会再有 pad-added / need-data 打进来。
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (bus_ != nullptr) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        clearCachedVideo();
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        // 以下都是 bin 持有的借用量，随 pipeline 一起没了 ⇒ 只置空。
        decoder_ = vconv_ = vcaps_ = vsink_ = aconv_ = acaps_ = asink_ = appsrc_ = nullptr;
    }

    // ── pad / appsrc 回调（GStreamer 流线程） ──────────────────────────────

    /// `pad-added` 是 GstElement 上的信号，回调前两个参数固定是「发信号的元件 + 新 pad」，
    /// 用户数据在第三位 —— 少写一个参数就会把元件当成 pad（编译期看不出来，stub 头
    /// 只按函数指针类型收）。
    static void onPadAdded(GstElement* /*element*/, GstPad* pad, gpointer user) {
        static_cast<GStreamerBackend*>(user)->linkPad(pad);
    }

    void linkPad(GstPad* pad) {
        if (pad == nullptr)
            return;
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (caps == nullptr)
            caps = gst_pad_query_caps(pad, nullptr);
        if (caps == nullptr)
            return;
        const GstStructure* structure = firstStructure(caps);
        const char* media = structure != nullptr ? gst_structure_get_name(structure) : nullptr;
        bool linked = false;
        bool isVideo = false;
        int rate = 0;
        int channels = 0;
        if (startsWith(media, "video/") && !videoLinked_) {
            linked = linkIntoBranch(pad, vconv_);
            isVideo = linked;
        } else if (startsWith(media, "audio/") && !audioLinked_) {
            linked = linkIntoBranch(pad, aconv_);
            isVideo = false;
            if (linked) {
                capsInt(structure, "rate", rate);
                capsInt(structure, "channels", channels);
            }
        }
        // 其余（字幕/数据流）故意不接：没链接的 pad 上 decodebin 不起解码任务，
        // 正好是「这一路我们不用」的语义。
        gst_caps_unref(caps);
        if (!linked)
            return;
        {
            std::lock_guard<std::mutex> lock(padMutex_);
            if (isVideo)
                videoLinked_ = true;
            else {
                audioRate_ = rate;
                audioChannels_ = channels;
                audioLinked_ = true;
            }
        }
        padCv_.notify_all();
    }

    /// 把 decodebin 的动态 src pad 接到分支头（videoconvert / audioconvert 的 sink）。
    /// 在 pad-added 回调里直接 link 是 GStreamer 官方示例的写法（pad 还没激活）。
    bool linkIntoBranch(GstPad* pad, GstElement* branchHead) {
        if (branchHead == nullptr)
            return false;
        GstPad* sink = gst_element_get_static_pad(branchHead, "sink");
        if (sink == nullptr)
            return false;
        const GstPadLinkReturn ret = gst_pad_link(pad, sink);
        gst_object_unref(sink);
        return ret == GST_PAD_LINK_OK;
    }

    static void needData(GstAppSrc* src, guint /*length*/, gpointer user) {
        static_cast<GStreamerBackend*>(user)->feedAppSrc(src);
    }

    static gboolean seekData(GstAppSrc* /*src*/, guint64 position, gpointer user) {
        auto* self = static_cast<GStreamerBackend*>(user);
        std::lock_guard<std::mutex> lock(self->srcMutex_);
        if (position > self->sourceBytes_.size())
            return FALSE;  // 超出这份内存 ⇒ 交给 GStreamer 自己报错
        self->srcPos_ = size_t(position);
        return TRUE;
    }

    /// 从自有字节里按块拷一份交给 GStreamer。**不让它指向 vector 的内存**：
    /// `sourceBytes_` 虽然活到后端析构，但 appsrc 会把它当成可长驻的封包，
    /// 而我们这边任何时候都可能 move/clear —— 多一次 memcpy 换掉这一类悬挂。
    void feedAppSrc(GstAppSrc* src) {
        size_t pos = 0;
        size_t take = 0;
        {
            std::lock_guard<std::mutex> lock(srcMutex_);
            if (srcPos_ >= sourceBytes_.size()) {
                gst_app_src_end_of_stream(src);
                return;
            }
            pos = srcPos_;
            take = std::min(kAppBlockBytes, sourceBytes_.size() - pos);
            srcPos_ = pos + take;
        }
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, take, nullptr);
        if (buffer == nullptr) {
            gst_app_src_end_of_stream(src);
            return;
        }
        GstMapInfo map {};
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE) == TRUE) {
            std::memcpy(map.data, sourceBytes_.data() + pos, take);
            gst_buffer_unmap(buffer, &map);
        }
        // `push_buffer` 无论成败都吃掉这个 buffer 的引用。
        gst_app_src_push_buffer(src, buffer);
    }

    // ── 取帧 ────────────────────────────────────────────────────────────────

    GstSample* takeVideoSample() {
        if (cachedVideo_ != nullptr) {
            GstSample* sample = cachedVideo_;
            cachedVideo_ = nullptr;
            return sample;
        }
        return gst_app_sink_try_pull_sample(GST_APP_SINK(vsink_), kPullSliceNs);
    }

    void clearCachedVideo() {
        if (cachedVideo_ != nullptr) {
            gst_sample_unref(cachedVideo_);
            cachedVideo_ = nullptr;
        }
    }

    bool frameFromSample(GstSample* sample, int64_t ptsUs, VideoFrame& out) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        const GstCaps* caps = gst_sample_get_caps(sample);
        if (buffer == nullptr || caps == nullptr)
            return fail("样本里没有 buffer/caps");
        const GstStructure* structure = firstStructure(caps);
        int width = 0;
        int height = 0;
        capsInt(structure, "width", width);
        capsInt(structure, "height", height);
        const GstVideoMeta* meta = gst_buffer_get_video_meta(buffer);
        if (meta != nullptr) {
            if (meta->width > 0) {
                width = int(meta->width);
                height = int(meta->height);
            }
            if (meta->format != GST_VIDEO_FORMAT_UNKNOWN && meta->format != GST_VIDEO_FORMAT_BGRx)
                return fail("期望 BGRx，实际是别的像素布局（capsfilter 没生效）");
        }
        if (width <= 0 || height <= 0)
            return fail("GStreamer 交出来的帧没有尺寸");

        const size_t rowBytes = size_t(width) * 4;
        // 行距/平面偏移一律以 meta 为准（不假设紧排）；4 字节格式的行距必然是 4 的
        // 倍数，所以按 uint32 读不会未对齐。
        const size_t stride = meta != nullptr && meta->stride[0] > 0
                                      ? size_t(meta->stride[0])
                                      : rowBytes;
        const size_t offset = meta != nullptr ? size_t(meta->offset[0]) : 0;

        GstMapInfo map {};
        if (gst_buffer_map(buffer, &map, GST_MAP_READ) != TRUE)
            return fail("gst_buffer_map 失败");
        if (map.size < offset + stride * size_t(height - 1) + rowBytes) {
            gst_buffer_unmap(buffer, &map);
            return fail("样本长度与尺寸/行距不符");
        }
        std::vector<uint8_t> pixels(rowBytes * size_t(height));
        for (int row = 0; row < height; ++row) {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(map.data + offset +
                                                                     stride * size_t(row));
            uint32_t* dst = reinterpret_cast<uint32_t*>(pixels.data() + rowBytes * size_t(row));
            // BGRx 的内存序就是 B,G,R,x ⇒ 与 Skia 的 kBGRA_8888 同序；x 那字节内容
            // 未定义，一律补 0xFF（与 MF／Android 后端同口径）。
            for (int col = 0; col < width; ++col)
                dst[col] = src[col] | 0xFF000000u;
        }
        gst_buffer_unmap(buffer, &map);

        const SkImageInfo imageInfo =
                SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
        sk_sp<SkData> data = SkData::MakeWithCopy(pixels.data(), pixels.size());
        if (!data)
            return fail("SkData 分配失败");
        out.image = SkImages::RasterFromData(imageInfo, std::move(data), rowBytes);
        if (!out.image)
            return fail("SkImage 创建失败");
        out.ptsUs = ptsUs;
        // 自适应码流改分辨率时如实更新申报（帧本身按自己的尺寸成图，画面不会歪；
        // 但线程模型下引擎只在打开时拷一次 info_，尺寸变化不会往上报 —— 见文件头）。
        if (width != info_.width || height != info_.height) {
            info_.width = width;
            info_.height = height;
        }
        return true;
    }

    // ── 取 PCM ──────────────────────────────────────────────────────────────

    /// 把音频 appsink 里已经解出来的样本全搬进自持队列（有上限；见文件头「背压」）。
    void fillPcmQueue() {
        if (!audioOut_)
            return;
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        while (pcmBacklogUs() < kPcmBacklogUs) {
            // timeout=0：只把已经排好的搬走，不等 —— `readAudio` 不能堵住调用方。
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(asink_), 0);
            if (sample == nullptr)
                break;  // 这一轮没有了（EOS 也算）
            appendPcm(sample, frameBytes);
            gst_sample_unref(sample);
        }
    }

    void appendPcm(GstSample* sample, int64_t frameBytes) {
        if (frameBytes <= 0)
            return;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer == nullptr)
            return;
        if (!capsIsF32Interleaved(gst_sample_get_caps(sample)))
            return;  // 布局不是钉死的那一种 ⇒ 整段不接（宁可这段没声）
        GstMapInfo map {};
        if (gst_buffer_map(buffer, &map, GST_MAP_READ) != TRUE)
            return;
        const size_t frames = map.size / size_t(frameBytes);
        if (frames > 0) {
            int64_t durationUs = 0;
            const int64_t pts = sampleMediaUs(sample, durationUs);
            (void)durationUs;
            if (pcmBuf_.empty())
                pcmPtsUs_ = pts >= 0 ? pts : lastAudioPtsUs_;
            const size_t bytes = frames * size_t(frameBytes);
            pcmBuf_.insert(pcmBuf_.end(), map.data, map.data + bytes);
            lastAudioPtsUs_ = pcmPtsUs_ + int64_t(frames) * kUsPerSecond /
                                            int64_t(audioFormat_.sampleRate);
        }
        gst_buffer_unmap(buffer, &map);
    }

    int64_t pcmBacklogUs() const {
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        if (frameBytes <= 0 || audioFormat_.sampleRate <= 0 || pcmBuf_.size() <= pcmPos_)
            return 0;
        const int64_t frames = int64_t(pcmBuf_.size() - pcmPos_) / frameBytes;
        return frames * kUsPerSecond / int64_t(audioFormat_.sampleRate);
    }

    // ── 错误与总线 ──────────────────────────────────────────────────────────

    bool fail(const std::string& text) {
        error_ = text;
        return false;
    }

    void appendError(const char* text) {
        if (text == nullptr || *text == 0)
            return;
        error_ = error_.empty() ? text : error_ + "；" + text;
    }

    /// 总线上的 ERROR 一次性收干净（不跑 GMainLoop ⇒ 只能自己 poll）。
    /// WARNING 不收：解码器对不认识的 tag 也报 warning，收进 `error_` 会让引擎把
    /// 一次正常播放判成失败。
    void checkBus() {
        if (bus_ == nullptr)
            return;
        for (;;) {
            GstMessage* msg = gst_bus_timed_pop_filtered(bus_, 0, GST_MESSAGE_ERROR);
            if (msg == nullptr)
                break;
            GError* gerr = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &gerr, &debug);
            if (gerr != nullptr) {
                appendError(gerr->message);
                g_error_free(gerr);
            }
            if (debug != nullptr)
                g_free(debug);
            gst_message_unref(msg);
        }
    }

    // ── 元件：除 pipeline_/bus_ 外都是 bin 持有的借用量 ─────────────────────
    GstElement* pipeline_ = nullptr;
    GstBus* bus_ = nullptr;
    GstElement* decoder_ = nullptr;
    GstElement* appsrc_ = nullptr;
    GstElement* vconv_ = nullptr;
    GstElement* vcaps_ = nullptr;
    GstElement* vsink_ = nullptr;
    GstElement* aconv_ = nullptr;
    GstElement* acaps_ = nullptr;
    GstElement* asink_ = nullptr;

    VideoInfo info_;
    std::string error_;
    bool eof_ = false;

    /// open() 拉到的第一帧（几何信息的来源），交给第一次 `readFrame` 复用。
    GstSample* cachedVideo_ = nullptr;

    // pad-added 那一侧的状态（`padMutex_` 保护；`open()` 之后基本恒定）
    std::mutex padMutex_;
    std::condition_variable padCv_;
    bool videoLinked_ = false;
    bool audioLinked_ = false;
    int audioRate_ = 0;
    int audioChannels_ = 0;

    bool audioOut_ = false;
    AudioFormat audioFormat_;

    // 节拍
    int64_t lastPtsUs_ = 0;
    int64_t dropBeforeUs_ = 0;
    int64_t lastSeekTargetUs_ = 0;

    // PCM 队列（一次 readAudio 交一片，与 Android 后端同形）
    std::vector<uint8_t> pcmBuf_;
    std::vector<uint8_t> pcmView_;
    size_t pcmPos_ = 0;
    int64_t pcmPtsUs_ = 0;
    int64_t lastAudioPtsUs_ = 0;

    // 内存来源
    std::mutex srcMutex_;
    std::vector<uint8_t> sourceBytes_;
    size_t srcPos_ = 0;
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> createGStreamerBackend(const VideoSource& source,
                                                            std::string& error) {
    auto backend = std::make_unique<GStreamerBackend>();
    if (!backend->open(source, error))
        return nullptr;
    return backend;
}

/// 运行期探测：编进来不等于这台机器真能用。GStreamer 是 `dlopen` 插件式的，
/// 少了 gst-plugins-base/good 那一套不会在链接期报错，只会在这一步露出来。
/// 只看元件在不在注册表里 —— 具体某个码流的解码器缺不缺，交给打开时的错误文本。
bool gStreamerVideoAvailable(std::string& note) {
    const GstInit& init = gstInit();
    if (!init.ok) {
        note = init.note.empty() ? "GStreamer 起不来（插件注册表读不到）" : init.note;
        return false;
    }
    static const char* const kNeed[] = {"uridecodebin", "decodebin", "videoconvert",
                                        "audioconvert", "appsink", "appsrc"};
    for (const char* name : kNeed) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (factory == nullptr) {
            note = std::string("GStreamer 注册表里没有元件 ") + name +
                   "（缺 gst-core/gst-plugins-base？）";
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

}  // namespace cui

#endif  // CUI_HAVE_GSTREAMER
