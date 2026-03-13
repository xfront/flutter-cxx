# build.cmake — cui_core 的一键构建入口（CMake 脚本模式）。
#
# 由仓库根的 build.cmd / build.sh 转发：
#   build.cmd  [平台] [profile] [选项]      (Windows)
#   bash build.sh [平台] [profile] [选项]      (Linux/macOS)
#
# 只做三件事：检查工具链 → 转调 CMakePresets → 报告产物。真正的编译参数仍
# 全在 CMakePresets.json 里，所以 IDE / CI 用预设直接构建与脚本结果完全一致。

cmake_minimum_required(VERSION 3.20)

# 用脚本自身所在目录（cmake/）解析相对路径与仓库根，不依赖调用方的当前
# 工作目录——build.sh 从仓库根运行，之前 cwd=仓库根时找不到 cmake/ 下的文件，
# 且 ".." 会算成仓库根的上一层。CMAKE_CURRENT_LIST_FILE 在脚本模式下可靠。
get_filename_component(_cui_script_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(CUI_ROOT "${_cui_script_dir}/.." ABSOLUTE)
include("${_cui_script_dir}/cui_build_common.cmake")

set(CUI_PLATFORM "")
set(CUI_PROFILE "")
set(CUI_TARGET "")
set(CUI_JOBS "")
set(CUI_INSTALL_PREFIX "")
set(CUI_ARCH "")
set(CUI_SKIP_TESTS FALSE)
set(CUI_CONFIGURE_ONLY FALSE)
set(CUI_SERVE FALSE)
set(CUI_INSTALL_EMSDK FALSE)
set(CUI_HELP FALSE)
set(CUI_EXTRA_CONFIGURE_ARGS "")
set(CUI_ZIP FALSE)
set(CUI_ZIP_NAME "")
set(_positionals "")

set(_platforms windows linux macos ios android web ohos)
set(_profiles debug release asan ubsan asan-ubsan)

# 取下一个参数（宏：在调用者作用域里改 _i / ${out_var}）。
macro(cui_take_next out_var)
    if(_i GREATER_EQUAL CMAKE_ARGC)
        cui_die("选项 ${_arg} 缺少取值。")
    endif()
    set(_next_name "CMAKE_ARGV${_i}")
    set(${out_var} "${${_next_name}}")
    math(EXPR _i "${_i} + 1")
endmacro()

# CMAKE_ARGV0=cmake ARGV1=-P ARGV2=<本文件>，用户参数从 3 开始。
set(_i 3)
while(_i LESS CMAKE_ARGC)
    set(_arg_name "CMAKE_ARGV${_i}")
    set(_arg "${${_arg_name}}")
    math(EXPR _i "${_i} + 1")

    if(_arg STREQUAL "--")
        # wrapper 用来隔开 cmake 自身的参数，忽略即可。
    elseif(_arg STREQUAL "-h" OR _arg STREQUAL "--help" OR _arg STREQUAL "help")
        set(CUI_HELP TRUE)
    elseif(_arg STREQUAL "--target")
        cui_take_next(CUI_TARGET)
    elseif(_arg STREQUAL "--jobs" OR _arg STREQUAL "-j")
        cui_take_next(CUI_JOBS)
    elseif(_arg STREQUAL "--install")
        cui_take_next(CUI_INSTALL_PREFIX)
    elseif(_arg STREQUAL "--arch")
        cui_take_next(CUI_ARCH)
    elseif(_arg STREQUAL "--no-test")
        set(CUI_SKIP_TESTS TRUE)
    elseif(_arg STREQUAL "--configure-only")
        set(CUI_CONFIGURE_ONLY TRUE)
    elseif(_arg STREQUAL "--serve")
        set(CUI_SERVE TRUE)
    elseif(_arg STREQUAL "--install-emsdk")
        set(CUI_INSTALL_EMSDK TRUE)
    elseif(_arg STREQUAL "--zip")
        set(CUI_ZIP TRUE)
    elseif(_arg STREQUAL "--zip-name")
        cui_take_next(CUI_ZIP_NAME)
    elseif(_arg MATCHES "^-D.+")
        list(APPEND CUI_EXTRA_CONFIGURE_ARGS "${_arg}")
    elseif(_arg MATCHES "^-")
        cui_die("未知选项 ${_arg}（build.cmd --help 查看用法）。")
    else()
        list(APPEND _positionals "${_arg}")
    endif()
endwhile()

if(CUI_HELP)
    message([[
cui_core 构建入口（纯 CMake 脚本，Windows 不需要 Git Bash）

用法：
  build.cmd     [平台] [profile] [选项]     Windows
  bash build.sh [平台] [profile] [选项]     Linux / macOS

平台与可用 profile（省略平台时取当前主机）：
  windows   x64 MSVC + Ninja            debug | release
  linux     x64 GCC / Clang             debug | release | asan | ubsan | asan-ubsan
  macos     Apple Silicon 或 Intel      debug | release（--arch arm64|x64）
  ios       arm64 设备静态库            debug | release
  android   arm64-v8a 静态库            debug | release
  web       wasm32（Emscripten）        debug | release（--serve 预览；缺工具链用 --install-emsdk）
  ohos      arm64（HarmonyOS NDK）      debug | release

选项：
  --target <名>       只构建指定目标（默认全量）
  --jobs <n>          并行度（默认由 Ninja 决定）
  --arch <arm64|x64>  macOS 目标架构（默认取主机架构）
  --install <前缀>    构建后执行 cmake --install 到该前缀
  --zip               构建后把 dist/ 打成 zip（默认名 cui_core-<平台>-<profile>.zip）
  --zip-name <名>     指定 zip 文件名（默认放在仓库根）
  --configure-only    只配置不构建（验证工具链）
  --no-test           跳过 ctest
  --serve             web：构建后用 Python 起本地 HTTP 服务（端口 8080）
  --install-emsdk     web：缺 Emscripten 时自动 clone 到 ~/emsdk 并装 latest
  -D<变量>=<值>       透传给 cmake --preset（如 -DCUI_BUILD_TESTS=OFF）
  -h, --help          显示本帮助

示例：
  build.cmd                            Windows 调试全量构建 + ctest
  build.cmd release                    Windows Release
  bash build.sh linux asan             Linux AddressSanitizer
  bash build.sh web --serve            构建 wasm 并在 http://127.0.0.1:8080 预览
  bash build.sh web --install-emsdk    先装 emsdk（clone + install latest）再构建
  bash build.sh android --configure-only  只验证 NDK 工具链是否打通
]])
    return()
endif()

# ── 位置参数：平台 / profile（顺序可颠倒，只写 profile 即用主机平台）──
foreach(_p IN LISTS _positionals)
    if(CUI_PLATFORM STREQUAL "" AND _p IN_LIST _platforms)
        set(CUI_PLATFORM "${_p}")
    elseif(CUI_PROFILE STREQUAL "" AND _p IN_LIST _profiles)
        set(CUI_PROFILE "${_p}")
    else()
        cui_die("无法识别的参数 '${_p}'（build.cmd --help 查看用法）。")
    endif()
endforeach()

cui_host_platform(CUI_HOST)
if(CUI_PLATFORM STREQUAL "")
    set(CUI_PLATFORM "${CUI_HOST}")
endif()
if(CUI_PROFILE STREQUAL "")
    set(CUI_PROFILE "debug")
endif()

if(CUI_PLATFORM STREQUAL "darwin")
    set(CUI_PLATFORM "macos")
endif()

if(NOT EXISTS "${_cui_script_dir}/platforms/${CUI_PLATFORM}.cmake")
    cui_die("未知平台 '${CUI_PLATFORM}'。可用：windows linux macos ios android web ohos")
endif()

if(CUI_INSTALL_EMSDK AND NOT CUI_PLATFORM STREQUAL "web")
    cui_warn("--install-emsdk 只对 web 平台生效，本次忽略。")
    set(CUI_INSTALL_EMSDK FALSE)
endif()

# Git Bash 会把挂载共享显示成 //host/Share/...；MSVC 的 cmd 规则不接受 UNC
# 工作目录，Windows 上先换成映射盘（Z:\...），否则链接阶段会 LNK1104。
set(_root_before "${CUI_ROOT}")
cui_unc_to_drive(CUI_ROOT "${CUI_ROOT}")
if(NOT "${CUI_ROOT}" STREQUAL "${_root_before}")
    cui_info("源码路径: ${CUI_ROOT}（UNC → 映射盘）")
endif()

message("")
message("cui_core build — 平台 ${CUI_PLATFORM} / ${CUI_PROFILE} / 主机 ${CUI_HOST}")

include("${_cui_script_dir}/platforms/${CUI_PLATFORM}.cmake")
cmake_language(CALL "cui_${CUI_PLATFORM}_main")

if(CUI_ZIP AND NOT CUI_CONFIGURE_ONLY)
    # 解析 dist 目录：默认仓库根 dist/；若用 -DCUI_DIST_DIR 覆盖了则优先用它。
    set(_cui_dist "${CUI_ROOT}/dist")
    foreach(_a IN LISTS CUI_EXTRA_CONFIGURE_ARGS)
        if(_a MATCHES "^-DCUI_DIST_DIR=(.+)$")
            set(_cui_dist "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    cui_package_zip("${_cui_dist}" "${CUI_PLATFORM}" "${CUI_PROFILE}" "${CUI_ZIP_NAME}")
endif()
