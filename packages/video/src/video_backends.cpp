/// @file video_backends.cpp
/// @brief 后端表 + 注册表 + 路线优先级 + 运行期探测（见 video_backends.h）。
///
/// 「哪条路线优先」「这台机器实际能用哪几个」「能力怎么向 UI 申报」三件事
/// 全部从这里出，`video_decoder.cpp` 只按 `orderedVideoBackends()` 的顺序试。

#include "video_backends.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace cui {

// ── 各后端工厂与探测（未编译进来时链接期不存在，所以条件声明） ──────────────
//
// 约定：每个后端文件提供 `<某>Backend(source, error)` 工厂 +
// `<某>VideoAvailable(note)` 探测。探测只做「这套系统 API 在这台机器上能不能
// 起来」的判断，不许在这里打开任何文件。

#if defined(CUI_HAVE_MEDIA_FOUNDATION)
std::unique_ptr<VideoDecoderBackend> createMediaFoundationBackend(const VideoSource& source,
                                                                  std::string& error);
bool mediaFoundationVideoAvailable(std::string& note);
#endif

#if defined(CUI_HAVE_AVFOUNDATION)
std::unique_ptr<VideoDecoderBackend> createAVFoundationBackend(const VideoSource& source,
                                                               std::string& error);
bool avFoundationVideoAvailable(std::string& note);
#endif

#if defined(CUI_HAVE_MEDIACODEC)
std::unique_ptr<VideoDecoderBackend> createMediaCodecBackend(const VideoSource& source,
                                                             std::string& error);
bool mediaCodecVideoAvailable(std::string& note);
#endif

#if defined(CUI_HAVE_BROWSER_VIDEO)
std::unique_ptr<VideoDecoderBackend> createBrowserVideoBackend(const VideoSource& source,
                                                               std::string& error);
bool browserVideoAvailable(std::string& note);
#endif

#if defined(CUI_HAVE_GSTREAMER)
std::unique_ptr<VideoDecoderBackend> createGStreamerBackend(const VideoSource& source,
                                                            std::string& error);
bool gStreamerVideoAvailable(std::string& note);
#endif

#if defined(CUI_HAVE_OH_AV_CODEC)
std::unique_ptr<VideoDecoderBackend> createOhosAvCodecBackend(const VideoSource& source,
                                                              std::string& error);
bool ohosAvCodecVideoAvailable(std::string& note);
#endif

#if defined(CUI_HAVE_FFMPEG)
std::unique_ptr<VideoDecoderBackend> createFFmpegBackend(const VideoSource& source,
                                                         std::string& error);
bool ffmpegVideoAvailable(std::string& note);
#endif

