/// @file video/video_decoder_web.cpp
/// @brief 浏览器解码后端：容器解复用 / 解码 / 出声全部交给 `<video>`。
///
/// 路线一（各端系统 API）在 Web 上的落点。角色与 `video_decoder_win32.cpp`
/// 对齐 —— **本仓不碰码流**，只把浏览器已经画好的那一帧搬进 Skia 位图。
/// 这也是这条路线存在的全部理由：往 wasm 里塞一份 FFmpeg 既大（几 MB）又慢
/// （单线程软解），而浏览器自带硬解、正确的容器解析，还有版权限（EME）。
///
/// 关键实现口径：
///   - **泵驱动**（`pumpDriven()` = true）。单线程 wasm 里 `std::thread` 一构造
///     就抛（见 `VideoPlayback::pumpDrivenPlatform()`），也没有开 ASYNCIFY 可以
///     阻塞等事件，所以播放/暂停/倍速/音量全部直接转发给元素，取帧由 GUI 线程
///     每帧一次 `pumpStep()` 拉。
///   - **元数据是异步的**：`loadedmetadata` 之前 `videoWidth`/`duration` 都是 0。
///     打开时只能先给一份「还不知道」的 `VideoInfo`，等事件回来再补齐 ——
///     为此 `VideoPlayback::pumpStep()` 每拍比对一次后端 info（只有泵驱动这条
///     路会真的变），`VideoPlayerController::poll()` 再把尺寸/长度补进 value。
///   - **时钟问元素要**：`pumpPositionUs()` = `currentTime`。声音是浏览器出的，
///     它本身就是主时钟，本仓不再叠加墙钟推算（`AudioSink` 那一路完全不参与）。
///   - **像素回读**：`drawImage` 到一张离屏 2D canvas（带 `willReadFrequently`
///     提示），`getImageData` 拿 RGBA，就地交换 R/B 写成 BGRA —— 与 MF/Android
///     后端同口径，alpha 恒写 0xFF 配 `kOpaque`（Graphite 上传纹理后仍按原始
///     通道采样，留 0 就是个透明洞）。跨域又没 CORS 头时 canvas 会被污染、
///     `getImageData` 抛 SecurityError：这是浏览器的硬规定，所以 `crossOrigin`
///     提前设成 `anonymous`，让「解不了」在加载期就报错，而不是每帧画黑。
///   - **出声不经本仓**：`hasAudioStream()` 返回 false ⇒ 引擎不会建 `AudioSink`；
///     音量走 `pumpSetVolume()` → `el.volume`。后端表里这一项 `selfAudio = true`。
///   - 元素**挂在文档里但移出可视区**（`left:-10000px`，而不是 `display:none`）：
///     有的引擎不为 `display:none` 的元素提交新帧，`drawImage` 会一直拿到同一张
///     旧画面；detached 元素（不进文档）也有同样问题，所以都不用。
///   - 「有没有新的一帧」= `currentTime` 变了，或者 seek 之后强制补一帧。
///     不用 `requestVideoFrameCallback`：不是所有引擎都有（Firefox 到 138 才
///     有），而这条路径本来就排在 rAF 之后一拍，多一个回调换不到什么。
///   - **循环不归元素**：`el.loop` 恒 false，播完了由引擎按 `looping_` seek 回 0
///     （与其它后端同一条口径，否则引擎的循环状态机和元素会打架）。
///   - 旋转：浏览器在 `videoWidth`/`drawImage` 里已经把容器旋转施加掉了，所以
///     这里报 `rotation = 0`、像素也不再翻一遍 —— 本端是唯一一处「后端自己转好」。
///
/// 有意未覆盖（都得在真实浏览器里才能验，本机只能编译 + 链接）：
///   - `audioTracks` 只有 Chrome 的实验开关下有 ⇒ 拿不到时按「有声」上报（宁可
///     多显示一个音量滑条，也不让有声音的视频调不了音量）。
///   - 直播流（`duration === Infinity`）：长度报 0，进度条因此不可用。
///   - MSE（`MediaSource`）来源、EME 加密轨、多音轨切换、倍速超出引擎支持范围。
///   - 帧率：浏览器不告诉上层 fps，`info().frameRate` 恒 0（未知）。

#include "cui/video/video_decoder.h"

#if defined(CUI_HAVE_BROWSER_VIDEO)

