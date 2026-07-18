# ── cui_code_editor ────────────────────────────────────────────────────────
# 代码编辑器（对齐 code_forge 11.7.0）：文档/行索引、自动缩进与配对、
# 撤销合并、折叠、查找替换、逐字符词法着色都在 src/code_editor.cpp。
# 它是一套「控件 + 编辑状态机」，只依赖 cui_wx 的控件层（文本框 / 滚动 /
# 焦点 / 键盘路由），不依赖任何其他包。
cui_add_library(cui_code_editor
        src/code_editor.cpp)

target_include_directories(cui_code_editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(cui_code_editor PUBLIC cui_wx)
