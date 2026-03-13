#pragma once
/// @file cocoa_window.h
/// @brief Cocoa/AppKit implementation of PlatformWindow (macOS).

#include "platform_window.h"

#ifdef __APPLE__

#include <deque>
#include <queue>
#include <string>

// Forward declarations (Objective-C types)
struct NSWindow;
struct NSView;
struct NSTrackingArea;

namespace cui {

class CocoaWindow : public PlatformWindow {
public:
    CocoaWindow();
    ~CocoaWindow() override;

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

    // Pointer grab
    void grabPointer() override;
    void ungrabPointer() override;

    // Clipboard
    bool clipboardSetText(const std::string& utf8) override;
    std::string clipboardText() const override;
    bool hasClipboardText() const override;

    // IME
    bool takeIMCommit(std::string& utf8) override;
    std::string imPreedit() const override;
    void setIMPreeditRect(int x, int y) override;

    // Called from Objective-C delegate/view
    void pushEvent(const PlatformEvent& event);
    void resize(int width, int height);

    // Event handlers called from CUIView
    void onMouseDown(double x, double y, int button, int modifiers);
    void onMouseUp(double x, double y, int button, int modifiers);
    void onMouseMoved(double x, double y, int modifiers);
    void onMouseEntered();
    void onMouseExited();
    void onScrollWheel(double deltaX, double deltaY, double x, double y, int modifiers);
    void onKeyDown(int keyCode, const std::string& characters, int modifiers);
    void onKeyUp(int keyCode, int modifiers);
    void onFlagsChanged(int modifiers);

    // IME callbacks
    void onIMCommit(const std::string& text);
    void onIMPreedit(const std::string& text);
    void onIMPreeditEnd();

private:
    void* nsWindow_ = nullptr;  // NSWindow*
    void* nsView_ = nullptr;    // NSView* (content view)
    void* trackingArea_ = nullptr;  // NSTrackingArea*
    int width_ = 0;
    int height_ = 0;
    int minWidth_ = 0;
    int minHeight_ = 0;

    bool pointerGrabbed_ = false;
    int currentModifiers_ = 0;

    std::queue<PlatformEvent> pendingEvents_;

    // IME state
    std::deque<std::string> imCommits_;
    std::string imPreedit_;
    int imPreeditX_ = 0;
    int imPreeditY_ = 0;
};

}  // namespace cui_core

#endif  // __APPLE__
