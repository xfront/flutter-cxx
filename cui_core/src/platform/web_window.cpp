/// @file web_window.cpp
/// @brief Web/WASM implementation of PlatformWindow using Emscripten.
///
/// HTML5 events are translated into PlatformEvents on the browser's main
/// thread; the Application loop is driven from requestAnimationFrame (see
/// Application::run()) and presents by copying the raster surface into the
/// canvas 2D context.

#include "cui/platform/web_window.h"

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <emscripten.h>
#include <emscripten/html5.h>

#include "cui/platform/services/app_lifecycle.h"
#include "cui/platform/services/method_channel.h"

namespace cui {

namespace {

WebWindow* sCurrentWindow = nullptr;
bool g_imeComposing = false;

int keyFromCode(const char* name) {
    if (!name) return static_cast<int>(KeyCode::None);
    if (strcmp(name, "Enter") == 0 || strcmp(name, "NumpadEnter") == 0)
        return static_cast<int>(KeyCode::Return);
    if (strcmp(name, "Escape") == 0)    return static_cast<int>(KeyCode::Escape);
    if (strcmp(name, "Backspace") == 0) return static_cast<int>(KeyCode::Backspace);
    if (strcmp(name, "Delete") == 0)    return static_cast<int>(KeyCode::Delete);
    if (strcmp(name, "Tab") == 0)       return static_cast<int>(KeyCode::Tab);
    if (strcmp(name, "Home") == 0)      return static_cast<int>(KeyCode::Home);
    if (strcmp(name, "End") == 0)       return static_cast<int>(KeyCode::End);
    if (strcmp(name, "PageUp") == 0)    return static_cast<int>(KeyCode::PageUp);
    if (strcmp(name, "PageDown") == 0)  return static_cast<int>(KeyCode::PageDown);
    if (strcmp(name, "ArrowLeft") == 0) return static_cast<int>(KeyCode::Left);
    if (strcmp(name, "ArrowUp") == 0)   return static_cast<int>(KeyCode::Up);
    if (strcmp(name, "ArrowRight") == 0) return static_cast<int>(KeyCode::Right);
    if (strcmp(name, "ArrowDown") == 0) return static_cast<int>(KeyCode::Down);
    return static_cast<int>(KeyCode::None);
}

int modifiersFromMouse(const EmscriptenMouseEvent* e) {
    int mods = 0;
    if (e->shiftKey) mods |= ModShift;
    if (e->ctrlKey)  mods |= ModCtrl;
    if (e->altKey)   mods |= ModAlt;
    if (e->metaKey)  mods |= ModMeta;
    return mods;
}

/// F1–F12 (the code field is "F<n>"); these belong to the browser.
bool isFunctionKey(const char* code) {
    if (!code || code[0] != 'F' || !code[1])
        return false;
    int digits = 0;
    for (const char* p = code + 1; *p; ++p, ++digits) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return digits <= 2;
}

int modifiersFromKeyboard(const EmscriptenKeyboardEvent* e) {
    int mods = 0;
    if (e->shiftKey) mods |= ModShift;
    if (e->ctrlKey)  mods |= ModCtrl;
    if (e->altKey)   mods |= ModAlt;
    if (e->metaKey)  mods |= ModMeta;
    return mods;
}

/// The DOM reports pointer positions in CSS pixels, and the canvas backing
/// store is sized in CSS pixels too (see syncCanvasSize), so no scaling is
/// involved: one canvas pixel is one layout pixel on every backend.
Point devicePoint(const WebWindow*, double x, double y) {
    return Point{x, y};
}

double wheelPixelScale(const WebWindow* window, unsigned long deltaMode, double delta) {
    // DOM_DELTA_LINE = 1 (~16px per line), DOM_DELTA_PAGE = 2.
    if (deltaMode == 1) return 16.0 * delta;
    if (deltaMode == 2) return window->height() * delta;
    return delta;
}

// ── Emscripten event callbacks ──────────────────────────────────────────────

EM_BOOL mouse_callback(int eventType, const EmscriptenMouseEvent* e, void* userData) {
    auto* window = static_cast<WebWindow*>(userData);
    PlatformEvent event;
    event.pos = devicePoint(window, e->targetX, e->targetY);
    event.modifiers = modifiersFromMouse(e);

    switch (eventType) {
        case EMSCRIPTEN_EVENT_MOUSEDOWN:
            event.type = EventType::MouseDown;
            event.button = e->button + 1;  // DOM 0=left → 1
            break;
        case EMSCRIPTEN_EVENT_MOUSEUP:
            event.type = EventType::MouseUp;
            event.button = e->button + 1;
            break;
        case EMSCRIPTEN_EVENT_MOUSEMOVE:
            event.type = EventType::MouseMove;
            break;
        default:
            return EM_FALSE;
    }

    window->pushEvent(event);
    return EM_TRUE;
}

EM_BOOL mouse_enter_callback(int, const EmscriptenMouseEvent*, void* userData) {
    static_cast<WebWindow*>(userData)->onMouseEnter();
    return EM_TRUE;
}

EM_BOOL mouse_leave_callback(int, const EmscriptenMouseEvent*, void* userData) {
    static_cast<WebWindow*>(userData)->onMouseLeave();
    return EM_TRUE;
}

EM_BOOL wheel_callback(int, const EmscriptenWheelEvent* e, void* userData) {
    auto* window = static_cast<WebWindow*>(userData);
    PlatformEvent event;
    event.type = EventType::MouseWheel;
    event.pos = devicePoint(window, e->mouse.targetX, e->mouse.targetY);
    // deltaMode 可能是像素(0)/行(1)/页(2)：统一换算成像素，避免「滚一格跳半页」。
    event.deltaX = wheelPixelScale(window, e->deltaMode, e->deltaX);
    event.deltaY = wheelPixelScale(window, e->deltaMode, e->deltaY);
    event.wheelInNotches = false;
    event.modifiers = modifiersFromMouse(&e->mouse);
    window->pushEvent(event);
    return EM_TRUE;  // preventDefault: the page must not scroll
}

EM_BOOL key_callback(int eventType, const EmscriptenKeyboardEvent* e, void* userData) {
    if (eventType != EMSCRIPTEN_EVENT_KEYDOWN && eventType != EMSCRIPTEN_EVENT_KEYUP)
        return EM_FALSE;
    auto* window = static_cast<WebWindow*>(userData);

    PlatformEvent event;
    event.type = eventType == EMSCRIPTEN_EVENT_KEYDOWN ? EventType::KeyDown
                                                       : EventType::KeyUp;
    event.key = keyFromCode(e->code);
    if (event.key == static_cast<int>(KeyCode::None))
        event.key = keyFromCode(e->key);
    event.modifiers = modifiersFromKeyboard(e);

    // Printable characters arrive through `key` (one code point in UTF-8).
    // Skip them while an IME composition is open (compositionend commits the
    // final text) and while a shortcut modifier is held (Ctrl+C / Cmd+V).
    if (event.type == EventType::KeyDown && !g_imeComposing && e->key[0] != '\0' &&
        !e->ctrlKey && !e->metaKey) {
        const size_t len = std::strlen(e->key);
        const bool nonAscii = (static_cast<unsigned char>(e->key[0]) & 0x80) != 0;
        if (len == 1 || (len > 1 && nonAscii))
            event.text = e->key;
    }

    window->pushEvent(event);
    // Chords (Ctrl/Cmd/Alt) and the function-key row stay with the browser —
    // reload, devtools and new-tab must keep working, and the app still sees
    // the KeyDown to map Ctrl+C/Ctrl+V itself. Everything else is consumed so
    // that Tab, Space, arrows and Backspace never move focus or scroll the page
    // out from under the canvas.
    const bool browserOwned = e->ctrlKey || e->metaKey || e->altKey || isFunctionKey(e->code);
    return browserOwned ? EM_FALSE : EM_TRUE;
}

EM_BOOL touch_callback(int eventType, const EmscriptenTouchEvent* e, void* userData) {
    auto* window = static_cast<WebWindow*>(userData);

    for (int i = 0; i < e->numTouches; ++i) {
        const EmscriptenTouchPoint& touch = e->touches[i];
        if (!touch.isChanged)
            continue;

        const Point pos = devicePoint(window, touch.targetX, touch.targetY);
        PlatformEvent event;
        event.pos = pos;
        event.touchId = static_cast<int>(touch.identifier);
        event.button = 1;

        // 移动端同时合成鼠标事件，InkWell/滚动/悬停判定无需触摸分支。
        PlatformEvent mouse;
        mouse.pos = pos;
        mouse.button = 1;
        mouse.modifiers = 0;
        mouse.type = EventType::None;

        switch (eventType) {
            case EMSCRIPTEN_EVENT_TOUCHSTART:
                event.type = EventType::TouchBegin;
                mouse.type = EventType::MouseDown;
                break;
            case EMSCRIPTEN_EVENT_TOUCHMOVE:
                event.type = EventType::TouchUpdate;
                mouse.type = EventType::MouseMove;
                break;
            case EMSCRIPTEN_EVENT_TOUCHEND:
            case EMSCRIPTEN_EVENT_TOUCHCANCEL:
                event.type = EventType::TouchEnd;
                mouse.type = EventType::MouseUp;
                break;
            default:
                continue;
        }

        window->pushEvent(mouse);
        window->pushEvent(event);
    }
    return EM_TRUE;
}

EM_BOOL resize_callback(int, const EmscriptenUiEvent*, void* userData) {
    static_cast<WebWindow*>(userData)->onWindowResize();
    return EM_TRUE;
}

EM_BOOL focus_callback(int eventType, const EmscriptenFocusEvent*, void* userData) {
    auto* window = static_cast<WebWindow*>(userData);
    if (eventType == EMSCRIPTEN_EVENT_FOCUS)
        window->onFocus();
    else
        window->onBlur();
    return EM_TRUE;
}

EM_BOOL pointerlock_callback(int, const EmscriptenPointerlockChangeEvent*, void*) {
    return EM_TRUE;
}

const char* beforeunload_callback(int, const void*, void*) {
    // The event loop will not run again — apply the state synchronously so
    // observers can persist work while the page is still alive.
    services::setAppLifecycleState(services::AppLifecycleState::Detached);
    return nullptr;
}

}  // namespace

// ── WebWindow ───────────────────────────────────────────────────────────────

WebWindow::WebWindow() : canvasId_("#canvas") {}

WebWindow::~WebWindow() {
    destroy();
}

void WebWindow::create(const std::string& title, int width, int height) {
    canvasId_ = "#canvas";
    title_ = title;
    alive_ = true;
    sCurrentWindow = this;

    // Canvas must be focusable for IME composition, and the composition /
    // paste / visibility hooks are installed once per element.
    EM_ASM({
        const canvas = Module['canvas'] || document.querySelector('#canvas');
        if (!canvas) return;
        if (!canvas.hasAttribute('tabindex')) canvas.setAttribute('tabindex', '0');
        if (canvas.__cuiHooks) return;
        canvas.__cuiHooks = true;
        canvas.addEventListener('mousedown', function () { canvas.focus(); });
        canvas.addEventListener('compositionstart', function () {
            if (Module.ccall) Module.ccall('cui_web_ime_composing', null, ['number'], [1]);
        });
        canvas.addEventListener('compositionupdate', function (e) {
            if (Module.ccall) Module.ccall('cui_web_ime_preedit', null, ['string'], [e.data || '']);
        });
        canvas.addEventListener('compositionend', function (e) {
            if (!Module.ccall) return;
            Module.ccall('cui_web_ime_composing', null, ['number'], [0]);
            Module.ccall('cui_web_ime_preedit', null, ['string'], ['']);
            if (e.data) Module.ccall('cui_web_ime_commit', null, ['string'], [e.data]);
        });
        canvas.addEventListener('paste', function (e) {
            e.preventDefault();
            const text = (e.clipboardData || window.clipboardData).getData('text');
            if (text && Module.ccall) Module.ccall('cui_web_ime_commit', null, ['string'], [text]);
        });
        document.addEventListener('visibilitychange', function () {
            if (Module.ccall) Module.ccall('cui_web_visibility', null, ['number'], [document.hidden ? 1 : 0]);
        });
    });

    // The shell stretches the canvas to the viewport; size the backing store to
    // the element's CSS box (1 canvas px = 1 CSS px = 1 layout px) and fall
    // back to the size requested by createWindow() before layout has run.
    double cssW = width, cssH = height;
    if (emscripten_get_element_css_size(canvasId_.c_str(), &cssW, &cssH) != EMSCRIPTEN_RESULT_SUCCESS ||
        cssW <= 0 || cssH <= 0) {
        cssW = width;
        cssH = height;
    }
    devicePixelRatio_ = emscripten_get_device_pixel_ratio();
    if (!(devicePixelRatio_ > 0))
        devicePixelRatio_ = 1.0;
    width_ = std::max(1, static_cast<int>(std::lround(cssW)));
    height_ = std::max(1, static_cast<int>(std::lround(cssH)));
    emscripten_set_canvas_element_size(canvasId_.c_str(), width_, height_);

    emscripten_set_mousedown_callback(canvasId_.c_str(), this, EM_TRUE, mouse_callback);
    emscripten_set_mouseup_callback(canvasId_.c_str(), this, EM_TRUE, mouse_callback);
    emscripten_set_mousemove_callback(canvasId_.c_str(), this, EM_TRUE, mouse_callback);
    emscripten_set_mouseenter_callback(canvasId_.c_str(), this, EM_TRUE, mouse_enter_callback);
    emscripten_set_mouseleave_callback(canvasId_.c_str(), this, EM_TRUE, mouse_leave_callback);
    emscripten_set_wheel_callback(canvasId_.c_str(), this, EM_TRUE, wheel_callback);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, key_callback);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, key_callback);
    emscripten_set_touchstart_callback(canvasId_.c_str(), this, EM_TRUE, touch_callback);
    emscripten_set_touchmove_callback(canvasId_.c_str(), this, EM_TRUE, touch_callback);
    emscripten_set_touchend_callback(canvasId_.c_str(), this, EM_TRUE, touch_callback);
    emscripten_set_touchcancel_callback(canvasId_.c_str(), this, EM_TRUE, touch_callback);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, resize_callback);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, focus_callback);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE, focus_callback);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_TRUE,
                                              pointerlock_callback);
    emscripten_set_beforeunload_callback(this, beforeunload_callback);

    emscripten_set_window_title(title.c_str());

    EM_ASM({
        document.addEventListener('contextmenu', function (e) {
            e.preventDefault();
            return false;
        });
    });

    // A freshly loaded page is active.
    reportLifecycle("resumed");
}

