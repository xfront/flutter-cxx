/// @file audio_sink_win32.cpp
/// @brief Windows 出声设备（WASAPI 共享模式 + 事件驱动搬运线程）+ 主时钟来源。
///
/// 为什么是共享模式而不是独占：独占能把设备钉死成音轨的采样率（连重采样都省了），
/// 但它会把整块声卡占下来 —— 浏览器/音乐播放器同时出声就失败，而「放视频时用户
/// 正在听别的东西」是常态。共享模式才是 WASAPI 的默认姿势，代价全落在下面那条
/// 格式差异上。
///
/// 格式差异由本层抹平（这是与 AAudio / ALSA 两份最关键的不同）：共享模式只能按
/// 设备的 mix format 开（`GetMixFormat`，通常是 IEEE float / 48000 / 2ch），而
/// `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` 官方口径**只对独占模式有效** ⇒ 系统不会
/// 替我们转换。于是：
///   - 44.1k 音轨（极常见）落在 48k 的设备上 ⇒ 线性插值重采样（够用，不是高质量
///     曲线；本仓要的是「有声且不漂」，不是 HiFi）；
///   - 单声道音轨 ⇒ 铺满每一路；声道数比设备少 ⇒ 多出来的那几路静音（正确的 2→5.1
///     上混要 APO，不在这一层做）；声道数比设备多 ⇒ 只保留前面那几路（5.1 → 立体声
///     会丢环绕与 LFE，但不会像按字节拷贝那样串成噪声）；
///   - 设备 mix format 是 16bit 定点（少数老驱动）⇒ 落点转成 S16；是别的位深就
///     直接谈不拢 ⇒ `open()` 失败退回无声。
/// `AudioRing` 内部恒 F32 交织且音量在那里施加，所以本层的换算发生在「已经乘过
/// 音量」的浮点域，没有二次量化。
///
/// 位置口径与 ALSA 那份相同：环形缓冲的消耗 = 已交给设备，`GetCurrentPadding()`
/// 里那些还没响的要扣掉（`positionUsMinus`）。搬运线程每次**只按设备腾出的空位取
/// 帧** ⇒ 取帧速率恒等于设备消耗速率，主时钟才跟着真实时间走。ALSA 那边还要额外
/// 加一条墙钟预算（PipeWire 的 alsa 插件虚报 avail，见 `audio_sink_alsa.cpp` 文件
/// 头），WASAPI 的 padding 由音频引擎自己给，不需要那层保护。
///
/// COM 只出现在搬运线程上：`open()` 里所有 `IAudioClient` 调用都由那条线程做完
/// （`CoInitializeEx` 一次、退出时 `CoUninitialize`），GUI 线程的
/// `start()`/`stop()`/`flush()`/`positionUs()` 只碰原子量加一个自建的唤醒事件
/// —— 于是「裸 COM 指针跨 apartment」这个隐患根本不存在，而不是只被锁挡住。代价
/// 是启停与 flush 最多晚一个轮询周期（20ms）生效，听不出来。
///
/// 真机口径（2026-11-07，MSVC 14.44 + SDK 10.0.26100.0，默认播放设备）：设备 mix
/// format 是 48000 / 2ch / 32bit 的 `WAVE_FORMAT_EXTENSIBLE` + IEEE float ⇒ 交上去
/// 的 44.1k 音轨走的正是上面那条重采样慢路径。实测两秒正弦：1012ms 墙钟里
/// `positionUs()` 前进 978ms（差的 ~22ms = 一个 20ms 轮询周期，就是上面那条代价），
/// `stop()` 之后位置冻结、`flush(1s)` 把基准挪到 1s 且 `queuedUs()` 归零、恢复后半
/// 秒再前进 478ms。
///
/// 兜底与 MF 后端同一条口径 —— 起不来（没有默认设备 / AudioSrv 没跑 / 格式谈不拢）
/// 就 `open()` 返回 false ⇒ 播放引擎退回「无声 + 墙钟节拍」，不算错误。

#include "cui/video/audio_sink.h"

#include "audio_ring.h"

