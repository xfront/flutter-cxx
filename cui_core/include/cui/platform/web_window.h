#pragma once
/// @file web_window.h
/// @brief Web/WASM implementation of PlatformWindow using Emscripten.
///
/// Rendering goes to an HTML5 `<canvas id="canvas">` (2D context).  Input is
/// translated from the browser's HTML5 events by src/platform/web_window.cpp.
/// The backing store is sized to the element's CSS box, so one canvas pixel is
/// one CSS pixel is one layout pixel; the browser upscales the canvas on HiDPI
/// displays (dpiScale() still reports devicePixelRatio).
///
/// Requires the Emscripten SDK; the whole class is compiled out elsewhere.

#include <mutex>
#include <queue>
#include <string>

#include "platform_window.h"

#ifdef __EMSCRIPTEN__

namespace cui {

class WebWindow : public PlatformWindow {
public:
    WebWindow();
    ~WebWindow() override;

    void create(const std::string& title, int width, int height) override;
    void destroy() override;
    bool pollEvent(PlatformEvent& event) override;
    bool waitForEvent(PlatformEvent& event, int timeoutMs) override;
    void invalidate() override;
    void show() override;
    int width() const override { return width_; }
    int height() const override { return height_; }
    void wakeUp() override;
    void setTitle(const std::string& title) override;
    void setMinimumSize(int width, int height) override;
    double dpiScale() const override;
    NativeHandles nativeHandles() override;

    // Pointer grab uses the Pointer Lock API.
    void grabPointer() override;
    void ungrabPointer() override;

    // Clipboard: writes are fire-and-forget (Clipboard API is async); reads
    // are unavailable because browsers forbid synchronous clipboard access.
    bool clipboardSetText(const std::string& utf8) override;
    std::string clipboardText() const override;
    bool hasClipboardText() const override;

    /// Canvas element selector (default "#canvas").
    const std::string& canvasId() const { return canvasId_; }

    // ── Callbacks from the Emscripten event handlers ──────────────────────
    void pushEvent(const PlatformEvent& event);
    void onResize(int width, int height);
    void onMouseEnter();
    void onMouseLeave();
    void onFocus();
    void onBlur();
    /// Text committed by the IME (compositionend / paste).
    void onImeCommit(const char* utf8);
    /// Updated composition string; empty clears the preedit.
    void onImePreedit(const char* utf8);
    /// Window resized: recompute the backing store from the CSS size × DPR.
    void onWindowResize();
    /// Document visibility changed (hidden=true when the tab is backgrounded).
    void onVisibilityChanged(bool hidden);

private:
    /// Grow/shrink the canvas backing store to the element's CSS size × DPR.
    /// Returns true when the size actually changed.
    bool syncCanvasSize();
    /// Forward an `AppLifecycleState.xxx` transition to the lifecycle channel.
    void reportLifecycle(const char* state);

    std::string canvasId_;
    std::string title_;
    int width_ = 0;
    int height_ = 0;
    double devicePixelRatio_ = 1.0;

    bool pointerLocked_ = false;
    bool hasFocus_ = false;
    bool alive_ = false;
    std::string lifecycleState_;

    std::queue<PlatformEvent> pendingEvents_;
    std::mutex eventMutex_;
};

}  // namespace cui_core

#endif  // __EMSCRIPTEN__
