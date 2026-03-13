/// @file foundation/video_decoder.cpp
/// @brief 视频解码核心的共享部分：按后端表挑一个、动图后端、播放引擎。
///
/// 两条路线（各端系统媒体栈 / FFmpeg）都在本包，候选项、优先级、运行期探测
/// 集中在 `video_backends.{h,cpp}`；真正能解容器的后端各自成文件：
/// `video_decoder_win32.cpp`（Media Foundation）、`video_decoder_darwin.mm`
/// （AVFoundation）、`video_decoder_android.cpp`（AMediaCodec）、
/// `video_decoder_web.cpp`（HTMLVideoElement）、`video_decoder_gst.cpp`
/// （GStreamer）、`video_decoder_ohos.cpp`（OH_AVDemuxer）、
/// `video_decoder_ffmpeg.cpp`（FFmpeg）；本文件只负责「按顺序试 + 统一的时钟
/// 与播放状态」。

#include "cui/video/video_decoder.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include "cui/foundation/image_data.h"
#include "cui/foundation/image_provider.h"
#include "video_backends.h"

namespace cui {

// ── VideoSource ─────────────────────────────────────────────────────────────

VideoSource VideoSource::file(std::string path) {
    VideoSource s;
    s.type = Type::File;
    s.uri = std::move(path);
    return s;
}

VideoSource VideoSource::network(std::string url) {
    VideoSource s;
    s.type = Type::Network;
    s.uri = std::move(url);
    return s;
}

VideoSource VideoSource::asset(std::string name) {
    VideoSource s;
    s.type = Type::Asset;
    s.uri = std::move(name);
    return s;
}

VideoSource VideoSource::memory(std::vector<uint8_t> bytes, std::string formatHint) {
    VideoSource s;
    s.type = Type::File;
    s.bytes = std::move(bytes);
    s.formatHint = std::move(formatHint);
    return s;
}

// ── 能力探测 ────────────────────────────────────────────────────────────────

std::string VideoCapabilities::summary() const {
    // 只报「实际会用的那一个」（`nativeName`），免得编进来但运行期用不了时
    // 谎报。字段 `mediaFoundation` / `ffmpeg` / `systemDecoder` 仍然表示
    // 「编译期有没有」，给调用方自己区分这两件事。
    // （不读编译宏，这样单测能自己造出两种取值分别验。）
    if (nativeName.empty()) {
        return "只有动图后端（GIF/WebP/APNG）；MP4 需要一个容器解码后端（系统端或 FFmpeg）";
    }
    std::string out = nativeName;
    out += " + 动图后端";
    if (nativeAudio)
        out += "（含音频）";
    return out;
}

VideoCapabilities videoCapabilities() {
    VideoCapabilities caps;
#if defined(CUI_HAVE_MEDIA_FOUNDATION)
    caps.mediaFoundation = true;
#endif
#if defined(CUI_HAVE_FFMPEG)
    caps.ffmpeg = true;
#endif

    // `nativeAudio` **不在这里按编译宏给**：宏只说明「某条出声设备编进来了」，
    // 而这个字段问的是「实际会用的那条后端能不能出声」。两者在「两条路线都关了 /
    // 运行期一个后端都不可用」时不一致，而那种情况下 `summary()` 报的是「只有动图
    // 后端」⇒ 字段跟着宏报 true 就与文案自相矛盾（test_video_audio 第 7 节钉的
    // 就是这条一致性）。注意这里只看「路线上有没有配套的出声设备」：设备真的
    // 打不开属于运行期退回无声，不改变这里的申报。
    //
    // `nativeName` = 按当前 preference 真正会接走解码的那一个（第一个运行期
    // 可用的）。注意只记第一个：`createNativeVideoBackend` 也只试到它为止。
    for (const auto& b : orderedVideoBackends()) {
        std::string note;
        if (!backendAvailable(b, note))
            continue;
        caps.systemDecoder = b.kind == VideoBackendKind::System;
        caps.systemName = caps.systemDecoder ? b.reg.name : std::string{};
        caps.systemAudioName = b.selfAudio ? std::string("后端自带") : b.reg.audioName;
        caps.nativeName = b.reg.name;
        caps.platformDecoder = caps.systemDecoder;
        caps.nativeAudio = b.selfAudio || !b.reg.audioName.empty();
        break;
    }
    return caps;
}

// ── 动图后端（GIF / 动画 WebP / APNG） ──────────────────────────────────────

namespace {

/// 音频队列的喂数据阈值：100 毫秒。远小于 `AudioRing` 的 2 秒容量，
/// 所以 `push()` 永远写得下（不会默默丢采样）。
constexpr int64_t kAudioLeadUs = 100 * 1000;
/// 视频对齐主时钟的容差：在这个窗口内直接上屏，不等。
constexpr int64_t kSyncToleranceUs = 20 * 1000;

/// 原生后端（真正能解容器的那一个），按后端表顺序只选一个。
/// 顺序 = 注册表→系统端→FFmpeg（`orderedVideoBackends()` 里已按 preference 排好）；
/// 每一项先过运行期探测，再用工厂试。全都没能打开时返回 nullptr；
/// 没编进来任何原生后端也不设 `error`（调用方退到动图）。
std::unique_ptr<VideoDecoderBackend> createNativeVideoBackend(const VideoSource& source,
                                                              std::string& error) {
    for (const auto& b : orderedVideoBackends()) {
        if (!b.reg.factory)
            continue;
        std::string note;
        if (!backendAvailable(b, note))
            continue;  // 这台机器用不了：跳过，不当错误（最后会报能力文案）
        std::string backendError;
        auto backend = b.reg.factory(source, backendError);
        if (backend)
            return backend;
        // 某个后端拒了这个容器（不支持的编码/来源）时继续往下试，但留下
        // 最具体的那条原因，免得最终报成含糊的「没有可用后端」。
        if (error.empty() && !backendError.empty())
            error = backendError;
    }
    return nullptr;
}

bool startsWith(const std::vector<uint8_t>& b, const char* magic, size_t n) {
    return b.size() >= n && std::memcmp(b.data(), magic, n) == 0;
}

/// 只按文件头判断「是不是 Skia 能解的图像」。真视频容器一律交给原生后端。
VideoMediaKind sniffKind(const std::vector<uint8_t>& head, const std::string& hint) {
    if (head.size() >= 6 && (startsWith(head, "GIF87a", 6) || startsWith(head, "GIF89a", 6)))
        return VideoMediaKind::AnimatedImage;
    if (head.size() >= 8 && startsWith(head, "\x89PNG\r\n\x1a\n", 8))
        return VideoMediaKind::AnimatedImage;  // APNG；单帧 PNG 也走这里（后台按帧数判定）
    if (head.size() >= 12 && startsWith(head, "RIFF", 4) && std::memcmp(head.data() + 8, "WEBP", 4) == 0)
        return VideoMediaKind::AnimatedImage;
    if (head.size() >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
        return VideoMediaKind::StaticImage;
    if (head.size() >= 2 && head[0] == 'B' && head[1] == 'M')
        return VideoMediaKind::StaticImage;

    std::string h = hint;
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (h == "gif" || h == "webp" || h == "png" || h == "apng")
        return VideoMediaKind::AnimatedImage;
    if (h == "jpg" || h == "jpeg" || h == "bmp")
        return VideoMediaKind::StaticImage;
    return VideoMediaKind::Unknown;
}

bool readProbeBytes(const VideoSource& source, std::vector<uint8_t>& out, std::string& error) {
    if (!source.bytes.empty()) {
        out = source.bytes;
        return true;
    }
    if (source.type == VideoSource::Type::Network) {
        error.clear();
        return false;  // 网络：先让原生后端直接流式打开
    }
    std::ifstream in(source.uri, std::ios::binary);
    if (!in) {
#if defined(__EMSCRIPTEN__)
        // wasm 里「本地路径」多半其实是页面相对 URL（MEMFS 只有 --preload-file
        // 塞进来的那几份，容器文件不会在那儿）。读不到不当错误，交给后端按
        // URL 打开 —— 否则浏览器后端永远轮不到上场。
        error.clear();
        return false;
#else
        error = "无法打开视频文件：" + source.uri;
        return false;
#endif
    }
    out.assign(64, 0);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(in.gcount()));
    return true;
}

/// 动图 / 单帧图后端：所有帧在打开时用 SkCodec 解好并合成（GIF 的增量帧
/// 由 `decodeImage` 处理），播放只是按累计时间戳吐帧。
class AnimatedImageBackend : public VideoDecoderBackend {
public:
    explicit AnimatedImageBackend(DecodedImagePtr decoded) : decoded_(std::move(decoded)) {
        const auto& frames = decoded_->frames;
        size_t totalMs = 0;
        for (const auto& f : frames)
            totalMs += static_cast<size_t>(frameDurationMs(f.durationMs));
        info_.width = decoded_->width();
        info_.height = decoded_->height();
        info_.duration = std::chrono::milliseconds(totalMs);
        info_.kind = frames.size() > 1 ? VideoMediaKind::AnimatedImage : VideoMediaKind::StaticImage;
        info_.codec = decoded_->format;
        info_.hasAudio = false;
        if (!frames.empty() && totalMs > 0)
            info_.frameRate = double(frames.size()) * 1000.0 / double(totalMs);
    }

