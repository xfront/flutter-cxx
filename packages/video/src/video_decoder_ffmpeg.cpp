/// @file foundation/video_decoder_ffmpeg.cpp
/// @brief FFmpeg 解码后端（除 web 外全平台；库固定来自 vcpkg 的 feature `ffmpeg`）。
///
/// 与 Windows 侧的 Media Foundation 后端同职责：解复用 + 解码 + 输出 BGRA，
/// 另外再解一条音轨交出 PCM。依赖 libavformat / libavcodec / libavutil /
/// libswscale / libswresample，不用 libavfilter。像素转换（YUV→BGRA）交给
/// sws_scale，尺寸与原帧一致；音频（任意来格式 → S16 交织）交给 swresample。
///
/// 关键实现口径：
///   - 内存来源用自定义 `AVIOContext`（`AVFMT_FLAG_CUSTOM_IO`），
///     路径/URL 直接交给 `avformat_open_input`。自定义 IO **必须带 seek
///     回调**，否则 avio 不可寻址、播完后再 seek(0) 失败（见 memorySeek）。
///   - 音频的形状与 AMediaCodec 后端一致：**读包的人管两条轨**
///     （`feedPacket` 按 `stream_index` 分流），`readAudio` 只从自有的 PCM
///     队列里取。这样解复用游标只有一个，不会出现两条线程抢 `av_read_frame`。
///   - PCM 一律重采成 **S16 交织**（`audio_sink.h` 给 FFmpeg 定的口径），声道数
///     照音轨给、采样率不换算 —— 各端设备按 `AudioFormat` 申请。
///   - 音频侧起不来（没有解码器 / 格式认不出 / 重采样失败）时**只关声音**，
///     画面照走，与 Android 后端的 `discardAudio()` 同一条口径。
///   - PCM 队列积压过阈值就丢掉新解出的那段（codec 的输出照收）：反过来
///     「不收输出」会让音频解码器的输入永久满，`av_read_frame` 那侧的游标
///     被顶死，画面也跟着停（与 Android 后端同一条理由）。
///   - 输出帧**不旋转**：`info().rotation` 只上报「UI 需要顺时针补偿多少度」，
///     与 Media Foundation 后端同口径（旋转由控件层的 RotatedBox 完成）。
///   - 跳转按关键帧向后找（`AVSEEK_FLAG_BACKWARD`），随后丢弃 pts 早于目标的
///     帧，保证暂停中 seek 显示的是目标位置的那一帧。
///   - 到流尾时先送空包把解码器里残留的帧收完，再置 EOF（两条轨都是这个做法）。

#include "cui/video/video_decoder.h"

#if defined(CUI_HAVE_FFMPEG)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

