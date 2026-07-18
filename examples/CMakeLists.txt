
# ── Examples / Apps ──────────────────────────────────────────────────────────
# 依赖方向：本目录（应用层）→ cui_core / packages（引擎层）。引擎侧的库与源码都不
# include 这里的头、也不编这里的源码。
#
# 这里只持有 **story 专属**的部分：控件树（gallery_shell / gallery_dock）、
# 桌面入口、以及 cui_core::createAppRoot() 的实现（app_root.cpp）。
# 平台宿主的实现尽可能留在 cui_core/src/platform/{android,ohos}/：入口 cpp、
# Java / ArkTS 平台服务桥、HAP 工程骨架全在那里，由引擎层持有；它们只依赖
# 引擎头声明的注入接口，不认识 story。Android 的 app 身份资产（Manifest、
# GalleryActivity）与 story 控件树一起归 examples/platform/android/。
#
# 但 **app 产物的构建定义**在本文件（应用层）：桌面 story、
# android libcui_gallery.so、ohos libcui.so。引擎目录不再有 CMakeLists 去编译
# app 目标 —— 目标在哪定义，就意味着谁拥有这个产物。
#
# 用 OBJECT 库而非静态库：gallery_shell.cpp / gallery_dock.cpp 里有只被静态
# 初始化或间接引用的符号，静态库不会拉进未直接引用的 .o。

set(CUI_APP_LIBS cui_core cui_charts cui_code_editor cui_video cui_dock)

add_library(cui_app_root OBJECT
    app_root.cpp
        story_shell.cpp
        story_dock.cpp)
set_target_properties(cui_app_root PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(cui_app_root PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cui_app_root PRIVATE ${CUI_APP_LIBS})

# ── 桌面 story ───────────────────────────────────────────────────────
# 与平台宿主共用同一个 cui_app_root，入口 main() 同样走 createAppRoot()。
# WIN32 → /subsystem:windows，运行时不附带控制台窗口；非 Windows 平台忽略。
# 入口仍是 main()，MSVC 下用 /ENTRY:mainCRTStartup 指回 CRT 的 main 启动桩。
if(CUI_BUILD_EXAMPLES)
    add_executable(story WIN32 story.cpp)
    target_link_libraries(story PRIVATE cui_app_root ${CUI_APP_LIBS})
    if(MSVC)
        target_link_options(story PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

# ── Android：libcui_gallery.so ──────────────────────────────────────────────
# Android 上不做多 .so 拆分（NativeActivity 只 load 一个 lib_name），引擎、
# 控件包、应用根都链进这一个 .so。宿主入口 cpp 在 cui_core/src/platform/android/cpp，
# 是平台实现（只依赖 cui_core/framework/app_root.h），由本文件（应用层）编译。
if(CUI_PLATFORM STREQUAL "android" AND CUI_BUILD_ANDROID_APP)
    # glue 是纯 C 源，根 project() 只声明了 CXX，需补语言才能推断链接语言。
    enable_language(C)
    set(CUI_APP_GLUE_DIR "${ANDROID_NDK}/sources/android/native_app_glue")
    if(NOT EXISTS "${CUI_APP_GLUE_DIR}/android_native_app_glue.c")
        message(FATAL_ERROR "找不到 android_native_app_glue：${CUI_APP_GLUE_DIR}")
    endif()
    add_library(cui_native_app_glue STATIC
            "${CUI_APP_GLUE_DIR}/android_native_app_glue.c")
    target_include_directories(cui_native_app_glue PUBLIC "${CUI_APP_GLUE_DIR}")
    target_link_libraries(cui_native_app_glue PUBLIC android log)

    set(CUI_ANDROID_HOST_DIR "${CMAKE_SOURCE_DIR}/cui_core/src/platform/android")
    add_library(cui_gallery SHARED "${CUI_ANDROID_HOST_DIR}/cpp/main.cpp")
    target_include_directories(cui_gallery PRIVATE "${CUI_ANDROID_HOST_DIR}/cpp")
    target_link_libraries(cui_gallery PRIVATE
            cui_app_root ${CUI_APP_LIBS} cui_native_app_glue android log)

    # glue 的 ANativeActivity_onCreate 只被 NativeActivity 按 dlsym 查找，平台
    # 通道的 JNI 入口同理；不 -u 钉住的话 --as-needed 会把这些对象整个丢掉。
    target_link_options(cui_gallery PRIVATE
            "-Wl,-u,ANativeActivity_onCreate"
            "-Wl,-u,Java_dev_cui_framework_CuiActivity_nativeInstallCuiPlatform"
            "-Wl,-u,Java_dev_cui_framework_CuiActivity_nativeUninstallCuiPlatform"
            "-Wl,-u,Java_dev_cui_framework_CuiActivity_nativeCuiMethodCall"
            "-Wl,-u,Java_dev_cui_framework_CuiActivity_nativeCuiNotify"
            "-Wl,-u,Java_dev_cui_framework_CuiActivity_nativeCuiEvent")
endif()

# ── OpenHarmony：libcui.so ──────────────────────────────────────────────────
# 库名必须是 cui_core：ArkTS 的 import 与 XComponent 的 libraryname 都按它找，napi
# 模块名在 services/ohos_channel.cpp 里也写死成 "cui_core"，故目标名另起、用
# OUTPUT_NAME 钉库名。模块注册靠 __attribute__((constructor))，下面那条 -u 把
# 「构造器不能丢」写成显式约定；-z defs 让未定义符号在链接期就暴露（dlopen
# 时才会报 undefined symbol，装机后更难查）。
if(CUI_PLATFORM STREQUAL "ohos" AND CUI_BUILD_OHOS_APP)
    set(CUI_OHOS_HOST_DIR "${CMAKE_SOURCE_DIR}/cui_core/src/platform/ohos")
    add_library(cui_ohos_host SHARED "${CUI_OHOS_HOST_DIR}/cpp/main.cpp")
    set_target_properties(cui_ohos_host PROPERTIES OUTPUT_NAME "cui_core")
    target_include_directories(cui_ohos_host PRIVATE "${CUI_OHOS_HOST_DIR}/cpp")
    target_link_libraries(cui_ohos_host PRIVATE
            cui_app_root ${CUI_APP_LIBS} ace_napi.z
            native_buffer native_window hilog_ndk.z)
    target_link_options(cui_ohos_host PRIVATE
            "-Wl,-u,CuiRegisterPlatformModule" "-Wl,-z,defs")
endif()