    const VideoInfo& info() const override { return info_; }

    bool readFrame(VideoFrame& out) override {
        const auto& frames = decoded_->frames;
        if (index_ >= frames.size()) {
            atEnd_ = true;
            return false;
        }
        const auto& f = frames[index_];
        out.image = f.image;
        out.ptsUs = startUs_;
        startUs_ += frameDurationMs(f.durationMs) * 1000;
        ++index_;
        atEnd_ = index_ >= frames.size();
        return out.image != nullptr;
    }

    bool seekTo(int64_t ptsUs) override {
        const auto& frames = decoded_->frames;
        int64_t acc = 0;
        index_ = 0;
        startUs_ = 0;
        while (index_ < frames.size()) {
            const int64_t dur = frameDurationMs(frames[index_].durationMs) * 1000;
            if (acc + dur > ptsUs)
                break;
            acc += dur;
            startUs_ = acc;
            ++index_;
        }
        atEnd_ = index_ >= frames.size();
        return true;
    }

    bool atEnd() const override { return atEnd_; }
    const std::string& error() const override { return error_; }

private:
    static int frameDurationMs(int ms) {
        // 浏览器口径：GIF 的 0ms 延迟按 100ms 处理，避免「一口气播完」。
        return ms > 0 ? ms : 100;
    }

