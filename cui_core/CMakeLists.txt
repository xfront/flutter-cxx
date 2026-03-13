# ── cui_core flutter 层：cui_core + cui_wx ─────────────────────────────────────────
# Widget / Element / RenderObject 三层引擎本体，头文件在 `include/cui_core/*`。
# 引擎拆成两个库，库类型由根 CMakeLists 的 CUI_LIBRARY_TYPE 决定
# （STATIC ⇒ .a，SHARED ⇒ .so，版本号由 cui_add_library 补）：
#
#   cui_core  libcui_core.*  foundation / platform / rendering / framework
#   cui_wx    libcui_wx.*    只收 flutter/src/widgets/ 的源码
#
# 源码按功能分成五组，依赖关系自底向上：
#
#   foundation <- platform <- rendering <- framework <- widgets
#
#   foundation  几何 / Key / Ticker / 主题 / 图像解码缓存 / 线程池 / i18n
#   platform    窗口、输入事件、Skia surface、剪贴板/文件框等平台服务
#   rendering   RenderObject / RenderBox / RenderView、PipelineOwner、Layer 树
#   framework   Widget / Element / BuildOwner / InheritedWidget 等框架协议
#   widgets     具体控件族（basic/layout/container/scroll/sliver/…）
#
# 前四组连同平台后端、ASan 例外都在 cui_core；widgets 单独成 cui_wx 并
# PUBLIC 链接 cui_core，静态库顺序即「cui_wx 在前、cui_core 在后」。
# 根 CMakeLists 只 `add_subdirectory(flutter)`；`cui_common`（工具链 + 第三方
# usage requirements）与 `cui_core` 聚合目标仍留在根上，因为 `package/*` 里的独立
# 控件包（video / charts / code_editor / dock）同样要链接它们。
#
# 头文件根路径由 cui_common 的 BUILD_INTERFACE 提供
# （`${CMAKE_SOURCE_DIR}/flutter/include`），并沿 cui_core -> cui_wx 传递。

set(CUI_CORE_SOURCES
    # ── foundation：无 UI 依赖的构建块 ──
        src/foundation/abstract_node.cpp
        src/foundation/color.cpp
        src/foundation/geometry.cpp
        src/foundation/image_cache.cpp
        src/foundation/image_data.cpp
        src/foundation/image_provider.cpp
        src/foundation/layer.cpp
        src/foundation/painter.cpp
        src/foundation/theme.cpp
        src/foundation/thread_pool.cpp
        src/foundation/ticker.cpp
        src/foundation/translator.cpp
        src/foundation/key.cpp

    # ── rendering：RenderObject / RenderBox / 管线 ──
        src/rendering/render_object.cpp
        src/rendering/render_box.cpp
        src/rendering/render_view.cpp
        src/rendering/painting_context.cpp
        src/rendering/render_box_widgets.cpp
        src/rendering/render_editable.cpp
        src/rendering/render_material.cpp
        src/rendering/render_interactive.cpp
        src/rendering/render_inkwell.cpp
        src/rendering/render_icon.cpp
        src/rendering/render_scroll.cpp
        src/rendering/animated_size.cpp
        src/rendering/sliver.cpp
        src/rendering/sliver_grid.cpp
        src/rendering/render_flex.cpp
        src/rendering/render_scaffold.cpp
        src/rendering/render_theater.cpp

    # ── framework + bindings ──
        src/framework/application.cpp
        src/framework/app_window.cpp
        src/framework/skia_renderer.cpp
        src/framework/scheduler_binding.cpp
        src/framework/overlay_anchor.cpp
        src/framework/app_lifecycle.cpp
        src/framework/build_owner.cpp
        src/framework/element.cpp
        src/framework/framework.cpp
        src/framework/render_object_widget.cpp
        src/framework/focus.cpp
)