namespace {

/// 出声设备名：由 CMake 在实际编进某个平台 sink 时给 `CUI_AUDIO_SINK_NAME`。
/// 空串 ⇒ 这一路只解码不出声（能力申报因此不会谎报「含音频」）。
#if defined(CUI_HAVE_AUDIO_SINK) && defined(CUI_AUDIO_SINK_NAME)
constexpr const char* kPlatformSinkName = CUI_AUDIO_SINK_NAME;
#else
constexpr const char* kPlatformSinkName = "";
#endif

/// 一条内置后端入表。
///
/// `decodesAudio` = **这个后端自己会不会交出 PCM**（覆写了 `readAudio`）。
/// 它与「这台机器上编进了哪个 sink」是两件事：引擎只在
/// `backend->hasAudioStream()` 为真时才去开设备（基类默认 false），所以一个
/// 不交 PCM 的后端配上再真实的 sink 也是无声 —— 如果把设备名照填，
/// `videoCapabilities()` 就会报出「含音频」而实际一声不出（Linux 上缺 GStreamer
/// ⇒ 选中 FFmpeg 这条路就是这种情形）。申报口径必须跟着「真正会用的那一条」走。
VideoBackendRegistration makeReg(std::string name, bool systemDecoder, VideoBackendFactory factory,
                                 bool decodesAudio = true) {
    VideoBackendRegistration reg;
    reg.name = std::move(name);
    reg.audioName = decodesAudio ? std::string(kPlatformSinkName) : std::string{};
    reg.factory = std::move(factory);
    reg.platformDecoder = systemDecoder;
    return reg;
}

/// 没有任何探测函数时的占位（恒可用）。
bool alwaysAvailable(std::string&) { return true; }

std::vector<BuiltinBackend> systemBackends() {
    std::vector<BuiltinBackend> out;
#if defined(CUI_HAVE_BROWSER_VIDEO)
    // 浏览器：解复用 / 解码 / 出声全交给 `<video>`，本仓不经手 PCM。
    {
        BuiltinBackend b;
        b.reg = makeReg("HTMLVideoElement", /*systemDecoder=*/true, &createBrowserVideoBackend);
        b.reg.audioName.clear();  // 自带声音，但不是本仓的 AudioSink
        b.kind = VideoBackendKind::System;
        b.probe = &browserVideoAvailable;
        b.selfAudio = true;
        out.push_back(std::move(b));
    }
#endif
#if defined(CUI_HAVE_MEDIACODEC)
    out.push_back({makeReg("AMediaCodec", true, &createMediaCodecBackend),
                   VideoBackendKind::System, &mediaCodecVideoAvailable});
#endif
#if defined(CUI_HAVE_MEDIA_FOUNDATION)
    // MF 这一份已经会交 PCM（`video_decoder_win32.cpp` 的 `readAudio`：音频流自己
    // 配 IEEE float / S16 输出类型）⇒ 申报带上 Windows 那套 WASAPI sink。
    // 配不上（video processing 模式把音频流收去自用那类）也只是这一路不出声，
    // 引擎退回无声 + 墙钟，不影响这里的申报口径（与 FFmpeg 那条同形）。
    out.push_back({makeReg("Media Foundation", true, &createMediaFoundationBackend),
                   VideoBackendKind::System, &mediaFoundationVideoAvailable});
#endif
#if defined(CUI_HAVE_AVFOUNDATION)
    out.push_back({makeReg("AVFoundation", true, &createAVFoundationBackend),
                   VideoBackendKind::System, &avFoundationVideoAvailable});
#endif
#if defined(CUI_HAVE_GSTREAMER)
    out.push_back({makeReg("GStreamer", true, &createGStreamerBackend),
                   VideoBackendKind::System, &gStreamerVideoAvailable});
#endif
#if defined(CUI_HAVE_OH_AV_CODEC)
    out.push_back({makeReg("OH_AVDemuxer", true, &createOhosAvCodecBackend),
                   VideoBackendKind::System, &ohosAvCodecVideoAvailable});
#endif
    return out;
}

std::vector<BuiltinBackend> ffmpegBackends() {
    std::vector<BuiltinBackend> out;
#if defined(CUI_HAVE_FFMPEG)
    // FFmpeg 这一份也已经会交 PCM（`video_decoder_ffmpeg.cpp` 的 `readAudio`：
    // swresample 一次转成 S16 交织）⇒ 申报带上平台 sink。
    // 上面那段「不交 PCM 就别报设备」的口径仍然成立，只是这条已经交 PCM 了：
    // Linux 上缺 gstreamer 开发包时整条路线落到 FFmpeg，现在 ALSA 真的会被用上。
    out.push_back({makeReg("FFmpeg", /*systemDecoder=*/false, &createFFmpegBackend),
                   VideoBackendKind::Ffmpeg, &ffmpegVideoAvailable});
#endif
    return out;
}

/// 探测结果缓存：一次进程内问一次（`gst_init_check` / `dlopen` 都不便宜，
/// 而且能力申报和真正打开都会走到这里）。
struct ProbeCache {
    std::mutex mutex;
    std::map<std::string, std::pair<bool, std::string>> results;
};

ProbeCache& probeCache() {
    static ProbeCache cache;
    return cache;
}

std::atomic<VideoBackendPreference> g_preference{VideoBackendPreference::Auto};

}  // namespace

// ── 注册表（应用扩展 / 单测假件） ──────────────────────────────────────────

namespace {

struct BackendRegistry {
    std::mutex mutex;
    std::vector<VideoBackendRegistration> entries;
};

/// 函数内 static：注册可能发生在任意静态初始化时机，不能靠 TU 顺序。
BackendRegistry& backendRegistry() {
    static BackendRegistry registry;
    return registry;
}

}  // namespace

bool registerVideoBackend(VideoBackendRegistration reg) {
    if (!reg.factory)
        return false;
    BackendRegistry& registry = backendRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (auto& entry : registry.entries) {
        if (entry.name == reg.name) {
            entry = std::move(reg);  // 同名替换：重复注册不会插出两条
            return true;
        }
    }
    registry.entries.push_back(std::move(reg));
    return true;
}

