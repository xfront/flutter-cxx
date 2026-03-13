# web.cmake — WebAssembly（Emscripten）。
#
# 预设：wasm32-emscripten-debug / wasm32-emscripten-release
#       构建目录分别是 build-wasm-debug / build-wasm-release
# 前置：emsdk（预设从 $env{EMSDK} 取工具链）。脚本会自动探测常见安装位置和
#       发行版 emscripten；都没有时可加 --install-emsdk 自动 clone + 安装。
#       另需 wasm32-emscripten triplet 的 vcpkg 依赖。
# 产物：源码根 dist/ 下的 demo_gallery.{html,js,wasm,data}、demo_ide.*
#
# --serve：用 Python 起本地 HTTP 服务（Emscripten 产物不能用 file:// 打开，
#          浏览器会拒绝加载 wasm）。

function(cui_web_main)
    if(NOT CUI_PROFILE MATCHES "^(debug|release)$")
        cui_die("web 的 profile 只支持 debug / release（收到 '${CUI_PROFILE}'）。")
    endif()

    # 会自动探测常见位置与发行版 emscripten；都找不到时可用 --install-emsdk
    # 让脚本 clone/安装。
    cui_require_emsdk()

    set(_preset "wasm32-emscripten-${CUI_PROFILE}")
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
    cui_step("可执行文件")
    cui_report_file("${_dir}" "../dist/demo_gallery.html" "示例画廊（页面）")
    cui_report_file("${_dir}" "../dist/demo_gallery.js" "示例画廊（入口）")
    cui_report_file("${_dir}" "../dist/demo_gallery.wasm" "示例画廊（wasm）")
    cui_report_file("${_dir}" "../dist/demo_ide.html" "示例 IDE（页面）")
    cui_info("")

    # shell.html 只有输出是 .html 时才会被 Emscripten 采用，这里按实际产物
    # 挑入口，而不是假定一定有页面。
    if(EXISTS "${_dir}/../dist/demo_gallery.html")
        set(_entry "demo_gallery.html")
    else()
        set(_entry "demo_gallery.js")
        cui_warn("没有生成 demo_gallery.html：--shell-file 只在输出为 .html 时生效。")
    endif()

    if(CUI_SERVE)
        find_program(_python NAMES python3 python)
        if(NOT _python)
            cui_die("找不到 Python，无法启动本地服务；可自行用任意静态服务器打开 ${_dir}/../dist。")
        endif()
        cui_step("本地预览 http://127.0.0.1:8080/${_entry}")
        cui_info("按 Ctrl+C 结束。")
        execute_process(
            COMMAND "${_python}" -m http.server 8080
            WORKING_DIRECTORY "${_dir}/../dist"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            cui_die("本地服务退出（${_rc}）。")
        endif()
    else()
        cui_info("预览：python3 -m http.server 8080 --directory ${_dir}/../dist")
        cui_info("      然后打开 http://127.0.0.1:8080/${_entry}")
        cui_footer("${_dir}")
    endif()
endfunction()
