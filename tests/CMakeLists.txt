
# 图像类测试（test_image_editor / test_image_painter）会把 PNG 落盘到 $TMPDIR，
# 而本机 /tmp 仅 10MB、且构建树下的 tmp 目录不一定被创建。这里统一创建目录并
# 给相关测试显式注入 TMPDIR，确保 ctest 子进程里该目录存在、可写。
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tmp")

add_executable(test_lock_free_queue test_lock_free_queue.cpp)
target_link_libraries(test_lock_free_queue PRIVATE cui)

add_executable(test_pipeline test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE cui)

# Widget 架构层单元测试
add_executable(test_framework test_framework.cpp)
target_link_libraries(test_framework PRIVATE cui)

# 导航栈（Overlay / Navigator / Router / Scaffold）单元测试
add_executable(test_navigation test_navigation.cpp)
target_link_libraries(test_navigation PRIVATE cui)

# 层输入 / 焦点基础设施单元测试
add_executable(test_input test_input.cpp)
target_link_libraries(test_input PRIVATE cui)

# 层动画驱动 + 液态玻璃切换单元测试
add_executable(test_animation test_animation.cpp)
target_link_libraries(test_animation PRIVATE cui)

# 协程异步（Future / Stream / AsyncBuilder）+ 隐式动画单元测试
add_executable(test_async test_async.cpp)
target_link_libraries(test_async PRIVATE cui)

# 层内容/装饰叶子 Widget（Text / Opacity / ClipRRect）单元测试
add_executable(test_widgets test_widgets.cpp)
target_link_libraries(test_widgets PRIVATE cui)

# 第二批布局/绘制 Widget（Transform / RotatedBox / FittedBox / Wrap / Offstage …）
add_executable(test_layout test_layout.cpp)
target_link_libraries(test_layout PRIVATE cui)

# 富文本 / 继承式 Widget / LayoutBuilder 单元测试
add_executable(test_text test_text.cpp)
target_link_libraries(test_text PRIVATE cui)

# Material 装饰/进度组件（Divider / Card / Chip / ProgressIndicator）单元测试
add_executable(test_material test_material.cpp)
target_link_libraries(test_material PRIVATE cui)

# 交互式控件（GestureDetector / Checkbox / Switch / Radio / Slider / Button）单元测试
add_executable(test_interactive test_interactive.cpp)
target_link_libraries(test_interactive PRIVATE cui)

# 滚动系列（ScrollController / Viewport / Scrollable / ListView / GridView / Scrollbar）单元测试
add_executable(test_scroll test_scroll.cpp)

# Material 第二批（Icon / IconButton / ListTile / Tooltip / Card elevation / 过渡动画）单元测试
add_executable(test_material2 test_material2.cpp)
target_link_libraries(test_material2 PRIVATE cui)

# Material 第三批（Chip 扩展 / InkWell / Card onTap / Slider 标签）单元测试
add_executable(test_material3 test_material3.cpp)
target_link_libraries(test_material3 PRIVATE cui)

# 滚动第二批（ScrollPhysics / ListViewLazy / GridViewLazy 懒加载）单元测试
add_executable(test_scroll2 test_scroll2.cpp)
target_link_libraries(test_scroll2 PRIVATE cui)

# 弹层组件（Dialog / AlertDialog / SimpleDialog / PopupMenuButton）单元测试
add_executable(test_popup test_popup.cpp)
target_link_libraries(test_popup PRIVATE cui)
target_link_libraries(test_scroll PRIVATE cui)

# 回归：Overlay 条目移除时其宿主 Element 必须先从脏列表摘除，否则
# flushBuildScope 对悬空指针 rebuild()→markNeedsBuild() 触发 UAF。
add_executable(test_overlay_free test_overlay_free.cpp)
target_link_libraries(test_overlay_free PRIVATE cui)

# 文本输入层（controller / 键盘路由 / IME / RenderEditable）单元测试
add_executable(test_text_input test_text_input.cpp)
target_link_libraries(test_text_input PRIVATE cui)

# TabController / TabBar / TabBarView / ExpansionTile / Stepper 单元测试
add_executable(test_tabs test_tabs.cpp)
target_link_libraries(test_tabs PRIVATE cui)

# 显式动画控件族（Fade/Slide/Scale/Rotation/Size/Align Transition）+ Align 因子语义
add_executable(test_transition test_transition.cpp)
target_link_libraries(test_transition PRIVATE cui)
add_executable(test_dock test_dock.cpp)
target_link_libraries(test_dock PRIVATE cui cui_dock)

