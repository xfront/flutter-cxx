#pragma once
/// @file foundation/video_decoder.h
/// @brief 视频解码核心：容器/码流 → 逐帧 BGRA 位图 + 呈现时间戳。
///
/// 对应 上游 `video_player` 的平台侧（`VideoPlayerPlatform`）：上层
/// `VideoPlayerController` 只负责状态机与 UI，解复用/解码全部落在本文件的
/// 后端里。两条路线共七个真容器后端 + 一个动图兜底，共用一个播放引擎：
///
///   | 后端 | 覆盖 | 何时编进来 | 音频 |
///   | --- | --- | --- | --- |
///   | Media Foundation | Windows 上的 MP4/MKV/MOV/AVI/HTTP | `CUI_PLATFORM=windows` | 交 PCM → WASAPI |
///   | AVFoundation | macOS / iOS | `CUI_PLATFORM=macos\|ios` | 交 PCM → AudioQueue |
///   | AMediaCodec | Android（AMediaExtractor 解复用） | `CUI_PLATFORM=android` | 交 PCM → AAudio |
///   | GStreamer | Linux | 本机装了 gstreamer 开发包 | 交 PCM → ALSA |
///   | OH_AVDemuxer | OHOS（OH_AVSource 解复用） | `CUI_PLATFORM=ohos` | 交 PCM → OH_AudioRenderer |
///   | 浏览器 | Web（HTMLVideoElement 解复用/解码/出声） | `CUI_PLATFORM=web` | 浏览器自己出 |
///   | FFmpeg | 除 web 外所有端的同上一整套 | `CUI_VIDEO_FFMPEG=ON` 且找到库 | 交 PCM（swresample）→ 平台 sink |
///   | Skia 动图 | GIF / 动画 WebP / APNG | 全平台（无外部依赖） | 无音轨 |
///
/// 两条路线的关系：上面六行「系统 API」是路线一（`CUI_VIDEO_SYS`），FFmpeg 是
/// 路线二（`CUI_VIDEO_FFMPEG`）。它们共用同一张「后端表」，**默认系统端优先**，
/// 顺序可由 `setVideoBackendPreference()` 改；每一项还要过运行期探测
/// （`videoBackendCatalog()`）—— 编进来了不等于这台机器真的能用。
///
/// 与 上游 的分工差异（有意，写在这里免得被当 bug）：
///   1. 上游把「播放/暂停/时钟」也交给平台插件；本仓把**时钟与播放状态**
///      收进 `VideoPlayback`，各后端只实现「打开 → 顺序吐帧 / 跳转」。
///      这样动图后端不必自造一套计时器，语义也与原生后端一致。
///      **例外**：Web 后端是泵驱动的（`pumpDriven()`），单线程 wasm 里既没有
///      工作线程也不能阻塞，播放控制全部转发给 `<video>` 元素。
///   2. 上游的每一帧是一块 GPU 纹理（Texture/PlatformView）；本仓直接给
///      `SkImage`，渲染层照常按图像绘制（Graphite 由 ImageProvider 上传）。
///   3. **有音轨、且这一端配到了出声设备 ⇒ 才有声音**：系统端与 FFmpeg 后端都把
///      PCM 交出来，出声与主时钟在 `cui/video/audio_sink.h`（WASAPI / AAudio /
///      AudioQueue / ALSA / OH_AudioRenderer），Web 由浏览器自己出声。退回
///      「无声 + 墙钟节拍」只剩三种局面：设备打不开（没声卡 / AudioSrv 没跑 /
///      ROM 缺库）、动图（恒无音轨）、或 MF 在 video processing 模式下配不上音频
///      输出类型（那条退路写在 `video_decoder_win32.cpp` 文件头，只关声音不报错）。
///      能力申报与实际会走的那条路对齐：`videoCapabilities()` 的 `nativeAudio`
///      就是按「选中的后端会不会交 PCM」给的（口径定在 `video_backends.cpp` 的
///      `makeReg(..., decodesAudio)`，不在这里重复一遍）。
///
/// 约定：本文件的对象**跨线程**（工作线程解码、UI 线程取帧），一律
/// `std::shared_ptr`；`cui::SharedPtr` 禁止跨线程，见 foundation/shared_ptr.h。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <include/core/SkImage.h>

