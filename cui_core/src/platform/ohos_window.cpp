/// @file ohos_window.cpp
/// @brief OpenHarmony NDK implementation of PlatformWindow.
///
/// 生命周期与 Android 端相反：surface 由 ArkTS 的 XComponent 创建，C++ 侧只是
/// 拿 surfaceId 换一个 OHNativeWindow（OH_NativeWindow_CreateNativeWindowFromSurfaceId
/// 的文档明写「本进程创建的 surface 可以从本进程取到」⇒ XComponent 场景可用）。
///
/// 关键实现口径：
///   - 呈现 = RequestBuffer → GetBufferHandleFromNative（拿几何）→
///     OH_NativeBuffer_Map → 逐行 memcpy → Unmap → FlushBuffer。
///     OH_NativeWindow_LockBuffer / UnlockAndFlushBuffer 是 API 23 才有的等价快捷路径，
///     本端要覆盖 12+，所以走这一条。
///   - 恒整帧提交：buffer 队列不保证后备区里还留着上一帧内容，按脏区只拷一块会花屏
///     （与 Android 支同一个理由）。FlushBuffer 的 region 因此留空 = 整帧脏。
///   - acquire fence 不等待：本 NDK sysroot 没有任何 fence wait 接口
///     （sync_wait / OH_NativeSync 都不存在），MapWaitFence 又是 API 23；生产侧只有
///     同步的 CPU memcpy，写完才 Flush，所以把 RequestBuffer 交回的 fence 原样转给
///     FlushBuffer（由系统关闭，不泄漏）是最省的口径 —— 它只会让合成器多等一个信号，
///     不会读到半帧。
///   - buffer 的生命周期全在 flushFrame 一次调用里（Request 之后每条退出路径都
///     Flush 或 Abort），所以不存在「队列被耗尽」的中间态需要维护。

#include "cui/platform/ohos_window.h"

#ifdef __OHOS__

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#undef LOG_TAG
#define LOG_TAG "cui_core-window"
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0201
#define LOGI(...) ((void)OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__))