#include <algorithm>
#include <cmath>
#include <emscripten.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <string>
#include <vector>

namespace cui {
namespace {

// ── C++ ⇄ JS 的约定 ────────────────────────────────────────────────────────
//
// 为什么不用 `emscripten::val`：本项目按默认配置编（`-fignore-exceptions`，
// 链接期没有真正的异常），而仓库里既有的 wasm 互操作一律是「原始 JS 块 +
// Module 上的状态表」（先例：cui_core/src/platform/web_window.cpp 的
// `canvas.__cuiHooks` 幂等守卫）。这里沿用同一套形状：所有状态放在
// `Module['__cuiVideo'].states[id]`，C++ 只握一个整数 id。
//
// 两条铁律，改这里之前先读：
//   1. **碰 wasm 堆（HEAPU8/HEAPU32）的代码必须写在 EM_ASM 体内**，不能存进
//      长寿命闭包。emscripten 把这些视图作为模块作用域变量注入到每次调用，
//      内存增长后是新鲜的；一旦被闭包捕获就固化了那一刻的 typed-array，
//      增长之后写进的是废弃缓冲 —— 症状是画面全黑且不报任何错。
//   2. **EM_ASM 的参数只能是整数**（内部按 intptr_t 走 varargs）。传 double
//      会按整数截断，所以倍速/音量按千分位整数过界，64 位微秒时刻拆成
//      高/低两个 32 位（JS 那边 `>>> 0` 补回无符号）。

/// `poll` 返回的位标记。
enum WebVideoFlag {
    kWebMetadata = 1 << 0,  ///< readyState >= 1：宽高/长度已知
    kWebHaveFrame = 1 << 1, ///< readyState >= 2：当前帧可以 drawImage
    kWebEnded = 1 << 2,
    kWebError = 1 << 3,   ///< error 事件 / play() 被拒 / 回读像素抛异常
    kWebSeeking = 1 << 4, ///< seek 还没落地（`seeking` 为真或 seeked 事件未回）
};

/// `getNum` 的读取项。
enum WebVideoNum {
    kNumDuration = 0,  ///< 秒（可能是 NaN/Infinity ⇒ 调用方自己 isfinite）
    kNumWidth = 1,     ///< videoWidth（已含旋转）
    kNumHeight = 2,    ///< videoHeight
    kNumCurrentTime = 3, ///< 秒
    kNumAudioTracks = 4, ///< 没有这个 API ⇒ -1（未知）
};

/// `setMilli` 的写入项（值按千分位整数传来）。
enum WebVideoSet {
    kSetPlaybackRate = 0,
    kSetVolume = 1,
    kSetMuted = 2,
};

/// 一次拷回的最长异常文本。
constexpr size_t kWebErrorCap = 256;

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

/// 把浏览器抛回来的异常翻成人话。只翻有确定含义的这几类，其余原样带出。
std::string describeJSError(const std::string& raw) {
    if (raw.empty())
        return raw;
    auto has = [&](const char* needle) { return raw.find(needle) != std::string::npos; };
    // canvas 被跨域视频污染：CORS 规定的表现，不是本仓的 bug。
    if (has("SecurityError"))
        return "跨域视频无法回读像素（canvas 被污染）：服务端需要给视频加 "
               "Access-Control-Allow-Origin";
    // 自动播放策略：没有用户手势且未静音时 play() 会被拒。
    if (has("NotAllowedError"))
        return "浏览器拒绝了播放（自动播放策略）：需要用户手势，或先静音";
    if (has("Canvas2DUnavailable"))
        return "这个宿主拿不到 2D canvas 上下文";
    if (raw.rfind("mediaerror:", 0) == 0) {
        // 形态：`mediaerror:<MediaError.code>:<message>`。code 是规范里的四类。
        const size_t second = raw.find(':', 11);
        const std::string code = raw.substr(11, second == std::string::npos ? 1 : second - 11);
        const std::string msg =
                second == std::string::npos ? std::string{} : raw.substr(second + 1);
        const char* what = "媒体错误";
        if (code == "1")
            what = "播放被中止（元素被重置或来源被移除）";
        else if (code == "2")
            what = "网络错误：取不到视频数据";
        else if (code == "3")
            what = "解码错误：这个码流浏览器解不开";
        else if (code == "4")
            what = "这个容器/编码浏览器不支持";
        return msg.empty() ? std::string(what) : std::string(what) + "：" + msg;
    }
    return raw;
}

/// `formatHint` → MIME（只给 Blob 来源用；URL 来源由浏览器按响应头/后缀判断）。
std::string mimeForHint(const std::string& hint) {
    std::string h = hint;
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (h == "mp4" || h == "m4v")
        return "video/mp4";
    if (h == "webm")
        return "video/webm";
    if (h == "mov")
        return "video/quicktime";
    if (h == "mkv")
        return "video/x-matroska";
    if (h == "avi")
        return "video/x-msvideo";
    if (h == "ogv" || h == "ogg")
        return "video/ogg";
    return {};
}

/// 来源 → 给元素的 URL。wasm 里「本地路径」其实就是页面相对 URL（MEMFS 只有
/// `--preload-file` 塞进来的那几份，容器文件不会在那儿）。
std::string urlForSource(const VideoSource& source) {
    if (startsWith(source.uri, "file://"))
        return source.uri.substr(7);
    return source.uri;
}

/// 秒 → 微秒（浏览器那边全是秒的浮点）。非有限值 ⇒ -1，调用方按「还不知道」处理。
int64_t secondsToUs(double seconds) {
    if (!std::isfinite(seconds))
        return -1;
    return std::llround(seconds * 1000000.0);
}

/// 浏览器后端。线程约定与其他后端一致（泵驱动 ⇒ 全部在 GUI 线程），
/// 所以内部不需要任何锁。
class BrowserVideoBackend final : public VideoDecoderBackend {
public:
    BrowserVideoBackend() = default;
    ~BrowserVideoBackend() override { destroyElement(); }
    BrowserVideoBackend(const BrowserVideoBackend&) = delete;
    BrowserVideoBackend& operator=(const BrowserVideoBackend&) = delete;

