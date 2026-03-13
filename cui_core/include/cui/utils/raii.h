#pragma once
/// @file raii.h
/// @brief RAII wrappers for platform resources.
///
/// Skia manages its own resources via sk_sp<>, so we only need
/// RAII for the platform layer (XCB connection on Linux).

#include <memory>

// ── X11/XCB (Linux) ────────────────────────────────────────────────────────

// 鸿蒙的 triple 是 aarch64-linux-ohos ⇒ `__linux__` 在鸿蒙上同样是 1，但 NDK
// sysroot 里没有 xcb —— 所有 Linux 桌面分支都得显式排除 __OHOS__。
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__OHOS__)

#include <xcb/xcb.h>

namespace cui {

struct XcbConnectionDeleter {
    void operator()(xcb_connection_t* c) { xcb_disconnect(c); }
};

using XcbConnectionPtr = std::unique_ptr<xcb_connection_t, XcbConnectionDeleter>;

}  // namespace cui_core

#endif  // __linux__

// ── Win32 (Windows) ─────────────────────────────────────────────────────────

#ifdef _WIN32

// Win32 resources (HDC, etc.) are managed per-call in Win32Window.
// No global RAII wrappers needed at this level.

#endif  // _WIN32

// ── macOS/Cocoa ─────────────────────────────────────────────────────────────

#ifdef __APPLE__

// Cocoa resources (NSWindow, NSView) are managed via Objective-C ARC.
// No manual RAII wrappers needed.

#endif  // __APPLE__

// ── Wayland (Linux) ─────────────────────────────────────────────────────────

// Wayland resources (wl_display, wl_surface, etc.) are managed
// directly in WaylandWindow's create/destroy methods.
