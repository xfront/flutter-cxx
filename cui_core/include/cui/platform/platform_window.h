#pragma once
/// @file platform_window.h
/// @brief Abstract platform window — the cross-platform seam.
///
/// Each platform provides a concrete implementation:
///   - X11Window     (Linux/X11)
///   - WaylandWindow (Linux/Wayland)
///   - Win32Window   (Windows)
///   - CocoaWindow   (macOS)
///
/// The Application owns a PlatformWindow and uses it for:
///   1. Creating the native OS window
///   2. Polling / dispatching events
///   3. Getting native handles (for Skia surface creation)

#include <cstdint>
#include <functional>
#include <string>

#include "event.h"

namespace cui {

class PlatformWindow {
public:
    virtual ~PlatformWindow() = default;

    /// Create the native window.
    virtual void create(const std::string& title, int width, int height) = 0;

    /// Destroy the native window.
    virtual void destroy() = 0;

    /// Poll one event. Returns true if an event was available.
    virtual bool pollEvent(PlatformEvent& event) = 0;

    /// Block until an event is available (with timeout in ms, 0 = non-blocking).
    virtual bool waitForEvent(PlatformEvent& event, int timeoutMs = -1) = 0;

    /// Request a repaint (triggers Expose event).
    virtual void invalidate() = 0;

    /// Show / map the window.
    virtual void show() = 0;

    /// Get window dimensions.
    virtual int width() const = 0;
    virtual int height() const = 0;

    /// Wake the event loop from another thread.
    virtual void wakeUp() = 0;

    /// Set window title at runtime.
    virtual void setTitle(const std::string& title) = 0;

    /// Set minimum window size.
    virtual void setMinimumSize(int width, int height) = 0;

    /// Remove/restore the system title bar and window decorations.
    /// When true, the entire window area is available for self-drawing
    /// (custom title bar, full-bleed content). Default false (system chrome).
    virtual void setBorderless(bool borderless) { (void)borderless; }

    // ── Pointer grab ───────────────────────────────────────────────────────
    // While grabbed, motion and button-release events keep flowing to this
    // window even after the pointer leaves it — without it a drag that ends
    // outside the window never receives its release and the widget keeps
    // tracking the cursor. Default no-op for platforms without grab support.
    virtual void grabPointer() {}
    virtual void ungrabPointer() {}

    // ── Input method (XIM / TSF / CETray…) ────────────────────────────────
    /// Pop one UTF-8 string committed by the active input method, returning
    /// false when none is pending. Backends that filter key events through
    /// an IM consume the original KeyPress, so the Application must pull
    /// commits itself (typically right before dispatching a KeyDown).
    virtual bool takeIMCommit(std::string& /*utf8*/) { return false; }
    /// Current composition string (preedit) reported by the IM; empty when
    /// not composing.
    virtual std::string imPreedit() const { return {}; }
    /// Position the IM candidate window at the root-space point (x, y).
    virtual void setIMPreeditRect(int /*x*/, int /*y*/) {}

    // ── Clipboard ──────────────────────────────────────────────────────────
    /// Copy UTF-8 text to the system clipboard. Returns true on success.
    virtual bool clipboardSetText(const std::string& /*utf8*/) { return false; }
    /// Paste UTF-8 text from the system clipboard. Returns empty string on failure.
    virtual std::string clipboardText() const { return {}; }
    /// Check if the clipboard contains text.
    virtual bool hasClipboardText() const { return false; }

    /// Get the DPI scale factor (for HiDPI support).
    virtual double dpiScale() const { return 1.0; }

    // ── Native handles (for Skia / GPU surface creation) ───────────────────

    struct NativeHandles {
        // X11 / XCB
        void* xcb_connection = nullptr;  // xcb_connection_t*
        uint32_t xcb_window = 0;
        void* xcb_visual = nullptr;  // xcb_visualtype_t*
        uint32_t xcb_gc = 0;         // xcb_gcontext_t for PutImage blits
        uint32_t xcb_depth = 0;      // Window visual depth (e.g. 24)

        // Wayland
        void* wl_display = nullptr;   // wl_display*
        void* wl_surface = nullptr;   // wl_surface*
        void* xdg_surface = nullptr;  // xdg_surface*

        // Win32
        void* hwnd = nullptr;       // HWND
        void* hdc = nullptr;        // HDC
        void* hinstance = nullptr;  // HINSTANCE

        // macOS / Cocoa
        void* ns_window = nullptr;  // NSWindow*
        void* ns_view = nullptr;    // NSView*

        // Android / OpenHarmony (OHNativeWindow*; set once the host has a surface)
        void* android_window = nullptr;
        void* ohos_window = nullptr;

        // Vulkan (for Impeller/Skia GPU)
        void* vk_instance = nullptr;
        void* vk_physical = nullptr;
        void* vk_device = nullptr;
    };

    virtual NativeHandles nativeHandles() = 0;

    // ── Presentation ───────────────────────────────────────────────────────
    /// Backends that own their submission state (native buffer queues need a
    /// per-window request/flush pairing) blit here instead of through
    /// Application::blitToNativeWindow().  Returning true means the pixels were
    /// consumed; the default false keeps the generic path (X11 / Win32 / …).
    /// x/y/w/h is the damaged rect in window pixels — a backend that can only
    /// submit whole frames may ignore it.
    virtual bool presentPixels(const void* /*pixels*/, int /*rowBytes*/, int /*x*/, int /*y*/,
                               int /*width*/, int /*height*/) {
        return false;
    }
};

}  // namespace cui_core