#include "cui/video/audio_sink.h"

namespace cui {

/// 媒体种类（决定选哪个后端，也用于给 UI 提示）。
enum class VideoMediaKind {
    Unknown,          ///< 还没探测
    VideoContainer,   ///< 真视频容器（MP4/MKV/AVI/…）
    AnimatedImage,    ///< GIF / 动画 WebP / APNG
    StaticImage,      ///< 单帧图片（可以显示，但没有时间轴）
};

/// 视频来源。对应 上游 的 `DataSource` + `DataSourceType`。
struct VideoSource {
    enum class Type { Asset, Network, File, ContentUri };

    Type type = Type::File;
    /// File/ContentUri = 本地路径；Network = URL；Asset = 资源名（本仓没有资源包，
    /// 按相对路径解析，与 File 同义）。
    std::string uri;
    /// 可选容器提示（"mp4" / "mjpeg"）；目前只用于日志与后端选择。
    std::string formatHint;
    /// 内存来源（`VideoSource::memory()`）；非空时不再读文件。
    std::vector<uint8_t> bytes;

    static VideoSource file(std::string path);
    static VideoSource network(std::string url);
    static VideoSource asset(std::string name);
    static VideoSource memory(std::vector<uint8_t> bytes, std::string formatHint = {});
};

/// 解码出的一帧。`image` 是 CPU 位图（Graphite 后端由 ImageProvider 上传）。
struct VideoFrame {
    sk_sp<SkImage> image;
    int64_t ptsUs = 0;  ///< 呈现时间戳（微秒，容器时间基）
};

/// 媒体信息（`initialize()` 完成后可读）。
struct VideoInfo {
    int width = 0;
    int height = 0;
    std::chrono::microseconds duration{0};
    double frameRate = 0.0;
    /// 顺时针补偿角 0/90/180/270（= 上游 `VideoPlayerValue.rotationCorrection`）。
    int rotation = 0;
    bool hasAudio = false;
    VideoMediaKind kind = VideoMediaKind::Unknown;
    std::string codec;  ///< 日志用（"h264" / "mjpeg" / "gif"…）
};

/// 后端接口。只做「打开 → 顺序吐帧 / 跳转」，不管理播放状态。
///
/// 线程约定：`readFrame` / `readAudio` / `seekTo` 只在播放引擎的工作线程里
/// 调用（泵驱动后端除外：那条路径全部在 GUI 线程，见 `pumpDriven()`）；
/// `info` 在创建后只读。
class VideoDecoderBackend {
public:
    virtual ~VideoDecoderBackend() = default;

    virtual const VideoInfo& info() const = 0;

    /// 取下一帧。阻塞直到解出一帧或流结束；返回 false 表示已到流尾/失败
    /// （`error()` 非空即失败）。泵驱动后端的含义不同：返回 false 且
    /// `atEnd()` 为假只表示「这一轮还没有新帧」。
    virtual bool readFrame(VideoFrame& out) = 0;

    /// 跳到 `ptsUs` 附近。之后 `readFrame` 返回的第一帧可能与目标略有偏差
    /// （关键帧起点），调用方可按 pts 丢弃过前的帧。
    virtual bool seekTo(int64_t ptsUs) = 0;

    virtual bool atEnd() const = 0;

    virtual const std::string& error() const = 0;

    // ── 音频（可选；默认实现 = 「没有音轨」，动图后端因此不用改） ────