namespace cui {
namespace {

std::string avErrorText(const char* what, int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return std::string(what) + "：" + buf;
}

/// 内存来源的读回调；`opaque` 指向 {data, size, pos}。
struct MemoryIo {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

int memoryRead(void* opaque, uint8_t* buf, int size) {
    auto* io = static_cast<MemoryIo*>(opaque);
    if (io->pos >= io->size)
        return AVERROR_EOF;
    const size_t remaining = io->size - io->pos;
    const int n = static_cast<int>(std::min<size_t>(remaining, size_t(size)));
    std::memcpy(buf, io->data + io->pos, size_t(n));
    io->pos += size_t(n);
    return n;
}

/// 内存来源的 seek 回调。**必须提供**：AVIO 没有 seek 就不可寻址，
/// `av_seek_frame` 会失败，而 `seekTo` 失败时 EOF 标记清不掉 —— 表现为
/// 「播完后 play() 回到开头立刻又判完成、再也播不动」（内存来源的视频
/// 页正是这条路径）。返回 `AVSEEK_SIZE` 时按 FFmpeg 约定上报总长度。
int64_t memorySeek(void* opaque, int64_t offset, int whence) {
    auto* io = static_cast<MemoryIo*>(opaque);
    if (whence & AVSEEK_SIZE)
        return static_cast<int64_t>(io->size);
    whence &= ~AVSEEK_FORCE;
    int64_t base = 0;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = static_cast<int64_t>(io->pos); break;
        case SEEK_END: base = static_cast<int64_t>(io->size); break;
        default: return AVERROR(EINVAL);
    }
    const int64_t target = base + offset;
    if (target < 0)
        return AVERROR(EINVAL);
    io->pos = static_cast<size_t>(
            std::min<int64_t>(target, static_cast<int64_t>(io->size)));
    return static_cast<int64_t>(io->pos);
}

/// 一段已重采样的 PCM（S16 交织）+ 它第一个采样帧的 pts。
struct PcmPiece {
    std::vector<uint8_t> bytes;
    int64_t ptsUs = 0;
};

/// 判断是不是 **已弃用的 JPEG 满量程格式**（YUVJ420P 那一族）。libswscale 在
/// 初始化时见到它们就打 "deprecated pixel format used, make sure you did set
/// range correctly"，并提醒改用「普通格式 + color_range」。FFmpeg 官方的迁移
/// 口径就是：喂 sws 前把 YUVJxxx 换成同结构的 xxx，自己记住它隐含满量程
/// （mjpeg / gif / pgm 解码器至今仍直接输出这套格式）。
bool isDeprecatedJpegFullRange(AVPixelFormat fmt) {
    return fmt == AV_PIX_FMT_YUVJ411P || fmt == AV_PIX_FMT_YUVJ420P ||
           fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ440P ||
           fmt == AV_PIX_FMT_YUVJ444P;
}

/// YUVJxxx → YUVxxx：位布局逐字节相同，只是不再用弃用格式表达满量程。
AVPixelFormat normalizePixFmt(AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_YUVJ411P: return AV_PIX_FMT_YUV411P;
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P: return AV_PIX_FMT_YUV440P;
        default: return fmt;
    }
}

/// 这帧是不是满量程（= RGB 直接映射、不需要把 16-235 拉开）。
/// 优先信帧自带的 color_range；没标但格式是 YUVJxxx 的按满量程算。
bool frameIsFullRange(const AVFrame& frame) {
    if (frame.color_range == AVCOL_RANGE_JPEG)
        return true;
    if (frame.color_range == AVCOL_RANGE_MPEG)
        return false;
    return isDeprecatedJpegFullRange(static_cast<AVPixelFormat>(frame.format));
}

/// 帧的 colorspace → sws 系数表 id（认不出来就用 BT.601，与 swscale 的
/// SWS_CS_DEFAULT 同口径）。
int swsColorspaceOf(const AVFrame& frame) {
    switch (frame.colorspace) {
        case AVCOL_SPC_BT709: return SWS_CS_ITU709;
        case AVCOL_SPC_FCC: return SWS_CS_FCC;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
        default: return SWS_CS_DEFAULT;
    }
}

class FFmpegBackend : public VideoDecoderBackend {
public:
    ~FFmpegBackend() override { close(); }

    bool open(const VideoSource& source, std::string& error) {
        int ret = 0;
        if (!source.bytes.empty()) {
            // 自定义 IO：缓冲区必须由我们自己释放（avformat_close_input 不管它）。
            constexpr int kIoBuffer = 32 * 1024;
            auto* buffer = static_cast<uint8_t*>(av_malloc(kIoBuffer));
            if (!buffer) {
                error = "FFmpeg IO 缓冲区分配失败";
                return false;
            }
            memIo_.data = source.bytes.data();
            memIo_.size = source.bytes.size();
            memIo_.pos = 0;
            io_ = avio_alloc_context(buffer, kIoBuffer, 0, &memIo_, &memoryRead, nullptr,
                                     &memorySeek);
            if (!io_) {
                av_free(buffer);
                error = "FFmpeg 创建 AVIOContext 失败";
                return false;
            }
            fmt_ = avformat_alloc_context();
            if (!fmt_) {
                error = "FFmpeg 创建 AVFormatContext 失败";
                return false;
            }
            fmt_->pb = io_;
            fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
            ret = avformat_open_input(&fmt_, nullptr, nullptr, nullptr);
        } else {
            AVDictionary* options = nullptr;
            // HTTP/RTSP 的读超时（微秒）；不支持该选项的协议会忽略。
            av_dict_set(&options, "timeout", "8000000", 0);
            av_dict_set(&options, "rw_timeout", "8000000", 0);
            ret = avformat_open_input(&fmt_, source.uri.c_str(), nullptr, &options);
            av_dict_free(&options);
        }
        if (ret < 0) {
            error = avErrorText("打开视频失败", ret);
            return false;
        }

        ret = avformat_find_stream_info(fmt_, nullptr);
        if (ret < 0) {
            error = avErrorText("读取流信息失败", ret);
            return false;
        }

        videoIndex_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIndex_ < 0) {
            error = "文件里没有视频流";
            return false;
        }
        AVStream* stream = fmt_->streams[videoIndex_];

        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            error = "系统 FFmpeg 没有对应的解码器（codec_id=" +
                    std::to_string(int(stream->codecpar->codec_id)) + "）";
            return false;
        }
        codecCtx_ = avcodec_alloc_context3(codec);
        if (!codecCtx_) {
            error = "FFmpeg 创建解码上下文失败";
            return false;
        }
        ret = avcodec_parameters_to_context(codecCtx_, stream->codecpar);
        if (ret < 0) {
            error = avErrorText("复制解码参数失败", ret);
            return false;
        }
        codecCtx_->pkt_timebase = stream->time_base;
        ret = avcodec_open2(codecCtx_, codec, nullptr);
        if (ret < 0) {
            error = avErrorText("打开解码器失败", ret);
            return false;
        }

