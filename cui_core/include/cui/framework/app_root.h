#pragma once
/// @file framework/app_root.h
/// @brief 应用根控件树的注入点（依赖倒置的接口侧）。
///
/// 平台宿主（android / ohos 的 NativeActivity / NAPI 入口）只认这个声明：它负责
/// 窗口、输入与帧循环，不知道最终跑的是 gallery 还是别的应用。具体实现由**应用
/// 层**提供并链进最终产物（本仓是 examples/app_root.cpp → cui_app_root 目标）。
///
/// 这样依赖方向严格单向：examples（应用）→ cui_core（引擎）；引擎的库与源码都不
/// include 任何 examples 头，也不编 examples 源码。
///
/// 注意：本头只有声明、没有实现。只链引擎库而不链应用实现的产物会在**链接期**
/// 因 unresolved `cui_core::createAppRoot()` 报错 —— 这正是刻意暴露的装配错误。
#include "cui/foundation/shared_ptr.h"

namespace cui {

class Widget;

/// 返回要挂到 `Application::setRoot()` 的控件树。由应用层实现。
SharedPtr<Widget> createAppRoot();

}  // namespace cui_core