namespace cui {

namespace {

// 宿主传进来的数值口径 = **ArkTS 组件枚举**（ets/component/enums.d.ts），因为输入是从
// ArkTS 的 onTouch/onMouse/onKeyEvent/onAxisEvent 转发下来的，不是 NDK 的
// <ace/xcomponent/*> 枚举。写成常量是为了不为了几个枚举值去拖进整套
// <ace/xcomponent/*> + <arkui/*> 头。
constexpr int32_t kTouchDown = 0;    // TouchType.Down
constexpr int32_t kTouchUp = 1;      // TouchType.Up
constexpr int32_t kTouchMove = 2;    // TouchType.Move
constexpr int32_t kTouchCancel = 3;  // TouchType.Cancel

constexpr int32_t kMousePress = 0;    // MouseAction.Press
constexpr int32_t kMouseRelease = 1;  // MouseAction.Release
constexpr int32_t kMouseMove = 2;     // MouseAction.Move
constexpr int32_t kMouseHover = 3;    // MouseAction.Hover
constexpr int32_t kMouseCancel = 13;  // MouseAction.CANCEL

constexpr int32_t kAxisEnd = 3;  // AxisAction.END

constexpr int32_t kMouseLeft = 0x01;    // bit = MouseButton.Left
constexpr int32_t kMouseRight = 0x02;   // bit = MouseButton.Right
constexpr int32_t kMouseMiddle = 0x04;  // bit = MouseButton.Middle

constexpr int32_t kKeyActionDown = 0;  // KeyType.Down
constexpr int32_t kKeyActionUp = 1;    // KeyType.Up

// ArkUI KeyEvent.keyCode 的实际取值（@ohos.multimodalInput.keyCode：KEYCODE_0=2000
// … KEYCODE_Z=2042 … KEYCODE_MOVE_END=2082；与 NDK OH_NativeXComponent_KeyCode 同套编号）。
constexpr int32_t kOhosKeyDpadUp = 2012;
constexpr int32_t kOhosKeyDpadDown = 2013;
constexpr int32_t kOhosKeyDpadLeft = 2014;
constexpr int32_t kOhosKeyDpadRight = 2015;
constexpr int32_t kOhosKeyTab = 2049;
constexpr int32_t kOhosKeyEnter = 2054;
constexpr int32_t kOhosKeyDel = 2055;  // 退格
constexpr int32_t kOhosKeyPageUp = 2068;
constexpr int32_t kOhosKeyPageDown = 2069;
constexpr int32_t kOhosKeyEscape = 2070;
constexpr int32_t kOhosKeyForwardDel = 2071;
constexpr int32_t kOhosKeyMoveHome = 2081;
constexpr int32_t kOhosKeyMoveEnd = 2082;

void closeFence(int fenceFd) {
    if (fenceFd >= 0)
        ::close(fenceFd);
}

}  // namespace

OhosWindow::OhosWindow() = default;

OhosWindow::~OhosWindow() {
    destroy();
}

// ── PlatformWindow ──────────────────────────────────────────────────────────

void OhosWindow::create(const std::string& title, int width, int height) {
    title_ = title;
    // 尺寸要等 attachSurface 从 buffer 几何里问出来；这里先记下宿主给的值，
    // 免得 attach 之前 width()/height() 是 0 让 AppWindow 建不出 Skia 面。
    if (width_.load() <= 0)
        width_.store(width);
    if (height_.load() <= 0)
        height_.store(height);
    LOGI("create(%{public}s, %{public}d, %{public}d)", title.c_str(), width, height);
}

void OhosWindow::destroy() {
    detachSurface(surfaceId_);
}

void OhosWindow::show() {
    // XComponent 的可见性由 ArkTS 侧管，这里无事可做。
}

void OhosWindow::invalidate() {
    // 与 X11 支同款：只要把事件循环叫醒，下一帧自然会重画。
    wakeUp();
}

void OhosWindow::setTitle(const std::string& title) {
    title_ = title;
}

void OhosWindow::setMinimumSize(int width, int height) {
    (void)width;
    (void)height;  // 窗口尺寸由布局决定，鸿蒙侧没有最小尺寸可设。
}

double OhosWindow::dpiScale() const {
    return density_ > 0.0f ? static_cast<double>(density_) : 1.0;
}

void OhosWindow::wakeUp() {
    eventCv_.notify_all();
}

bool OhosWindow::pollEvent(PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (pendingEvents_.empty())
        return false;
    event = pendingEvents_.front();
    pendingEvents_.pop();
    return true;
}

bool OhosWindow::waitForEvent(PlatformEvent& event, int timeoutMs) {
    std::unique_lock<std::mutex> lock(eventMutex_);
    if (pendingEvents_.empty()) {
        if (timeoutMs == 0)
            return false;
        if (timeoutMs < 0) {
            eventCv_.wait(lock, [this] { return !pendingEvents_.empty(); });
        } else {
            eventCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return !pendingEvents_.empty(); });
        }
    }
    if (pendingEvents_.empty())
        return false;
    event = pendingEvents_.front();
    pendingEvents_.pop();
    return true;
}

PlatformWindow::NativeHandles OhosWindow::nativeHandles() {
    NativeHandles h;
    // OHNativeWindow 同时是呈现目标（presentPixels）与将来的 GPU 目标。
    h.ohos_window = window_;
    return h;
}

// ── surface 生命周期（napi 宿主契约）─────────────────────────────────────────

