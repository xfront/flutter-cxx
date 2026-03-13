#pragma once
/// @file platform_factory.h
/// @brief Convenience header to create the right PlatformWindow for the current
/// OS.
///
/// Supports: Android, iOS, Web/WASM, Windows, macOS, Linux (X11/Wayland)
///
/// HarmonyOS 走 OhosWindow：surface 由 ArkTS 的 XComponent 创建，C++ 侧靠
/// attachSurface(surfaceId) 把它换成 OHNativeWindow（见 ohos_window.h）。在挂上
/// surface 之前窗口尺寸未知，所以宿主仍可以先用 createOffscreen() 起管线。
///
/// Usage:
///   #include "cui_core/platform_factory.h"
///   auto window = cui_core::createPlatformWindow();

#include "cui/platform/platform_window.h"

#if defined(__ANDROID__)
#include "cui/platform/android_window.h"
#elif defined(__EMSCRIPTEN__)
#include "cui/platform/web_window.h"
#elif defined(_WIN32)
#include "cui/platform/win32_window.h"
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include "cui/platform/ios_window.h"
#else
#include "cui/platform/cocoa_window.h"
#endif
#elif defined(__OHOS__)
// 鸿蒙的 triple 是 aarch64-linux-ohos ⇒ __linux__ 同为 1，这一支必须排在它前面。
#include "cui/platform/ohos_window.h"
#elif defined(__linux__) && !defined(__OHOS__)
#include "cui/platform/x11_window.h"
#endif

#include <memory>

namespace cui {

/// Create a PlatformWindow appropriate for the current platform.
inline std::unique_ptr<PlatformWindow> createPlatformWindow() {
#if defined(__ANDROID__)
    return std::make_unique<AndroidWindow>();
#elif defined(__EMSCRIPTEN__)
    return std::make_unique<WebWindow>();
#elif defined(_WIN32)
    return std::make_unique<Win32Window>();
#elif defined(__OHOS__)
    return std::make_unique<OhosWindow>();
#elif defined(__APPLE__)
#if TARGET_OS_IOS
    return std::make_unique<IOSWindow>();
#else
    return std::make_unique<CocoaWindow>();
#endif
#elif defined(__linux__)
    return std::make_unique<X11Window>();
#else
#error "Unsupported platform"
#endif
}

}  // namespace cui_core
