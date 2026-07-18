# cui_build_common.cmake — cmake/build.cmake 与各平台模块共用的助手。
#
# 只在 CMake 脚本模式（cmake -P）下运行：不做 project()，不参与构建，只负责
# 检查环境、转调 CMake 预设、报告产物。被 include() 使用。
#
# 之所以不用 shell 脚本：Windows 上 vcvars / 路径 / 引号三件事在 cmd 与 Git
# Bash 之间来回转换很容易出错；脚本模式下三端（cmd / PowerShell / Git Bash）
# 行为一致，只需要 CMake 本身。

# ── 输出 ────────────────────────────────────────────────────────────────────
function(cui_step msg)
    message("")
    message("==> ${msg}")
endfunction()

function(cui_info msg)
    message("    ${msg}")
endfunction()

function(cui_warn msg)
    message("WARN: ${msg}")
endfunction()

# 多行提示写成相邻字符串字面量时，CMake 会把它们当成多个实参（不像 C 会
# 自动拼接），只取第一个就只剩标题行——这里统一拼回去。
function(cui_die msg)
    set(_text "${msg}")
    foreach(_part IN LISTS ARGN)
        string(APPEND _text "${_part}")
    endforeach()
    message(FATAL_ERROR "${_text}")
endfunction()

# ── 主机 / 平台 ─────────────────────────────────────────────────────────────
function(cui_host_platform out_var)
    if(CMAKE_HOST_WIN32)
        set(_p windows)
    elseif(CMAKE_HOST_APPLE)
        set(_p macos)
    elseif(CMAKE_HOST_UNIX)
        set(_p linux)
    else()
        set(_p unknown)
    endif()
    set(${out_var} "${_p}" PARENT_SCOPE)
endfunction()

# 预设 → 构建目录（wasm 两个预设单独覆盖过 binaryDir）。
function(cui_build_dir preset out_var)
    if(preset STREQUAL "wasm32-emscripten-debug")
        set(_d "${CUI_ROOT}/build-wasm-debug")
    elseif(preset STREQUAL "wasm32-emscripten-release")
        set(_d "${CUI_ROOT}/build-wasm-release")
    else()
        set(_d "${CUI_ROOT}/build-${preset}")
    endif()
    set(${out_var} "${_d}" PARENT_SCOPE)
endfunction()

# ── 外部命令 ────────────────────────────────────────────────────────────────
# 会显式检查返回码的调用（用于 --list-presets 之类的探测）。
function(cui_capture out_var)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${CUI_ROOT}"
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc)
    set(${out_var} "${_out}" PARENT_SCOPE)
    set(${out_var}_RC "${_rc}" PARENT_SCOPE)
endfunction()

# 打印完整输出并终止的调用（configure / build / ctest / install）。
function(cui_run_checked what)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${CUI_ROOT}"
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc)
    if(NOT _out STREQUAL "")
        message("${_out}")
    endif()
    if(NOT _err STREQUAL "")
        message("${_err}")
    endif()
    if(NOT _rc EQUAL 0)
        cui_die("${what} 失败（退出码 ${_rc}）。")
    endif()
endfunction()

# 全静默探测，返回码放 ${out_var}（0 = 成功）。
function(cui_try_run out_var)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${CUI_ROOT}"
        OUTPUT_QUIET
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    set(${out_var} "${_rc}" PARENT_SCOPE)
endfunction()

