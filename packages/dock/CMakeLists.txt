# ── cui_dock ───────────────────────────────────────────────────────────────
# 停靠系统：DockManager（树的唯一真相）/ DockPanel / DockArea / Splitter /
# 浮动窗口 / 自动隐藏栏 / 标签拖拽换序，实现在 src/dock.cpp。
# 只依赖 flutter 的控件层；不引用 charts / code_editor / video 任一包。
cui_add_library(cui_dock
        src/dock.cpp)

target_include_directories(cui_dock PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(cui_dock PUBLIC cui_wx)