    bool open(const VideoSource& source, std::string& error) {
        const bool useBytes = !source.bytes.empty();
        const std::string url = urlForSource(source);
        if (!useBytes && url.empty()) {
            error = "浏览器后端需要 URL 或内存来源";
            return false;
        }
        const std::string mime = mimeForHint(source.formatHint);
        const void* mimePtr = mime.empty() ? nullptr : static_cast<const void*>(mime.c_str());
        const void* dataPtr =
                useBytes ? static_cast<const void*>(source.bytes.data()) : nullptr;

        const int id = EM_ASM_INT((
{
            var doc = (typeof document !== 'undefined') ? document : null;
            if (!doc || typeof doc.createElement !== 'function')
                return 0;
            if (typeof HTMLVideoElement === 'undefined')
                return 0;
            var el;
            try {
                el = doc.createElement('video');
            } catch (e) {
                return 0;
            }
            if (!el || typeof el.canPlayType !== 'function')
                return 0;

            var ns = Module['__cuiVideo'];
            if (!ns)
                ns = Module['__cuiVideo'] = {states: {}, nextId: 1};
            var id = ns.nextId++;
            var st = {el: el, objectUrl: null, canvas: null, ctx: null, err: '', seekPending: false};
            ns.states[id] = st;

            try {
                var s = el.style;
                s.position = 'absolute';
                s.left = '-10000px';
                s.top = '0';
                s.width = '1px';
                s.height = '1px';
                s.opacity = '0';
                s.pointerEvents = 'none';
                el.setAttribute('aria-hidden', 'true');
                el.setAttribute('tabindex', '-1');
                // 必须能回读像素 ⇒ 提前要求 CORS：跨域又没头时，这样会在加载期
                // 就报一个明确的媒体错误，而不是每帧 drawImage 之后才抛污染。
                el.crossOrigin = 'anonymous';
                el.preload = 'auto';
                el.loop = false;  // 循环由引擎管（见文件头）
                if (doc.body)
                    doc.body.appendChild(el);
            } catch (e) {
            }

            el.addEventListener('error', function() {
                var m = el.error;
                st.err = m ? ('mediaerror:' + (m.code | 0) + ':' + String(m.message || ''))
                           : 'mediaerror:?';
            });
            el.addEventListener('seeked', function() { st.seekPending = false; });

            try {
                if ($2) {
                    // 内存来源：先当场拷成 JS 侧的 Uint8Array（wasm 堆之后可能
                    // realloc/释放，Blob 不会自己跟着搬），再包 Blob + objectURL。
                    var copy = new Uint8Array($3);
                    copy.set(new Uint8Array(HEAPU8.buffer, $2, $3));
                    var type = $1 ? UTF8ToString($1) : '';
                    var blob = type ? new Blob([copy], {type: type}) : new Blob([copy]);
                    st.objectUrl = URL.createObjectURL(blob);
                    el.src = st.objectUrl;
                } else {
                    el.src = UTF8ToString($0);
                }
            } catch (e) {
                st.err = String((e && e.name) ? e.name : e);
            }
            // src 根本没设进去（URL 非法）时元素不会报错，只能自己标一条。
            if (!el.src && !$2)
                st.err = '来源为空或非法';
            return id;
        }), reinterpret_cast<uintptr_t>(url.c_str()), reinterpret_cast<uintptr_t>(mimePtr),
           reinterpret_cast<uintptr_t>(dataPtr), static_cast<uint32_t>(source.bytes.size()));

        if (id == 0) {
            error = "这个环境没有可用的 HTMLVideoElement（非浏览器宿主，或元素创建失败）";
            return false;
        }
        id_ = id;
        lastPtsUs_ = -1;
        flags_ = 0;
        metaApplied_ = false;
        forceNext_ = true;  // 元数据一到就补出第一帧（海报位画面）
        error_.clear();
        info_ = VideoInfo{};
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = "html5-video";
        info_.rotation = 0;  // 浏览器已经转好了（见文件头）
        info_.frameRate = 0.0;
        // 尺寸/长度/有没有声音：等 loadedmetadata（见文件头「元数据是异步的」）。
        return true;
    }

