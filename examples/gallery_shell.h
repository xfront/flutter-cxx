#pragma once
/// @file gallery_shell.h — gallery 壳与页面的**对外薄接口**。
///
/// 全部实现（8461 行内联代码：所有 Widget 子类、各 pageXxx() 页面工厂、
/// `gallery::GalleryShell`、`GalleryPagesPanel`、`buildAnimationDemos()` 等）
/// 已搬到 `gallery_shell.cpp`。本头文件只保留外部 TU（app_root.cpp / anitchk3）
/// 需要的两个工厂函数声明，以及 `gallery::GalleryShell` 的前向声明。
///
/// 这样拆分后：
///   - 实现只在一处（gallery_shell.cpp）编译，不再因被多个 TU include
///     而重复定义；
///   - 薄头不再拉进所有 cui 控件头，消除同名控件类（Tooltip/Switch/…）的
///     ODR 重定义冲突隐患。

#include <vector>

#include "cui/framework/framework.h"
#include "animated_demo.h"  // AnimDemoSpec / AnimPlayback（buildAnimationDemos 签名）
#include "cui/foundation/shared_ptr.h"

namespace cui {

/// gallery 壳（标题栏 + 侧边栏 + 页面区 + 状态条）。实现见 gallery_shell.cpp。
/// 平台宿主不直接调它，而是经 cui/framework/app_root.h 的 createAppRoot() 注入。
SharedPtr<Widget> makeGalleryShell();

namespace gallery {

class GalleryShell;  ///< 前向声明：仅 gallery_shell.cpp 需要完整定义

/// 动画页：返回一组动画 demo 规格，由 anitchk3 等外部测试驱动。
std::vector<AnimDemoSpec> buildAnimationDemos(SharedPtr<AnimPlayback> playback);

}  // namespace gallery
}  // namespace cui