    DecodedImagePtr decoded_;
    VideoInfo info_;
    size_t index_ = 0;
    int64_t startUs_ = 0;
    bool atEnd_ = false;
    std::string error_;
};

DecodedImagePtr decodeSourceImage(const VideoSource& source, std::string& error) {
    if (!source.bytes.empty()) {
        auto decoded = decodeImage(source.bytes);
        if (!decoded || !*decoded) {
            error = decoded && !decoded->error.empty() ? decoded->error : "动图解码失败";
            return nullptr;
        }
        return decoded;
    }
    if (source.type == VideoSource::Type::Network) {
        if (!hasHttpTransport()) {
            error = "没有网络传输后端，无法加载网络动图";
            return nullptr;
        }
        std::vector<uint8_t> body;
        std::string httpError;
        auto cancel = std::make_shared<ImageCancellation>();
        if (!currentHttpTransport()(source.uri, {}, body, nullptr, cancel, httpError)) {
            error = "下载失败：" + httpError;
            return nullptr;
        }
        auto decoded = decodeImage(body);
        if (!decoded || !*decoded) {
            error = decoded && !decoded->error.empty() ? decoded->error : "动图解码失败";
            return nullptr;
        }
        return decoded;
    }
    auto decoded = decodeImageFromFile(source.uri);
    if (!decoded || !*decoded) {
        error = decoded && !decoded->error.empty() ? decoded->error : ("无法解码：" + source.uri);
        return nullptr;
    }
    return decoded;
}

std::unique_ptr<VideoDecoderBackend> createAnimatedImageBackend(const VideoSource& source,
                                                                std::string& error) {
    auto decoded = decodeSourceImage(source, error);
    if (!decoded)
        return nullptr;
    return std::make_unique<AnimatedImageBackend>(std::move(decoded));
}

std::unique_ptr<VideoDecoderBackend> createBackend(const VideoSource& source, std::string& error) {
    std::vector<uint8_t> head;
    std::string probeError;
    const bool probed = readProbeBytes(source, head, probeError);

    if (source.type == VideoSource::Type::Network) {
        // 网络来源没法先嗅探（除非下载整份）；原生后端能直接流式打开，优先。
        auto native = createNativeVideoBackend(source, error);
        if (native)
            return native;
        // 没有原生后端 / 打不开：退化成「下载整份再当动图解」。
        std::string imageError;
        if (auto image = createAnimatedImageBackend(source, imageError))
            return image;
        if (!error.empty())
            error += "；" + imageError;
        else
            error = imageError;
        return nullptr;
    }

    if (probed) {
        const VideoMediaKind kind = sniffKind(head, source.formatHint);
        if (kind == VideoMediaKind::AnimatedImage || kind == VideoMediaKind::StaticImage) {
            if (auto image = createAnimatedImageBackend(source, error))
                return image;
            return nullptr;  // 图片解不出来就直接失败，不再尝试视频后端
        }
    } else if (!probeError.empty()) {
        error = probeError;
        return nullptr;
    }

    auto native = createNativeVideoBackend(source, error);
    if (native)
        return native;

    // 没有原生后端：再试一次动图（可能是不认识的图片格式）。
    std::string imageError;
    if (auto image = createAnimatedImageBackend(source, imageError))
        return image;

    if (error.empty())
        error = "没有可用的视频解码后端（" + videoCapabilities().summary() + "）";
    return nullptr;
}

}  // namespace

// ── VideoPlayback ───────────────────────────────────────────────────────────

bool VideoPlayback::pumpDrivenPlatform() {
#if defined(__EMSCRIPTEN__)
    // 单线程 wasm（没开 -pthread）里 `std::thread` 一构造就抛 system_error，
    // 而 `ThreadPool::submit()` 本来就是就地执行。所以这条路上的后端必须在
    // 调用线程上同步打开，取帧交给 GUI 线程逐帧 `pumpStep()`。
    return true;
#else
    return false;
#endif
}

std::shared_ptr<VideoPlayback> VideoPlayback::create(const VideoSource& source, std::string& error) {
    auto playback = std::shared_ptr<VideoPlayback>(new VideoPlayback());

    if (pumpDrivenPlatform()) {
        std::string openError;
        if (!playback->openInline(source, openError)) {
            error = openError.empty() ? "视频打开失败" : openError;
            return nullptr;
        }
        error.clear();
        return playback;
    }

    VideoPlayback* raw = playback.get();
    playback->worker_ = std::thread([raw, source] { raw->workerLoop(source); });

    {
        std::unique_lock<std::mutex> lock(playback->mutex_);
        playback->cv_.wait(lock, [&] { return playback->initDone_; });
    }
    if (!playback->backend_) {
        error = playback->error_.empty() ? "视频打开失败" : playback->error_;
        playback->dispose();
        return nullptr;
    }
    error.clear();
    return playback;
}

std::shared_ptr<VideoPlayback> VideoPlayback::createWithBackend(
        std::unique_ptr<VideoDecoderBackend> backend, std::unique_ptr<AudioSink> sink) {
    auto playback = std::shared_ptr<VideoPlayback>(new VideoPlayback());
    std::lock_guard<std::mutex> lock(playback->mutex_);
    playback->backend_ = std::move(backend);
    playback->sink_ = std::move(sink);
    if (playback->backend_) {
        playback->bindBackendLocked();
        // 注入的 sink 已经 open 过（测试自己造），这里只把音量对齐。
        if (playback->sink_)
            playback->sink_->setVolume(playback->effectiveVolume());
    }
    playback->initDone_ = true;
    return playback;
}

bool VideoPlayback::openInline(const VideoSource& source, std::string& error) {
    std::string openError;
    auto backend = createBackend(source, openError);
    if (!backend) {
        error = openError.empty() ? "没有可用的视频解码后端" : openError;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    backend_ = std::move(backend);
    bindBackendLocked();
    initDone_ = true;
    return true;
}

void VideoPlayback::bindBackendLocked() {
    info_ = backend_->info();
    baseWall_ = std::chrono::steady_clock::now();
    pumpDriven_ = backend_->pumpDriven();
    if (!sink_ && backend_->hasAudioStream()) {
        // 设备起不来不算错误：退回「无声 + 墙钟」，与后端解不开音频时同一条退路
        // （现在每条路都配了 sink：ALSA / AAudio / AudioQueue / WASAPI /
        // OH_AudioRenderer，哪个都可能打不开）。
        // 顺序：注册表里带来的设备（应用/单测塞的，通常与它的后端配套）
        // → 内置的平台设备。前者起不来时照样往下退，不至于整条路变无声。
        auto openSink = [&](std::unique_ptr<AudioSink> candidate) {
            if (!candidate)
                return false;
            std::string sinkError;
            if (!candidate->open(backend_->audioFormat(), sinkError))
                return false;
            candidate->setVolume(effectiveVolume());
            sink_ = std::move(candidate);
            return true;
        };
        if (!openSink(createRegisteredAudioSink()))
            openSink(createPlatformAudioSink());
    }
    if (sink_)
        sink_->flush(baseMediaUs_);
    if (pumpDriven_ && backend_)
        backend_->pumpSetVolume(effectiveVolume());
}

VideoPlayback::~VideoPlayback() { dispose(); }

void VideoPlayback::dispose() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ && !worker_.joinable())
            return;
        stop_ = true;
        playing_ = false;
        if (sink_ && audioRunning_) {
            sink_->stop();
            audioRunning_ = false;
        }
        if (pumpDriven_ && backend_)
            backend_->pumpSetPlaying(false);
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    // 后端仍在工作线程里释放（COM/MF 有线程亲和，销毁留在同一线程）；
    // 音频设备在 join 之后关掉，所以上面那些访问 sink_ 的分支不会再并发。
    std::lock_guard<std::mutex> lock(mutex_);
    sink_.reset();
}