void clearRegisteredVideoBackends() {
    BackendRegistry& registry = backendRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.entries.clear();
}

std::vector<VideoBackendRegistration> registeredVideoBackends() {
    BackendRegistry& registry = backendRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    return registry.entries;
}

std::unique_ptr<AudioSink> createRegisteredAudioSink() {
    // 与解码后端的选定相互独立：只要有人提供了设备工厂就拿来用。
    // 起不来（返回空或 open 失败）由调用方退回 `createPlatformAudioSink()`。
    for (const auto& reg : registeredVideoBackends()) {
        if (!reg.sinkFactory)
            continue;
        if (std::unique_ptr<AudioSink> sink = reg.sinkFactory())
            return sink;
    }
    return nullptr;
}

// ── 内置表与顺序 ────────────────────────────────────────────────────────────

const std::vector<BuiltinBackend>& builtinVideoBackends() {
    static const std::vector<BuiltinBackend> table = [] {
        std::vector<BuiltinBackend> out = systemBackends();
        for (auto& b : ffmpegBackends())
            out.push_back(std::move(b));
        return out;
    }();
    return table;
}

void setVideoBackendPreference(VideoBackendPreference preference) {
    g_preference.store(preference);
}

VideoBackendPreference videoBackendPreference() { return g_preference.load(); }

bool backendAvailable(const BuiltinBackend& backend, std::string& note) {
    if (!backend.probe) {
        note.clear();
        return true;
    }
    ProbeCache& cache = probeCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    auto it = cache.results.find(backend.reg.name);
    if (it == cache.results.end()) {
        std::string reason;
        const bool ok = backend.probe(reason);
        it = cache.results.emplace(backend.reg.name, std::make_pair(ok, ok ? std::string{} : reason))
                 .first;
    }
    note = it->second.second;
    return it->second.first;
}

std::vector<BuiltinBackend> orderedVideoBackends() {
    std::vector<std::string> registered;
    std::vector<BuiltinBackend> all;
    for (const auto& reg : registeredVideoBackends()) {
        if (!reg.factory)
            continue;
        BuiltinBackend b;
        b.reg = reg;
        b.kind = reg.platformDecoder ? VideoBackendKind::System : VideoBackendKind::Ffmpeg;
        b.probe = &alwaysAvailable;
        registered.push_back(reg.name);
        all.push_back(std::move(b));
    }

    const VideoBackendPreference preference = videoBackendPreference();
    const bool wantSystem = preference != VideoBackendPreference::FfmpegOnly;
    const bool wantFfmpeg = preference != VideoBackendPreference::SystemOnly;
    const bool ffmpegFirst = preference == VideoBackendPreference::FfmpegFirst;

    // 内置项：注册表里同名的那一条**顶掉**内置项（单测拿同名假件替换真实
    // 后端就靠这一条），其余照原顺序入列。
    for (const auto& b : builtinVideoBackends()) {
        bool taken = false;
        for (const auto& name : registered)
            taken = taken || name == b.reg.name;
        if (taken)
            continue;
        all.push_back(b);
    }

    // preference 对**整张表**生效（含注册项），否则「系统端优先」在应用塞了
    // 假件时就测不出来了。同一组内保持原有顺序（注册项在前）。
    std::vector<BuiltinBackend> out;
    auto takeGroup = [&](VideoBackendKind kind, bool wanted) {
        if (!wanted)
            return;
        for (const auto& b : all)
            if (b.kind == kind)
                out.push_back(b);
    };
    if (ffmpegFirst) {
        takeGroup(VideoBackendKind::Ffmpeg, wantFfmpeg);
        takeGroup(VideoBackendKind::System, wantSystem);
    } else {
        takeGroup(VideoBackendKind::System, wantSystem);
        takeGroup(VideoBackendKind::Ffmpeg, wantFfmpeg);
    }
    return out;
}

std::vector<VideoBackendStatus> videoBackendCatalog() {
    std::vector<VideoBackendStatus> out;
    for (const auto& b : orderedVideoBackends()) {
        VideoBackendStatus st;
        st.name = b.reg.name;
        st.kind = b.kind;
        st.compiled = true;
        st.selfAudio = b.selfAudio;
        st.audioName = b.reg.audioName;
        std::string note;
        st.available = backendAvailable(b, note);
        st.note = note;
        out.push_back(std::move(st));
    }
    return out;
}

}  // namespace cui_core
