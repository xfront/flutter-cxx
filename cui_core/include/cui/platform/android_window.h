#pragma once
/// @file android_window.h
/// @brief Android NDK implementation of PlatformWindow.
///
/// Uses ANativeWindow for rendering surface and Android input events.
/// Requires Android NDK API level 21+.

#include "platform_window.h"

#ifdef __ANDROID__

#include <android/configuration.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <mutex>
#include <queue>
#include <string>

namespace cui {

class AndroidWindow : public PlatformWindow {
public:
    AndroidWindow();
    ~AndroidWindow() override;

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

    // Pointer grab (no-op on Android, touch doesn't need grab)
    void grabPointer() override {}
    void ungrabPointer() override {}

    // Clipboard
    bool clipboardSetText(const std::string& utf8) override;
    std::string clipboardText() const override;
    bool hasClipboardText() const override;

    // Android-specific callbacks (called from Java/Kotlin via JNI)
    void onSurfaceCreated(ANativeWindow* window);
    void onSurfaceChanged(int width, int height);
    void onSurfaceDestroyed();
    bool onInputEvent(AInputEvent* event);
    void onAppCmd(int cmd);
    void onFocusChanged(bool hasFocus);

    // Set the Android app state
    void setAppState(void* app) { appState_ = app; }
    void setDensity(int dpi) { density_ = dpi; }

private:
    // Key code mapping
    int keyFromAndroidKeyCode(int32_t keyCode) const;
    int modifiersFromMetaState(int32_t metaState) const;

    ANativeWindow* nativeWindow_ = nullptr;
    void* appState_ = nullptr;  // android_app*
    ALooper* looper_ = nullptr;
    AInputQueue* inputQueue_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int density_ = 160;  // DPI

    bool hasFocus_ = false;
    std::string title_;
    std::queue<PlatformEvent> pendingEvents_;
    std::mutex eventMutex_;
};

}  // namespace cui_core

#endif  // __ANDROID__