void VideoPlayback::play() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
            return;
        if (completed_) {
            completed_ = false;
            pendingSeekUs_ = 0;
        }
        baseWall_ = std::chrono::steady_clock::now();
        playing_ = true;
        syncAudioDeviceLocked();
        if (pumpDriven_ && backend_)
            backend_->pumpSetPlaying(true);
    }
    cv_.notify_all();
}

void VideoPlayback::pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
            return;
        // 先在仍 `playing_` 的时候把位置问出来（音频/后端都是活的），
        // 再置假 —— `positionLocked()` 在暂停后直接回 `baseMediaUs_`。
        if (playing_)
            baseMediaUs_ = positionLocked();
        playing_ = false;
        syncAudioDeviceLocked();
        if (pumpDriven_ && backend_)
            backend_->pumpSetPlaying(false);
    }
    cv_.notify_all();
}

void VideoPlayback::seekTo(std::chrono::microseconds target) {
    int64_t t = std::max<int64_t>(0, target.count());
    if (info_.duration.count() > 0)
        t = std::min<int64_t>(t, info_.duration.count());
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_)
        return;
    baseMediaUs_ = t;
    baseWall_ = std::chrono::steady_clock::now();
    completed_ = false;
    pendingSeekUs_ = t;
    publishOnce_ = !playing_;
    // 已解出来但还没到点的泵驱动帧跟着作废（它属于旧位置）。
    staged_ = VideoFrame{};
    syncAudioDeviceLocked();
    cv_.notify_all();
}