void WebWindow::destroy() {
    if (!alive_) {
        sCurrentWindow = nullptr;
        return;
    }
    alive_ = false;

    emscripten_set_mousedown_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_mouseup_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_mousemove_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_mouseenter_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_mouseleave_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_wheel_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, nullptr);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, nullptr);
    emscripten_set_touchstart_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_touchmove_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_touchend_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_touchcancel_callback(canvasId_.c_str(), nullptr, EM_TRUE, nullptr);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, nullptr);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, nullptr);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, nullptr);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE,
                                              nullptr);
    emscripten_set_beforeunload_callback(nullptr, nullptr);

    if (pointerLocked_)
        emscripten_exit_pointerlock();

    services::setAppLifecycleState(services::AppLifecycleState::Detached);
    lifecycleState_ = "detached";
    sCurrentWindow = nullptr;
}

void WebWindow::show() {}

void WebWindow::invalidate() {}

void WebWindow::setTitle(const std::string& title) {
    title_ = title;
    emscripten_set_window_title(title.c_str());
}

void WebWindow::setMinimumSize(int, int) {}

double WebWindow::dpiScale() const {
    return devicePixelRatio_;
}

void WebWindow::wakeUp() {}

void WebWindow::grabPointer() {
    if (pointerLocked_)
        return;
    pointerLocked_ = emscripten_request_pointerlock(canvasId_.c_str(), EM_TRUE) ==
                     EMSCRIPTEN_RESULT_SUCCESS;
}