bool OhosWindow::attachSurface(uint64_t surfaceId) {
    if (window_ && surfaceId_ == surfaceId) {
        PlatformEvent event;
        event.type = EventType::Expose;
        pushEvent(event);
        return true;
    }
    if (window_)
        detachSurface(surfaceId_);

    OHNativeWindow* window = nullptr;
    const int32_t rc = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (rc != 0 || !window) {
        LOGE("CreateNativeWindowFromSurfaceId(%{public}llu) rc=%{public}d",
             static_cast<unsigned long long>(surfaceId), rc);
        return false;
    }
    window_ = window;
    surfaceId_ = surfaceId;

    // usage/format 必须在第一次 RequestBuffer 之前钉好：buffer 队列按这三样分配，
    // CPU 写不进 GPU-only 的 buffer。
    const uint64_t usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_USAGE, usage);
    OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT,
                                          static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_RGBA_8888));

    int32_t width = 0;
    int32_t height = 0;
    // GET_BUFFER_GEOMETRY 的可变参数顺序是「先 height 再 width」（头里明写）。
    OH_NativeWindow_NativeWindowHandleOpt(window_, GET_BUFFER_GEOMETRY, &height, &width);
    if (width > 0 && height > 0) {
        applyGeometry(width, height);
    } else {
        width = width_.load();
        height = height_.load();
        if (width > 0 && height > 0)
            applyGeometry(width, height);
    }

    LOGI("attachSurface(%{public}llu) %{public}dx%{public}d", static_cast<unsigned long long>(surfaceId),
         width_.load(), height_.load());

    PlatformEvent event;
    event.type = EventType::Expose;
    pushEvent(event);
    return true;
}

void OhosWindow::resizeSurface(uint64_t surfaceId, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return;
    if (window_ && surfaceId != surfaceId_) {
        LOGW("resizeSurface(%{public}llu) 与当前 surface(%{public}llu) 不一致，忽略",
             static_cast<unsigned long long>(surfaceId), static_cast<unsigned long long>(surfaceId_));
        return;
    }
    const bool changed = width != width_.load() || height != height_.load();
    applyGeometry(width, height);
    if (!changed)
        return;

    PlatformEvent event;
    event.type = EventType::Resize;
    event.width = width;
    event.height = height;
    pushEvent(event);
}

void OhosWindow::detachSurface(uint64_t surfaceId) {
    if (!window_)
        return;
    if (surfaceId != 0 && surfaceId != surfaceId_) {
        LOGW("detachSurface(%{public}llu) 与当前 surface(%{public}llu) 不一致，忽略",
             static_cast<unsigned long long>(surfaceId), static_cast<unsigned long long>(surfaceId_));
        return;
    }
    LOGI("detachSurface(%{public}llu)", static_cast<unsigned long long>(surfaceId_));
    OH_NativeWindow_DestroyNativeWindow(window_);
    window_ = nullptr;
    surfaceId_ = 0;
    // 不推 Close：XComponent 隐藏 / 转屏都会走这条路，Application 与元素树还活着，
    // 下一次 attachSurface 会重新推 Expose（同 Android onSurfaceDestroyed 的口径）。
}

void OhosWindow::applyGeometry(int32_t width, int32_t height) {
    width_.store(width);
    height_.store(height);
    if (window_)
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_BUFFER_GEOMETRY, width, height);
}

// ── 输入（napi 宿主契约）─────────────────────────────────────────────────────

void OhosWindow::pushEvent(const PlatformEvent& event) {
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        pendingEvents_.push(event);
    }
    eventCv_.notify_all();
}

void OhosWindow::dispatchTouch(int32_t actionType, int32_t pointerId, float x, float y) {
    PlatformEvent pe;
    pe.pos = Point{static_cast<double>(x), static_cast<double>(y)};
    pe.touchId = pointerId;
    pe.button = 1;  // 触摸按左键上报，兼容只订了鼠标的手势

    switch (actionType) {
        case kTouchDown:
            pe.type = EventType::TouchBegin;
            break;
        case kTouchMove:
            pe.type = EventType::TouchUpdate;
            break;
        case kTouchUp:
        case kTouchCancel:
            pe.type = EventType::TouchEnd;
            break;
        default:  // 9..12 = TouchType.HOVER_*：悬停只从 dispatchMouse 进，避免双计
            return;
    }
    pushEvent(pe);

    // 与 Android 支一致：触摸另配一条鼠标事件，鼠标手势（hover/drag）才拿得到输入。
    PlatformEvent mouse;
    mouse.pos = pe.pos;
    mouse.button = 1;
    mouse.type = pe.type == EventType::TouchBegin
                     ? EventType::MouseDown
                     : (pe.type == EventType::TouchUpdate ? EventType::MouseMove : EventType::MouseUp);
    pushEvent(mouse);
}