void VideoPlayback::setPlaybackSpeed(double speed) {
    if (speed <= 0.0)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_ || speed_ == speed)
        return;
    const int64_t at = positionLocked();
    if (playing_)
        baseMediaUs_ = at;
    baseWall_ = std::chrono::steady_clock::now();
    speed_ = speed;
    if (pumpDriven_ && backend_) {
        backend_->pumpSetSpeed(speed);
        cv_.notify_all();
        return;
    }
    // 音频设备不会跟着变速：换速后「以当前点重新打开一遍」（对齐真实
    // 播放器的倍速行为），倍速非 1.0 期间由墙钟做主时钟且不出声。
    if (sink_) {
        sink_->stop();
        sink_->flush(at);
        audioRunning_ = false;
    }
    syncAudioDeviceLocked();
    cv_.notify_all();
}

void VideoPlayback::setLooping(bool looping) {
    std::lock_guard<std::mutex> lock(mutex_);
    looping_ = looping;
}

void VideoPlayback::setVolume(double volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    volume_ = std::clamp(volume, 0.0, 1.0);
    if (stop_)
        return;
    if (sink_)
        sink_->setVolume(effectiveVolume());
    if (pumpDriven_ && backend_)
        backend_->pumpSetVolume(effectiveVolume());
}

