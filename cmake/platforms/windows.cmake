# windows.cmake — Windows x64（MSVC + Ninja）。
#
# 预设：x64-windows-debug / x64-windows-release
# 前置：Visual Studio 或 Build Tools（「使用 C++ 的桌面开发」）+ vcpkg。
# 产物：源码根 dist/ 下统一放置 cui_core-core.lib / cui_core-wx.lib / *.dll、
#       demo_gallery.exe、demo_ide.exe、vs2022.exe 及 vcpkg 的 skia/ffmpeg DLL。

function(cui_windows_main)
    if(NOT CMAKE_HOST_WIN32)
        cui_die("windows 平台只能在 Windows 主机上构建（当前主机：${CUI_HOST}）。")
    endif()
    if(CUI_ROOT MATCHES "^//")
        cui_die(
            "源码在 UNC 路径（${CUI_ROOT}）上，且该共享没有映射盘。\n"
            "  MSVC 的 cmd 规则不支持 UNC 工作目录，请从映射盘（如 Z:\\...）运行。")
    endif()
    if(NOT CUI_PROFILE MATCHES "^(debug|release)$")
        cui_die("windows 的 profile 只支持 debug / release（收到 '${CUI_PROFILE}'）。")
    endif()

    set(_preset "x64-windows-${CUI_PROFILE}")
    cui_import_msvc_env()
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
    cui_exe_suffix(_exe)
    cui_step("可执行文件")
    cui_report_file("${_dir}" "../dist/demo_gallery${_exe}" "示例画廊")
    cui_report_file("${_dir}" "../dist/demo_ide${_exe}" "示例 IDE")
    cui_report_file("${_dir}" "../dist/vs2022${_exe}" "可视化设计器")
    cui_info("")
    cui_info("运行示例：${_dir}\\..\\dist\\demo_gallery${_exe}")
    cui_footer("${_dir}")
endfunction()