void WebWindow::ungrabPointer() {
    if (!pointerLocked_)
        return;
    emscripten_exit_pointerlock();
    pointerLocked_ = false;
}

bool WebWindow::clipboardSetText(const std::string& utf8) {
    EM_ASM({
        const text = UTF8ToString($0);
        function legacyCopy() {
            const ta = document.createElement('textarea');
            ta.value = text;
            ta.style.position = 'fixed';
            ta.style.opacity = '0';
            document.body.appendChild(ta);
            ta.select();
            try { document.execCommand('copy'); } catch (e) {}
            document.body.removeChild(ta);
        }
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).catch(legacyCopy);
        } else {
            legacyCopy();
        }
    }, utf8.c_str());
    return true;
}

std::string WebWindow::clipboardText() const {
    // Browsers only expose the clipboard asynchronously (navigator.clipboard
    // .readText); paste is delivered through the canvas `paste` listener.
    return {};
}

bool WebWindow::hasClipboardText() const {
    return false;
}

PlatformWindow::NativeHandles WebWindow::nativeHandles() {
    // No native handles: WebWindow presents through Application::blitToNativeWindow
    // writing into the canvas 2D context.
    return NativeHandles{};
}

bool WebWindow::pollEvent(PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (pendingEvents_.empty())
        return false;
    event = pendingEvents_.front();
    pendingEvents_.pop();
    return true;
}