void OhosWindow::dispatchMouse(int32_t action, float x, float y, int32_t buttons) {
    PlatformEvent pe;
    pe.pos = Point{static_cast<double>(x), static_cast<double>(y)};
    pe.button = buttonFromOhosMask(buttons);

    switch (action) {
        case kMousePress:
            pe.type = EventType::MouseDown;
            lastMouseButton_ = pe.button ? pe.button : 1;
            break;
        case kMouseRelease:
            pe.type = EventType::MouseUp;
            if (pe.button == 0)
                pe.button = lastMouseButton_;
            lastMouseButton_ = 0;
            break;
        case kMouseMove:
            pe.type = EventType::MouseMove;
            pe.button = lastMouseButton_;
            break;
        case kMouseHover:  // 无键移动：本仓没有 MouseEnter/Leave 类型，等价于 button=0 的 Move
            pe.type = EventType::MouseMove;
            pe.button = 0;
            break;
        case kMouseCancel:  // 焦点被系统抢走等中断：补一次 Up，否则拖拽永远收不了尾
            if (lastMouseButton_ == 0)
                return;
            pe.type = EventType::MouseUp;
            pe.button = lastMouseButton_;
            lastMouseButton_ = 0;
            break;
        default:  // 4/5 = ENTER_WINDOW / LEAVE_WINDOW：框架没有对应事件，丢弃
            return;
    }
    pushEvent(pe);
}

void OhosWindow::dispatchWheel(int32_t action, float x, float y, float dx, float dy) {
    if (action == kAxisEnd) {
        // END 只是「这一段滚动结束」，增量恒为 0；真正驱动滚动的是 BEGIN/UPDATE。
        return;
    }
    PlatformEvent pe;
    pe.type = EventType::MouseWheel;
    pe.pos = Point{static_cast<double>(x), static_cast<double>(y)};
    pe.deltaX = dx;
    pe.deltaY = dy;
    pe.wheelInNotches = false;  // 宿主给的已是像素（见 ohos_window.h 口径说明）
    pushEvent(pe);
}

void OhosWindow::dispatchKey(int32_t action, int32_t keyCode, const std::string& text) {
    PlatformEvent pe;
    pe.key = keyFromOhosKeyCode(keyCode);
    pe.text = text;
    switch (action) {
        case kKeyActionDown:
            pe.type = EventType::KeyDown;
            break;
        case kKeyActionUp:
            pe.type = EventType::KeyUp;
            break;
        default:  // KeyType.CANCEL(3)，以及宿主传来其它按键动作编号
            return;
    }
    pushEvent(pe);
}

void OhosWindow::setDensity(float density) {
    // 倍数（densityPixels，2.0/3.0）与 dpi（densityDpi，480）两种给法都收：
    // ArkTS 侧两个值都拿得到，>10 只可能是 dpi。
    density_ = density > 10.0f ? density / 160.0f : density;
    if (density_ <= 0.0f)
        density_ = 1.0f;
}

int OhosWindow::keyFromOhosKeyCode(int32_t keyCode) {
    switch (keyCode) {
        case kOhosKeyEnter:      return static_cast<int>(KeyCode::Return);
        case kOhosKeyEscape:     return static_cast<int>(KeyCode::Escape);
        case kOhosKeyDel:        return static_cast<int>(KeyCode::Backspace);
        case kOhosKeyForwardDel: return static_cast<int>(KeyCode::Delete);
        case kOhosKeyTab:        return static_cast<int>(KeyCode::Tab);
        case kOhosKeyMoveHome:   return static_cast<int>(KeyCode::Home);
        case kOhosKeyMoveEnd:    return static_cast<int>(KeyCode::End);
        case kOhosKeyPageUp:     return static_cast<int>(KeyCode::PageUp);
        case kOhosKeyPageDown:   return static_cast<int>(KeyCode::PageDown);
        case kOhosKeyDpadLeft:   return static_cast<int>(KeyCode::Left);
        case kOhosKeyDpadUp:     return static_cast<int>(KeyCode::Up);
        case kOhosKeyDpadRight:  return static_cast<int>(KeyCode::Right);
        case kOhosKeyDpadDown:   return static_cast<int>(KeyCode::Down);
        default:                 return static_cast<int>(KeyCode::None);
    }
}