#if defined(_WIN32) && __has_include(<audioclient.h>)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// `<initguid.h>` 必须在设备/音频头之前：`KSDATAFORMAT_SUBTYPE_*` 在 ksmedia.h 里走
// `DEFINE_GUIDSTRUCT`，只有先包含它才会在本 TU 里给出定义（不必再链任何库）。
// 注意它**管不到** WASAPI 那几个 `IID_`/`CLSID_` —— 见下面的 GUID 常量块。
#include <initguid.h>

#include <windows.h>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef AUDCLNT_STREAMFLAGS_EVENTCALLING
// 事件驱动旗标的拼写：Windows SDK（实测 10.0.26100.0 的 AudioSessionTypes.h:134）
// 只给 `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`，值 0x00040000；部分 mingw-w64 头另给
// `..._EVENTCALLING` 这个同值别名。补一句让下面的调用点两种头都编得过。
#define AUDCLNT_STREAMFLAGS_EVENTCALLING AUDCLNT_STREAMFLAGS_EVENTCALLBACK
#endif

namespace cui {
namespace {

/// WASAPI 那四个 COM 标识的取法 —— 两套工具链不一样，而且是能不能链接的关键。
///
/// MSVC 侧：本机 SDK（实测 10.0.26100.0）对它们**只有声明** —— `mmdeviceapi.h:753`
/// 的 `EXTERN_C const IID IID_IMMDeviceEnumerator;`、`audioclient.h:243` 的
/// `EXTERN_C const IID IID_IAudioClient;`，定义既不落在任何头里，也不落在
/// `Lib/10.0.26100.0/um/x64` 下任何一个导入库里（`dumpbin /linkermember:1` 逐个扫过
/// Uuid.Lib / OneCore.Lib 等，命中 0；同目录的 mfuuid.lib 反倒有
/// `IID_IMFPresentationDescriptor`，所以 MF 那一路不受影响）。于是
/// `<initguid.h>` 变不出它们，链接期直接 LNK2019。正解是编译期的 `__uuidof`：接口带
/// `MIDL_INTERFACE("…")`、coclass 带 `DECLSPEC_UUID("…")`，值随本次编译进本 TU，
/// 不需要任何库。
///
/// mingw 侧：gcc 没有 `__uuidof`，但它的头用 `DEFINE_GUID` 给出了真符号（值与上面
/// 同一套 IDL），而且这一端只跑 `-fsyntax-only` 从不链接，照用声明即可。
#if defined(_MSC_VER)
    #define CUI_WASAPI_CLSID_ENUMERATOR __uuidof(MMDeviceEnumerator)
    #define CUI_WASAPI_IID_ENUMERATOR __uuidof(IMMDeviceEnumerator)
    #define CUI_WASAPI_IID_AUDIO_CLIENT __uuidof(IAudioClient)
    #define CUI_WASAPI_IID_AUDIO_RENDER_CLIENT __uuidof(IAudioRenderClient)
#else
    #define CUI_WASAPI_CLSID_ENUMERATOR CLSID_MMDeviceEnumerator
    #define CUI_WASAPI_IID_ENUMERATOR IID_IMMDeviceEnumerator
    #define CUI_WASAPI_IID_AUDIO_CLIENT IID_IAudioClient
    #define CUI_WASAPI_IID_AUDIO_RENDER_CLIENT IID_IAudioRenderClient
#endif

/// 极简 COM 智能指针（与 `video_decoder_win32.cpp` 里那份同形：只有
/// `put/get/attach/reset`，禁拷贝）。本仓不引 Windows Runtime / WIL，
/// `Microsoft::WRL::ComPtr` 在 mingw 上也不全，所以自带一份。
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
    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

std::string hrText(const char* what, HRESULT hr) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

/// 一次轮询等待的上限（毫秒）。设备事件正常会自己敲门，这个超时只用来保证
/// 「暂停 / flush 这类状态变化」即使没人 SetEvent 也能在有限时间内被搬运线程看到。
constexpr DWORD kWaitTimeoutMs = 20;

class WasapiSink final : public AudioSink {
public:
    WasapiSink() = default;
    ~WasapiSink() override { close(); }