    virtual bool hasAudioStream() const { return false; }
    /// 交给 `AudioSink::open()` 的格式（S16 交织，已由后端重采样到位）。
    virtual AudioFormat audioFormat() const { return {}; }
    /// 取下一段 PCM。返回 false = 目前没有了（不代表出错，也不代表结束）。
    /// `out.samples` 由后端持有，到下一次 `readAudio`/`flushAudio` 有效。
    virtual bool readAudio(AudioChunk& /*out*/) { return false; }
    /// seek 之后丢弃内部未交出的 PCM 与重采样残留。
    virtual void flushAudio() {}

    // ── 泵驱动模式（Web）：播放引擎不起工作线程，控制直接下发 ───────────

    /// true ⇒ `VideoPlayback::create()` 同步打开、不起线程，取帧由 GUI 线程
    /// 每帧调一次 `pumpStep()`；播放/暂停/倍速/音量都是后端的转发。
    virtual bool pumpDriven() const { return false; }
    virtual void pumpSetPlaying(bool /*playing*/) {}
    virtual void pumpSetSpeed(double /*speed*/) {}
    virtual void pumpSetVolume(double /*volume*/) {}
    /// 当前媒体位置；返回负数表示后端还没准备好（按 0 处理）。
    virtual int64_t pumpPositionUs() const { return -1; }
};

// ── 后端表（系统端 / FFmpeg 端 / 动图） ────────────────────────────────────
//
// 两条实现都在 `packages/video` 这一个库里，按平台条件编译：
//
//   | 路线 | 文件 | 何时编进来 |
//   | --- | --- | --- |
//   | 系统 API | `video_decoder_win32.cpp` / `_darwin.mm` / `_android.cpp` |
//   |          | `_web.cpp` / `_gst.cpp` / `_ohos.cpp` | 对应平台且 `CUI_VIDEO_SYS=ON` |
//   | FFmpeg | `video_decoder_ffmpeg.cpp` | 除 web 外所有端（`CUI_VIDEO_FFMPEG=ON`） |
//   | Skia 动图 | 本文件 | 恒编（兜底） |
//
// 两个开关各管一条路线：关掉的那条连编都不参与，表里因此没有那一类条目，
// 不用改 preference 就自动走剩下的那条（`CUI_VIDEO_SYS` 在包里，`CUI_VIDEO_FFMPEG`
// 因要在 project() 前定，只能定在根 CMakeLists）。
//
// FFmpeg 这份从哪来：**全平台固定用 vcpkg 的那一份**（根 CMakeLists 默认把
// feature `ffmpeg` 追加进 `VCPKG_MANIFEST_FEATURES`，vcpkg 按当前 triplet
// 下载现编；位置取全局 `VCPKG_ROOT`）。不再探发行版的 libav*：版本随环境飘，
// 而且交叉编译时 pkg-config 解析的是宿主机的树（会把 x86-64 的 .so 端给 arm64
// 链接）。vcpkg 没有 ohos/emscripten 的 ffmpeg ⇒ OHOS 要这条路线只能自交叉编一份
// 再设 `CMAKE_PREFIX_PATH`（并 `-DCUI_VIDEO_FFMPEG_VCPKG=OFF`），web 端则用 HTMLVideoElement。
//
// 挑选顺序：注册表（应用/单测自己塞的）→ 系统端 → FFmpeg → 动图；
// `setVideoBackendPreference()` 可调这两条的先后，也可以钉死其中一条。
// 「编进来」与「这台机器能用」是两件事，所以每项还带一个运行期探测，
// 结果由 `videoBackendCatalog()` 如实上报（缺 SDK / API level 太低 /
// 浏览器没有 HTMLVideoElement 都会在这里露出来）。

/// 后端属于哪条路线。
enum class VideoBackendKind { System, Ffmpeg, AnimatedImage };

/// 两条路线的先后。`Auto` = 系统端优先（默认）。
enum class VideoBackendPreference {
    Auto,
    SystemFirst,  ///< 系统端试完再退 FFmpeg（与 Auto 同行为，写明给调用方用）
    FfmpegFirst,  ///< FFmpeg 优先，打不开再退系统端
    SystemOnly,   ///< 只用系统端（FFmpeg 不参与）
    FfmpegOnly,   ///< 只用 FFmpeg（系统端不参与）
};
void setVideoBackendPreference(VideoBackendPreference preference);
VideoBackendPreference videoBackendPreference();

/// 一个已编进来的后端条目及其运行期可用性（诊断 / UI 提示用）。
struct VideoBackendStatus {
    std::string name;
    VideoBackendKind kind = VideoBackendKind::System;
    bool compiled = false;  ///< 编译期是否进来了
    bool available = false;  ///< 运行期探测结果
    bool selfAudio = false;  ///< 后端自己出声（浏览器 / 系统播放器），不经本仓 `AudioSink`
    std::string audioName;   ///< 配套出声设备；空且 !selfAudio ⇒ 这一路无声
    std::string note;       ///< 不可用的原因（探测失败时填）
};
/// 按当前 preference 的尝试顺序返回。动图后端不在这张表里（它是兜底，恒可用）。
std::vector<VideoBackendStatus> videoBackendCatalog();

/// 构建期 + 运行期能力探测（给 UI 提示用）。
struct VideoCapabilities {
    bool mediaFoundation = false;
    bool ffmpeg = false;
    bool animatedImage = true;   ///< 恒 true：Skia 编解码器始终在
    /// 真正会被选中的那一条后端路线上有没有出声设备（系统解码器 / FFmpeg / 浏览器）。
    /// **不是编译期宏的镜像**：宏只说明「某条 sink 编进来了」，而这里问的是
    /// 「实际会用的那条路能不能出声」—— 两条路线都关、或运行期一个后端都不可用时，
    /// 它就是 false（`summary()` 的「含音频」跟它同源，见 `videoCapabilities()`）。
    bool nativeAudio = false;
    bool platformDecoder = false; ///< 解复用/解码交给操作系统本身（浏览器）
    /// 由系统媒体栈解码（本包的系统端后端，或应用注册进来的那一条）。
    bool systemDecoder = false;
    std::string systemName;         ///< "Media Foundation" / "AVFoundation" / "AMediaCodec" …
    /// 选中那条路线配套的出声设备名（"AAudio" / "AudioQueue" / "ALSA" /
    /// "OH_AudioRenderer"），浏览器那一条报 "后端自带"；空 = 这一路无声。
    /// 注意它与 `systemDecoder` 无关：走 FFmpeg 时同样用平台 sink。
    std::string systemAudioName;
    std::string nativeName;      ///< "Media Foundation" / "FFmpeg" / "浏览器"；都没有时为空
    std::string summary() const;
};
VideoCapabilities videoCapabilities();

// ── 后端注册表（系统媒体栈的接入点） ──────────────────────────────────────
//
// 后端注册表是这张表的**对外开孔**：应用与单测可以塞进自己的后端（排在内置
// 表之前），内置的系统端/FFmpeg 端也共用同一套工厂与探测签名。
// 显式注册（而不是静态初始化器）是必要的：静态库里没人引用的目标文件会被
// 链接器整段丢掉，`__attribute__((constructor))` 那一条在 MSVC 上又不存在。

/// 系统后端工厂，与内置后端同口径：打开失败返回 nullptr 并填 `error`。
using VideoBackendFactory = std::function<std::unique_ptr<VideoDecoderBackend>(
        const VideoSource&, std::string& error)>;

/// 系统出声设备的工厂（与 `createPlatformAudioSink()` 同口径：起不来返回 nullptr）。
/// 后端只负责解码，PCM 始终交给 sink，主时钟也从 sink 来。
using AudioSinkFactory = std::function<std::unique_ptr<AudioSink>()>;

/// 一条注册项。`factory` 为空则整条忽略。
struct VideoBackendRegistration {
    std::string name;        ///< 后端名（进 `VideoCapabilities::summary()`）
    std::string audioName;   ///< 配套出声设备名；空 = 这一路无声
    VideoBackendFactory factory;
    /// 配套出声设备。为空 ⇒ 退回 `createPlatformAudioSink()`。
    /// 浏览器/系统播放器那类「自己出声」的后端也留空，并在 `info().hasAudio` 里如实上报。
    AudioSinkFactory sinkFactory;
    /// 是否真的由操作系统解码（目前恒 true，留给「系统 API + 自带软解」的混合路）。
    bool platformDecoder = true;
};

/// 注册一个后端（应用扩展 / 单测假件）；同名条目替换（幂等，可重复调用）。返回是否生效。
bool registerVideoBackend(VideoBackendRegistration reg);
/// 清空注册表（单测用；也让应用退回 FFmpeg 路线）。
void clearRegisteredVideoBackends();
/// 当前注册表（按优先级顺序）。
std::vector<VideoBackendRegistration> registeredVideoBackends();
/// 取第一个带出声工厂的注册项起一个设备；都没有则返回 nullptr。
/// （与解码后端的选定相互独立：只要有人提供了设备就用，免得每端再各自推一套。）
std::unique_ptr<AudioSink> createRegisteredAudioSink();

/// 共享播放引擎：工作线程 + 媒体时钟。
///
/// 上游 `VideoPlayerPlatform.play/pause/seekTo/getPosition` 的语义在这里
/// 合成一份：`position()` 是**由时钟推算**的当前位置（不是最后一帧的 pts），
/// 与上游「每 100ms 轮询平台位置」的效果一致但更平滑。
///
/// 两条时钟路线（`sink_` 是否存在决定走哪条）：
///   - 有音频 sink ⇒ **音频做主时钟**（对齐 ffplay）：位置来自
///     `AudioSink::positionUs()`，视频帧按 pts 向它对齐；
///   - 无音轨 / 设备打不开 ⇒ 维持原来的墙钟推算（`baseWall_` + `baseMediaUs_`），
///     动图与 FFmpeg / MF 配不上设备那几条因此与出声改造前一字不差。
class VideoPlayback {
public:
    /// 打开并探测来源。失败时返回 nullptr 并填 `error`。
    static std::shared_ptr<VideoPlayback> create(const VideoSource& source, std::string& error);