    const VideoInfo& info() const override { return info_; }

    bool pumpDriven() const override { return true; }
    /// 出声在浏览器那头，本仓不经手 PCM ⇒ 引擎不会建 AudioSink。
    bool hasAudioStream() const override { return false; }

    bool readFrame(VideoFrame& out) override {
        if (id_ == 0)
            return fail("浏览器后端已释放");
        takeError();
        if (!error_.empty())
            return false;

        flags_ = poll();
        if (!metaApplied_ && (flags_ & kWebMetadata) != 0)
            applyMetadata();
        if ((flags_ & kWebMetadata) == 0)
            return false;  // 元数据还没到：这一轮没有新帧（不算错）
        if ((flags_ & kWebSeeking) != 0)
            return false;  // seek 进行中：等它落地，免得贴上旧位置的一帧

        const int64_t ptsUs = secondsToUs(getNum(kNumCurrentTime));
        if (ptsUs < 0)
            return false;
        if (!forceNext_ && ptsUs == lastPtsUs_)
            return false;  // 没有新的一帧呈现

        forceNext_ = false;
        if (!grabPixels(out, ptsUs)) {
            takeError();  // 失败原因可能只体现在 JS 侧抛的异常里
            return false;
        }
        lastPtsUs_ = ptsUs;
        return true;
    }

    bool seekTo(int64_t ptsUs) override {
        if (id_ == 0)
            return false;
        forceNext_ = true;
        const uint32_t low = uint32_t(uint64_t(ptsUs) & 0xFFFFFFFFu);
        const uint32_t high = uint32_t(uint64_t(ptsUs) >> 32);
        EM_ASM((

            {
                var st = Module['__cuiVideo'] && Module['__cuiVideo'].states[$0];
                if (!st)
                    return;
                var el = st.el;
                // 64 位微秒拆两个 32 位过界（EM_ASM 参数只能是整数），
                // `>>> 0` 把可能被当负数传过来的低半字补回无符号。
                // 只传了三个参数（$0=id, $1=high, $2=low）：$3 不存在，
                // 误读它会让 sec 变成 low/1e6（或 NaN）⇒ 拖进度条跳到 0 附近，
                // 甚至抛 InvalidStateError 后卡在 seekPending（画面不再刷新）。
                var sec = (($1 >>> 0) * 4294967296 + ($2 >>> 0)) / 1000000.0;
                st.seekPending = true;
                if (el.readyState < 1) {
                    // 元数据之前设 currentTime 会被忽略甚至抛 InvalidStateError
                    // ⇒ 记下来，loadedmetadata 之后补一次。
                    el.addEventListener('loadedmetadata', function once() {
                        el.removeEventListener('loadedmetadata', once);
                        try {
                            el.currentTime = sec;
                        } catch (e) {
                            st.err = String((e && e.name) ? e.name : e);
                        }
                    });
                    return;
                }
                try {
                    el.currentTime = sec;
                } catch (e) {
                    st.seekPending = false;
                    st.err = String((e && e.name) ? e.name : e);
                }
            }),
            id_, high, low);
        return true;
    }