    WasapiSink(const WasapiSink&) = delete;
    WasapiSink& operator=(const WasapiSink&) = delete;

    bool open(const AudioFormat& format, std::string& error) override {
        if (!format.isValid()) {
            error = "AudioFormat 无效";
            return false;
        }
        ring_.reset(format);
        trackRate_ = format.sampleRate;
        trackChannels_ = std::max(1, format.channels);
        // 换算用的 scratch（只在搬运线程碰）：一次凑这么多帧来率数据，48000/44100
        // 这类比值用上很宽裕，refill 摊销掉的成本可以忽略。
        scratchFrames_ = std::max<size_t>(1024, size_t(trackRate_) / 4);
        scratch_.assign((scratchFrames_ + 2) * size_t(trackChannels_), 0.0f);
        scratchPos_ = scratchCount_ = 0;
        frac_ = 0.0;

        wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wakeEvent_ == nullptr) {
            error = "CreateEvent 失败";
            return false;
        }
        stopped_.store(false);
        running_.store(false);
        resetRequested_.store(false);
        inFlightTrackFrames_.store(0);
        worker_ = std::thread([this] { feedLoop(); });

        // 等设备真的开起来再返回：WASAPI 的失败（没有默认输出设备、AudioSrv 停了、
        // mix format 谈不拢）全是即时的，而调用方要靠这个返回值决定「有声还是无声」。
        std::unique_lock<std::mutex> lock(readyMutex_);
        readyCv_.wait(lock, [this] { return readyDone_; });
        error = readyError_;
        if (!readyOk_) {
            lock.unlock();
            // 线程在发出失败信号后就会退出（见 `feedLoop` 开头），join 不会等太久。
            stopped_.store(true);
            SetEvent(wakeEvent_);
            if (worker_.joinable())
                worker_.join();
            CloseHandle(wakeEvent_);
            wakeEvent_ = nullptr;
            return false;
        }
        opened_.store(true);
        return true;
    }

    void close() override {
        stopped_.store(true);
        if (wakeEvent_ != nullptr)
            SetEvent(wakeEvent_);
        if (worker_.joinable())
            worker_.join();
        if (wakeEvent_ != nullptr) {
            CloseHandle(wakeEvent_);
            wakeEvent_ = nullptr;
        }
        opened_.store(false);
        // 没开起来时线程根本没碰设备（`open()` 失败那条路已经 join 过了），收尾
        // 全在 `shutdownDevice()` 里做完了，这里不必再动 COM。
    }

    void start() override {
        if (!opened_.load() || running_.exchange(true))
            return;
        SetEvent(wakeEvent_);
    }

    void stop() override {
        if (!opened_.load() || !running_.exchange(false))
            return;
        // `Stop()` 保留已交给设备的采样（与 AAudio 的 requestPause、ALSA 的 pause
        // 同一条语义）：恢复后继续播，而暂停期间环形缓冲不再被消耗 ⇒ 位置冻住。
        SetEvent(wakeEvent_);
    }

    void flush(int64_t ptsUs) override {
        ring_.clear(ptsUs);
        inFlightTrackFrames_.store(0);
        if (!opened_.load())
            return;
        // 设备里剩下的采样也要丢：`Reset()` 只在停止态合法，所以这事交给搬运线程
        // （它先 Stop、再 Reset、播放中就再 Start），这里只递一个请求。
        resetRequested_.store(true);
        SetEvent(wakeEvent_);
    }

    size_t push(const AudioChunk& chunk) override { return ring_.push(chunk); }

    void setVolume(float volume) override { ring_.setVolume(volume); }