        info_.width = codecCtx_->width;
        info_.height = codecCtx_->height;
        info_.frameRate = frameRateOf(*stream);
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = codec->name ? codec->name : "unknown";
        info_.duration = durationOf(*stream);
        info_.hasAudio = hasStream(AVMEDIA_TYPE_AUDIO);
        info_.rotation = rotationOf(*stream);
        timeBase_ = stream->time_base;

        // 音轨：解不开就只关声音，`error` 一个字都不写（下面 openAudio 的注释）。
        openAudio();
        return true;
    }

    const VideoInfo& info() const override { return info_; }
    bool atEnd() const override { return eof_; }
    const std::string& error() const override { return error_; }

    // ── 音频（基类那四个虚函数的真实现） ─────────────────────────────────

    bool hasAudioStream() const override { return audioCodecCtx_ != nullptr; }
    AudioFormat audioFormat() const override { return audioFormat_; }

    /// 只从队列取；队列空 = 「目前没有了」（不代表结束，也不代表出错）。
    /// PCM 是 `feedPacket` 顺手解出来的，所以调用方必须在 `readFrame` 之外
    /// 循环叫它 —— 与 Android 后端同一个契约（引擎 `feedAudio()` 正是这么做的）。
    bool readAudio(AudioChunk& out) override {
        if (!audioCodecCtx_ || !audioFormat_.isValid())
            return false;
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        for (;;) {
            if (pcmQueue_.empty())
                return false;
            PcmPiece& front = pcmQueue_.front();
            if (pcmPosBytes_ >= front.bytes.size()) {
                // 这一整段都交出去了：出队（下一段是**更晚**的 PCM，正是我们要的顺序）。
                pcmQueuedFrames_ -= int64_t(front.bytes.size() / size_t(frameBytes));
                pcmQueue_.pop_front();
                pcmPosBytes_ = 0;
                continue;
            }
            const size_t availFrames = (front.bytes.size() - pcmPosBytes_) / size_t(frameBytes);
            const size_t want = std::min<size_t>(availFrames, kPcmChunkFrames);
            const size_t wantBytes = want * size_t(frameBytes);
            // 交给 `AudioChunk` 的那截拷进 `pcmView_`：契约是「指针到下一次
            // readAudio/flushAudio 有效」，而 `AudioSink::push()` 当场就拷进环形
            // 缓冲，所以复用这一份视图就够（与 Android 后端同写法）。
            pcmView_.assign(front.bytes.begin() + pcmPosBytes_,
                            front.bytes.begin() + pcmPosBytes_ + wantBytes);
            const int64_t skippedFrames = int64_t(pcmPosBytes_ / size_t(frameBytes));
            out.frames = want;
            out.ptsUs = front.ptsUs + skippedFrames * 1000000 / audioFormat_.sampleRate;
            out.format = audioFormat_.format;
            out.s16 = reinterpret_cast<const int16_t*>(pcmView_.data());
            out.f32 = nullptr;
            pcmPosBytes_ += wantBytes;
            return true;
        }
    }

    void flushAudio() override {
        pcmQueue_.clear();
        pcmView_.clear();
        pcmPosBytes_ = 0;
        pcmQueuedFrames_ = 0;
        // `audioTailUs_` 不归零：它只是「这一路解到了哪里」，归零反而会在
        // 无 pts 的裸流上把推算基准拉回开头。
        // swr 里可能留着跨 seek 的延迟样本（整数比转换才有），最省事的正确做法
        // 是重建一份 —— 只在 seek 上付一次，不在每帧 PCM 上付。
        if (swr_) {
            swr_free(&swr_);
            swr_ = nullptr;
        }
        swrInFormat_ = -1;
        swrInRate_ = 0;
        swrInChannels_ = 0;
    }

    bool readFrame(VideoFrame& out) override {
        if (!codecCtx_ || eof_)
            return false;

        for (;;) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                error_ = "FFmpeg 分配 AVFrame 失败";
                eof_ = true;
                return false;
            }
            const int ret = avcodec_receive_frame(codecCtx_, frame);
            if (ret == 0) {
                const bool converted = convertFrame(frame, out);
                av_frame_free(&frame);
                if (!converted) {
                    eof_ = true;
                    return false;
                }
                if (out.ptsUs + 1 < dropBeforeUs_) {
                    // seek 后关键帧早于目标：继续解码但不呈现。
                    continue;
                }
                return true;
            }
            av_frame_free(&frame);
            if (ret == AVERROR_EOF) {
                eof_ = true;
                return false;
            }
            if (ret != AVERROR(EAGAIN)) {
                error_ = avErrorText("解码失败", ret);
                eof_ = true;
                return false;
            }
            if (flushSent_) {
                eof_ = true;
                return false;
            }
            if (!feedPacket()) {
                if (eof_)
                    return false;
            }
        }
    }

    bool seekTo(int64_t ptsUs) override {
        if (!fmt_ || !codecCtx_ || videoIndex_ < 0)
            return false;
        AVStream* stream = fmt_->streams[videoIndex_];
        const int64_t timestamp = av_rescale_q(std::max<int64_t>(0, ptsUs), AV_TIME_BASE_Q,
                                               stream->time_base);
        const int ret = av_seek_frame(fmt_, videoIndex_, timestamp, AVSEEK_FLAG_BACKWARD);
        if (ret < 0)
            return false;
        avcodec_flush_buffers(codecCtx_);
        // 音频：解码器窗口与未交出的 PCM 全部作废（引擎随后还会叫一次
        // `flushAudio()`，这里是同一个动作，重复无害）。
        if (audioCodecCtx_) {
            avcodec_flush_buffers(audioCodecCtx_);
            audioFlushSent_ = false;
            flushAudio();
        }
        eof_ = false;
        flushSent_ = false;
        dropBeforeUs_ = ptsUs;
        return true;
    }