# 隐式动画控件族（AnimatedOpacity/Scale/Rotation/Slide/Padding/Align/
# Positioned/Container/Switcher/CrossFade + TweenAnimationBuilder）
add_executable(test_implicit_animation test_implicit_animation.cpp)
target_link_libraries(test_implicit_animation PRIVATE cui)

# ── CTest integration ─────────────────────────────────────────────────────────
# 层生命周期回归（卸载期安全性 / 焦点 / 监听注销）
add_executable(test_lifecycle test_lifecycle.cpp)
target_link_libraries(test_lifecycle PRIVATE cui)

# Sliver 渲染层（约束/几何协议 + toBoxAdapter/padding/fixedExtentList + sliver 视口）
add_executable(test_sliver test_sliver.cpp)
target_link_libraries(test_sliver PRIVATE cui)

# Sliver 控件层（SliverToBoxAdapter/SliverPadding/SliverList + CustomScrollView 接线）
add_executable(test_sliver_widgets test_sliver_widgets.cpp)
target_link_libraries(test_sliver_widgets PRIVATE cui)

# Sliver 懒加载控件层（child delegate + SliverMultiBoxAdaptorElement 托管人）
add_executable(test_sliver_lazy test_sliver_lazy.cpp)
target_link_libraries(test_sliver_lazy PRIVATE cui)

# Sliver 网格（SliverGridLayout 度量 + 两族 gridDelegate + RenderSliverGrid + SliverGrid）
add_executable(test_sliver_grid test_sliver_grid.cpp)
target_link_libraries(test_sliver_grid PRIVATE cui)

# 通知框架（Notification 冒泡 + NotificationListener + ScrollNotification 家族 + 视口 depth）
add_executable(test_notification test_notification.cpp)
target_link_libraries(test_notification PRIVATE cui)

# 鼠标悬停区域（MouseRegion/RenderMouseRegion + enter/exit 差分顺序 + opaque 遮挡）
add_executable(test_mouse_region test_mouse_region.cpp)
target_link_libraries(test_mouse_region PRIVATE cui)

# 脏区/重绘链路（子树消失必须有脏区 + 根换渲染类型后宿主接回新根）
add_executable(test_repaint_damage test_repaint_damage.cpp)
target_link_libraries(test_repaint_damage PRIVATE cui)

# 绘制位置（推图层换录制画布后，兄弟节点的绘制基准不能漂移）
add_executable(test_paint_position test_paint_position.cpp)
target_link_libraries(test_paint_position PRIVATE cui)

# 尺寸补间（AnimatedSize / RenderAnimatedSize 四态状态机 + 溢出裁剪 + 续跑）
add_executable(test_animated_size test_animated_size.cpp)
target_link_libraries(test_animated_size PRIVATE cui)

# intrinsic 尺寸协议（记忆化 + 缓存失效耦合 + 各盒公式 + IntrinsicWidth/Height）
add_executable(test_intrinsic test_intrinsic.cpp)
target_link_libraries(test_intrinsic PRIVATE cui)

# 拖拽投放（Draggable/LongPressDraggable/DragTarget + MetaData 命中标记 + avatar 生命周期）
add_executable(test_drag_target test_drag_target.cpp)
target_link_libraries(test_drag_target PRIVATE cui)

# 批次B 前置件（Ticker 静音 / TickerMode / 帧末回调 / 子代访问 / getTransformTo）
add_executable(test_ticker_mode test_ticker_mode.cpp)
target_link_libraries(test_ticker_mode PRIVATE cui)

# 批次B 路由转场层（OverlayRoute / TransitionRoute / ModalRoute / 本地历史 /
# PageRoute / PopupRoute / secondaryAnimation / offstage）
add_executable(test_routes test_routes.cpp)
target_link_libraries(test_routes PRIVATE cui)

# 批次C Hero 飞行（Hero / HeroController / HeroMode + Overlay shuttle + divert）
add_executable(test_hero test_hero.cpp)
target_link_libraries(test_hero PRIVATE cui)

# Material Motion 转场族（DualTransitionBuilder / FadeScale / FadeThrough /
# SharedAxis / PageTransitionSwitcher / OpenContainer）
add_executable(test_material_motion test_material_motion.cpp)
target_link_libraries(test_material_motion PRIVATE cui)