    bool atEnd() const override { return (flags_ & kWebEnded) != 0; }

    const std::string& error() const override { return error_; }

    void pumpSetPlaying(bool playing) override {
        if (id_ == 0)
            return;
        const int what = playing ? 0 : 1;
        EM_ASM((

            {
                var st = Module['__cuiVideo'] && Module['__cuiVideo'].states[$0];
                if (!st)
                    return;
                var el = st.el;
                if ($1 == 0) {
                    var p;
                    try {
                        p = el.play();
                    } catch (e) {
                        st.err = String((e && e.name) ? e.name : e);
                        return;
                    }
                    // play() 返回 Promise；不接住 reject 就是一条 unhandled
                    // rejection（浏览器控制台红一片，且我们看不到原因）。
                    if (p && typeof p['catch'] === 'function') {
                        p['catch'](function(e) {
                            // pause() 打断 play() 是正常竞态，不算错误。
                            if (e && e.name == 'AbortError')
                                return;
                            st.err = String((e && e.name) ? e.name : e);
                        });
                    }
                } else {
                    try {
                        el.pause();
                    } catch (e) {
                    }
                }
            }),
            id_, what);
    }

    void pumpSetSpeed(double speed) override {
        if (id_ == 0 || !(speed > 0.0))
            return;
        setMilli(kSetPlaybackRate, int32_t(speed * 1000.0));
    }

    void pumpSetVolume(double volume) override {
        if (id_ == 0)
            return;
        const double v = std::clamp(volume, 0.0, 1.0);
        setMilli(kSetVolume, int32_t(v * 1000.0));
        // 静音顺带置 muted：一是 volume=0 时浏览器仍会占着音频焦点，二是
        // 「muted 的元素」才允许在没有用户手势时自动播放。
        setMilli(kSetMuted, v <= 0.0 ? 1000 : 0);
    }

    int64_t pumpPositionUs() const override {
        if (id_ == 0 || !metaApplied_)
            return -1;  // 还没准备好：让引擎按它的兜底口径处理
        return secondsToUs(getNum(kNumCurrentTime));
    }

private:
    bool fail(const std::string& text) {
        error_ = text;
        return false;
    }

    int poll() const {
        return EM_ASM_INT((

            {
                var st = Module['__cuiVideo'] && Module['__cuiVideo'].states[$0];
                if (!st)
                    return 8;  // 已经没有元素了：当错误
                var el = st.el;
                var f = 0;
                if (el.readyState >= 1)
                    f |= 1;
                if (el.readyState >= 2)
                    f |= 2;
                if (el.ended)
                    f |= 4;
                if (st.err)
                    f |= 8;
                if (el.seeking || st.seekPending)
                    f |= 16;
                return f;
            }),
            id_);
    }

    double getNum(int what) const {
        return EM_ASM_DOUBLE((

            {
                var st = Module['__cuiVideo'] && Module['__cuiVideo'].states[$0];
                if (!st)
                    return -1;
                var el = st.el;
                switch ($1) {
                    case 0:
                        return el.duration;  // 可能是 NaN / Infinity
                    case 1:
                        return el.videoWidth || 0;
                    case 2:
                        return el.videoHeight || 0;
                    case 3:
                        return el.currentTime || 0;
                    case 4: {
                        var t = el.audioTracks;  // 只有 Chrome 的实验开关下有
                        return (t && typeof t.length === 'number') ? t.length : -1;
                    }
                }
                return -1;
            }),
            id_, what);
    }

    /// 千分位整数过界（见文件头铁律 2）。JS 那边除回 1000。
    void setMilli(int what, int32_t milli) const {
        EM_ASM((

            {
                var st = Module['__cuiVideo'] && Module['__cuiVideo'].states[$0];
                if (!st)
                    return;
                var el = st.el;
                var v = $2 / 1000;
                try {
                    if ($1 == 0)
                        el.playbackRate = v;
                    else if ($1 == 1)
                        el.volume = v;
                    else
                        el.muted = v > 0;
                } catch (e) {
                    st.err = String((e && e.name) ? e.name : e);
                }
            }),
            id_, what, milli);
    }