private:
    static constexpr size_t kPcmChunkFrames = 2048;
    /// PCM 积压的高水位（与 Android 后端的 `kPcmBacklogSeconds` 同口径）。
    static constexpr int64_t kPcmBacklogUs = 2 * 1000 * 1000;

    /// 取一个包送进解码器：视频包送视频 codec（本轮就结束），音轨包送音频
    /// codec 并当场把 PCM 收进队列，其余轨道直接扔。
    /// 返回 false 表示这轮没送出视频包（已 EOF 或出错）。
    bool feedPacket() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            error_ = "FFmpeg 分配 AVPacket 失败";
            eof_ = true;
            return false;
        }
        for (;;) {
            const int ret = av_read_frame(fmt_, packet);
            if (ret < 0) {
                av_packet_free(&packet);
                if (!flushSent_) {
                    flushSent_ = true;
                    avcodec_send_packet(codecCtx_, nullptr);
                    // 视频到流尾 = 解复用结束，音频也一样：送空包把解码器里
                    // 残留的那几段 PCM 收干净，否则末尾一秒半的声音会丢。
                    if (audioCodecCtx_ && !audioFlushSent_) {
                        audioFlushSent_ = true;
                        avcodec_send_packet(audioCodecCtx_, nullptr);
                        drainAudio();
                    }
                    return false;
                }
                eof_ = true;
                return false;
            }
            if (packet->stream_index == videoIndex_) {
                const int sent = avcodec_send_packet(codecCtx_, packet);
                av_packet_free(&packet);
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    error_ = avErrorText("送包给解码器失败", sent);
                    eof_ = true;
                    return false;
                }
                return true;
            }
            if (packet->stream_index == audioIndex_ && audioCodecCtx_) {
                feedAudioPacket(packet);
                av_packet_unref(packet);
                // 音频那一侧坏了（`disableAudio`）也不拦这一步：下一轮的
                // `audioCodecCtx_` 为空，同一个包的位置会落到下面的丢弃分支。
                continue;
            }
            av_packet_unref(packet);
        }
    }

    /// 打开音轨解码器。**不写 `error`**：音频起不来不是「打开视频失败」，
    /// 只把这一路关掉（与 Android 后端 `discardAudio()` 同一条口径）。
    bool openAudio() {
        audioIndex_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audioIndex_ < 0)
            return false;
        AVStream* stream = fmt_->streams[audioIndex_];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            audioIndex_ = -1;
            return false;
        }
        audioCodecCtx_ = avcodec_alloc_context3(codec);
        if (!audioCodecCtx_) {
            audioIndex_ = -1;
            return false;
        }
        if (avcodec_parameters_to_context(audioCodecCtx_, stream->codecpar) < 0 ||
            avcodec_open2(audioCodecCtx_, codec, nullptr) < 0) {
            avcodec_free_context(&audioCodecCtx_);
            audioCodecCtx_ = nullptr;
            audioIndex_ = -1;
            return false;
        }
        audioCodecCtx_->pkt_timebase = stream->time_base;
        const int rate = audioCodecCtx_->sample_rate;
        const int channels = audioFormatChannels();
        if (rate <= 0 || channels <= 0) {
            audioIndex_ = -1;
            closeAudio();
            return false;
        }
        audioTimeBase_ = stream->time_base;
        audioFormat_.sampleRate = rate;
        audioFormat_.channels = channels;
        audioFormat_.format = AudioSampleFormat::S16Interleaved;
        return true;
    }

    /// 音频的声道数：先看 codec 上下文，再看容器给的 codecpar（裸流/某些
    /// 容器上 `ch_layout` 只有后者填了）。
    int audioFormatChannels() const {
        if (audioCodecCtx_ && audioCodecCtx_->ch_layout.nb_channels > 0)
            return audioCodecCtx_->ch_layout.nb_channels;
        if (audioIndex_ >= 0 && fmt_)
            return fmt_->streams[audioIndex_]->codecpar->ch_layout.nb_channels;
        return 0;
    }

    /// 音频解码器整体不可用：只关声音，画面照走。
    void disableAudio() {
        closeAudio();
        audioIndex_ = -1;
        audioFormat_ = AudioFormat{};
    }

    void closeAudio() {
        flushAudio();
        if (audioCodecCtx_) {
            avcodec_free_context(&audioCodecCtx_);
            audioCodecCtx_ = nullptr;
        }
        audioFlushSent_ = false;
    }

    /// 音轨包进解码器，随后立刻把输出收干净（不这么做，PCM 就永远躺在
    /// codec 的内部缓冲里，而 `readAudio` 只看我们自己的队列）。
    void feedAudioPacket(AVPacket* packet) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int sent = avcodec_send_packet(audioCodecCtx_, packet);
            if (sent == 0)
                break;
            if (sent == AVERROR(EAGAIN)) {
                // 输入满：包**没被消起**，先收输出腾位，再用同一个包重试。
                drainAudio();
                continue;
            }
            if (sent == AVERROR_EOF)
                break;  // 尾部空包已送过：这包丢弃就是（不算错）
            disableAudio();
            return;
        }
        drainAudio();
    }

    void drainAudio() {
        for (;;) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                disableAudio();
                return;
            }
            const int ret = avcodec_receive_frame(audioCodecCtx_, frame);
            if (ret != 0) {
                av_frame_free(&frame);
                // EAGAIN = 这一轮解完了；AVERROR_EOF = 空包之后真的到底了。
                // 其余错误认作「这一路坏了」，只关声音。
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    return;
                disableAudio();
                return;
            }
            convertAudioFrame(frame);
            av_frame_free(&frame);
        }
    }

    void convertAudioFrame(AVFrame* frame) {
        if (!ensureSwr(*frame)) {
            disableAudio();
            return;
        }
        const int inSamples = frame->nb_samples;
        if (inSamples <= 0)
            return;
        const size_t frameBytes = size_t(audioFormat_.bytesPerFrame());
        // 输出上限：采样率不变时 swr 给的就是输入帧数；多留 256 帧给带
        // 整数比/延迟的情况（`swr_convert` 只会给到它真攒够的部分）。
        const int outMax = inSamples + 256;
        swrBuf_.resize(size_t(outMax) * frameBytes);
        uint8_t* outData[1] = {swrBuf_.data()};
        const int got = swr_convert(swr_, outData, outMax,
                                   const_cast<const uint8_t**>(frame->data), inSamples);
        if (got <= 0)
            return;  // 延迟期给 0 帧不是错误（下一帧会一起出来）
        const int64_t ptsUs = audioPtsOf(*frame);
        if (pcmQueuedFrames_ * int64_t(1000000) / audioFormat_.sampleRate > kPcmBacklogUs) {
            // 积压过阈值：丢掉这段**最新**的 PCM（队列里留下的是贴近画面位置的
            // 旧段），但上面照样把 codec 的输出收了 —— 见文件头那条理由。
            audioTailUs_ = ptsUs + int64_t(got) * 1000000 / audioFormat_.sampleRate;
            return;
        }
        PcmPiece piece;
        piece.bytes.assign(swrBuf_.begin(), swrBuf_.begin() + size_t(got) * frameBytes);
        piece.ptsUs = ptsUs;
        audioTailUs_ = ptsUs + int64_t(got) * 1000000 / audioFormat_.sampleRate;
        pcmQueuedFrames_ += int64_t(got);
        pcmQueue_.push_back(std::move(piece));
    }

    /// 重采样上下文：输入的格式/采样率/声道数任一项变了就重建（换轨、
    /// 码流中途改格式都会走到这里）。失败返回 false ⇒ 调用方只关声音。
    bool ensureSwr(const AVFrame& frame) {
        const AVSampleFormat inFmt = static_cast<AVSampleFormat>(frame.format);
        const int inRate = frame.sample_rate > 0 ? frame.sample_rate : audioFormat_.sampleRate;
        const int inChannels = frame.ch_layout.nb_channels > 0
                                       ? frame.ch_layout.nb_channels : audioFormat_.channels;
        if (swr_ && swrInFormat_ == int(inFmt) && swrInRate_ == inRate &&
            swrInChannels_ == inChannels)
            return true;
        if (swr_)
            swr_free(&swr_);
        // `swr_alloc_set_opts2` 内部拷贝两个 layout（走 av_opt_set_chlayout），
        // 所以直接指 `frame.ch_layout` 是安全的；只有它没填的时候才自造一份
        // 按声道数的默认布局（下面按 `in == &inLayout` 判定要不要自己收尾）。
        AVChannelLayout inLayout {};
        const AVChannelLayout* in = &frame.ch_layout;
        if (inChannels > 0 && frame.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&inLayout, inChannels);
            in = &inLayout;
        }
        AVChannelLayout outLayout {};
        av_channel_layout_default(&outLayout, audioFormat_.channels);
        const int ret = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_S16,
                                            audioFormat_.sampleRate, in, inFmt, inRate, 0,
                                            nullptr);
        av_channel_layout_uninit(&outLayout);
        if (in == &inLayout)
            av_channel_layout_uninit(&inLayout);
        if (ret < 0 || !swr_ || swr_init(swr_) < 0) {
            if (swr_)
                swr_free(&swr_);
            swr_ = nullptr;
            swrInFormat_ = -1;
            return false;
        }
        swrInFormat_ = int(inFmt);
        swrInRate_ = inRate;
        swrInChannels_ = inChannels;
        return true;
    }

    /// 音频帧的 pts。没有 pts（裸流/索引缺失）就按上一段的末尾推算 ——
    /// 这个字段只给 `AudioChunk::ptsUs` 用（日志与调试），主时钟来自
    /// `AudioRing` 的消耗计数，所以推算值不会把同步带偏。
    int64_t audioPtsOf(const AVFrame& frame) {
        int64_t ts = frame.best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE)
            ts = frame.pts;
        if (ts == AV_NOPTS_VALUE || audioTimeBase_.den <= 0 || audioTimeBase_.num <= 0)
            return audioTailUs_;
        const int64_t us = av_rescale_q(ts, audioTimeBase_, AV_TIME_BASE_Q);
        if (us < 0)
            return audioTailUs_;
        return us;
    }

    bool convertFrame(AVFrame* frame, VideoFrame& out) {
        const int width = frame->width;
        const int height = frame->height;
        if (width <= 0 || height <= 0) {
            error_ = "解码出的帧尺寸无效";
            return false;
        }

        // 弃用的 YUVJxxx 先归一成 YUVxxx（位布局不变）再建上下文 —— swscale
        // 只要在初始化时见到源格式是 YUVJ 那一族，就会打
        // "deprecated pixel format used, make sure you did set range
        // correctly"，并提醒改用「现代格式 + 显式 range」；range 由下面的
        // sws_setColorspaceDetails 补回来，所以画面颜色不受影响。
        const AVPixelFormat srcFormat =
                normalizePixFmt(static_cast<AVPixelFormat>(frame->format));
        // 满量程只在源真是平面 YUV 时才声明（-1 = 不调 setColorspaceDetails）：
        // 源已经是 RGB / PAL8 时 srcRange 无意义，硬塞反而多余。
        const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(srcFormat);
        const bool planarYuv = srcDesc && !(srcDesc->flags & AV_PIX_FMT_FLAG_RGB) &&
                               !(srcDesc->flags & AV_PIX_FMT_FLAG_HWACCEL) &&
                               !(srcDesc->flags & AV_PIX_FMT_FLAG_PAL) &&
                               av_pix_fmt_count_planes(srcFormat) >= 3;
        const int srcRange = planarYuv ? (frameIsFullRange(*frame) ? 1 : 0) : -1;

        if (!sws_ || swsWidth_ != width || swsHeight_ != height ||
            swsFormat_ != srcFormat || swsRange_ != srcRange) {
            sws_freeContext(sws_);
            sws_ = sws_getContext(width, height, srcFormat,
                                  width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                                  nullptr, nullptr);
            if (!sws_) {
                error_ = "创建像素转换上下文失败";
                return false;
            }
            // 目标 BGRA 恒为满量程；源量程按帧声明，否则 sws 会按有限量程
            // （16-235）解释满量程数据，画面发灰。
            if (srcRange >= 0) {
                const int cs = swsColorspaceOf(*frame);
                const int* coeffs = sws_getCoefficients(cs);
                sws_setColorspaceDetails(sws_, coeffs, srcRange, coeffs, /*dstRange=*/1,
                                         /*brightness=*/0, /*contrast=*/1 << 16,
                                         /*saturation=*/1 << 16);
            }
            swsWidth_ = width;
            swsHeight_ = height;
            swsFormat_ = srcFormat;
            swsRange_ = srcRange;
        }

        const int stride = width * 4;
        std::vector<uint8_t> pixels(size_t(stride) * size_t(height));
        uint8_t* dstData[4] = {pixels.data(), nullptr, nullptr, nullptr};
        int dstLines[4] = {stride, 0, 0, 0};
        sws_scale(sws_, frame->data, frame->linesize, 0, height, dstData, dstLines);

        const SkImageInfo imageInfo = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                                                        kOpaque_SkAlphaType);
        auto data = SkData::MakeWithCopy(pixels.data(), pixels.size());
        if (!data)
            return false;
        out.image = SkImages::RasterFromData(imageInfo, std::move(data), size_t(stride));
        out.ptsUs = ptsOf(*frame);
        lastPtsUs_ = out.ptsUs;
        return out.image != nullptr;
    }

    int64_t ptsOf(const AVFrame& frame) const {
        int64_t ts = frame.best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE)
            ts = frame.pts;
        if (ts == AV_NOPTS_VALUE)
            return lastPtsUs_;
        const int64_t us = av_rescale_q(ts, timeBase_, AV_TIME_BASE_Q);
        return us >= 0 ? us : 0;
    }

    static double frameRateOf(const AVStream& stream) {
        if (stream.avg_frame_rate.num > 0 && stream.avg_frame_rate.den > 0)
            return double(stream.avg_frame_rate.num) / double(stream.avg_frame_rate.den);
        if (stream.r_frame_rate.num > 0 && stream.r_frame_rate.den > 0)
            return double(stream.r_frame_rate.num) / double(stream.r_frame_rate.den);
        return 0.0;
    }

    std::chrono::microseconds durationOf(const AVStream& stream) const {
        if (fmt_ && fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0)
            return std::chrono::microseconds(fmt_->duration);
        if (stream.duration != AV_NOPTS_VALUE && stream.duration > 0)
            return std::chrono::microseconds(av_rescale_q(stream.duration, stream.time_base,
                                                          AV_TIME_BASE_Q));
        return std::chrono::microseconds(0);
    }

    bool hasStream(AVMediaType type) const {
        for (unsigned i = 0; fmt_ && i < fmt_->nb_streams; ++i) {
            if (fmt_->streams[i]->codecpar->codec_type == type)
                return true;
        }
        return false;
    }

    /// 显示矩阵 → 顺时针补偿角（0/90/180/270）。
    /// `av_display_rotation_get` 给的是逆时针角度，取负并归一化。
    static int rotationOf(const AVStream& stream) {
        const AVPacketSideData* side =
                av_packet_side_data_get(stream.codecpar->coded_side_data,
                                        stream.codecpar->nb_coded_side_data,
                                        AV_PKT_DATA_DISPLAYMATRIX);
        if (!side || side->size < 9 * sizeof(int32_t))
            return 0;
        const double angle =
                av_display_rotation_get(reinterpret_cast<const int32_t*>(side->data));
        if (std::isnan(angle))
            return 0;
        int deg = int(std::lround(-angle)) % 360;
        if (deg < 0)
            deg += 360;
        return deg;
    }

    void close() {
        // 音频资源先放（`closeAudio` 里会清队列与重采样上下文）。
        if (audioCodecCtx_)
            closeAudio();
        else
            flushAudio();
        if (codecCtx_) {
            avcodec_free_context(&codecCtx_);
            codecCtx_ = nullptr;
        }
        if (fmt_) {
            avformat_close_input(&fmt_);
            fmt_ = nullptr;
        }
        if (io_) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
            io_ = nullptr;
        }
        if (sws_) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
    }

    AVFormatContext* fmt_ = nullptr;
    AVIOContext* io_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwsContext* sws_ = nullptr;
    int swsWidth_ = 0;
    int swsHeight_ = 0;
    int swsFormat_ = -1;
    int swsRange_ = -1;  ///< 上次建上下文时声明的源量程（1=满量程），变了要重建
    int videoIndex_ = -1;
    AVRational timeBase_{1, 1000000};
    VideoInfo info_;
    std::string error_;
    bool eof_ = false;
    bool flushSent_ = false;
    int64_t dropBeforeUs_ = 0;
    int64_t lastPtsUs_ = 0;
    MemoryIo memIo_;

    // ── 音频（全部只在播放引擎的工作线程上碰，不需额外同步） ──────────
    int audioIndex_ = -1;
    AVCodecContext* audioCodecCtx_ = nullptr;
    AVRational audioTimeBase_{1, 1000000};
    AudioFormat audioFormat_;
    SwrContext* swr_ = nullptr;
    int swrInFormat_ = -1;   ///< 当前 swr_ 的输入格式（变了重建）
    int swrInRate_ = 0;
    int swrInChannels_ = 0;
    std::vector<uint8_t> swrBuf_;      ///< 重采样的落点（逐段复用）
    std::deque<PcmPiece> pcmQueue_;    ///< 已重采、还没交出去的 PCM
    std::vector<uint8_t> pcmView_;     ///< `readAudio` 交出去的那一截
    size_t pcmPosBytes_ = 0;           ///< 队首已交出的字节数
    int64_t pcmQueuedFrames_ = 0;      ///< 队列里的采样帧总数（背压判定用）
    int64_t audioTailUs_ = 0;          ///< 无 pts 时的推算基准
    bool audioFlushSent_ = false;      ///< 尾部空包只送一次
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> createFFmpegBackend(const VideoSource& source,
                                                         std::string& error) {
    auto backend = std::make_unique<FFmpegBackend>();
    if (!backend->open(source, error))
        return nullptr;
    return backend;
}

/// 运行期探测：编进来不等于这台机器真能用。libav* 是链接期依赖，加载失败
/// 根本走不到这里，所以这里只看 ABI 主版本：发行版升过 ffmpeg（major 变了）
/// 而二进制还是旧头编的，那一切换下去必踩，不如提前报「不可用」退回系统端。
bool ffmpegVideoAvailable(std::string& note) {
    const int major = avformat_version() >> 16;
    if (major != LIBAVFORMAT_VERSION_MAJOR) {
        note = "libavformat 运行期主版本 " + std::to_string(major) +
               " 与编译期的 " + std::to_string(LIBAVFORMAT_VERSION_MAJOR) + " 不一致";
        return false;
    }
    return true;
}

}  // namespace cui_core

#endif  // CUI_HAVE_FFMPEG