add_test(NAME test_lock_free_queue COMMAND test_lock_free_queue)
add_test(NAME test_pipeline COMMAND test_pipeline)
add_test(NAME test_framework COMMAND test_framework)
add_test(NAME test_navigation COMMAND test_navigation)
add_test(NAME test_input COMMAND test_input)
add_test(NAME test_animation COMMAND test_animation)
add_test(NAME test_async COMMAND test_async)
add_test(NAME test_widgets COMMAND test_widgets)
add_test(NAME test_layout COMMAND test_layout)
add_test(NAME test_text COMMAND test_text)
add_test(NAME test_material COMMAND test_material)
add_test(NAME test_interactive COMMAND test_interactive)
add_test(NAME test_scroll COMMAND test_scroll)
add_test(NAME test_material2 COMMAND test_material2)
add_test(NAME test_material3 COMMAND test_material3)
add_test(NAME test_scroll2 COMMAND test_scroll2)
add_test(NAME test_popup COMMAND test_popup)
add_test(NAME test_text_input COMMAND test_text_input)
add_test(NAME test_tabs COMMAND test_tabs)
add_test(NAME test_lifecycle COMMAND test_lifecycle)
add_test(NAME test_transition COMMAND test_transition)
add_test(NAME test_dock COMMAND test_dock)
add_test(NAME test_implicit_animation COMMAND test_implicit_animation)
add_test(NAME test_sliver COMMAND test_sliver)
add_test(NAME test_sliver_widgets COMMAND test_sliver_widgets)
add_test(NAME test_sliver_lazy COMMAND test_sliver_lazy)
add_test(NAME test_sliver_grid COMMAND test_sliver_grid)
add_test(NAME test_notification COMMAND test_notification)
add_test(NAME test_mouse_region COMMAND test_mouse_region)
add_test(NAME test_repaint_damage COMMAND test_repaint_damage)
add_test(NAME test_paint_position COMMAND test_paint_position)
add_test(NAME test_animated_size COMMAND test_animated_size)
add_test(NAME test_intrinsic COMMAND test_intrinsic)
add_test(NAME test_drag_target COMMAND test_drag_target)
add_test(NAME test_ticker_mode COMMAND test_ticker_mode)
add_test(NAME test_routes COMMAND test_routes)
add_test(NAME test_hero COMMAND test_hero)
add_test(NAME test_material_motion COMMAND test_material_motion)

# ComboBox（上游 RawAutocomplete 移植）：过滤 / 键盘导航 / 浮层 / 拆除
add_executable(test_combobox test_combobox.cpp)
target_link_libraries(test_combobox PRIVATE cui)
add_test(NAME test_combobox COMMAND test_combobox)

# AutoHideToolbar（rui story 移植）：悬停展开 / 延迟收回 / pin / 盒子尺寸命中闸门
add_executable(test_auto_hide_toolbar test_auto_hide_toolbar.cpp)
target_link_libraries(test_auto_hide_toolbar PRIVATE cui)
add_test(NAME test_auto_hide_toolbar COMMAND test_auto_hide_toolbar)

# DropdownButton（widgets/controls.h）：菜单宽度与按钮同宽 / 长选项撑宽 / 选中收起
add_executable(test_dropdown test_dropdown.cpp)
target_link_libraries(test_dropdown PRIVATE cui)
add_test(NAME test_dropdown COMMAND test_dropdown)

# BuildOwner::onBuildScheduled：setState 必须自己请求一帧（上升沿 / 同批合并 / flush 复位）
add_executable(test_build_scheduling test_build_scheduling.cpp)
target_link_libraries(test_build_scheduling PRIVATE cui)
add_test(NAME test_build_scheduling COMMAND test_build_scheduling)

# SearchBar / SearchableList / OtpInput 的真实键盘输入路由（键入 / 过滤 / 跳格 / 拆除）
add_executable(test_keyboard_inputs test_keyboard_inputs.cpp)
target_link_libraries(test_keyboard_inputs PRIVATE cui)
add_test(NAME test_keyboard_inputs COMMAND test_keyboard_inputs)

# Calendar（widgets/data.h）：日期格点击选中 / 选中态高亮 / 月份导航
add_executable(test_calendar test_calendar.cpp)
target_link_libraries(test_calendar PRIVATE cui)
add_test(NAME test_calendar COMMAND test_calendar)