    /// loadedmetadata 之后补一次真实信息（尺寸/长度/有没有声音）。
    void applyMetadata() {
        metaApplied_ = true;
        info_.width = int(std::llround(getNum(kNumWidth)));
        info_.height = int(std::llround(getNum(kNumHeight)));
        const double seconds = getNum(kNumDuration);
        // 直播流是 Infinity ⇒ 报 0（进度条因此不可用，但不会算出个假长度）。
        info_.duration =
                std::isfinite(seconds) && seconds > 0.0
                        ? std::chrono::microseconds(std::llround(seconds * 1000000.0))
                        : std::chrono::microseconds(0);
        const double tracks = getNum(kNumAudioTracks);
        info_.hasAudio = tracks < 0.0 ? true : tracks > 0.0;  // 未知 ⇒ 按有声上报
    }

    /// drawImage + getImageData + RGBA→BGRA，一次 EM_ASM 做完（铁律 1）。
    bool grabPixels(VideoFrame& out, int64_t ptsUs) {
        // 分辨率可能随自适应流变，所以每次都现取编码尺寸并据此配缓冲；
        // 与 info_ 不一致时顺手改 info_（引擎每拍比对，UI 会跟上）。
        const int w = int(std::llround(getNum(kNumWidth)));
        const int h = int(std::llround(getNum(kNumHeight)));
        if (w <= 0 || h <= 0)
            return false;
        if (w != info_.width || h != info_.height) {
            info_.width = w;
            info_.height = h;
        }
        const size_t pixels = size_t(w) * size_t(h);
        if (pixels_.size() < pixels)
            pixels_.resize(pixels);

        const int count = EM_ASM_INT((

            {
                var st = Module['__cuiVideo'] && Module['__cuiVideo'].states[$0];
                if (!st)
                    return 0;
                var el = st.el;
                var w = el.videoWidth | 0;
                var h = el.videoHeight | 0;
                if (!w || !h || el.readyState < 2)
                    return 0;
                if ($2 < w * h)
                    return -1;  // 缓冲不够（分辨率刚变大）⇒ 这一帧丢掉
                if (!st.ctx) {
                    st.canvas = document.createElement('canvas');
                    // willReadFrequently：我们每帧都要 getImageData，不加这句
                    // Chrome 会把 canvas 放在 GPU 上、每帧回读一次同步等待。
                    st.ctx = st.canvas.getContext('2d', {willReadFrequently: true});
                }
                if (!st.ctx) {
                    st.err = 'Canvas2DUnavailable';
                    return 0;
                }
                if (st.canvas.width != w)
                    st.canvas.width = w;
                if (st.canvas.height != h)
                    st.canvas.height = h;
                try {
                    st.ctx.drawImage(el, 0, 0, w, h);
                    var src = st.ctx.getImageData(0, 0, w, h).data;  // RGBA
                    // BGRA 的字节序 = 小端 uint32 `B | G<<8 | R<<16 | A<<24`。
                    var dst = $1 >>> 2;
                    var n = w * h;
                    for (var i = 0; i < n; i++) {
                        var o = i << 2;
                        HEAPU32[dst + i] =
                            (src[o + 2] | (src[o + 1] << 8) | (src[o] << 16) | 0xFF000000) | 0;
                    }
                    return n;
                } catch (e) {
                    st.err = String((e && e.name) ? e.name : e);
                    if (e && e.message)
                        st.err += ':' + e.message;
                    return 0;
                }
            }),
            id_, reinterpret_cast<uintptr_t>(pixels_.data()),
            static_cast<uint32_t>(pixels));

        if (count <= 0) {
            if (count == -1)
                pixels_.clear();  // 下一轮按新尺寸重新分配
            return false;
        }

        const SkImageInfo imageInfo =
                SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
        sk_sp<SkData> data = SkData::MakeWithCopy(pixels_.data(), size_t(count) * sizeof(uint32_t));
        out.image = SkImages::RasterFromData(imageInfo, std::move(data), w * 4);
        if (!out.image)
            return fail("SkImages::RasterFromData 失败");
        out.ptsUs = ptsUs;
        return true;
    }

