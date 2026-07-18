# ── cui_core 独立控件包 ───────────────────────────────────────────────────────────
# `cui_wx/` 是 Widget / Element / RenderObject 三层引擎本体；这里放「按控件族
# 切出来」的独立包。每个包自带 include/ 与 src/，对外只暴露自己那一层头：
#
#   cui_core/charts/chart.h            图表（Line / Bar / Pie / Scatter / Radar…）
#   cui_core/code_editor/code_editor.h 代码编辑器（对齐 code_forge）
#   cui_core/dock/dock.h               停靠系统（DockManager / DockPanel / Splitter…）
#   cui_core/video/video_player.h      播放器控件 + 解码后端（FFmpeg / Media Foundation）
#
# 依赖方向严格单向：package -> cui_wx -> cui_core。引擎本体不 include 任何
# package，所以只做引擎的消费方不必付 FFmpeg / 编辑器 / 停靠系统的编译与链接代价。
#
# 库类型跟根 CMakeLists 的 CUI_LIBRARY_TYPE（STATIC / SHARED）走，与
# cui_core / cui_wx 完全一致：切换开关后包名、头路径、链接写法都不变。

add_subdirectory(charts)
add_subdirectory(code_editor)
add_subdirectory(dock)
add_subdirectory(video)