double VideoPlayback::volume() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return volume_;
}

bool VideoPlayback::hasAudioOutput() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sink_)
        return true;
    // 泵驱动后端（浏览器）自己出声，不经过 sink_。
    return pumpDriven_ && info_.hasAudio;
}

float VideoPlayback::effectiveVolume() const { return float(volume_); }

bool VideoPlayback::isPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
}

bool VideoPlayback::isCompleted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_;
}

double VideoPlayback::playbackSpeed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return speed_;
}

std::string VideoPlayback::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool VideoPlayback::audioMasterLocked() const {
    // 音频做主时钟的条件：设备在、倍速 1.0（设备不会跟着变速）。
    return sink_ != nullptr && speed_ == 1.0;
}

int64_t VideoPlayback::positionLocked() const {
    auto clamp = [this](int64_t pos) {
        pos = std::max<int64_t>(0, pos);
        if (info_.duration.count() > 0)
            pos = std::min<int64_t>(pos, info_.duration.count());
        return pos;
    };
    if (completed_)
        return clamp(info_.duration.count());
    if (!playing_)
        return clamp(baseMediaUs_);

    // 主时钟优先级：音频 > 泵驱动后端的媒体时间 > 墙钟推算。
    // 暂停时不走上面任何一条 —— `baseMediaUs_` 在 `pause()` 里已经抓好了，
    // 设备再多消耗几毫秒也不会让进度条倒退式地抖动。
    if (audioMasterLocked())
        return clamp(sink_->positionUs());
    if (pumpDriven_ && backend_) {
        const int64_t master = backend_->pumpPositionUs();
        if (master >= 0)
            return clamp(master);
    }

    const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  baseWall_);
    return clamp(baseMediaUs_ + static_cast<int64_t>(double(elapsed.count()) * speed_));
}

int64_t VideoPlayback::videoDelayUsLocked(int64_t ptsUs) const {
    if (audioMasterLocked())
        return ptsUs - sink_->positionUs();
    if (pumpDriven_ && backend_) {
        const int64_t master = backend_->pumpPositionUs();
        if (master >= 0)
            return ptsUs - master;
        return 0;
    }
    const double speed = speed_ > 0.0 ? speed_ : 1.0;
    const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  baseWall_);
    const int64_t pos = baseMediaUs_ + static_cast<int64_t>(double(elapsed.count()) * speed);
    // 返回的是**墙上等待时长**：媒体上差多少除以倍速。
    return static_cast<int64_t>(double(ptsUs - pos) / speed);
}