    /// 测试注入口：跳过来源探测，直接用现成的后端/音频设备起引擎。
    /// **不起工作线程** —— 所有节拍由调用方显式驱动 `pumpStep()`，后端
    /// 需申报 `pumpDriven()`。只在单测里用（拿真解码器才能验证的口径不靠它）。
    static std::shared_ptr<VideoPlayback> createWithBackend(
            std::unique_ptr<VideoDecoderBackend> backend, std::unique_ptr<AudioSink> sink = nullptr);

    ~VideoPlayback();
    VideoPlayback(const VideoPlayback&) = delete;
    VideoPlayback& operator=(const VideoPlayback&) = delete;

    const VideoInfo& info() const { return info_; }

    void play();
    void pause();
    /// 跳到目标位置（自动夹到 `[0, duration]`）。
    void seekTo(std::chrono::microseconds target);
    /// 0.25 / 0.5 / 1 / 2 …（<=0 忽略，与 上游 setPlaybackSpeed 的断言同口径）。
    void setPlaybackSpeed(double speed);
    void setLooping(bool looping);
    /// 音量 [0,1]。没有音频 sink 时只记录（与 上游 的 value 字段对齐）。
    /// 静音不做单独开关（与 上游 同口径：`VideoControls` 用音量 0 表示静音）。
    void setVolume(double volume);
    double volume() const;
    /// 是否真的有声音（设备打开成功且有音轨），UI 可据此决定是否显示音量。
    bool hasAudioOutput() const;

