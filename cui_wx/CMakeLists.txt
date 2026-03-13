# ── cui_core flutter 层：cui_core + cui_wx ─────────────────────────────────────────
# Widget / Element / RenderObject 三层引擎本体，头文件在 `include/cui_core/*`。
# 引擎拆成两个库，库类型由根 CMakeLists 的 CUI_LIBRARY_TYPE 决定
# （STATIC ⇒ .a，SHARED ⇒ .so，版本号由 cui_add_library 补）：
#
#   cui_core  libcui_core.*  foundation / platform / rendering / framework
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

# ── cui_wx：只负责 flutter/src/widgets ──────────────────────────────────────
set(CUI_WX_SOURCES
    # ── 动画原语 ──
        src/widgets/animation/animation.cpp
        src/widgets/animation/shared_animation.cpp
        src/widgets/animation/liquid_glass.cpp

    # ── 控件实现（框架层已抽到 core，见上面 CUI_CORE_SOURCES）──
        src/widgets/layout.cpp
        src/widgets/hero.cpp
        src/widgets/navigator.cpp
        src/widgets/overlay.cpp
        src/widgets/routes.cpp
        src/widgets/router.cpp
        src/widgets/scaffold.cpp
        src/widgets/sliver.cpp
        src/widgets/widgets_app.cpp
        src/widgets/container.cpp
        src/widgets/controls.cpp
        src/widgets/implicit_animation.cpp
        src/widgets/interactive.cpp
        src/widgets/tabs.cpp
        src/widgets/text_input.cpp
)


cui_add_library(cui_wx ${CUI_WX_SOURCES})

target_include_directories(cui_wx PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/cui_core/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/cui_wx/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(cui_wx PUBLIC cui_core)