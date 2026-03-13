/// @file android_window.cpp
/// @brief Android NDK implementation of PlatformWindow.
///
/// Android uses a different lifecycle model:
///   - The Activity (Java/Kotlin) creates the surface
///   - ANativeWindow provides the rendering surface
///   - AInputQueue delivers input events
///   - ALooper manages the event loop

#include "cui/platform/android_window.h"

#ifdef __ANDROID__

#include "cui/platform/services/method_channel.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <jni.h>
#include <cstdio>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "CUI", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CUI", __VA_ARGS__)

namespace cui {

// ── Key code mapping ─────────────────────────────────────────────────────────

namespace {

// 键码 / meta 状态常量来自 NDK 的 <android/keycodes.h>（AKEYCODE_*、AMETA_*），
// 此处不再手写一份，避免两套数字漂移。

int keyFromAndroidKeyCode(int32_t keyCode) {
    switch (keyCode) {
        case AKEYCODE_ENTER:      return static_cast<int>(KeyCode::Return);
        case AKEYCODE_ESCAPE:     return static_cast<int>(KeyCode::Escape);
        case AKEYCODE_BACK:       return static_cast<int>(KeyCode::Escape);
        case AKEYCODE_DEL:        return static_cast<int>(KeyCode::Backspace);
        case AKEYCODE_FORWARD_DEL: return static_cast<int>(KeyCode::Delete);
        case AKEYCODE_TAB:        return static_cast<int>(KeyCode::Tab);
        case AKEYCODE_MOVE_HOME:  return static_cast<int>(KeyCode::Home);
        case AKEYCODE_MOVE_END:   return static_cast<int>(KeyCode::End);
        case AKEYCODE_PAGE_UP:    return static_cast<int>(KeyCode::PageUp);
        case AKEYCODE_PAGE_DOWN:  return static_cast<int>(KeyCode::PageDown);
        case AKEYCODE_DPAD_LEFT:  return static_cast<int>(KeyCode::Left);
        case AKEYCODE_DPAD_UP:    return static_cast<int>(KeyCode::Up);
        case AKEYCODE_DPAD_RIGHT: return static_cast<int>(KeyCode::Right);
        case AKEYCODE_DPAD_DOWN:  return static_cast<int>(KeyCode::Down);
        default:                  return static_cast<int>(KeyCode::None);
    }
}

int modifiersFromMetaState(int32_t metaState) {
    int mods = 0;
    if (metaState & AMETA_SHIFT_ON) mods |= ModShift;
    if (metaState & AMETA_CTRL_ON)  mods |= ModCtrl;
    if (metaState & AMETA_ALT_ON)   mods |= ModAlt;
    if (metaState & AMETA_META_ON)  mods |= ModMeta;
    return mods;
}

// 硬件键盘上的可见 ASCII 字符。NDK 没有 getUnicodeChar（那是 Java
// KeyEvent.getUnicodeChar 的能力），软键盘文字走 IME / InputConnection，
// 这里只负责让外接键盘的字母、数字、空格与符号能直接进输入框。
char asciiForKeyCode(int32_t keyCode, bool shift) {
    if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z) {
        const char lower = static_cast<char>('a' + (keyCode - AKEYCODE_A));
        return shift ? static_cast<char>(lower - 'a' + 'A') : lower;
    }
    if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9) {
        static const char kUnshifted[] = "0123456789";
        static const char kShifted[] = ")!@#$%^&*(";
        const int idx = keyCode - AKEYCODE_0;
        return shift ? kShifted[idx] : kUnshifted[idx];
    }
    switch (keyCode) {
        case AKEYCODE_SPACE:          return ' ';
        case AKEYCODE_COMMA:          return shift ? '<' : ',';
        case AKEYCODE_PERIOD:         return shift ? '>' : '.';
        case AKEYCODE_SLASH:          return shift ? '?' : '/';
        case AKEYCODE_SEMICOLON:      return shift ? ':' : ';';
        case AKEYCODE_APOSTROPHE:     return shift ? '"' : '\'';
        case AKEYCODE_MINUS:          return shift ? '_' : '-';
        case AKEYCODE_EQUALS:         return shift ? '+' : '=';
        case AKEYCODE_LEFT_BRACKET:   return shift ? '{' : '[';
        case AKEYCODE_RIGHT_BRACKET:  return shift ? '}' : ']';
        case AKEYCODE_BACKSLASH:      return shift ? '|' : '\\';
        case AKEYCODE_GRAVE:          return shift ? '~' : '`';
        default:                      return 0;
    }
}

}  // namespace

