# android.cmake — Android arm64-v8a 构建。
#
# 预设：android-arm64-debug / android-arm64-release
# 前置：Android SDK + NDK、vcpkg（arm64-android triplet）。脚本会在未设置
#       ANDROID_NDK_HOME 时从 ANDROID_HOME/ndk/ 里挑最新版本。
# 产物：源码根 dist/ 下的引擎与包静态库，以及 libcui_gallery.so
#       （NativeActivity 入口 + native_app_glue）。
#
# 出可安装的 APK（manifest + Java + dex + .so + 签名，不需要 Gradle）：
#       bash scripts/package_apk.sh
# 平台宿主实现就在引擎平台目录（app 产物的构建定义在 examples/CMakeLists.txt）：
#       flutter/src/platform/android/{java/dev/cui_core/framework, cpp/main.cpp}
#       examples/platform/android/{AndroidManifest.xml, java/dev/cui_core/gallery}

function(cui_android_main)
    if(NOT CUI_PROFILE MATCHES "^(debug|release)$")
        cui_die("android 的 profile 只支持 debug / release（收到 '${CUI_PROFILE}'）。")
    endif()

    set(_preset "android-arm64-${CUI_PROFILE}")
    cui_require_android_ndk()
    cui_require_vcpkg()
    cui_configure("${_preset}")
    cui_build_dir("${_preset}" _dir)

    if(CUI_CONFIGURE_ONLY)
        cui_info("--configure-only：停在配置阶段 ${_dir}")
        return()
    endif()

    cui_build("${_preset}")
    cui_install("${_preset}")

    cui_report_libs("${_dir}")
    cui_info("")
    cui_info("出 APK：bash scripts/package_apk.sh（arm64-v8a，含签名）")
    cui_info("宿主：flutter/src/platform/android（平台桥 java/ + cpp/main.cpp），")
    cui_info("      引擎与包静态链进 libcui_gallery.so，由 NativeActivity 加载。")
    cui_footer("${_dir}")
endfunction()