void VideoPlayback::syncAudioDeviceLocked() {
    if (!sink_)
        return;
    const bool wantRunning = playing_ && !stop_ && audioMasterLocked() && !pendingSeekUs_.has_value();
    if (wantRunning && !audioRunning_) {
        // 从当前媒体位置重新起算：否则设备消耗计数还是上一段的。
        sink_->flush(baseMediaUs_);
        sink_->start();
        audioRunning_ = true;
    } else if (!wantRunning && audioRunning_) {
        sink_->stop();
        audioRunning_ = false;
    }
}

void VideoPlayback::feedAudio() {
    if (!sink_ || !backend_)
        return;
    while (sink_->queuedUs() < kAudioLeadUs) {
        AudioChunk chunk;
        if (!backend_->readAudio(chunk))
            break;
        sink_->push(chunk);
    }
}

std::chrono::microseconds VideoPlayback::position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::chrono::microseconds(positionLocked());
}

bool VideoPlayback::takeLatestFrame(VideoFrame& out, uint64_t& serial) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_.image || serial == serial_)
        return false;
    out = latest_;
    serial = serial_;
    return true;
}

bool VideoPlayback::hasPendingFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publishOnce_ || pendingSeekUs_.has_value();
}

bool VideoPlayback::pumpStep() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stop_ || !pumpDriven_ || !backend_)
        return false;

    // 元数据迟到：浏览器要到 `loadedmetadata` 才知道尺寸/长度（打开时只能给
    // 一份「还不知道」的 VideoInfo，见 video_decoder_web.cpp 文件头）。`info_`
    // 只在 bindBackendLocked 拷一次，所以泵驱动这条路上每拍补一下；其余后端
    // 的 info() 打开后恒定 ⇒ 比较不相等，等于零副作用。
    if (const VideoInfo& late = backend_->info();
        late.width != info_.width || late.height != info_.height ||
        late.duration != info_.duration || late.hasAudio != info_.hasAudio) {
        // 只补申报（尺寸/长度/有无音轨）。不在这里补建音频设备：主时钟已经挂在
        // 后端/墙钟上，中途换源会跳，而泵驱动那一路（浏览器）本来就不经本仓出声。
        info_ = late;
    }

    if (pendingSeekUs_.has_value()) {
        const int64_t target = *pendingSeekUs_;
        pendingSeekUs_.reset();
        baseMediaUs_ = target;
        baseWall_ = std::chrono::steady_clock::now();
        staged_ = VideoFrame{};
        lock.unlock();
        backend_->seekTo(target);
        backend_->flushAudio();
        lock.lock();
        if (stop_)
            return false;
        syncAudioDeviceLocked();
        return false;
    }

    const bool publishOnce = publishOnce_;
    if (!playing_ && !publishOnce)
        return false;

    // 泵驱动下没有工作线程，音频也得在这一拍里喂（sink 为空时 no-op）。
    if (sink_ && playing_) {
        lock.unlock();
        feedAudio();
        lock.lock();
        if (stop_)
            return false;
    }

    if (!staged_.image) {
        lock.unlock();
        const bool ok = backend_->readFrame(staged_);
        lock.lock();
        if (stop_)
            return false;
        if (!ok) {
            const bool ended = backend_->atEnd();
            const std::string backendError = backend_->error();
            if (ended) {
                if (looping_ && info_.duration.count() > 0) {
                    pendingSeekUs_ = 0;
                } else {
                    playing_ = false;
                    publishOnce_ = false;
                    completed_ = true;
                }
            } else if (!backendError.empty()) {
                // 泵驱动后端的「这一轮还没有新帧」既不是结束也不是错误。
                error_ = backendError;
                playing_ = false;
                publishOnce_ = false;
                completed_ = true;
            }
            return false;
        }
    }

    // 到点判定与工作线程那一条共用 `videoDelayUsLocked()`。
    if (playing_ && videoDelayUsLocked(staged_.ptsUs) > kSyncToleranceUs)
        return false;

    latest_ = std::move(staged_);
    staged_ = VideoFrame{};
    ++serial_;
    if (!playing_)
        baseMediaUs_ = latest_.ptsUs;  // 暂停中补帧：位置跟到这一帧
    publishOnce_ = false;
    syncAudioDeviceLocked();
    return true;
}

