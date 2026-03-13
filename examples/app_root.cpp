/// @file app_root.cpp — 应用根的实现侧（gallery 示例 App）。
///
/// 引擎宿主只声明 `cui_core::createAppRoot()`，这里给出实现并把 gallery 控件树装配
/// 进去。把它单独放一个 TU，是为了让平台宿主的 .cpp / CMake 不必 include 任何
/// examples 头：宿主只依赖 cui_core/framework/app_root.h，最终链接时由
/// `cui_app_root` 目标把这些对象挂进宿主产物。

#include "cui/framework/app_root.h"

#include "gallery_shell.h"

namespace cui {

SharedPtr<Widget> createAppRoot() {
    return makeGalleryShell();
}

}  // namespace cui_core