    bool isPlaying() const;
    bool isCompleted() const;  ///< 播完且未循环
    std::chrono::microseconds position() const;
    std::chrono::microseconds duration() const { return info_.duration; }
    double playbackSpeed() const;
    /// 解码期错误（空 = 正常）。到流尾不算错误。
    std::string error() const;

    /// 取最新一帧。`serial` 单调递增，和上次相同就表示没有新帧
    /// （UI 每帧轮询一次即可，不需要消息队列）。
    bool takeLatestFrame(VideoFrame& out, uint64_t& serial) const;

    /// 是否还有「暂停中也该补出」的帧（seek 之后的那一帧）。
    /// 控件层据此决定定时器还用不用继续跑。
    bool hasPendingFrame() const;

    /// 泵驱动后端（Web）专用：GUI 线程每帧调一次，向后端要一帧并发布。
    /// 非泵驱动后端调用没有副作用（返回 false）。
    bool pumpStep();

    /// 停线程、释放后端。幂等；之后所有调用都是空操作。
    void dispose();

    /// 平台是否应该走「同步打开、不起工作线程」这条路。
    /// 单线程 wasm 里 `std::thread` 一构造就抛，所以编译期就得决定。
    static bool pumpDrivenPlatform();

private:
    VideoPlayback() = default;
    /// 工作线程主体：先在这里创建后端（COM/MF 有线程亲和），再做取帧节拍。
    void workerLoop(VideoSource source);
    /// 同步创建后端（泵驱动平台 / 测试用）；成功返回 true。
    bool openInline(const VideoSource& source, std::string& error);
    /// 后端与音频设备就绪后的公共接线（调用方持锁）。
    void bindBackendLocked();
    /// 把后端已有的 PCM 喂给 sink，直到积压超过阈值（调用方**不持锁**）。
    void feedAudio();
    /// 当前媒体位置（内部，调用方持锁）。
    int64_t positionLocked() const;
    /// 主时钟是不是音频（只有倍速 1.0 时是；其余情况退回墙钟/后端）。
    bool audioMasterLocked() const;
    /// 视频帧 `ptsUs` 相对主时钟还要等多久（微秒）。<=0 表示已到点。
    /// 线程驱动与泵驱动两条路径共用这一份判定，免得两边算法走歪。
    int64_t videoDelayUsLocked(int64_t ptsUs) const;
    /// 音频设备的开/关：只有「有 sink + 正在播 + 倍速 1.0」才真的放声。
    void syncAudioDeviceLocked();
    /// 当前音量，下发给 sink 或泵驱动后端。
    float effectiveVolume() const;