AndroidWindow::AndroidWindow() {
}

AndroidWindow::~AndroidWindow() {
    destroy();
}

void AndroidWindow::create(const std::string& title, int width, int height) {
    width_ = width;
    height_ = height;
    title_ = title;

    LOGI("AndroidWindow::create(%s, %d, %d)", title.c_str(), width, height);

    // On Android, the window/surface is created by the Activity
    // We just initialize state here
    looper_ = ALooper_forThread();
    if (!looper_) {
        LOGE("No ALooper available - must be called from main thread");
    }
}

void AndroidWindow::destroy() {
    LOGI("AndroidWindow::destroy()");
    if (nativeWindow_) {
        ANativeWindow_release(nativeWindow_);
        nativeWindow_ = nullptr;
    }
}

void AndroidWindow::show() {
    // On Android, the window is shown by the Activity
    LOGI("AndroidWindow::show()");
}

void AndroidWindow::invalidate() {
    // Request a redraw - on Android this would trigger onDraw
    LOGI("AndroidWindow::invalidate()");
}

void AndroidWindow::setTitle(const std::string& title) {
    title_ = title;
    // On Android, title is set by the Activity
}

void AndroidWindow::setMinimumSize(int width, int height) {
    // Android manages window sizing via Activity layout
}

double AndroidWindow::dpiScale() const {
    return density_ / 160.0;  // 160 DPI = 1.0 scale
}

void AndroidWindow::wakeUp() {
    if (looper_) {
        ALooper_wake(looper_);
    }
}

bool AndroidWindow::pollEvent(PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (!pendingEvents_.empty()) {
        event = pendingEvents_.front();
        pendingEvents_.pop();
        return true;
    }
    return false;
}

bool AndroidWindow::waitForEvent(PlatformEvent& event, int timeoutMs) {
    if (timeoutMs == 0)
        return pollEvent(event);

    if (looper_) {
        // Process events with timeout
        int events;
        void* data;
        int result = ALooper_pollOnce(timeoutMs, nullptr, &events, &data);
        if (result == ALOOPER_POLL_CALLBACK) {
            // Processed a callback
        }
    }

    return pollEvent(event);
}

PlatformWindow::NativeHandles AndroidWindow::nativeHandles() {
    NativeHandles h;
    // ANativeWindow 是 Android 的呈现目标（AppWindow::blitToNativeWindow
    // 用 ANativeWindow_lock/unlockAndPost 上屏）。
    h.android_window = nativeWindow_;
    return h;
}

// ── Clipboard ────────────────────────────────────────────────────────────────

bool AndroidWindow::clipboardSetText(const std::string& utf8) {
    auto reply = services::invokeMethod("cui_core/clipboard", "set", utf8);
    return reply && (*reply == "ok" || *reply == "1" || *reply == "true");
}

std::string AndroidWindow::clipboardText() const {
    auto reply = services::invokeMethod("cui_core/clipboard", "get");
    return reply.value_or(std::string{});
}

bool AndroidWindow::hasClipboardText() const {
    auto reply = services::invokeMethod("cui_core/clipboard", "has");
    return reply && (*reply == "1" || *reply == "true");
}

// ── Android lifecycle callbacks ─────────────────────────────────────────────