void VideoPlayback::workerLoop(VideoSource source) {
    std::string openError;
    auto backend = createBackend(source, openError);

    std::unique_lock<std::mutex> lock(mutex_);
    if (!backend) {
        error_ = openError.empty() ? "没有可用的视频解码后端" : openError;
        initDone_ = true;
        cv_.notify_all();
        return;
    }
    backend_ = std::move(backend);
    bindBackendLocked();
    initDone_ = true;
    cv_.notify_all();

    while (!stop_) {
        // 注意 publishOnce_ 也在等待条件里：暂停中 seek 之后要补出一帧，
        // 但那时 playing_ 仍是 false、pendingSeekUs_ 已被消费掉 —— 漏掉它
        // 工作线程就会在补帧之前睡死（首帧永远上不了屏）。
        cv_.wait(lock, [&] {
            return stop_ || playing_ || pendingSeekUs_.has_value() || publishOnce_;
        });
        if (stop_)
            break;

        if (pendingSeekUs_) {
            const int64_t target = *pendingSeekUs_;
            pendingSeekUs_.reset();
            baseMediaUs_ = target;
            baseWall_ = std::chrono::steady_clock::now();
            lock.unlock();
            backend_->seekTo(target);
            backend_->flushAudio();
            lock.lock();
            if (stop_)
                break;
            if (sink_) {
                sink_->flush(target);
                audioRunning_ = false;  // flush 之后设备里什么都没有，待 start 重新起
            }
            syncAudioDeviceLocked();
            continue;
        }

        syncAudioDeviceLocked();

        const bool publishOnce = publishOnce_;
        if (!playing_ && !publishOnce)
            continue;

        lock.unlock();
        feedAudio();
        VideoFrame frame;
        const bool ok = backend_->readFrame(frame);
        lock.lock();
        if (stop_)
            break;

        if (!ok) {
            if (backend_->atEnd()) {
                if (looping_ && info_.duration.count() > 0) {
                    pendingSeekUs_ = 0;
                } else {
                    playing_ = false;
                    publishOnce_ = false;
                    completed_ = true;
                    syncAudioDeviceLocked();
                    cv_.notify_all();
                }
            } else {
                error_ = backend_->error().empty() ? "解码失败" : backend_->error();
                playing_ = false;
                publishOnce_ = false;
                completed_ = true;
                syncAudioDeviceLocked();
                cv_.notify_all();
            }
            continue;
        }

        if (!playing_) {
            // 暂停中 seek：补一帧让画面跟到新位置，然后回到等待。
            if (publishOnce) {
                latest_ = std::move(frame);
                ++serial_;
                baseMediaUs_ = latest_.ptsUs;
                publishOnce_ = false;
                cv_.notify_all();
            }
            continue;
        }

        // 呈现节拍：到点就上屏。主时钟由 `videoDelayUsLocked()` 选（音频 /
        // 泵驱动后端的媒体时间 / 墙钟），超前则最多切一片 50ms 再重看，
        // 免得音频位置一直在动而这里睡过头。
        while (!stop_ && playing_ && !pendingSeekUs_.has_value()) {
            const int64_t delay = videoDelayUsLocked(frame.ptsUs);
            if (delay <= kSyncToleranceUs)
                break;
            cv_.wait_for(lock, std::chrono::microseconds(std::min<int64_t>(delay, 50 * 1000)));
        }
        if (stop_)
            break;
        if (!playing_ || pendingSeekUs_.has_value())
            continue;  // 暂停/跳转打破节拍：这一帧丢弃，恢复后继续读

        latest_ = std::move(frame);
        ++serial_;
    }

    lock.unlock();
    backend_.reset();
}

}  // namespace cui_core