# ExpandableFab（widgets/controls.h）：四种尺寸变体 / 普通+禁用 FAB / Fan·Up·Side 展开 / 子项动画
add_executable(test_expandable_fab test_expandable_fab.cpp)
target_link_libraries(test_expandable_fab PRIVATE cui)
add_test(NAME test_expandable_fab COMMAND test_expandable_fab)

# 平台服务层（platform/services）：MethodChannel / EventChannel / 权限 / 相机 / 蓝牙
add_executable(test_platform_services test_platform_services.cpp)
target_link_libraries(test_platform_services PRIVATE cui)
add_test(NAME test_platform_services COMMAND test_platform_services)

# RichTextEditor（对齐 Quill）：Delta 文档模型 / 编辑与格式命令 / 历史 /
# 逐行渲染几何 / 真实键盘与鼠标编辑 / 工具栏 chip / 长行横向滚动 / 拆除
add_executable(test_rich_text_editor test_rich_text_editor.cpp)
target_link_libraries(test_rich_text_editor PRIVATE cui)
add_test(NAME test_rich_text_editor COMMAND test_rich_text_editor)

# 图像核心层（foundation/image_{data,cache,provider}.h）：SkCodec 解码（含多帧
# 合成与降采样）/ 像素几何 / 三级缓存 / Provider 与加载器（去重·取消·进度合并）
add_executable(test_image_core test_image_core.cpp)
target_link_libraries(test_image_core PRIVATE cui)
target_compile_definitions(test_image_core PRIVATE
    CUI_SAMPLE_GIF="${CMAKE_CURRENT_SOURCE_DIR}/../examples/res/sample.gif")
add_test(NAME test_image_core COMMAND test_image_core)

# ExtendedImage 控件层：占位/进度/失败兜底、gapless 换源、cacheWidth 签名、
# 动图播放（Ticker）与 dispose 取消；素材用同一张 sample.gif。
add_executable(test_extended_image test_extended_image.cpp)
target_link_libraries(test_extended_image PRIVATE cui)
target_compile_definitions(test_extended_image PRIVATE
    CUI_SAMPLE_GIF="${CMAKE_CURRENT_SOURCE_DIR}/../examples/res/sample.gif")
add_test(NAME test_extended_image COMMAND test_extended_image)

# InteractiveViewer / TransformationController：矩阵几何（AABB·越界量·视口逆变换）、
# 滚轮缩放锚定与 min/max 夹取、单指/双指手势表、panAxis 锁轴、边界夹取与树形。
add_executable(test_interactive_viewer test_interactive_viewer.cpp)
target_link_libraries(test_interactive_viewer PRIVATE cui)
add_test(NAME test_interactive_viewer COMMAND test_interactive_viewer)

# PhotoView / PhotoViewGallery / SlidePage：边界与档位数学、两个 controller 契约、
# 建树几何、双击档位循环 + 补间、平移夹取与撞边界逃逸、捏合旋转回弹、滚轮锚定、
# 回调与 disableGestures、content fit/tightMode/hero/异步、画廊窗口与翻页、滑动关闭。
add_executable(test_photo_view test_photo_view.cpp)
target_link_libraries(test_photo_view PRIVATE cui)
add_test(NAME test_photo_view COMMAND test_photo_view)

# ImageEditor（对齐 extended_image 的 editor 层）：几何小工具与 EditActionDetails
# 口径、8 个把手的命中与拖动、最小尺寸/比例锁定、maxScale 与滚轮、单指拖图、
# 90° 步进旋转与水平/垂直翻转（烘进位图后的所见即所得）、归中补间、出图与 PNG 落盘。
add_executable(test_image_editor test_image_editor.cpp)
target_link_libraries(test_image_editor PRIVATE cui)
add_test(NAME test_image_editor COMMAND test_image_editor)