void AndroidWindow::onSurfaceCreated(ANativeWindow* window) {
    LOGI("onSurfaceCreated");
    if (nativeWindow_) {
        ANativeWindow_release(nativeWindow_);
    }
    nativeWindow_ = window;
    ANativeWindow_acquire(nativeWindow_);

    // Query surface size
    width_ = ANativeWindow_getWidth(nativeWindow_);
    height_ = ANativeWindow_getHeight(nativeWindow_);

    // 软件呈现（AppWindow::blitToNativeWindow 按 4 字节/像素逐行 memcpy Skia 的
    // N32 缓冲）要求 buffer 恒为 RGBA_8888。NativeActivity 不给格式时由系统挑，
    // 模拟器 / 省电设备会给 RGB_565，于是 8 字节的行写进 4 字节的行距 ⇒ 首帧
    // 就在 buffer 映射尾部 SIGSEGV。这里显式钉一次（blit 侧还有兜底检查）。
    ANativeWindow_setBuffersGeometry(nativeWindow_, 0, 0, WINDOW_FORMAT_RGBA_8888);
    LOGI("surface %dx%d fmt=%d",
         ANativeWindow_getWidth(nativeWindow_), ANativeWindow_getHeight(nativeWindow_),
         ANativeWindow_getFormat(nativeWindow_));

    PlatformEvent event;
    event.type = EventType::Expose;
    std::lock_guard<std::mutex> lock(eventMutex_);
    pendingEvents_.push(event);
}

void AndroidWindow::onSurfaceChanged(int width, int height) {
    LOGI("onSurfaceChanged(%d, %d)", width, height);
    width_ = width;
    height_ = height;

    PlatformEvent event;
    event.type = EventType::Resize;
    event.width = width;
    event.height = height;
    std::lock_guard<std::mutex> lock(eventMutex_);
    pendingEvents_.push(event);
}

void AndroidWindow::onSurfaceDestroyed() {
    LOGI("onSurfaceDestroyed");
    if (nativeWindow_) {
        ANativeWindow_release(nativeWindow_);
        nativeWindow_ = nullptr;
    }

    // 不推 Close：Activity 切后台 / 旋转都会销毁 surface，但应用窗口仍然
    // 活着（数据不丢）。真正退出由 NativeActivity 的 APP_CMD_DESTROY 走
    // Application::quit()；下一个 APP_CMD_INIT_WINDOW 会用新 surface 重绘。
}

void AndroidWindow::onFocusChanged(bool hasFocus) {
    LOGI("onFocusChanged(%d)", hasFocus);
    hasFocus_ = hasFocus;

    PlatformEvent event;
    event.type = hasFocus ? EventType::FocusIn : EventType::FocusOut;
    std::lock_guard<std::mutex> lock(eventMutex_);
    pendingEvents_.push(event);
}

