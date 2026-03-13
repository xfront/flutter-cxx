# linux.cmake — Linux x64（GCC / Clang + Ninja）。
#
# 预设：x64-linux-{debug,release,asan,ubsan,asan-ubsan}
# 前置：X11/xcb、fontconfig 开发包；Skia 及其依赖由 vcpkg 提供。
# 产物：源码根 dist/ 下统一放置 libcui-core.* / libcui-wx.*、
#       demo_gallery、demo_ide、vs2022 及 vcpkg 的 libskia.so / libav*.so 等。

function(cui_linux_main)
    if(NOT CMAKE_HOST_UNIX OR CMAKE_HOST_APPLE)
        cui_die("linux 平台只能在 Linux 主机上构建（当前主机：${CUI_HOST}）。")
    endif()
    if(NOT CUI_PROFILE MATCHES "^(debug|release|asan|ubsan|asan-ubsan)$")
        cui_die("linux 的 profile 支持 debug / release / asan / ubsan / asan-ubsan（收到 '${CUI_PROFILE}'）。")
    endif()

    set(_preset "x64-linux-${CUI_PROFILE}")
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
    cui_report_file("${_dir}" "../dist/vs2022" "可视化设计器")
    cui_info("")
    if(CUI_PROFILE STREQUAL "asan" OR CUI_PROFILE STREQUAL "asan-ubsan")
        cui_info("ASan 运行示例前设置：ASAN_OPTIONS=new_delete_type_mismatch=0")
        cui_info("（本机 vcpkg 的 Skia 头/库宏配置有偏斜，见 docs/BUILDING.md）")
    endif()
    cui_info("运行示例：${_dir}/../dist/demo_gallery")
    cui_footer("${_dir}")
endfunction()