# ── MSVC / Ninja ────────────────────────────────────────────────────────────
# 在 Windows 上找 VS 安装（vcwhere → 常见路径），用于 vcvars64.bat 与自带的
# Ninja。返回 VS 安装根目录。
function(cui_find_vs out_var)
    # 注意：$ENV{ProgramFiles(x86)} 不是合法写法（CMake 变量名不允许括号），
    # 只能按 vswhere 的固定安装位置逐个探测。
    foreach(_vswhere IN ITEMS
            "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
            "C:/Program Files/Microsoft Visual Studio/Installer/vswhere.exe")
        if(EXISTS "${_vswhere}")
            cui_capture(_dump "${_vswhere}" -latest -products
                Microsoft.VisualStudio.Product.BuildTools -property installationPath)
            string(STRIP "${_dump}" _install)
            if(_install STREQUAL "")
                cui_capture(_dump "${_vswhere}" -latest -property installationPath)
                string(STRIP "${_dump}" _install)
            endif()
            if(NOT _install STREQUAL "" AND
               EXISTS "${_install}/VC/Auxiliary/Build/vcvars64.bat")
                set(${out_var} "${_install}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()

    file(GLOB _candidates
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat"
        "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat"
        "D:/Program Files/VisualStudio/*/BuildTools/VC/Auxiliary/Build/vcvars64.bat")
    list(SORT _candidates COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH _candidates _n)
    if(_n GREATER 0)
        list(GET _candidates 0 _vcvars)
        get_filename_component(_prefix "${_vcvars}" DIRECTORY)   # .../Build
        get_filename_component(_prefix "${_prefix}" DIRECTORY)   # .../Auxiliary
        get_filename_component(_prefix "${_prefix}" DIRECTORY)   # .../VC
        get_filename_component(_prefix "${_prefix}" DIRECTORY)   # <VS root>
        set(${out_var} "${_prefix}" PARENT_SCOPE)
        return()
    endif()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# Windows 的 Ninja/CL 只能从 vcvars 得到。把 vcvars 打印的环境导入本进程。
#
# 两个坑：
#  1) `call vcvars.bat >nul` 由 cmd 起子 shell，环境变量会丢；call 与 set 必须
#     在同一个 cmd 里执行。
#  2) `cmd /c "call \"<带空格的路径>\" && set"` 的引号嵌套会被 cmd 吃掉
#     （「D:\Program Files\...」被截成「D:\Program」），所以写一个一次性
#     .bat 让 cmd 只接一个引号参数。
function(cui_import_msvc_env)
    if(NOT CMAKE_HOST_WIN32)
        return()
    endif()
    find_program(_cl cl)
    find_program(_ninja ninja)
    if(_cl AND _ninja)
        return()
    endif()

    cui_find_vs(_vs)
    if(_vs STREQUAL "")
        cui_die(
            "找不到 Visual Studio / Build Tools。\n"
            "  请安装「使用 C++ 的桌面开发」工作负载；Linux/macOS 主机用对应的脚本。")
    endif()
    set(_vcvars "${_vs}/VC/Auxiliary/Build/vcvars64.bat")
    file(TO_NATIVE_PATH "${_vcvars}" _vcvars_native)
    cui_info("MSVC:  ${_vs}")

    set(_shim_dir "$ENV{TEMP}")
    if(_shim_dir STREQUAL "")
        set(_shim_dir "${CUI_ROOT}/build-x64-windows-debug/tmp")
    endif()
    file(MAKE_DIRECTORY "${_shim_dir}")
    set(_shim "${_shim_dir}/cui_vcvars_env.bat")
    file(WRITE "${_shim}"
        "@echo off\r\n"
        "call \"${_vcvars_native}\" >nul 2>&1\r\n"
        "if errorlevel 1 exit /b 1\r\n"
        "set\r\n")
    execute_process(
        COMMAND cmd /c "${_shim}"
        WORKING_DIRECTORY "${CUI_ROOT}"
        OUTPUT_VARIABLE _env_dump
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    file(REMOVE "${_shim}")
    if(NOT _rc EQUAL 0 OR _env_dump STREQUAL "")
        cui_die("vcvars64.bat 执行失败（${_vcvars}）。")
    endif()

    # 逐行切分：env 值里有分号（PATH），不能把整段塞进 CMake list。
    string(REPLACE "\r\n" "\n" _env_dump "${_env_dump}")
    string(REPLACE "\r" "\n" _env_dump "${_env_dump}")
    set(_imported 0)
    while(NOT _env_dump STREQUAL "")
        string(FIND "${_env_dump}" "\n" _nl)
        if(_nl EQUAL -1)
            set(_line "${_env_dump}")
            set(_env_dump "")
        else()
            string(SUBSTRING "${_env_dump}" 0 ${_nl} _line)
            math(EXPR _next "${_nl} + 1")
            string(SUBSTRING "${_env_dump}" ${_next} -1 _env_dump)
        endif()
        if(_line MATCHES "^([A-Za-z_][A-Za-z0-9_()]*)=(.*)$")
            set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
            math(EXPR _imported "${_imported} + 1")
        endif()
    endwhile()
    cui_info("vcvars: 已导入 ${_imported} 个环境变量")

    unset(_cl CACHE)
    unset(_ninja CACHE)
    unset(_cl)
    unset(_ninja)
    find_program(_cl cl)
    find_program(_ninja ninja)
    if(NOT _cl)
        cui_die("导入 vcvars 之后仍找不到 cl.exe。")
    endif()
    if(NOT _ninja)
        cui_die("导入 vcvars 之后仍找不到 ninja（VS 自带于 Common7/IDE/...）。")
    endif()
endfunction()

# ── 预设 ────────────────────────────────────────────────────────────────────
function(cui_find_preset mode name out_var)
    cui_capture(_list cmake --list-presets=${mode})
    if(_list MATCHES "\"${name}\"")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(cui_configure preset)
    cui_find_preset(configure "${preset}" _ok)
    if(NOT _ok)
        cui_die("预设 ${preset} 不在 CMakePresets.json 里（或不在当前主机可见）。")
    endif()
    cui_step("配置 ${preset}")
    cui_run_checked("配置" cmake --preset "${preset}" ${CUI_EXTRA_CONFIGURE_ARGS})
endfunction()

function(cui_build preset)
    set(_extra)
    if(DEFINED CUI_TARGET AND NOT CUI_TARGET STREQUAL "")
        list(APPEND _extra --target "${CUI_TARGET}")
    endif()
    if(DEFINED CUI_JOBS AND NOT CUI_JOBS STREQUAL "")
        list(APPEND _extra --parallel "${CUI_JOBS}")
    endif()
    if(DEFINED CUI_TARGET AND NOT CUI_TARGET STREQUAL "")
        cui_step("构建 ${preset}（只建 ${CUI_TARGET}）")
    else()
        cui_step("构建 ${preset}")
    endif()
    cui_run_checked("构建" cmake --build --preset "${preset}" ${_extra})
endfunction()

function(cui_test preset)
    if(CUI_SKIP_TESTS)
        cui_info("--no-test：跳过 ctest")
        return()
    endif()
    cui_find_preset(test "${preset}" _ok)
    if(NOT _ok)
        cui_info("${preset} 没有 test 预设（交叉编译平台通常不注册测试），跳过 ctest")
        return()
    endif()
    cui_step("ctest --preset ${preset}")
    cui_run_checked("测试" ctest --preset "${preset}")
endfunction()

function(cui_install preset)
    if(CUI_INSTALL_PREFIX STREQUAL "")
        return()
    endif()
    cui_build_dir("${preset}" _dir)
    cui_step("cmake --install → ${CUI_INSTALL_PREFIX}")
    cui_run_checked("安装" cmake --install "${_dir}" --prefix "${CUI_INSTALL_PREFIX}")
endfunction()

# ── UNC → 盘符 ──────────────────────────────────────────────────────────────
# MSVC 的编译/链接规则由 cmd.exe 以 `cd . && cl/lib ...` 执行，而 cmd 不支持
# UNC 工作目录（会退回 C:\Windows，产物写不进去，LNK1104）。如果源码树在
# UNC 共享上、而这个共享有映射盘（如 \\vmware-host\Shared Folders → Z:），
# 就把根换成盘符路径，让 Ninja 生成的绝对路径对 cmd 可用。
function(cui_unc_to_drive out_var path)
    set(${out_var} "${path}" PARENT_SCOPE)
    if(NOT CMAKE_HOST_WIN32 OR NOT path MATCHES "^//")
        return()
    endif()
    string(REGEX MATCH "^//([^/]+)/([^/]+)" _m "${path}")
    set(_share "\\\\${CMAKE_MATCH_1}\\${CMAKE_MATCH_2}")
    string(TOLOWER "${_share}" _share_lc)

    execute_process(
        COMMAND cmd /c net use
        OUTPUT_VARIABLE _dump
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR _dump STREQUAL "")
        return()
    endif()

    string(REPLACE "\r\n" "\n" _dump "${_dump}")
    string(REPLACE "\r" "\n" _dump "${_dump}")
    while(NOT _dump STREQUAL "")
        string(FIND "${_dump}" "\n" _nl)
        if(_nl EQUAL -1)
            set(_line "${_dump}")
            set(_dump "")
        else()
            string(SUBSTRING "${_dump}" 0 ${_nl} _line)
            math(EXPR _next "${_nl} + 1")
            string(SUBSTRING "${_dump}" ${_next} -1 _dump)
        endif()
        string(TOLOWER "${_line}" _line_lc)
        string(FIND "${_line_lc}" "${_share_lc}" _hit)
        if(_hit GREATER -1 AND _line MATCHES "([A-Za-z]):")
            set(_letter "${CMAKE_MATCH_1}")
            string(TOUPPER "${_letter}" _letter)
            string(REGEX REPLACE "^//[^/]+/[^/]+" "" _rest "${path}")
            set(${out_var} "${_letter}:${_rest}" PARENT_SCOPE)
            return()
        endif()
    endwhile()
endfunction()

# ── 工具链前置条件 ──────────────────────────────────────────────────────────
function(cui_exe_suffix out_var)
    if(CMAKE_HOST_WIN32)
        set(${out_var} ".exe" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

# 根 CMakeLists 自己解析 toolchain 路径（scripts/ 与 cmake/ 两种 vcpkg 布局都认），
# 这里只负责把 VCPKG_ROOT 补齐：先认现成的环境，再探测几个常见安装位置。
function(cui_require_vcpkg)
    set(_rel_paths "scripts/buildsystems/vcpkg.cmake" "cmake/buildsystems/vcpkg.cmake")
    foreach(_candidate IN ITEMS
            "$ENV{VCPKG_ROOT}"
            "C:/vcpkg"
            "C:/dev/vcpkg"
            "D:/vcpkg"
            "E:/dev-cache/vcpkg"
            "/tools/dev-cache/vcpkg"
            "$ENV{HOME}/vcpkg")
        if(_candidate STREQUAL "")
            continue()
        endif()
        foreach(_rel IN LISTS _rel_paths)
            if(EXISTS "${_candidate}/${_rel}")
                set(ENV{VCPKG_ROOT} "${_candidate}")
                cui_info("vcpkg:  ${_candidate}")
                return()
            endif()
        endforeach()
    endforeach()
    cui_die(
        "找不到 vcpkg（需要 VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake）。\n"
        "  git clone https://github.com/microsoft/vcpkg\n"
        "  ./vcpkg/bootstrap-vcpkg\n"
        "  然后 set VCPKG_ROOT=<vcpkg 目录> 再重试。")
endfunction()

# ── Emscripten（web）────────────────────────────────────────────────────────
# 预设把 VCPKG_CHAINLOAD_TOOLCHAIN_FILE 写死成
#   $env{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
# 所以 EMSDK 必须指到「emsdk 根」。来源有三种：已激活的环境、常见安装位置、
# 发行版 emscripten 包（没有 upstream/ 布局时在构建目录里补符号链接）。
function(cui_emsdk_layout_ok root out_var)
    if(NOT root STREQUAL ""
       AND EXISTS "${root}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# 写 EMSDK / EM_CONFIG 并把 emsdk 的入口目录拼到 PATH 前面（等价于
# source emsdk_env.sh 里与本仓库相关的那几行）。
function(cui_use_emsdk root)
    set(ENV{EMSDK} "${root}")
    if(EXISTS "${root}/.emscripten")
        set(ENV{EM_CONFIG} "${root}/.emscripten")
    endif()
    if(CMAKE_HOST_WIN32)
        set(_sep ";")
        set(_node_patterns "${root}/node/*")
    else()
        set(_sep ":")
        set(_node_patterns "${root}/node/*/bin")
    endif()
    set(_prefix "${root}/upstream/emscripten")
    file(GLOB _nodes ${_node_patterns})
    foreach(_n IN LISTS _nodes)
        if(IS_DIRECTORY "${_n}")
            set(_prefix "${_prefix}${_sep}${_n}")
        endif()
    endforeach()
    set(ENV{PATH} "${_prefix}${_sep}$ENV{PATH}")
endfunction()

# --install-emsdk：clone 到 <home>/emsdk（已有则复用）再 install/activate
# latest。需要 git 与 Python 3，安装包从网上拉，首次要几分钟。
function(cui_install_emsdk home out_var)
    find_program(_git git)
    if(NOT _git)
        cui_die("--install-emsdk 需要 git；也可以手工 clone 到 ${home}/emsdk 后重试。")
    endif()
    find_program(_python NAMES python3 python)
    if(NOT _python)
        cui_die("--install-emsdk 需要 Python 3（emsdk 的安装脚本依赖它）。")
    endif()

    set(_dir "${home}/emsdk")
    if(EXISTS "${_dir}/emsdk" OR EXISTS "${_dir}/emsdk.bat")
        cui_info("复用已有 emsdk: ${_dir}")
    else()
        cui_step("安装 emsdk → ${_dir}")
        cui_info("git clone --depth 1 https://github.com/emscripten-core/emsdk.git")
        file(MAKE_DIRECTORY "${home}")
        cui_run_checked("git clone" "${_git}" clone --depth 1
            https://github.com/emscripten-core/emsdk.git "${_dir}")
    endif()

    cui_step("emsdk install latest（首次下载工具链，可能较久）")
    if(CMAKE_HOST_WIN32)
        file(TO_NATIVE_PATH "${_dir}" _dir_native)
        set(_shim "${_dir}/cui_emsdk_setup.bat")
        # call 与后续命令必须在同一个 cmd 进程里，且路径带空格时只能靠一次性
        # 批处理避免 cmd 的引号嵌套问题（同 vcvars 的处理）。
        file(WRITE "${_shim}"
            "@echo off\r\n"
            "call \"${_dir_native}\\emsdk.bat\" install latest\r\n"
            "if errorlevel 1 exit /b 1\r\n"
            "call \"${_dir_native}\\emsdk.bat\" activate latest\r\n"
            "if errorlevel 1 exit /b 2\r\n")
        execute_process(
            COMMAND cmd /c "${_shim}"
            WORKING_DIRECTORY "${_dir}"
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err
            RESULT_VARIABLE _rc)
        file(REMOVE "${_shim}")
    else()
        execute_process(
            COMMAND "${_dir}/emsdk" install latest
            WORKING_DIRECTORY "${_dir}"
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err
            RESULT_VARIABLE _rc)
    endif()
    if(NOT _out STREQUAL "")
        message("${_out}")
    endif()
    if(NOT _err STREQUAL "")
        message("${_err}")
    endif()
    if(NOT _rc EQUAL 0)
        cui_die("emsdk install/activate latest 失败（退出码 ${_rc}）。")
    endif()

    cui_emsdk_layout_ok("${_dir}" _ok)
    if(NOT _ok)
        cui_die("emsdk 安装完成但 ${_dir}/upstream/emscripten 仍不存在，请检查上面的输出。")
    endif()
    set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()

function(cui_require_emsdk)
    cui_emsdk_layout_ok("$ENV{EMSDK}" _ok)
    if(_ok)
        cui_use_emsdk("$ENV{EMSDK}")
        cui_info("EMSDK:  $ENV{EMSDK}")
        return()
    endif()
    if(NOT "$ENV{EMSDK}" STREQUAL "")
        cui_warn("EMSDK=$ENV{EMSDK} 下没有 upstream/emscripten，继续探测其它位置。")
    endif()

    set(_home "$ENV{HOME}")
    if(_home STREQUAL "")
        set(_home "$ENV{USERPROFILE}")
    endif()
    if(CMAKE_HOST_WIN32 AND NOT _home STREQUAL "")
        file(TO_CMAKE_PATH "${_home}" _home)
    endif()

    set(_candidates)
    if(NOT _home STREQUAL "")
        list(APPEND _candidates "${_home}/emsdk" "${_home}/.emsdk")
        file(GLOB _more "${_home}/*/emsdk" "${_home}/*/emsdk.git")
        list(APPEND _candidates ${_more})
    endif()
    list(APPEND _candidates
        "/opt/emsdk"
        "/usr/local/emsdk"
        "/usr/lib/emsdk"
        "C:/emsdk"
        "D:/emsdk"
        "E:/dev-cache/emsdk")
    foreach(_c IN LISTS _candidates)
        cui_emsdk_layout_ok("${_c}" _ok)
        if(_ok)
            cui_use_emsdk("${_c}")
            cui_info("EMSDK:  ${_c}（由探测得到，已写入环境）")
            return()
        endif()
    endforeach()

    # 发行版包（apt install emscripten）没有 upstream/ 布局：问 emcc 自己的
    # 资源目录，再按 emsdk 布局把根补齐（Unix 用符号链接）。
    find_program(_emcc NAMES emcc emcc.bat)
    if(_emcc)
        cui_capture(_res "${_emcc}" --print-resource-dir)
        string(STRIP "${_res}" _res)
        if(NOT _res STREQUAL ""
           AND EXISTS "${_res}/cmake/Modules/Platform/Emscripten.cmake")
            get_filename_component(_real "${_res}" ABSOLUTE)
            get_filename_component(_upstream "${_real}" DIRECTORY)
            get_filename_component(_root_candidate "${_upstream}" DIRECTORY)
            cui_emsdk_layout_ok("${_root_candidate}" _ok)
            if(_ok)
                cui_use_emsdk("${_root_candidate}")
                cui_info("EMSDK:  ${_root_candidate}（由 emcc --print-resource-dir 反推）")
                return()
            endif()
            if(CMAKE_HOST_UNIX)
                set(_shim_root "${CUI_ROOT}/build-wasm-${CUI_PROFILE}/.emsdk")
                file(MAKE_DIRECTORY "${_shim_root}/upstream")
                set(_link "${_shim_root}/upstream/emscripten")
                file(REMOVE "${_link}")
                file(CREATE_LINK "${_real}" "${_link}" SYMBOLIC RESULT _link_rc)
                cui_emsdk_layout_ok("${_shim_root}" _ok)
                if(_ok)
                    cui_use_emsdk("${_shim_root}")
                    cui_info("EMSDK:  ${_shim_root} → ${_real}（发行版 emscripten）")
                    return()
                endif()
                cui_warn("无法在 ${_shim_root} 建立符号链接（${_link_rc}）。")
            endif()
        endif()
    endif()

    if(CUI_INSTALL_EMSDK)
        if(_home STREQUAL "")
            set(_home "$ENV{TEMP}")
        endif()
        if(_home STREQUAL "")
            cui_die("--install-emsdk 无法确定安装位置（HOME / USERPROFILE / TEMP 都为空）。")
        endif()
        cui_install_emsdk("${_home}" _emsdk_root)
        cui_use_emsdk("${_emsdk_root}")
        cui_info("EMSDK:  ${_emsdk_root}（本次已安装）")
        return()
    endif()

    cui_die(
        "找不到 Emscripten（EMSDK 未设置或布局不符）。\n"
        "  自动安装：bash build.sh web --install-emsdk"
        "（Windows 用 build.cmd web --install-emsdk）\n"
        "  手工安装：\n"
        "    git clone https://github.com/emscripten-core/emsdk ~/emsdk\n"
        "    cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest\n"
        "    source ./emsdk_env.sh\n"
        "  也支持发行版包：sudo apt install emscripten（脚本会用 emcc 反推布局）。")
endfunction()

# Android 预设的 VCPKG_CHAINLOAD_TOOLCHAIN_FILE 写的是 $env{ANDROID_NDK_HOME}。
function(cui_require_android_ndk)
    if(NOT "$ENV{ANDROID_NDK_HOME}" STREQUAL ""
       AND EXISTS "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
        cui_info("NDK:    $ENV{ANDROID_NDK_HOME}")
        return()
    endif()
    set(_sdk "$ENV{ANDROID_HOME}")
    if(_sdk STREQUAL "")
        set(_sdk "$ENV{ANDROID_SDK_ROOT}")
    endif()
    if(_sdk STREQUAL "")
        cui_die(
            "找不到 Android NDK：ANDROID_HOME / ANDROID_SDK_ROOT / ANDROID_NDK_HOME 都没设置。\n"
            "  装好 SDK 后用 sdkmanager 安装 NDK：\n"
            "  sdkmanager --install \"ndk;<版本>\"\n"
            "  set ANDROID_NDK_HOME=%ANDROID_HOME%\\ndk\\<版本>")
    endif()
    file(GLOB _ndks "${_sdk}/ndk/*")
    list(SORT _ndks COMPARE NATURAL ORDER DESCENDING)
    if(_ndks)
        list(GET _ndks 0 _ndk)
        set(ENV{ANDROID_NDK_HOME} "${_ndk}")
        cui_info("NDK:    ${_ndk}（由 ${_sdk}/ndk 探测得到）")
        return()
    endif()
    cui_die(
        "Android SDK（${_sdk}）下没有 ndk/ 目录。\n"
        "  sdkmanager --install \"ndk;<版本>\"  然后再重试。")
endfunction()

# ── 无预设的手工配置（OHOS 没有对应预设）────────────────────────────────────
function(cui_configure_dir dir)
    cui_step("配置 → ${dir}")
    cui_run_checked("配置" cmake -S "${CUI_ROOT}" -B "${dir}" ${ARGN}
        ${CUI_EXTRA_CONFIGURE_ARGS})
endfunction()

function(cui_build_dir_only dir)
    set(_extra)
    if(DEFINED CUI_TARGET AND NOT CUI_TARGET STREQUAL "")
        list(APPEND _extra --target "${CUI_TARGET}")
    endif()
    if(DEFINED CUI_JOBS AND NOT CUI_JOBS STREQUAL "")
        list(APPEND _extra --parallel "${CUI_JOBS}")
    endif()
    cui_step("构建 → ${dir}")
    cui_run_checked("构建" cmake --build "${dir}" ${_extra})
endfunction()

# ── 产物报告 ────────────────────────────────────────────────────────────────
function(cui_report_libs dir)
    cui_step("产物")
    cui_info("构建目录: ${dir}")
    # 引擎拆成 cui_core / cui_wx 两个库，控件包各自成库。
    # 本仓产物统一落在 dist/（默认源码根，即构建目录的上一级）；
    # 旧的构建树路径只作为 -DCUI_DIST_DIR 指向别处时的兼容兜底。
    file(GLOB _libs
        "${dir}/../dist/libcui_core.*" "${dir}/../dist/libcui_wx.*"
        "${dir}/../dist/cui_core.*" "${dir}/../dist/cui_wx.*"
        "${dir}/../dist/libcui_*.*" "${dir}/../dist/cui_*.*"
        "${dir}/../dist/libskia.so*" "${dir}/../dist/libav*.so*"
        "${dir}/../dist/libsw*.so*"
        "${dir}/cui_core/libcui_core.*" "${dir}/cui_wx/libcui_wx.*"
        "${dir}/cui_core/cui_core.*" "${dir}/cui_wx/cui_wx.*"
        "${dir}/packages/*/libcui_*.*" "${dir}/packages/*/cui_*.*")
    list(SORT _libs)
    if(_libs)
        foreach(_f IN LISTS _libs)
            file(SIZE "${_f}" _size)
            math(EXPR _kb "${_size} / 1024")
            file(RELATIVE_PATH _rel "${dir}" "${_f}")
            cui_info("${_rel}  (${_kb} KB)")
        endforeach()
    else()
        cui_info("（${dir} 下还没有库产物）")
    endif()
endfunction()

function(cui_report_file dir rel label)
    if(EXISTS "${dir}/${rel}")
        file(SIZE "${dir}/${rel}" _size)
        math(EXPR _kb "${_size} / 1024")
        cui_info("${rel}  (${_kb} KB)   ${label}")
    endif()
endfunction()

# 统一的收尾提示。
function(cui_footer dir)
    cui_info("")
    cui_info("后续可直接用 CMake 预设复现：cmake --preset <preset> && cmake --build --preset <preset>")
    cui_info("构建目录: ${dir}")
endfunction()

# ── 打包 ──────────────────────────────────────────────────────────────────
# 把 dist 目录打包成 zip（内含全部 .so / exe 等运行时产物，顶层为 dist/ 文件夹）。
# 优先用 zip 命令（Linux / macOS 自带），缺失时退到 python3 的 zipfile 模块，
# 这样不依赖某个特定的压缩工具。
function(cui_package_zip dist_dir platform profile zip_name)
    if(NOT EXISTS "${dist_dir}")
        cui_warn("dist 目录不存在，跳过打包：${dist_dir}")
        return()
    endif()
    if(zip_name STREQUAL "")
        set(zip_name "cui_core-${platform}-${profile}.zip")
    endif()
    if(NOT IS_ABSOLUTE "${zip_name}")
        set(zip_name "${CUI_ROOT}/${zip_name}")
    endif()
    get_filename_component(_dist_parent "${dist_dir}" DIRECTORY)
    get_filename_component(_dist_name "${dist_dir}" NAME)
    cui_step("打包 zip → ${zip_name}")

    find_program(_zip_prog NAMES zip)
    if(_zip_prog)
        execute_process(
            COMMAND "${_zip_prog}" -r -q "${zip_name}" "${_dist_name}"
            WORKING_DIRECTORY "${_dist_parent}"
            RESULT_VARIABLE _rc)
    else()
        find_program(_python NAMES python3 python)
        if(NOT _python)
            cui_die("找不到 zip 命令也没有 python3，无法打包（请安装 zip）。")
        endif()
        set(_py [=[
import os, sys, zipfile
zp, root = sys.argv[1], sys.argv[2]
parent = os.path.dirname(root)
with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
    for dp, _, fns in os.walk(root):
        for fn in fns:
            fp = os.path.join(dp, fn)
            z.write(fp, os.path.relpath(fp, parent))
]=])
        execute_process(
            COMMAND "${_python}" -c "${_py}" "${zip_name}" "${dist_dir}"
            RESULT_VARIABLE _rc)
    endif()

    if(NOT _rc EQUAL 0)
        cui_die("zip 打包失败（退出码 ${_rc}）。")
    endif()
    file(SIZE "${zip_name}" _size)
    math(EXPR _kb "${_size} / 1024")
    cui_info("${zip_name}  (${_kb} KB)")
endfunction()