bool AndroidWindow::onInputEvent(AInputEvent* event) {
    int32_t type = AInputEvent_getType(event);

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(event);
        int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
        int32_t pointerCount = AMotionEvent_getPointerCount(event);

        // Process all pointers (multi-touch support)
        for (int32_t i = 0; i < pointerCount; ++i) {
            float x = AMotionEvent_getX(event, i);
            float y = AMotionEvent_getY(event, i);
            int32_t pointerId = AMotionEvent_getPointerId(event, i);

            PlatformEvent pe;
            pe.pos = {x, y};
            pe.touchId = pointerId;

            // For multi-touch, we need to check which pointer changed
            int32_t pointerIndex = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                                   >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

            switch (actionMasked) {
                case AMOTION_EVENT_ACTION_DOWN:
                    if (i == 0) {  // First pointer
                        pe.type = EventType::TouchBegin;
                        pe.button = 1;  // Simulate left click for touch
                        // Also generate MouseDown for compatibility
                        PlatformEvent mouseEvent;
                        mouseEvent.type = EventType::MouseDown;
                        mouseEvent.pos = {x, y};
                        mouseEvent.button = 1;
                        mouseEvent.modifiers = 0;
                        std::lock_guard<std::mutex> lock(eventMutex_);
                        pendingEvents_.push(pe);
                        pendingEvents_.push(mouseEvent);
                        return true;
                    }
                    break;

                case AMOTION_EVENT_ACTION_POINTER_DOWN:
                    if (i == pointerIndex) {
                        pe.type = EventType::TouchBegin;
                        std::lock_guard<std::mutex> lock(eventMutex_);
                        pendingEvents_.push(pe);
                        return true;
                    }
                    break;

                case AMOTION_EVENT_ACTION_MOVE: {
                    pe.type = EventType::TouchUpdate;
                    std::lock_guard<std::mutex> lock(eventMutex_);
                    pendingEvents_.push(pe);
                    // Also generate MouseMove for compatibility
                    PlatformEvent mouseEvent;
                    mouseEvent.type = EventType::MouseMove;
                    mouseEvent.pos = {x, y};
                    mouseEvent.button = 1;
                    mouseEvent.modifiers = 0;
                    pendingEvents_.push(mouseEvent);
                    return true;
                }

                case AMOTION_EVENT_ACTION_UP:
                case AMOTION_EVENT_ACTION_CANCEL: {
                    if (i == 0) {  // First pointer
                        pe.type = EventType::TouchEnd;
                        pe.button = 1;
                        // Also generate MouseUp for compatibility
                        PlatformEvent mouseEvent;
                        mouseEvent.type = EventType::MouseUp;
                        mouseEvent.pos = {x, y};
                        mouseEvent.button = 1;
                        mouseEvent.modifiers = 0;
                        std::lock_guard<std::mutex> lock(eventMutex_);
                        pendingEvents_.push(pe);
                        pendingEvents_.push(mouseEvent);
                        return true;
                    }
                    break;
                }

                case AMOTION_EVENT_ACTION_POINTER_UP: {
                    if (i == pointerIndex) {
                        pe.type = EventType::TouchEnd;
                        std::lock_guard<std::mutex> lock(eventMutex_);
                        pendingEvents_.push(pe);
                        return true;
                    }
                    break;
                }

                default:
                    break;
            }
        }
        return true;

    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t action = AKeyEvent_getAction(event);
        int32_t keyCode = AKeyEvent_getKeyCode(event);
        int32_t metaState = AKeyEvent_getMetaState(event);

        PlatformEvent pe;
        pe.key = keyFromAndroidKeyCode(keyCode);
        pe.modifiers = modifiersFromMetaState(metaState);

        // Get character for key events
        if (action == AKEY_EVENT_ACTION_DOWN && (metaState & AMETA_CTRL_ON) == 0) {
            if (const char ch = asciiForKeyCode(keyCode, (metaState & AMETA_SHIFT_ON) != 0); ch != 0) {
                pe.text.assign(1, ch);
            }
        }

        switch (action) {
            case AKEY_EVENT_ACTION_DOWN:
                pe.type = EventType::KeyDown;
                break;
            case AKEY_EVENT_ACTION_UP:
                pe.type = EventType::KeyUp;
                break;
            default:
                return false;
        }

        std::lock_guard<std::mutex> lock(eventMutex_);
        pendingEvents_.push(pe);
        return true;
    }

    return false;
}

void AndroidWindow::onAppCmd(int cmd) {
    // Handle Android app commands
    LOGI("onAppCmd(%d)", cmd);
}

// ── Key code mapping ─────────────────────────────────────────────────────────

int AndroidWindow::keyFromAndroidKeyCode(int32_t keyCode) const {
    return keyFromAndroidKeyCode(keyCode);
}

int AndroidWindow::modifiersFromMetaState(int32_t metaState) const {
    return modifiersFromMetaState(metaState);
}

}  // namespace cui_core

#endif  // __ANDROID__