    int64_t positionUs() const override {
        // 「读出来了但还没响」的两段都要从已消耗里扣掉：设备缓冲里的
        // `inFlightTrackFrames_`，和重采样那条路上还躺在 `scratch_` 里的
        // `stagedFrames_`（少了后者，来率一低就等于凭空快进一整块 scratch：
        // 1000Hz 音轨 + 48kHz 设备实测一开播就报到 ~1002ms，而真实时间才走 20ms）。
        return ring_.positionUsMinus(inFlightTrackFrames_.load() + stagedFrames_.load());
    }
    int64_t queuedUs() const override { return ring_.queuedUs(); }

private:
    // ── 搬运线程 ──────────────────────────────────────────────────────

    /// 这一条线程是唯一碰 COM 与 `IAudioClient` 的地方（见文件头那条纪律）。
    void feedLoop() {
        std::string initError;
        const bool ok = initDevice(initError);
        {
            std::lock_guard<std::mutex> lock(readyMutex_);
            readyOk_ = ok;
            readyError_ = initError;
            readyDone_ = true;
        }
        readyCv_.notify_all();
        if (!ok) {
            shutdownDevice();
            return;
        }

        HANDLE handles[2] = {playEvent_, wakeEvent_};
        bool started = false;
        while (!stopped_.load()) {
            // 1) flush/seek 的请求：`Reset()` 只在停止态合法 ⇒ Stop 之后才能丢设备里
            //    的残留；播不播由下一轮的 `running` 定（这里不 Start）。
            if (resetRequested_.exchange(false)) {
                if (started) {
                    client_->Stop();
                    started = false;
                }
                client_->Reset();
                inFlightTrackFrames_.store(0);
                stagedFrames_.store(0);
                // 插值光标跟着作废：基准已经搬到新的 pts，继续用旧残留会插出一段
                // 上一个位置的样本。
                scratchPos_ = scratchCount_ = 0;
                frac_ = 0.0;
            }

            const bool running = running_.load();

            // 2) 暂停：把设备按下去（`Stop()` 保留缓冲里的采样，恢复后接着播），
            //    并且**不碰环形缓冲** ⇒ 消耗不推进 ⇒ 位置冻住（`AudioSink::stop()` 的
            //    语义就落在这一步）。只等唤醒事件，带超时兜底。
            if (!running) {
                if (started) {
                    client_->Stop();
                    started = false;
                }
                WaitForSingleObject(wakeEvent_, kWaitTimeoutMs);
                continue;
            }

            // 3) 该播但还没开播（首次 / 刚 flush 过）：先把能填的空位填满再 Start。
            //    反过来的话引擎第一拍消费的就是设备缓冲里的未定义内容 ⇒ 开头一声
            //    爆音（`Reset()` 之后缓冲是空的，所以 seek 后重新开播也走这一支）。
            UINT32 filled = 0;
            if (!started) {
                if (!fillFree(filled))
                    break;  // 设备没了（拔耳机 / AudioSrv 停）：退出线程，播放侧退回无声。
                if (FAILED(client_->Start()))
                    break;  // 同上：Initialize 之后设备被拔掉就会走到这里，别原地空转。
                started = true;
                WaitForMultipleObjects(2, handles, FALSE, kWaitTimeoutMs);
                continue;
            }

            // 4) 正常搬运：等引擎敲门（或 GUI 那边的唤醒），然后把腾出来的空位填满。
            //    「设备要多少给多少」就是主时钟的节拍本身，所以这里不需要 ALSA 那份
            //    额外的墙钟预算（见文件头）。
            WaitForMultipleObjects(2, handles, FALSE, kWaitTimeoutMs);
            if (!fillFree(filled))
                break;
        }

        if (started)
            client_->Stop();
        shutdownDevice();
    }

