# ios.cmake — iOS arm64（真机）静态库。
#
# 预设：ios-arm64-debug / ios-arm64-release
# 前置：macOS + Xcode + vcpkg（arm64-ios triplet）。
# 产物：源码根 dist/ 下的 libcui-core.a + libcui-wx.a
#
# 只出静态库：iOS 的进程入口在 Xcode 工程里（Swift/ObjC 宿主，见
# src/platform/ios/：CuiApp.swift + CuiHost.mm + ios_window.mm + project.yml）。
# 打包：scripts/package_ipa.sh（cmake 出静态库 → XcodeGen 生成 ios.xcodeproj →
# xcodebuild archive/exportArchive 出 IPA），示例/测试/设计器在预设里已关闭。

function(cui_ios_main)
    if(NOT CMAKE_HOST_APPLE)
        cui_die("ios 平台只能在 macOS 主机上构建（当前主机：${CUI_HOST}）。")
    endif()
    if(NOT CUI_PROFILE MATCHES "^(debug|release)$")
        cui_die("ios 的 profile 只支持 debug / release（收到 '${CUI_PROFILE}'）。")
    endif()

    set(_preset "ios-arm64-${CUI_PROFILE}")
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
    cui_info("接入 Xcode：把上面的 .a 与 src/platform/ios/ 下的 Swift 桥加入工程，")
    cui_info("并链接 UIKit / Metal / Foundation / CoreFoundation。")
    cui_footer("${_dir}")
endfunction()