    /// 把 JS 侧的异常文本取回来（取完即清）并翻成中文写进 `error_`。
    /// 「拷 + 清」在一次调用里做完，分两步会丢「问的时候又冒出来」的那一条。
    void takeError() {
        if (id_ == 0)
            return;
        char buf[kWebErrorCap] = {};
        const int len = EM_ASM_INT((

            {
                var st = Module['__cuiVideo'] && Module['__cuiVideo'].states[$0];
                if (!st || !st.err)
                    return 0;
                var s = st.err;
                st.err = '';
                var n = s.length;
                if (n > $2)
                    n = $2;
                // 只按 charCode 逐字节写（非 ASCII 压成 '?'）：异常文本基本都是
                // ASCII，为这一条引一套 UTF-8 编码器不值。
                for (var i = 0; i < n; i++) {
                    var c = s.charCodeAt(i);
                    HEAPU8[$1 + i] = (c > 0x20 && c < 0x80) ? c : 0x3F;
                }
                HEAPU8[$1 + n] = 0;
                return n;
            }),
            id_, reinterpret_cast<uintptr_t>(buf), static_cast<uint32_t>(kWebErrorCap - 1));
        if (len <= 0)
            return;
        error_ = describeJSError(std::string(buf, size_t(len)));
    }

    void destroyElement() {
        if (id_ == 0)
            return;
        const int id = id_;
        id_ = 0;
        EM_ASM((

            {
                var ns = Module['__cuiVideo'];
                var st = ns && ns.states[$0];
                if (!st)
                    return;
                delete ns.states[$0];
                var el = st.el;
                try {
                    el.pause();
                } catch (e) {
                }
                // 摘掉 src 再 load()：否则浏览器会继续占着这条连接和这台解码器。
                try {
                    el.removeAttribute('src');
                    el.load();
                } catch (e) {
                }
                if (st.objectUrl) {
                    try {
                        URL.revokeObjectURL(st.objectUrl);
                    } catch (e) {
                    }
                }
                if (el.parentNode)
                    el.parentNode.removeChild(el);
            }),
            id);
    }

    int id_ = 0;  ///< JS 侧状态表里的序号；0 = 没有元素
    VideoInfo info_;
    std::string error_;
    int flags_ = 0;
    bool metaApplied_ = false;  ///< info_ 已经按 loadedmetadata 补过
    bool forceNext_ = false;    ///< seek/首帧：不看 currentTime 也补一帧
    int64_t lastPtsUs_ = -1;
    std::vector<uint32_t> pixels_;  ///< BGRA，按字存（JS 那边写 HEAPU32）
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> createBrowserVideoBackend(const VideoSource& source,
                                                               std::string& error) {
    auto backend = std::make_unique<BrowserVideoBackend>();
    if (!backend->open(source, error))
        return nullptr;
    return backend;
}

/// 运行期探测：有 `HTMLVideoElement` 还不够，得真的认识某种网页视频编码，
/// 否则「可用」是假的。`canPlayType` 只回答「这个引擎说它可能解」；具体某个
/// 文件能不能解，只能在 `open()` 之后由 error 事件见分晓。
bool browserVideoAvailable(std::string& note) {
    const int ok = EM_ASM_INT((
{
        if (typeof document === 'undefined' || !document)
            return 0;
        if (typeof document.createElement !== 'function')
            return 0;
        if (typeof HTMLVideoElement === 'undefined')
            return 0;
        try {
            var v = document.createElement('video');
            if (!v || typeof v.canPlayType !== 'function')
                return 0;
            // 空串 = 不支持；'maybe' / 'probably' 都算有这条路。
            return (v.canPlayType('video/mp4; codecs="avc1.42E01E, mp4a.40.2"') ||
                    v.canPlayType('video/webm; codecs="vp8, vorbis"') ||
                    v.canPlayType('video/ogg; codecs="theora, vorbis"'))
                         ? 1
                         : 0;
        } catch (e) {
            return 0;
        }
    }));
    if (ok == 0)
        note = "这个宿主没有 HTMLVideoElement，或不支持 MP4/WebM/OGV 中的任何一种";
    return ok != 0;
}

}  // namespace cui_core

#endif  // CUI_HAVE_BROWSER_VIDEO
