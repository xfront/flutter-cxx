/// @file foundation/video_decoder_win32.cpp
/// @brief Media Foundation 解码后端（Windows）。
///
/// 对应 上游 Android 侧的 ExoPlayer 角色：解复用 + 解码 + 输出 BGRA 帧，
/// 另外再解一条音轨交出 PCM（与 FFmpeg / AMediaCodec 后端同一个契约）。
/// 选择 Media Foundation 而不是给 Windows 配一套 FFmpeg，是因为它随系统
/// 提供（零依赖），且 mp4/mkv/mov/avi/MP3-in-MP4 等常见容器都在。
///
/// 关键实现口径：
///   - 用 `IMFSourceReader` 的 **video processing** 模式，直接要 RGB32 输出，
///     缩放/颜色转换交给系统（否则要自己接 IMFTransform）。
///   - 音频：`readFrame` 与 `readAudio` **各自向自己的流要样本**（SourceReader
///     按流下标分流，不需要像 FFmpeg 那样共用一个解复用游标）。系统偶尔会把
///     音频样本从视频那一路递出来，所以 `readFrame` 见到下标等于音频流的样本
///     就把它存进 `audioQueue_`，`readAudio` 先读队列再向 reader 要。
///   - 音频输出类型首选 **IEEE float**（MF 的音频解码器原生就是浮点），系统
///     拒绝再退 16bit 定点；浮点还可能是**平面**（`MF_MT_ALL_SAMPLES_INDEPENDENT`），
///     三种布局 `AudioChunk` 都表达得了，环形缓冲一律归一成 F32 交织。
///   - **音频这一路起不来就只关声音**（`disableAudio()`，不写 `error`）：配置
///     失败、reader 报错、或者 video processing 模式把音频流收去自用（官方文档
///     留了这种可能：那种模式下 reader 不交付音频样本）⇒ 画面照走，退回
///     「无声 + 墙钟」，与改造前一模一样。这条退留着的理由不是「没法实测」：
///     本仓现在就在 Windows 上，这份文件与 `audio_sink_win32.cpp` 都是真
///     MSVC + Windows SDK 编出来并跑过测试的；留退路是因为「reader 在 video
///     processing 模式下到底交不交付音频样本」随 OS build 与解码器实现而变，
///     官方文档明确允许那种模式下由 reader 自己消费掉音频流。
///   - SourceReader 在**工作线程**上创建（`VideoPlayback` 保证所有后端调用
///     都在该线程），所以 COM 也在这里初始化；MFStartup 进程内只做一次。
///   - RGB32 的内存布局是 BGRX；Skia 用 `kBGRA_8888 + kOpaque` 读它，
///     alpha 字节按不透明处理，不需要额外遍历像素。
///   - 行距与行序都按 `MF_MT_DEFAULT_STRIDE` 来：系统可能把行对齐到 16 字节
///     （源行距 ≠ width*4），负值 = 自下而上 DIB，复制时翻转并压紧给 Skia；
///     可读行数再按 `Lock` 给出的 `curLength` 夹一次，不越过样本真实长度。

#include "cui/video/video_decoder.h"

#if defined(CUI_HAVE_MEDIA_FOUNDATION)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>

#if defined(__MINGW32__)
// 本机这份 mingw-w64 头里没有 `MFCreateMFByteStreamOnStream` 的原型（Windows SDK 的
// mfreadwrite.h 有，与同文件里其它 MFCreate* 一样是 `extern "C"` 导出）。补这一句
// 只是为了让「拿 mingw 做交叉语法校验」能跑通；真构建走的是 MSVC + SDK 头。
extern "C" {
HRESULT WINAPI MFCreateMFByteStreamOnStream(IStream* pStream, IMFByteStream** ppByteStream);
}
#endif