# ── platform：平台服务 + 每平台恰好一个窗口后端（都归 cui_core）──────────
# 平台服务（channels / permissions / camera / Bluetooth）是便携的：JNI / NAPI /
# Objective-C 桥在别的平台上各自编成 stub，对外契约保持一致。
list(APPEND CUI_CORE_SOURCES
        src/platform/services/method_channel.cpp
        src/platform/services/event_channel.cpp
        src/platform/services/android_channel.cpp
        src/platform/services/ohos_channel.cpp
        src/platform/services/ios_channel.cpp
        src/platform/services/permission.cpp
        src/platform/services/camera.cpp
        src/platform/services/bluetooth.cpp
        src/platform/services/app_lifecycle.cpp
        src/platform/services/system_chrome.cpp
        src/platform/services/haptic_feedback.cpp
        src/platform/services/system_navigator.cpp
        src/platform/services/file_dialog.cpp)

# 静态库自己不携带系统框架/库的引用，这些依赖必须 PUBLIC 传给消费方。
set(CUI_CORE_PUBLIC_LIBS "")
if(CUI_PLATFORM STREQUAL "linux")
    list(APPEND CUI_CORE_SOURCES src/platform/x11_window.cpp)
elseif(CUI_PLATFORM STREQUAL "windows")
    list(APPEND CUI_CORE_SOURCES src/platform/win32_window.cpp)
elseif(CUI_PLATFORM STREQUAL "android")
    list(APPEND CUI_CORE_SOURCES src/platform/android_window.cpp)
elseif(CUI_PLATFORM STREQUAL "web")
    # Emscripten backend: HTML5 canvas presentation + DOM input events.
    # web/shell.html 由根 CMakeLists 传给 emcc 的 --shell-file。
    list(APPEND CUI_CORE_SOURCES src/platform/web_window.cpp)
elseif(CUI_PLATFORM STREQUAL "macos")
    # AppKit backend（Objective-C++，故为 .mm）。
    list(APPEND CUI_CORE_SOURCES src/platform/cocoa_window.mm)
    list(APPEND CUI_CORE_PUBLIC_LIBS "-framework Cocoa")
elseif(CUI_PLATFORM STREQUAL "ios")
    list(APPEND CUI_CORE_SOURCES src/platform/ios_window.mm)
    list(APPEND CUI_CORE_PUBLIC_LIBS "-framework UIKit")
elseif(CUI_PLATFORM STREQUAL "ohos")
    # OHOS NDK backend：OHNativeWindow buffer queue + hilog。
    list(APPEND CUI_CORE_SOURCES src/platform/ohos_window.cpp)
    list(APPEND CUI_CORE_PUBLIC_LIBS native_window hilog_ndk.z)
endif()

# Skia 的预编译 libskia.a 不带 -fsanitize=address，而 ASan 会改变 Skia 内联容器
# skia_private::TArray 的布局（Recording::fFinishedProcs 16→24 字节），导致本 TU
# 看到的 Recording（184）比库实际分配的（176）大 8 字节。该文件跨边界释放
# Recording（Recorder::snap() 返回的 unique_ptr 在 flush() 析构），带 ASan 时会在
# 释放处触发 new-delete-type-mismatch（并破坏堆）。因此在 skia_renderer.cpp 上关掉
# ASan，使其 Recording 布局与库一致；该文件仅为渲染胶水层，内存访问不插桩无影响。
# 其他 Skia 对象（SkSurface/SkImage 等）经 sk_sp 由库内 unref() 释放，不受影响。
if(NOT MSVC AND CUI_SANITIZE_ADDRESS)
    set_source_files_properties(src/framework/skia_renderer.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
endif()

cui_add_library(cui_core ${CUI_CORE_SOURCES})

target_include_directories(cui_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/cui_core/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(cui_core PUBLIC cui_common ${CUI_CORE_PUBLIC_LIBS})

# RenderRepaintBoundary 是纯头文件（= RenderProxyBox + isRepaintBoundary()）。
# 边界层的创建/复用都在 painting_context.cpp。
