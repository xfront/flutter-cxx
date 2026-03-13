#pragma once
/// @file win32_window.h
/// @brief Win32 implementation of PlatformWindow (Windows).

#include "platform_window.h"

#ifdef _WIN32

#include <queue>
#include <string>
#include <windows.h>

namespace cui {

class Win32Window : public PlatformWindow {
public:
    Win32Window();
    ~Win32Window() override;

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
    void setBorderless(bool borderless) override;
    double dpiScale() const override;
    NativeHandles nativeHandles() override;

    // Pointer grab — keeps motion/release events flowing during drag
    void grabPointer() override;
    void ungrabPointer() override;

    // Input method (IME) support
    bool takeIMCommit(std::string& utf8) override;
    std::string imPreedit() const override;
    void setIMPreeditRect(int x, int y) override;

    // Clipboard
    bool clipboardSetText(const std::string& utf8) override;
    std::string clipboardText() const override;
    bool hasClipboardText() const override;

    // Win32 window procedure (public for RegisterClassExW)
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    // IME helpers
    void setupIME();
    void destroyIME();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int minWidth_ = 0;
    int minHeight_ = 0;

    bool pointerGrabbed_ = false;
    bool trackingMouseLeave_ = false;

    // IME state
    HIMC imeContext_ = nullptr;
    std::string imPreedit_;
    std::queue<std::string> imCommits_;
    POINT imeCaretPos_ = {0, 0};
    /// Chars the IME echoes back as WM_CHAR/WM_IME_CHAR for the last
    /// committed composition string. Swallowed so the text enters only
    /// once (through the commit stream); consumed on match, cleared on
    /// mismatch and when a real (non-IME) key arrives.
    std::wstring imeEcho_;

    // Pending KeyDown event waiting for WM_CHAR to fill text
    PlatformEvent pendingKeyDown_;

    std::queue<PlatformEvent> pendingEvents_;
};

}  // namespace cui_core

#endif  // _WIN32