    /// 问一次 padding、把设备腾出来的空位全部填满。返回 false = 设备调用已经失败
    /// （拔设备 / 服务停），调用方该收线程了。只在搬运线程里调。
    bool fillFree(UINT32& filled) {
        filled = 0;
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding)))
            return false;
        inFlightTrackFrames_.store(framesToTrackFrames(padding));
        const UINT32 free = padding < bufferFrames_ ? bufferFrames_ - padding : 0;
        if (free == 0)
            return true;
        BYTE* data = nullptr;
        if (FAILED(render_->GetBuffer(free, &data)) || data == nullptr)
            return true;  // 拿不到写指针不算设备坏了，下一拍再问
        fillDeviceFrames(data, free);
        render_->ReleaseBuffer(free, 0);
        filled = free;
        // 刚交给设备的那些也还没响 ⇒ 「已消耗 - 在飞」才是听众听到的位置；padding 是
        // 填之前问的，所以这里得把 `filled` 一起算进去。
        inFlightTrackFrames_.store(framesToTrackFrames(padding + filled));
        return true;
    }

    bool initDevice(std::string& error) {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // S_FALSE = 这条线程已经初始化过（同模式），但它照样加了一次计数 ⇒ 仍要配对
        // CoUninitialize。RPC_E_CHANGED_MODE 是**失败**、没加计数（别人在这条线程上
        // 开过 STA；本仓搬运线程是自己起的，正常碰不到），这时再 CoUninitialize 就会
        // 把别人的那次初始化减掉，所以只认 SUCCEEDED。
        comInitialized_ = SUCCEEDED(co);

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(CUI_WASAPI_CLSID_ENUMERATOR, nullptr, CLSCTX_ALL,
                                      CUI_WASAPI_IID_ENUMERATOR,
                                      reinterpret_cast<void**>(enumerator.put()));
        if (FAILED(hr)) {
            error = hrText("CoCreateInstance(MMDeviceEnumerator) 失败", hr);
            return false;
        }
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
        if (FAILED(hr) || !device) {
            error = hrText("没有默认播放设备", hr);
            return false;
        }
        hr = device->Activate(CUI_WASAPI_IID_AUDIO_CLIENT, CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client_.put()));
        if (FAILED(hr) || !client_) {
            error = hrText("激活 IAudioClient 失败", hr);
            return false;
        }

        WAVEFORMATEX* mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr) || mix == nullptr) {
            error = hrText("GetMixFormat 失败", hr);
            return false;
        }
        const bool parsed = readMixFormat(*mix);
        const WORD bits = mix->wBitsPerSample;
        if (!parsed) {
            CoTaskMemFree(mix);
            error = "不支持的设备 mix format（位深 " + std::to_string(int(bits)) + "）";
            return false;
        }

        playEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (playEvent_ == nullptr) {
            CoTaskMemFree(mix);
            error = "CreateEvent 失败";
            return false;
        }
        // `hnsBufferDuration = 0` ⇒ 共享模式按设备默认时长给缓冲（就是 MSDN 示例的
        // 写法），随后用 `GetBufferSize` 问出帧数；`hnsPeriodicity` 在共享模式下必须为 0。
        // `pFormat` 就是刚才那份 mix format：共享模式只认它，所以不自己「把音轨
        // 的格式递上去」，差异全部由 `fillConverted` 抹平（见文件头）。
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLING, 0, 0,
                                 mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) {
            error = hrText("IAudioClient::Initialize 失败", hr);
            return false;
        }
        // SetEventHandle 必须在 Start 之前；放在 Initialize 之后是 MSDN 示例的顺序。
        hr = client_->SetEventHandle(playEvent_);
        if (FAILED(hr)) {
            error = hrText("SetEventHandle 失败", hr);
            return false;
        }
        hr = client_->GetService(CUI_WASAPI_IID_AUDIO_RENDER_CLIENT,
                                 reinterpret_cast<void**>(render_.put()));
        if (FAILED(hr) || !render_) {
            error = hrText("GetService(IAudioRenderClient) 失败", hr);
            return false;
        }
        hr = client_->GetBufferSize(&bufferFrames_);
        if (FAILED(hr) || bufferFrames_ == 0) {
            error = hrText("GetBufferSize 失败", hr);
            return false;
        }
        return true;
    }

    void shutdownDevice() {
        render_.reset();
        client_.reset();
        if (playEvent_ != nullptr) {
            CloseHandle(playEvent_);
            playEvent_ = nullptr;
        }
        if (comInitialized_) {
            comInitialized_ = false;
            CoUninitialize();
        }
    }

    /// mix format（含 `WAVEFORMATEXTENSIBLE` 的 SubFormat）→ `mixRate_/mixChannels_/mixFloat_`。
    /// 只认 float32 与 s16 两种落点，其余位深返回 false（无声，好过出坏声音）。
    bool readMixFormat(const WAVEFORMATEX& fx) {
        if (fx.nSamplesPerSec == 0 || fx.nChannels == 0)
            return false;
        GUID sub = KSDATAFORMAT_SUBTYPE_PCM;
        if (fx.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(fx);
            sub = ext.SubFormat;
        } else if (fx.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            sub = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        mixFloat_ = IsEqualGUID(sub, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const WORD bits = fx.wBitsPerSample;
        if (mixFloat_ && bits != 0 && bits != 32)
            return false;
        if (!mixFloat_ && bits != 0 && bits != 16)
            return false;
        mixRate_ = int(fx.nSamplesPerSec);
        mixChannels_ = int(fx.nChannels);
        return mixRate_ > 0 && mixChannels_ > 0;
    }

    /// 设备帧数 → 来率（音轨采样率）下的帧数：环形缓冲记的是「来率的帧」，
    /// 而 padding 是设备率的，不换算就会把位置算少。
    int64_t framesToTrackFrames(UINT32 deviceFrames) const {
        if (mixRate_ <= 0 || trackRate_ <= 0)
            return 0;
        if (mixRate_ == trackRate_)
            return int64_t(deviceFrames);
        return int64_t(deviceFrames) * int64_t(trackRate_) / int64_t(mixRate_);
    }

    /// 把 `deviceFrames` 帧设备数据落到 `dst`（按 mix format 的交织布局）。
    /// 顺带把「已从环形缓冲读出、但还没交给设备」的那一段报给 `positionUs()`：
    /// 环形缓冲的消耗计数是在 `readF32` 那一刻推进的，而慢路径是**先取一整块
    /// scratch 再逐帧插值**，所以那块没用完的部分其实还没响过。
    void fillDeviceFrames(void* dst, UINT32 deviceFrames) {
        // 快路径：格式完全一致 ⇒ 一次读到设备缓冲里，零拷贝零换算。
        if (trackRate_ == mixRate_ && trackChannels_ == mixChannels_) {
            stagedFrames_.store(0);
            if (mixFloat_)
                ring_.readF32(static_cast<float*>(dst), size_t(deviceFrames));
            else
                ring_.readS16(static_cast<int16_t*>(dst), size_t(deviceFrames));
            return;
        }
        if (mixFloat_)
            fillConverted(static_cast<float*>(dst), deviceFrames);
        else
            fillConverted(static_cast<int16_t*>(dst), deviceFrames);
        // 多扣一帧（光标那一帧只播出了 `frac_` 那么远）⇒ 位置只会偏保守，不会虚高。
        stagedFrames_.store(scratchCount_ > scratchPos_
                                    ? int64_t(scratchCount_ - scratchPos_)
                                    : 0);
    }

    /// 慢路径：从环形缓冲按来率取帧到 `scratch_`，线性插值 + 声道映射，落点按
    /// mix format 是 float 还是 s16 决定（模板只为了少写一遍循环体）。
    template <typename Dst>
    void fillConverted(Dst* dst, UINT32 deviceFrames) {
        const size_t inCh = size_t(trackChannels_);
        const size_t outCh = size_t(mixChannels_);
        const double step = double(trackRate_) / double(mixRate_);
        // `GetBuffer` 交出来的内存是未初始化的，而 `ReleaseBuffer(free, 0)` 会把整段
        // 都提交给引擎 ⇒ 下面任何一处提前 break 都必须留下静音而不是随机噪声。
        std::memset(dst, 0, size_t(deviceFrames) * outCh * sizeof(Dst));
        for (UINT32 d = 0; d < deviceFrames; ++d) {
            if (scratchPos_ + 2 > scratchCount_ && !refillScratch())
                break;
            const float* a = scratch_.data() + scratchPos_ * inCh;
            const float* b = a + inCh;
            const float t = float(frac_);
            for (size_t c = 0; c < outCh; ++c) {
                const size_t src = sourceChannel(c);
                const float v = src < inCh ? (a[src] + (b[src] - a[src]) * t) : 0.0f;
                storeSample(dst + size_t(d) * outCh + c, v);
            }
            frac_ += step;
            while (frac_ >= 1.0) {
                frac_ -= 1.0;
                ++scratchPos_;
            }
        }
    }

    /// 保证插值的左右两端都在 `scratch_` 里：不够就把光标那一帧搬到开头再补一块。
    /// `readF32` 欠载时补零且照样计消耗 ⇒ 这里总能拿到要的长度。
    bool refillScratch() {
        const size_t ch = size_t(trackChannels_);
        size_t kept = 0;
        if (scratchPos_ < scratchCount_) {
            std::memmove(scratch_.data(), scratch_.data() + scratchPos_ * ch,
                         ch * sizeof(float));
            kept = 1;
        }
        const size_t want = scratchFrames_;
        ring_.readF32(scratch_.data() + kept * ch, want);
        scratchPos_ = 0;
        scratchCount_ = kept + want;
        return scratchCount_ >= 2;
    }

    /// 设备声道 c 对应哪一个来声道；`size_t(-1)` = 静音。
    /// 单声道要铺满每一路（否则右声道全哑）；比音轨多的那几路静音；比音轨少就只
    /// 保留前面那几路（不做真正的下混矩阵）。
    size_t sourceChannel(size_t c) const {
        if (trackChannels_ == 1)
            return 0;
        return c < size_t(trackChannels_) ? c : size_t(-1);
    }

    static void storeSample(float* dst, float v) { *dst = v; }
    static void storeSample(int16_t* dst, float v) {
        const float clamped = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        *dst = int16_t(clamped >= 0.0f ? clamped * 32767.0f : clamped * 32768.0f);
    }

    AudioRing ring_;

    // 只在搬运线程读写（GUI 线程看不到）：
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE playEvent_ = nullptr;
    UINT32 bufferFrames_ = 0;
    bool comInitialized_ = false;
    int mixRate_ = 0;
    int mixChannels_ = 0;
    bool mixFloat_ = true;
    std::vector<float> scratch_;
    size_t scratchFrames_ = 0;
    size_t scratchPos_ = 0;
    size_t scratchCount_ = 0;
    double frac_ = 0.0;

    int trackRate_ = 0;
    int trackChannels_ = 2;

    std::thread worker_;
    /// 跨线程状态：GUI 线程置位后 `SetEvent(wakeEvent_)`，搬运线程醒来处理。
    std::atomic<bool> opened_{false};
    std::atomic<bool> stopped_{true};
    std::atomic<bool> running_{false};
    std::atomic<bool> resetRequested_{false};
    /// 「已交给设备但还没响」的帧数（换算成来率），从搬运线程写、GUI 线程读。
    std::atomic<int64_t> inFlightTrackFrames_{0};
    /// 「已从环形缓冲读出、还留在重采样 scratch 里」的帧数：同样是还没响，
    /// 只在搬运线程写（`fillDeviceFrames` 每次填完刷新）、GUI 线程读。
    std::atomic<int64_t> stagedFrames_{0};
    HANDLE wakeEvent_ = nullptr;

    /// `open()` 等这条线程把设备开完的握手（只有 `open()`/线程启动阶段用一次）。
    std::mutex readyMutex_;
    std::condition_variable readyCv_;
    bool readyDone_ = false;
    bool readyOk_ = false;
    std::string readyError_;
};

}  // namespace

/// WASAPI 的可用性只有真去激活设备才知道（没有输出设备 / AudioSrv 停了都会失败），
/// 而格式要等拿到音轨才定 ⇒ 这里无条件给一个未开的对象，`open()` 失败由播放引擎
/// 退回「无声 + 墙钟」，与 ALSA / AAudio 那两份同一条口径。
std::unique_ptr<AudioSink> createPlatformAudioSink() { return std::make_unique<WasapiSink>(); }

}  // namespace cui

#endif  // _WIN32 && <audioclient.h>