int OhosWindow::buttonFromOhosMask(int32_t buttons) {
    // PlatformEvent::button 是单值（1=左 2=中 3=右），OHOS 是位掩码 ⇒ 取最低位。
    if (buttons & kMouseLeft)
        return 1;
    if (buttons & kMouseMiddle)
        return 2;
    if (buttons & kMouseRight)
        return 3;
    return 0;
}

// ── 呈现 ────────────────────────────────────────────────────────────────────

bool OhosWindow::presentPixels(const void* pixels, int rowBytes, int x, int y, int width, int height) {
    (void)x;
    (void)y;  // 恒整帧提交，理由见文件头
    if (!window_ || !pixels || rowBytes <= 0 || width <= 0 || height <= 0)
        return false;
    return flushFrame(pixels, rowBytes, width, height);
}

bool OhosWindow::flushFrame(const void* pixels, int rowBytes, int width, int height) {
    OHNativeWindowBuffer* buffer = nullptr;
    int fenceFd = -1;
    if (OH_NativeWindow_NativeWindowRequestBuffer(window_, &buffer, &fenceFd) != 0 || !buffer) {
        closeFence(fenceFd);
        LOGE("RequestBuffer failed");
        return false;
    }
    // Abort 之前 fence 归我们关；一旦交给 FlushBuffer 就由系统关。
    auto abortFrame = [&] {
        OH_NativeWindow_NativeWindowAbortBuffer(window_, buffer);
        closeFence(fenceFd);
    };

    BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    OH_NativeBuffer* nativeBuffer = nullptr;
    if (!handle || OH_NativeBuffer_FromNativeWindowBuffer(buffer, &nativeBuffer) != 0 || !nativeBuffer) {
        abortFrame();
        LOGE("buffer handle/native buffer 取不到");
        return false;
    }

    if (handle->format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888 &&
        handle->format != NATIVEBUFFER_PIXEL_FMT_RGBX_8888) {
        abortFrame();
        LOGE("buffer 格式 %{public}d 不是 RGBA_8888（Skia N32 的字节序假定不成立）", handle->format);
        return false;
    }

    void* dst = nullptr;
    if (OH_NativeBuffer_Map(nativeBuffer, &dst) != 0 || !dst) {
        abortFrame();
        LOGE("OH_NativeBuffer_Map failed");
        return false;
    }

    const int copyW = std::min(width, handle->width);
    int copyH = std::min(height, handle->height);
    // BufferHandle::stride 的头注释写「bytes」，实现里按「像素」给的版本也存在
    // ⇒ 先按「不足一行宽就当作像素」归一，再用映射长度夹住行数。
    int dstRowBytes = handle->stride > 0 ? handle->stride : copyW * 4;
    if (dstRowBytes < copyW * 4)
        dstRowBytes *= 4;
    if (handle->size > 0) {
        const int fitRows = handle->size / dstRowBytes;
        if (fitRows < copyH)
            copyH = fitRows;
    }

    if (copyW > 0 && copyH > 0) {
        const auto* src = static_cast<const uint8_t*>(pixels);
        auto* out = static_cast<uint8_t*>(dst);
        for (int row = 0; row < copyH; ++row)
            std::memcpy(out + static_cast<size_t>(row) * dstRowBytes,
                        src + static_cast<size_t>(row) * rowBytes,
                        static_cast<size_t>(copyW) * 4);
    }

    OH_NativeBuffer_Unmap(nativeBuffer);

    Region region{};  // rects == nullptr ⇒ 整帧脏
    const int32_t rc = OH_NativeWindow_NativeWindowFlushBuffer(window_, buffer, fenceFd, region);
    if (rc != 0) {
        LOGE("FlushBuffer rc=%{public}d", rc);
        return false;
    }
    return true;
}

}  // namespace cui_core

#endif  // __OHOS__
