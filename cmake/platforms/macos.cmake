# macos.cmake — macOS（Apple Silicon / Intel）。
#
# 预设：macos-arm64-debug / macos-arm64-release / macos-x64-debug / macos-x64-release
# 前置：Xcode 命令行工具 + vcpkg；Apple 平台的 Skia 走 metal + graphite。
# 产物：源码根 dist/ 下统一放置 libcui-core.* / libcui-wx.*、
#       demo_gallery、demo_ide 及 vcpkg 的 libskia.dylib / libav*.dylib 等。

function(cui_macos_main)
    if(NOT CMAKE_HOST_APPLE)
        cui_die("macos 平台只能在 macOS 主机上构建（当前主机：${CUI_HOST}）。")
    endif()
    if(NOT CUI_PROFILE MATCHES "^(debug|release)$")
        cui_die("macos 的 profile 只支持 debug / release（收到 '${CUI_PROFILE}'）。")
    endif()

    if(CUI_ARCH STREQUAL "")
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
            set(CUI_ARCH arm64)
        else()
            set(CUI_ARCH x64)
        endif()
    endif()
    if(NOT CUI_ARCH MATCHES "^(arm64|x64)$")
        cui_die("--arch 只支持 arm64 / x64（收到 '${CUI_ARCH}'）。")
    endif()

    set(_preset "macos-${CUI_ARCH}-${CUI_PROFILE}")
    cui_require_vcpkg()
    cui_configure("${_preset}")
    cui_build_dir("${_preset}" _dir)

    if(CUI_CONFIGURE_ONLY)
        cui_info("--configure-only：停在配置阶段 ${_dir}")
        return()
    endif()

    cui_build("${_preset}")
    cui_test("${_preset}")
    cui_install("${_preset}")

    cui_report_libs("${_dir}")
    cui_step("可执行文件")
    cui_report_file("${_dir}" "../dist/demo_gallery" "示例画廊")
    cui_report_file("${_dir}" "../dist/demo_ide" "示例 IDE")
    cui_info("")
    cui_info("运行示例：${_dir}/../dist/demo_gallery")
    cui_footer("${_dir}")
endfunction()