bool WebWindow::waitForEvent(PlatformEvent& event, int timeoutMs) {
    // requestAnimationFrame drives the loop; never block the browser thread.
    (void)timeoutMs;
    return pollEvent(event);
}

void WebWindow::pushEvent(const PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    pendingEvents_.push(event);
}

void WebWindow::onResize(int width, int height) {
    if (width == width_ && height == height_)
        return;
    width_ = width;
    height_ = height;

    PlatformEvent event;
    event.type = EventType::Resize;
    event.width = width;
    event.height = height;
    pushEvent(event);
}

bool WebWindow::syncCanvasSize() {
    double cssW = 0, cssH = 0;
    if (emscripten_get_element_css_size(canvasId_.c_str(), &cssW, &cssH) != EMSCRIPTEN_RESULT_SUCCESS)
        return false;
    if (cssW <= 0 || cssH <= 0)
        return false;

    double dpr = emscripten_get_device_pixel_ratio();
    if (!(dpr > 0))
        dpr = 1.0;
    devicePixelRatio_ = dpr;

    // CSS pixels only: the framework paints 1:1 into a raster surface, so a
    // DPR-scaled backing store would shrink every widget by the scale factor
    // (there is no presentation transform to compensate). The browser upscales
    // the canvas on HiDPI displays instead; dpiScale() still reports the DPR.
    const int w = std::max(1, static_cast<int>(std::lround(cssW)));
    const int h = std::max(1, static_cast<int>(std::lround(cssH)));
    if (w == width_ && h == height_)
        return false;

    width_ = w;
    height_ = h;
    emscripten_set_canvas_element_size(canvasId_.c_str(), w, h);
    return true;
}