# ImagePainterLayer / ImagePainterController（自由手绘层）：操作级撤销栈（clear 可撤销）、
# 采点与落笔快照、单指守卫、订阅句柄的挂载/重绑/退订、像素级的「笔迹在上 + 橡皮只擦笔迹层」、
# compose 的两种坐标映射与 savePng 落盘回读。
add_executable(test_image_painter test_image_painter.cpp)
target_link_libraries(test_image_painter PRIVATE cui)
add_test(NAME test_image_painter COMMAND test_image_painter)
set_tests_properties(test_image_editor test_image_painter PROPERTIES
                      ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/tmp")

# video_player 控件族（对齐上游 video_player 包）：controller 状态机（seek 夹取/
# 音量/倍速/播完/循环/dispose）+ VideoPlayer 画当前帧 + VideoProgressIndicator
# 两段进度与 scrubber 点击 seek。素材用 video_demo.h 现场合成的 MJPEG AVI。
add_executable(test_video_player test_video_player.cpp)
target_link_libraries(test_video_player PRIVATE cui cui_video)
add_test(NAME test_video_player COMMAND test_video_player)

# 播放引擎的音频链路与主时钟（假后端 + 假设备，不依赖任何真解码器）：
# 有 sink ⇒ 位置跟音频走 / 无 sink ⇒ 退回墙钟；play/pause/seek/倍速/音量/dispose
# 与设备的同步次数；泵驱动（Web）那一条不起工作线程、且「这一轮没有新帧」
# 不被误判成 EOF；PCM 喂给的背压阈值；能力文案与 nativeAudio 一致。
add_executable(test_video_audio test_video_audio.cpp)
target_link_libraries(test_video_audio PRIVATE cui cui_video)
add_test(NAME test_video_audio COMMAND test_video_audio)

# 后端表（两条路线并存之后怎么选）：内置表与能力申报一致、preference 的先后
# 与整组开关、同名顶掉内置后端、打不开时往后退且 error 留最具体的一条原因、
# 注册项的出声设备优先于平台设备。全部走假件，不依赖真解码器。
add_executable(test_video_backends test_video_backends.cpp)
target_link_libraries(test_video_backends PRIVATE cui cui_video)
add_test(NAME test_video_backends COMMAND test_video_backends)

# 各端出声设备共用的那份 PCM 环形缓冲（packages/video/src/audio_ring.h）：三种来格式归一、
# 欠载补零也计进消耗、clear() 的基准语义（每 seek 一次位置不能多出一截）、在途帧扣减、
# 容量/背压/跨尾界、音量夹取。header-only 且只依赖 std ⇒ 不链 cui_core，也不碰任何真设备。
add_executable(test_audio_ring test_audio_ring.cpp)
target_include_directories(test_audio_ring PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/video/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/video/include)
add_test(NAME test_audio_ring COMMAND test_audio_ring)

# CodeEditor（对齐 code_forge 11.7.0）：文档/行索引、自动配对与自动缩进、UTF-8
# 退格、撤销合并与 Compound、行移动/复制/缩进/自动检测、折叠检测与可见行、查找
# 替换（大小写/整词/正则/循环）、控件挂载的焦点键路由与监听器摘除。
add_executable(test_code_editor test_code_editor.cpp)
target_link_libraries(test_code_editor PRIVATE cui cui_code_editor)
add_test(NAME test_code_editor COMMAND test_code_editor)

# 多窗口：Application 持有多个 AppWindow，每窗一套 PlatformWindow/Skia surface/
# BuildOwner/PipelineOwner/RenderView；输入按来源窗口派发，closeWindow 从列表摘除。
add_executable(test_multi_window test_multi_window.cpp)
target_link_libraries(test_multi_window PRIVATE cui)
add_test(NAME test_multi_window COMMAND test_multi_window)

# fontconfig keeps its process-lifetime global caches (matched patterns plus
# the expat-parsed font config) alive until exit, so AddressSanitizer builds
# report hundreds of bytes leaked from Fc*/expat on every run even though
# cui_core itself is leak-free. Silence only those third-party frames; real
# leaks in cui_core code still surface.
if(CUI_SANITIZE_ADDRESS)
    set_tests_properties(test_lock_free_queue test_pipeline test_framework
                         test_navigation test_input
                         test_animation test_async test_widgets test_layout
                         test_text test_material test_interactive
                         test_scroll test_material2
                         test_material3 test_scroll2
                         test_popup test_text_input test_tabs
                         test_lifecycle test_transition test_dock
                         test_implicit_animation test_sliver
                         test_sliver_widgets test_sliver_lazy
                         test_sliver_grid test_notification test_mouse_region
                         test_repaint_damage test_animated_size test_intrinsic
                         test_drag_target test_ticker_mode test_routes
                         test_hero test_material_motion test_combobox
                         test_auto_hide_toolbar test_paint_position test_dropdown
                         test_build_scheduling test_keyboard_inputs test_calendar test_expandable_fab
                         test_platform_services test_rich_text_editor test_image_core
                         test_extended_image test_interactive_viewer
                         test_photo_view test_image_editor test_image_painter
                         test_video_player test_video_audio test_video_backends
                         test_code_editor test_multi_window PROPERTIES
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_fontconfig.supp")
endif()
