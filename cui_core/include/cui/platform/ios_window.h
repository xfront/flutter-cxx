#pragma once
/// @file ios_window.h
/// @brief iOS/UIKit implementation of PlatformWindow.
///
/// Uses UIView for rendering and UIKit gestures for input.
/// Requires iOS 13+ for modern SwiftUI/UIKit integration.

#include "platform_window.h"

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_IPHONE

#include <cmath>
#include <mutex>
#include <queue>
#include <string>

// Forward declarations
struct UIWindow;
struct UIView;
struct UIViewController;

namespace cui {

class IOSWindow : public PlatformWindow {
public:
  IOSWindow();
  ~IOSWindow() override;

  void create(const std::string &title, int width, int height) override;
  void destroy() override;
  bool pollEvent(PlatformEvent &event) override;
  bool waitForEvent(PlatformEvent &event, int timeoutMs) override;
  void invalidate() override;
  void show() override;
  // 返回「设备像素」尺寸（逻辑点数 × UIScreen.scale），与 Android/OHOS 的
  // 设备像素窗口约定对齐：Skia surface 据此按 Retina 分辨率建，再由 CALayer
  // 适配上屏 → 高清。不放大则 1 surface 像素对应多个物理像素，文字/线条发虚。
  int width() const override {
    double s = scale_ > 0.0 ? scale_ : 1.0;
    return static_cast<int>(std::lround(static_cast<double>(width_) * s));
  }
  int height() const override {
    double s = scale_ > 0.0 ? scale_ : 1.0;
    return static_cast<int>(std::lround(static_cast<double>(height_) * s));
  }
  void wakeUp() override;
  void setTitle(const std::string &title) override;
  void setMinimumSize(int width, int height) override;
  double dpiScale() const override;
  // 供 UIView 把触摸坐标从「点」换算到「设备像素」窗口空间（见 onTouch）。
  double contentScale() const { return scale_ > 0.0 ? scale_ : 1.0; }
  NativeHandles nativeHandles() override;

  // iOS-specific callbacks (from UIView delegate)
  void pushEvent(const PlatformEvent &event);
  void resize(int width, int height);
  void onTouch(int touchId, double x, double y, int phase);

private:
  void *uiWindow_ = nullptr;       // UIWindow*
  void *uiView_ = nullptr;         // UIView*
  void *viewController_ = nullptr; // UIViewController*

  int width_ = 0;
  int height_ = 0;
  double scale_ = 1.0;

  std::string title_;
  std::queue<PlatformEvent> pendingEvents_;
  std::mutex eventMutex_;
};

} // namespace cui_core

#endif // TARGET_OS_IOS
#endif // __APPLE__