    std::unique_ptr<VideoDecoderBackend> backend_;
    /// 有音轨且设备打开成功才非空；非空 ⇒ 主时钟改由它提供。
    /// 只在 workerLoop 里创建、在 `dispose()` join 之后销毁（都在锁内），
    /// GUI 线程的 start/stop/setVolume 因此不会碰到指针生命周期。
    std::unique_ptr<AudioSink> sink_;
    VideoInfo info_;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool initDone_ = false;  ///< 后端创建完成（成功或失败），`create` 等到它
    bool playing_ = false;
    bool looping_ = false;
    bool stop_ = false;
    bool completed_ = false;
    bool pumpDriven_ = false;  ///< 没有工作线程，取帧靠 `pumpStep()`
    bool audioRunning_ = false; ///< sink_ 当前是否开着（免得反复 start/stop）
    double volume_ = 1.0;
    double speed_ = 1.0;
    int64_t baseMediaUs_ = 0;                                  ///< 时钟基点（媒体时间）
    std::chrono::steady_clock::time_point baseWall_{};         ///< 时钟基点（墙钟）
    std::optional<int64_t> pendingSeekUs_;
    bool publishOnce_ = false;  ///< 暂停中 seek 之后需要补出一帧
    std::string error_;         ///< 解码期错误

    VideoFrame latest_;
    uint64_t serial_ = 0;
    /// 泵驱动模式里「解出来了但还没到呈现时间」的那一帧，下一轮接着判定。
    VideoFrame staged_;

    std::thread worker_;
};

}  // namespace cui