namespace cui {
namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() {
        reset();
        return &ptr_;
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    void attach(T* p) {
        reset();
        ptr_ = p;
    }
    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

/// 一段已经从 reader 拿到、还没交给上层的 PCM。
/// `bytes` 按 `AudioFormat::bytesPerFrame()` 排布（交织或平面都存同一份
/// 连续缓冲，平面只是解释方式不同，见 `fillAudioChunk`）。
struct AudioPiece {
    std::vector<uint8_t> bytes;
    int64_t ptsUs = 0;
    size_t frames = 0;
};

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

/// COM 单元是**每线程**的，而每个 `VideoPlayback` 都自起一条工作线程并在上面
/// `open()`（video_decoder.cpp 的 `workerLoop`）⇒ `CoInitializeEx` 不能跟 MFStartup
/// 一起塞进进程级 `call_once`：那样只有第一条线程初始化过，第二条起
/// `MFCreateAttributes` / `MFCreateSourceReader*` 全部 CO_E_NOTINITIALIZED。
struct ThreadCom {
    bool owned = false;
    ThreadCom() { owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ThreadCom() {
        if (owned)
            CoUninitialize();
    }
};

void ensureMfStarted() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    static thread_local ThreadCom com;
}

std::string hrText(const char* what, HRESULT hr) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (0x%08lX)", static_cast<unsigned long>(hr));
    return std::string(what) + buf;
}

class MFBackend : public VideoDecoderBackend {
public:
    ~MFBackend() override = default;

    bool open(const VideoSource& source, std::string& error) {
        ensureMfStarted();

        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.put(), 2))) {
            error = "MFCreateAttributes 失败";
            return false;
        }
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);

        HRESULT hr = S_OK;
        if (!source.bytes.empty()) {
            // 内存来源：IStream → IMFByteStream → SourceReader。
            ComPtr<IStream> stream;
            stream.attach(SHCreateMemStream(source.bytes.data(),
                                            static_cast<UINT>(source.bytes.size())));
            if (!stream) {
                error = "内存来源创建 IStream 失败";
                return false;
            }
            ComPtr<IMFByteStream> byteStream;
            hr = MFCreateMFByteStreamOnStream(stream.get(), byteStream.put());
            if (SUCCEEDED(hr))
                hr = MFCreateSourceReaderFromByteStream(byteStream.get(), attrs.get(),
                                                        reader_.put());
        } else {
            std::wstring url = utf8ToWide(source.uri);
            const std::wstring filePrefix = L"file://";
            if (url.rfind(filePrefix, 0) == 0) {
                url = url.substr(filePrefix.size());
                std::replace(url.begin(), url.end(), L'/', L'\\');
            }
            hr = MFCreateSourceReaderFromURL(url.c_str(), attrs.get(), reader_.put());
        }
        if (FAILED(hr) || !reader_) {
            error = hrText("MFCreateSourceReader 失败", hr);
            return false;
        }

        reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

        if (!readInfo(hr)) {
            error = hrText("读取视频流信息失败", hr);
            return false;
        }
        if (!configureOutput(hr)) {
            error = hrText("设置 RGB32 输出失败", hr);
            return false;
        }
        // 音轨：配不上就只关声音，`error` 一个字都不写（见文件头那条退路）。
        configureAudio();
        return true;
    }

    const VideoInfo& info() const override { return info_; }
    bool atEnd() const override { return eof_; }
    const std::string& error() const override { return error_; }

    // ── 音频（基类那四个虚函数的真实现）───────────────────────────

    bool hasAudioStream() const override { return audioEnabled_; }
    AudioFormat audioFormat() const override { return audioFormat_; }

    /// 先取 `audioQueue_`（`readFrame` 顺手分流过来的），队列空了才向 reader 要。
    /// 返回 false = 「这一轮没有新 PCM」，不代表出错也不代表结束（与 FFmpeg
    /// 后端同口径）；真正的结束记在 `audioEof_`，之后不再白问。
    bool readAudio(AudioChunk& out) override {
        if (!audioEnabled_ || !reader_)
            return false;
        if (!audioQueue_.empty()) {
            AudioPiece front = std::move(audioQueue_.front());
            audioQueue_.pop_front();
            audioView_.swap(front.bytes);
            return fillAudioChunk(out, front.ptsUs, front.frames);
        }
        if (audioEof_)
            return false;
        for (int guard = 0; guard < 32; ++guard) {
            DWORD flags = 0;
            DWORD streamIndex = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader_->ReadSample(audioStreamIndex_, 0, &streamIndex, &flags,
                                                  &timestamp, sample.put());
            if (flags & MF_SOURCE_READERF_ERROR) {
                disableAudio();
                return false;
            }
            if (FAILED(hr)) {
                // 音频读不出来不是「播放失败」：只把这一路关掉，画面照走。
                // （video processing 模式下 reader 把音频流收去自用就会走到这里。）
                disableAudio();
                return false;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                audioEof_ = true;
                return false;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                refreshAudioFormat();
            if (!sample)
                continue;  // 这一拍没有音频数据（STREAMTICK），再问一次
            LONGLONG time100 = timestamp;
            sample->GetSampleTime(&time100);
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer)
                continue;
            BYTE* data = nullptr;
            DWORD maxLength = 0;
            DWORD curLength = 0;
            if (FAILED(buffer->Lock(&data, &maxLength, &curLength)) || !data)
                continue;
            const int64_t frameBytes = audioFormat_.bytesPerFrame();
            const size_t frames =
                    frameBytes > 0 ? size_t(curLength) / size_t(frameBytes) : 0;
            if (frames > 0)
                audioView_.assign(data, data + frames * size_t(frameBytes));
            buffer->Unlock();
            if (frames == 0)
                continue;  // 空的一拍不算数
            return fillAudioChunk(out, time100 / 10, frames);
        }
        // 32 拍都不给样本：这一路当它坏了（宁可无声，也不要每拍都重试 32 次）。
        disableAudio();
        return false;
    }

    void flushAudio() override {
        audioQueue_.clear();
        audioView_.clear();
        // seek 之后流尾标记要清掉：reader 的位置已经回去了，音频也一样能再读。
        audioEof_ = false;
    }

    bool readFrame(VideoFrame& out) override {
        if (!reader_ || eof_)
            return false;

        for (int guard = 0; guard < 32; ++guard) {
            DWORD flags = 0;
            DWORD streamIndex = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                                   &streamIndex, &flags, &timestamp,
                                                   sample.put());
            if (FAILED(hr)) {
                error_ = hrText("ReadSample 失败", hr);
                eof_ = true;
                return false;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                eof_ = true;
                return false;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                refreshOutputType();
            }
            if (!sample)
                continue;  // STREAMTICK：没有新画面，继续取

            // 分流保护：reader 把音频样本从视频这一路递出来时存进队列，这一轮
            // 继续向视频要。只有描述符枚举出了**具体**下标才拦得住（虚拟下标
            // 永远不等于返回的 `streamIndex`）。
            if (audioEnabled_ && audioConcreteIndex_ == streamIndex) {
                stashAudioSample(sample.get(), timestamp);
                continue;
            }

            if (!copySample(sample.get(), out, timestamp)) {
                eof_ = true;
                return false;
            }
            return true;
        }
        error_ = "连续 32 次 ReadSample 没有拿到画面";
        eof_ = true;
        return false;
    }

    bool seekTo(int64_t ptsUs) override {
        if (!reader_)
            return false;
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = ptsUs * 10;  // 100ns
        const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
        if (FAILED(hr))
            return false;
        eof_ = false;
        // 音频：未交出的 PCM 全部作废（引擎随后还会叫一次 `flushAudio()`，
        // 同一个动作，重复无害），流尾标记也跟着清。
        flushAudio();
        return true;
    }