void WebWindow::onWindowResize() {
    if (!syncCanvasSize())
        return;
    // syncCanvasSize() already updated width_/height_, so build the event
    // here instead of going through onResize()'s change check.
    PlatformEvent event;
    event.type = EventType::Resize;
    event.width = width_;
    event.height = height_;
    pushEvent(event);
}

void WebWindow::onMouseEnter() {}

void WebWindow::onMouseLeave() {}

void WebWindow::onFocus() {
    reportLifecycle("resumed");
    if (hasFocus_)
        return;
    hasFocus_ = true;

    PlatformEvent event;
    event.type = EventType::FocusIn;
    pushEvent(event);
}

void WebWindow::onBlur() {
    reportLifecycle("inactive");
    if (!hasFocus_)
        return;
    hasFocus_ = false;

    PlatformEvent event;
    event.type = EventType::FocusOut;
    pushEvent(event);
}

void WebWindow::onVisibilityChanged(bool hidden) {
    if (hidden) {
        onBlur();
        reportLifecycle("hidden");
        return;
    }
    reportLifecycle("resumed");
    onFocus();
}

void WebWindow::onImeCommit(const char* utf8) {
    if (!utf8 || !*utf8)
        return;
    PlatformEvent event;
    event.type = EventType::KeyDown;
    event.text = utf8;
    pushEvent(event);
}

void WebWindow::onImePreedit(const char* utf8) {
    PlatformEvent event;
    event.type = EventType::Preedit;
    event.text = utf8 ? utf8 : "";
    pushEvent(event);
}

void WebWindow::reportLifecycle(const char* state) {
    if (!state || lifecycleState_ == state)
        return;
    lifecycleState_ = state;
    services::pushInboundCall("cui_core/lifecycle", "AppLifecycleState.changed",
                              std::string("AppLifecycleState.") + state);
}

// ── JS-callable hooks (EMSCRIPTEN_KEEPALIVE exports them) ──────────────────

extern "C" {

EMSCRIPTEN_KEEPALIVE void cui_web_ime_composing(int composing) {
    g_imeComposing = composing != 0;
}

EMSCRIPTEN_KEEPALIVE void cui_web_ime_commit(const char* utf8) {
    if (sCurrentWindow && utf8)
        sCurrentWindow->onImeCommit(utf8);
}

EMSCRIPTEN_KEEPALIVE void cui_web_ime_preedit(const char* utf8) {
    if (sCurrentWindow)
        sCurrentWindow->onImePreedit(utf8 ? utf8 : "");
}

EMSCRIPTEN_KEEPALIVE void cui_web_visibility(int hidden) {
    if (sCurrentWindow)
        sCurrentWindow->onVisibilityChanged(hidden != 0);
}

}  // extern "C"

}  // namespace cui_core

#endif  // __EMSCRIPTEN__