private:
    bool readInfo(HRESULT& hr) {
        ComPtr<IMFMediaType> native;
        hr = reader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, native.put());
        if (FAILED(hr) || !native)
            return false;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(native.get(), MF_MT_FRAME_SIZE, &w, &h);
        UINT32 rateNum = 0, rateDen = 1;
        MFGetAttributeRatio(native.get(), MF_MT_FRAME_RATE, &rateNum, &rateDen);
        UINT32 rotation = MFGetAttributeUINT32(native.get(), MF_MT_VIDEO_ROTATION, 0);

        info_.width = int(w);
        info_.height = int(h);
        info_.frameRate = rateDen ? double(rateNum) / double(rateDen) : 0.0;
        info_.rotation = int(rotation) % 360;
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = "mediafoundation";

        // 时长：SourceReader 级别的 presentation attribute（MF_PD_DURATION）。
        // 媒体源的 PresentationDescriptor 在部分容器（如本仓运行时拼的 AVI）
        // 上 GetServiceForStream 会拒绝，而 GetPresentationAttribute 走的是
        // SourceReader 已经解出的同一份属性，稳妥得多。
        PROPVARIANT durVar;
        PropVariantInit(&durVar);
        hr = reader_->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &durVar);
        if (SUCCEEDED(hr)) {
            if (durVar.vt == VT_UI8)
                info_.duration = std::chrono::microseconds(int64_t(durVar.uhVal.QuadPart / 10));
            else if (durVar.vt == VT_I8)
                info_.duration = std::chrono::microseconds(int64_t(durVar.hVal.QuadPart / 10));
        }
        PropVariantClear(&durVar);

        // 音频轨存在性：PresentationDescriptor 上的流描述符。
        ComPtr<IMFPresentationDescriptor> desc;
        hr = reader_->GetServiceForStream(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), GUID_NULL,
                IID_IMFPresentationDescriptor, reinterpret_cast<void**>(desc.put()));
        if (SUCCEEDED(hr) && desc) {
            DWORD count = 0;
            if (SUCCEEDED(desc->GetStreamDescriptorCount(&count))) {
                for (DWORD i = 0; i < count; ++i) {
                    BOOL selected = FALSE;
                    ComPtr<IMFStreamDescriptor> sd;
                    if (FAILED(desc->GetStreamDescriptorByIndex(i, &selected, sd.put())) || !sd)
                        continue;
                    ComPtr<IMFMediaTypeHandler> handler;
                    if (FAILED(sd->GetMediaTypeHandler(handler.put())) || !handler)
                        continue;
                    GUID major{};
                    if (SUCCEEDED(handler->GetMajorType(&major)) && major == MFMediaType_Audio) {
                        info_.hasAudio = true;
                        // 拿到具体下标更好：`ReadSample` / `SetCurrentMediaType` 都吃
                        // 下标，而返回的 `streamIndex` 只能是具体的那一个（分流保护
                        // 靠它）。枚举不上时退回虚拟下标，只是分流保护就失效。
                        if (audioConcreteIndex_ == DWORD(-1)) {
                            audioConcreteIndex_ = i;
                            audioStreamIndex_ = i;
                        }
                    }
                }
            }
        }

        // 采样率/声道数（= `configureAudio` 申请输出类型的输入）向 reader 问一条
        // native 媒体类型：这一句同时兼顾两种局面 ——
        //   - 描述符那段被 `GetServiceForStream` 拒绝（本仓运行时拼的 AVI 就是一例，
        //     与时长换招的理由相同）⇒ 连有没有音轨都不知道，这里用虚拟下标兜底；
        //   - 描述符认出来了但采样率还空着 ⇒ 用已经拿到的具体下标再问一次。
        // 问不到就保持 `audioRate_ == 0`：`configureAudio` 会直接早退当没有音轨，
        // `info_.hasAudio`（容器里确实有音轨）却不会被谎改成 false。
        if (audioRate_ <= 0 || audioChannels_ <= 0) {
            const DWORD probeIndex = audioConcreteIndex_ != static_cast<DWORD>(-1)
                                             ? audioConcreteIndex_
                                             : static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
            ComPtr<IMFMediaType> nativeAudio;
            const HRESULT audioHr = reader_->GetNativeMediaType(probeIndex, 0, nativeAudio.put());
            if (SUCCEEDED(audioHr) && nativeAudio) {
                const UINT32 rate =
                        MFGetAttributeUINT32(nativeAudio.get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
                const UINT32 channels =
                        MFGetAttributeUINT32(nativeAudio.get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
                if (rate > 0 && channels > 0) {
                    info_.hasAudio = true;
                    audioRate_ = int(rate);
                    audioChannels_ = int(channels);
                }
            }
        }
        hr = S_OK;
        return true;
    }

    // ── 音频的实现细节 ──────────────────────────────────────────────

    /// 选上音轨并配好输出类型。任何一步不行就整条退回「只看画面」：不写
    /// `error_`、不改 `info_.hasAudio`（它报的是容器里有没有音轨，与 FFmpeg
    /// 后端同口径），与 Android 的 `discardAudio()` 一模一样。
    void configureAudio() {
        if (!reader_ || audioRate_ <= 0 || audioChannels_ <= 0)
            return;
        if (FAILED(reader_->SetStreamSelection(audioStreamIndex_, TRUE)))
            return;
        // 首选 IEEE float（MF 的音频解码器原生就是浮点，省一次转换），
        // 系统拒绝再退 16bit 定点。两种布点 `AudioRing` 都直接吃。
        if (tryAudioOutputType(AudioFormat{audioRate_, audioChannels_,
                                           AudioSampleFormat::F32Interleaved}) ||
            tryAudioOutputType(AudioFormat{audioRate_, audioChannels_,
                                           AudioSampleFormat::S16Interleaved})) {
            audioEnabled_ = true;
            return;
        }
        audioFormat_ = AudioFormat{};
        reader_->SetStreamSelection(audioStreamIndex_, FALSE);
    }

    /// 向系统申请一种音频输出类型；成就把「实际生效的那份」读进
    /// `audioFormat_` 并返回 true（采样率/声道数/是不是平面都可能与申请的不同）。
    bool tryAudioOutputType(const AudioFormat& want) {
        const bool pcm = want.format == AudioSampleFormat::S16Interleaved;
        const UINT32 sampleBytes = pcm ? 2u : 4u;
        ComPtr<IMFMediaType> out;
        if (FAILED(MFCreateMediaType(out.put())) || !out)
            return false;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, pcm ? MFAudioFormat_PCM : MFAudioFormat_Float);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(want.sampleRate));
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(want.channels));
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 8u * sampleBytes);
        const UINT32 blockAlign = UINT32(want.channels) * sampleBytes;
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, UINT32(want.sampleRate) * blockAlign);
        if (FAILED(reader_->SetCurrentMediaType(audioStreamIndex_, nullptr, out.get())))
            return false;
        return refreshAudioFormat();
    }

    /// 以系统当前生效的音频输出类型为准重写 `audioFormat_`。返回它是不是还能用。
    bool refreshAudioFormat() {
        AudioFormat next;
        ComPtr<IMFMediaType> current;
        if (FAILED(reader_->GetCurrentMediaType(audioStreamIndex_, current.put())) || !current) {
            audioFormat_ = next;
            return false;
        }
        UINT32 rate = 0;
        UINT32 channels = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) && rate > 0)
            next.sampleRate = int(rate);
        if (SUCCEEDED(current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) && channels > 0)
            next.channels = int(channels);
        GUID sub = MFAudioFormat_Float;
        if (FAILED(current->GetGUID(MF_MT_SUBTYPE, &sub)))
            sub = MFAudioFormat_PCM;
        if (sub == MFAudioFormat_PCM) {
            next.format = AudioSampleFormat::S16Interleaved;
        } else {
            // `MF_MT_ALL_SAMPLES_INDEPENDENT` 非 0 = 每个声道自己的连续空间，
            // 也就是**平面** float；它直接决定 `AudioChunk` 填哪一组指针。
            next.format =
                    MFGetAttributeUINT32(current.get(), MF_MT_ALL_SAMPLES_INDEPENDENT, 0) != 0
                            ? AudioSampleFormat::F32Planar
                            : AudioSampleFormat::F32Interleaved;
        }
        audioFormat_ = next;
        return next.isValid();
    }

    /// 把 `audioView_` 里的 `frames` 帧按 `audioFormat_` 解释成 `AudioChunk`。
    /// 指针的有效期就是契约那一句：到下一次 `readAudio`/`flushAudio`。
    bool fillAudioChunk(AudioChunk& out, int64_t ptsUs, size_t frames) {
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        if (frameBytes <= 0 || frames == 0 || audioView_.size() < frames * size_t(frameBytes))
            return false;
        out.frames = frames;
        out.ptsUs = ptsUs;
        out.format = audioFormat_.format;
        out.s16 = nullptr;
        out.f32 = nullptr;
        for (int i = 0; i < kAudioMaxPlanes; ++i)
            out.plane[i] = nullptr;
        switch (audioFormat_.format) {
            case AudioSampleFormat::S16Interleaved:
                out.s16 = reinterpret_cast<const int16_t*>(audioView_.data());
                break;
            case AudioSampleFormat::F32Interleaved:
                out.f32 = reinterpret_cast<const float*>(audioView_.data());
                break;
            case AudioSampleFormat::F32Planar: {
                const float* base = reinterpret_cast<const float*>(audioView_.data());
                const int planes = std::min(audioFormat_.channels, kAudioMaxPlanes);
                for (int c = 0; c < planes; ++c)
                    out.plane[c] = base + size_t(c) * frames;
                break;
            }
        }
        return true;
    }

    /// `readFrame` 那一路撞见音频样本时的分流：拷进队列，等 `readAudio` 来取。
    void stashAudioSample(IMFSample* sample, LONGLONG firstTimestamp) {
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        // 队列积压到 64 段（≈1 秒）还不消费 = 上层已经不想要 PCM 了，丢掉最新的
        // 比无限成长好（与 FFmpeg 后端的背压同口径）。
        if (frameBytes <= 0 || audioQueue_.size() >= 64)
            return;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer)
            return;
        BYTE* data = nullptr;
        DWORD maxLength = 0;
        DWORD curLength = 0;
        if (FAILED(buffer->Lock(&data, &maxLength, &curLength)) || !data)
            return;
        LONGLONG time100 = firstTimestamp;
        sample->GetSampleTime(&time100);
        AudioPiece piece;
        piece.frames = size_t(curLength) / size_t(frameBytes);
        if (piece.frames > 0)
            piece.bytes.assign(data, data + piece.frames * size_t(frameBytes));
        piece.ptsUs = time100 / 10;  // 100ns → µs
        buffer->Unlock();
        if (piece.frames > 0)
            audioQueue_.push_back(std::move(piece));
    }

    /// 只关声音：PCM 这一路作废，画面照走（不写 `error_`）。
    void disableAudio() {
        audioEnabled_ = false;
        audioQueue_.clear();
        audioView_.clear();
        audioFormat_ = AudioFormat{};
        if (reader_)
            reader_->SetStreamSelection(audioStreamIndex_, FALSE);
    }

    bool configureOutput(HRESULT& hr) {
        ComPtr<IMFMediaType> out;
        hr = MFCreateMediaType(out.put());
        if (FAILED(hr))
            return false;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, UINT32(info_.width), UINT32(info_.height));
        MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out.get());
        if (FAILED(hr))
            return false;
        refreshOutputType();
        return true;
    }

    /// 以实际生效的输出类型为准（系统可能做了旋转/缩放）。
    void refreshOutputType() {
        ComPtr<IMFMediaType> current;
        if (FAILED(reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                                current.put())))
            return;
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &w, &h)) && w && h) {
            width_ = int(w);
            height_ = int(h);
        }
        LONG stride = 0;
        stride_ = 0;
        bottomUp_ = false;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride)))) {
            bottomUp_ = stride < 0;
            // 负 stride = 自下而上 DIB，行距取绝对值。系统可能把行对齐到 16 字节，
            // 所以源行距不一定等于 width*4（copySample 按它逐行取，再压紧给 Skia）。
            stride_ = stride < 0 ? size_t(-stride) : size_t(stride);
        }
        UINT32 rotation = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation)))
            info_.rotation = int(rotation) % 360;
    }

    bool copySample(IMFSample* sample, VideoFrame& out, LONGLONG firstTimestamp) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer) {
            error_ = "ConvertToContiguousBuffer 失败";
            return false;
        }
        BYTE* data = nullptr;
        DWORD maxLength = 0;
        DWORD curLength = 0;
        if (FAILED(buffer->Lock(&data, &maxLength, &curLength)) || !data) {
            error_ = "Lock 媒体缓冲失败";
            return false;
        }
        const int w = width_ > 0 ? width_ : info_.width;
        const int h = height_ > 0 ? height_ : info_.height;
        const size_t rowBytes = size_t(w) * 4;
        // 源行距以媒体类型报的为准（系统可能对齐到 16 字节），没报就按紧凑算。
        const size_t srcStride = stride_ > 0 ? stride_ : rowBytes;
        if (w <= 0 || h <= 0 || srcStride < rowBytes) {
            error_ = "RGB32 帧尺寸/行距不合法";
            buffer->Unlock();
            return false;
        }
        // 缓冲比「整幅画面」短时只取装得下的行，绝不越过 Lock 给出的 curLength
        // （maxLength 是容量、curLength 才是这一拍真有的字节数）。
        const size_t usableRows = std::min(size_t(h), size_t(curLength) / srcStride);
        if (usableRows == 0) {
            error_ = "RGB32 样本为空";
            buffer->Unlock();
            return false;
        }
        // MF 的 RGB32 是 BGRX：第 4 字节未定义，多数编码器给 0。声明成 kOpaque
        // 只影响 Skia 的 CPU 光栅，Graphite 上传纹理后仍然按原始通道采样 ——
        // X=0 就成了「整帧 alpha 0」，画到屏幕上是一块透明洞。复制时把 alpha
        // 归一成 0xFF，让像素数据与 kOpaque 声明一致。
        auto copyRow = [](uint8_t* dst, const uint8_t* src, size_t bytes) {
            std::memcpy(dst, src, bytes);
            for (size_t i = 3; i < bytes; i += 4)
                dst[i] = 0xFF;
        };
        std::vector<uint8_t> straightened(rowBytes * usableRows, 0xFF);
        for (size_t y = 0; y < usableRows; ++y) {
            // 自下而上 DIB：内存里的第 0 行是画面的最后一行。
            const size_t dstRow = bottomUp_ ? usableRows - 1 - y : y;
            copyRow(straightened.data() + rowBytes * dstRow, data + srcStride * y, rowBytes);
        }
        sk_sp<SkData> pixels = SkData::MakeWithCopy(straightened.data(), straightened.size());
        buffer->Unlock();

        LONGLONG timestamp = firstTimestamp;
        sample->GetSampleTime(&timestamp);
        const SkImageInfo imageInfo =
                SkImageInfo::Make(w, int(usableRows), kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
        out.image = SkImages::RasterFromData(imageInfo, std::move(pixels), rowBytes);
        out.ptsUs = timestamp / 10;  // 100ns → µs
        return out.image != nullptr;
    }

    ComPtr<IMFSourceReader> reader_;
    VideoInfo info_;
    std::string error_;
    int width_ = 0;
    int height_ = 0;
    /// 源缓冲的行距（绝对值）；0 = 媒体类型没报，按 width*4 紧凑算。
    size_t stride_ = 0;
    bool bottomUp_ = false;
    bool eof_ = false;

    // ── 音频状态 ──────────────────────────────────────────────────────

    /// `ReadSample` / `SetCurrentMediaType` 用的流下标。默认虚拟下标
    /// `FIRST_AUDIO_STREAM`；描述符枚举出具体那一条时换成它（虚拟下标永远不会
    /// 等于 `ReadSample` 返回的 `streamIndex`，分流保护因此需要具体下标）。
    DWORD audioStreamIndex_ = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    /// `DWORD(-1)` = 没枚举出来，只有虚拟下标可用。
    DWORD audioConcreteIndex_ = static_cast<DWORD>(-1);
    /// 容器报出来的采样率/声道数 —— 向系统申请输出类型的输入（不是最终生效值，
    /// 那一份在 `audioFormat_`）。为 0 ⇒ `configureAudio` 早退当没有音轨。
    int audioRate_ = 0;
    int audioChannels_ = 0;
    /// 输出类型真的配上、并且还没坏 ⇒ 才交 PCM。`info_.hasAudio` 报的是容器，
    /// 两者可以不一致（与 FFmpeg 后端 `hasStream(AUDIO)` / 解码器起得来的关系同形）。
    bool audioEnabled_ = false;
    bool audioEof_ = false;
    AudioFormat audioFormat_;
    /// `readFrame` 那一路撞见音频样本时分流出来的 PCM，等 `readAudio` 来取。
    std::deque<AudioPiece> audioQueue_;
    /// 当前这一段 PCM 的字节；`AudioChunk` 里的指针指向它（有效期见基类契约）。
    std::vector<uint8_t> audioView_;
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> createMediaFoundationBackend(const VideoSource& source,
                                                                  std::string& error) {
    auto backend = std::make_unique<MFBackend>();
    if (!backend->open(source, error))
        return nullptr;
    return backend;
}

/// 运行期探测：mfplat 是链接期依赖（可执行文件一起来就加载好了），Windows 7
/// 之后系统必带，没有「编进来了但这台机器没有 MF」这种局面；
/// 「这个容器能不能解」只能在 `open()` 里见分晓，不在这里预测。
bool mediaFoundationVideoAvailable(std::string& note) {
    (void)note;
    return true;
}

}  // namespace cui

#endif  // CUI_HAVE_MEDIA_FOUNDATION
