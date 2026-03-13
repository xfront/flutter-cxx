cmake_minimum_required(VERSION 3.20)

# ── Rendering backend ───────────────────────────────────────────────────────
option(USE_SKIA "Use Skia rendering backend (requires vcpkg)" ON)

# ── vcpkg integration ───────────────────────────────────────────────────────
# 第三方端口（skia / ffmpeg）一律由 vcpkg 的 manifest 模式装，根目录取全局环境变量
# VCPKG_ROOT —— CMakePresets 里的 toolchainFile 与 VCPKG_INSTALLED_DIR 用的也是
# 这一个，两处必须同源，否则配置期找到的库和实际装机的是两棵树。
# CMAKE_TOOLCHAIN_FILE must be set before the first project() call, otherwise
# it only takes effect from the next configure run. Keep this block above project().
if(USE_SKIA AND NOT CMAKE_TOOLCHAIN_FILE)
    set(_vcpkg_root "$ENV{VCPKG_ROOT}")
    # 两种布局都认：官方 checkout 是 scripts/buildsystems/vcpkg.cmake，部分自建
    # 镜像把它放在 cmake/buildsystems/ 下。预设不再写死 toolchainFile，由这里定。
    set(_vcpkg_toolchain "")
    foreach(_rel IN ITEMS "scripts/buildsystems/vcpkg.cmake" "cmake/buildsystems/vcpkg.cmake")
        if(NOT _vcpkg_root STREQUAL "" AND EXISTS "${_vcpkg_root}/${_rel}")
            set(_vcpkg_toolchain "${_vcpkg_root}/${_rel}")
            break()
        endif()
    endforeach()
    if(_vcpkg_toolchain STREQUAL "")
        message(FATAL_ERROR
            "USE_SKIA 需要 vcpkg，但 VCPKG_ROOT 没设置或它指向的地方没有 "
            "scripts/buildsystems/vcpkg.cmake（当前='${_vcpkg_root}'）。"
            "请 export VCPKG_ROOT=<vcpkg 根目录>，或直接给 "
            "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_toolchain}"
        CACHE STRING "vcpkg toolchain file")
    # Install dependencies to VCPKG_ROOT/installed (shared across all build directories)
    if(NOT DEFINED VCPKG_INSTALLED_DIR)
        set(VCPKG_INSTALLED_DIR "${_vcpkg_root}/installed" CACHE STRING "")
    endif()
    message(STATUS "vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
    message(STATUS "vcpkg installed: ${VCPKG_INSTALLED_DIR}")
    unset(_vcpkg_root)
    unset(_vcpkg_toolchain)
endif()

# Fallback: when the toolchain was injected externally (e.g. via preset or
# command line) VCPKG_INSTALLED_DIR may still be unset — derive it from
# VCPKG_ROOT so the paths below resolve correctly.
if(USE_SKIA AND NOT DEFINED VCPKG_INSTALLED_DIR)
    if(NOT "$ENV{VCPKG_ROOT}" STREQUAL "" AND EXISTS "$ENV{VCPKG_ROOT}/installed")
        set(VCPKG_INSTALLED_DIR "$ENV{VCPKG_ROOT}/installed" CACHE STRING "")
    else()
        set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed" CACHE STRING "")
    endif()
endif()

# ── FFmpeg 从哪来：全平台固定用 vcpkg 那一份 ────────────────────────────────
# 不再用系统 libav*：发行版的版本随环境飘（编进来一份、运行期找到另一份就得降级），
# 而这条路线要在 Linux/macOS/Windows/Android/iOS 上是同一个行为。vcpkg 的 ffmpeg
# port 自带 FindFFMPEG + wrapper，天然按 `VCPKG_TARGET_TRIPLET` 隔离，交叉编译也就
# 碰不到 pkg-config 解析宿主机树那个坑。要改用自己备的那份（OHOS 等）：
# -DCUI_VIDEO_FFMPEG_VCPKG=OFF + CMAKE_PREFIX_PATH。
#
# vcpkg 的 manifest install 在 project()（= include toolchain）里就跑完了，所以
# 「要不要把 ffmpeg 这个 feature 打开」必须在那之前定。判据只能用 triplet 与
# 两个路线开关（此时还拿不到 CUI_PLATFORM）；triplet 由 preset 给，
# 直接调 cmake 时按宿主机推一个。
option(CUI_VIDEO_FFMPEG "解码路线二：FFmpeg 后端（全平台用 vcpkg 的那一份）" ON)
option(CUI_VIDEO_FFMPEG_VCPKG "FFmpeg 由 vcpkg 自动下载并编译（OFF 才退回系统 libav*/CMAKE_PREFIX_PATH）" ON)

if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
    if(CMAKE_HOST_WIN32)
        set(_cui_triplet "x64-windows")
    else()
        set(_cui_triplet "x64-linux")
    endif()
else()
    set(_cui_triplet "${VCPKG_TARGET_TRIPLET}")
endif()

# 本次配置是否让 vcpkg 带上 ffmpeg feature（packages/video 据此决定怎么解释
# 「找不到 FFmpeg」）。每次配置重算。
set(CUI_FFMPEG_VCPKG_ENABLED FALSE CACHE BOOL "本次配置是否打开 vcpkg 的 ffmpeg feature" FORCE)
if(CUI_VIDEO_FFMPEG AND CUI_VIDEO_FFMPEG_VCPKG)
    if(_cui_triplet MATCHES "-emscripten$")
        # 浏览器：HTMLVideoElement 划算得多，而且 vcpkg 的 ffmpeg port 不支持 emscripten
    elseif(_cui_triplet MATCHES "-ohos$")
        # vcpkg 这边**是**支持的（triplets/community/arm64-ohos.cmake，ffmpeg port 也
        # 有 VCPKG_TARGET_IS_OHOS 分支），所以不配这一条只是不想让「配置一次鸿蒙」
        # 顺手触发几小时的交叉编译 —— 要 FFmpeg 请显式 -DCUI_VIDEO_FFMPEG_VCPKG=ON。
        # skia 这一端已经补齐：仓库 overlay port 的 016-ohos.patch + target_os="ohos"
        # 分支（cmake/vcpkg-overlay-ports/skia），由 cmake/platforms/ohos.cmake 接上。
    else()
        set(CUI_FFMPEG_VCPKG_ENABLED TRUE CACHE BOOL "" FORCE)
    endif()
endif()

if(CUI_FFMPEG_VCPKG_ENABLED AND NOT "ffmpeg" IN_LIST VCPKG_MANIFEST_FEATURES)
    list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
    set(VCPKG_MANIFEST_FEATURES "${VCPKG_MANIFEST_FEATURES}" CACHE STRING "" FORCE)
    message(STATUS "vcpkg: 自动带上 ffmpeg feature（${_cui_triplet}；已装则几秒过，没装就开始下载现编）")
endif()
unset(_cui_triplet)

# ── MSVC toolchain auto-detection (Ninja generator) ─────────────────────
project(cui_core VERSION 0.1.0 LANGUAGES CXX)

# The Apple window backends are Objective-C++ (.mm). Enabling the language
# globally on Windows/Linux would make CMake look for an ObjC++ compiler that
# does not exist, so it is opt-in per host.
if(APPLE)
    enable_language(OBJCXX)
endif()

# ── Build options ───────────────────────────────────────────────────────────
option(CUI_BUILD_EXAMPLES "Build the example applications (demo_gallery, dock-based)" ON)
option(CUI_BUILD_IDE "Build the visual designer (vs2022)" ON)
option(CUI_BUILD_TESTS "Build and register the unit tests" ON)
option(CUI_ENABLE_INSTALL "Generate install rules and CMake/pkg-config package files" ON)
option(CUI_ENABLE_PIC "Compile the static libraries as position-independent code" ON)
option(CUI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(CUI_SANITIZE "" CACHE STRING
    "Comma-separated sanitizers (address, undefined, thread); empty disables")
set_property(CACHE CUI_SANITIZE PROPERTY STRINGS "" address undefined thread "address,undefined")
mark_as_advanced(CUI_SANITIZE)

if(CUI_ENABLE_PIC)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ── Library flavour (static / shared) ───────────────────────────────────────
# flutter 层的两个引擎库（cui_core / cui_wx）与 packages/* 的四个控件包共用
# 同一个开关：
#   STATIC（默认）⇒ libcui_core.a + libcui_wx.a + 四个包的 .a，
#                   最终跑出单文件可执行程序；
#   SHARED      ⇒ 六个 .so / .dll（引擎 2 + 控件包 4），gallery 等消费方
#                 运行时动态链接。
# 两种情况下 target 名、头文件与链接写法完全一致，切换只需
#   cmake -B <dir> -S . -DCUI_LIBRARY_TYPE=SHARED
set(CUI_LIBRARY_TYPE "STATIC" CACHE STRING
    "Link the cui_core cui_wx/packages libraries as STATIC or SHARED")
set_property(CACHE CUI_LIBRARY_TYPE PROPERTY STRINGS STATIC SHARED)
string(TOUPPER "${CUI_LIBRARY_TYPE}" CUI_LIBRARY_TYPE)
if(NOT CUI_LIBRARY_TYPE MATCHES "^(STATIC|SHARED)$")
    message(FATAL_ERROR
        "CUI_LIBRARY_TYPE must be STATIC or SHARED (got '${CUI_LIBRARY_TYPE}')")
endif()
message(STATUS "Library type: ${CUI_LIBRARY_TYPE}")

if(CUI_LIBRARY_TYPE STREQUAL "SHARED")
    # Windows 没有源码级的 __declspec(dllexport)：让 CMake 从目标文件里导出
    # 全部外部符号（类/函数/变量），否则 DLL 导出表为空、消费方链接期报
    # "unresolved external symbol"。
    if(MSVC)
        set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
    # 安装后的相对布局：exe 在 bin/，库在 lib/。让两边的 rpath 同时覆盖
    # 自身目录与 ../lib，装机后无需 LD_LIBRARY_PATH。构建树里的运行靠 CMake
    # 自动生成的 build rpath（指向各模块的构建目录）。
    if(APPLE)
        set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/../lib")
    elseif(UNIX)
        set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
    endif()
endif()

# 引擎库与控件包统一走这里建库：库类型跟 CUI_LIBRARY_TYPE 走，SHARED 时
# 补上版本号（libcui_core.so -> .so.0 -> .so.0.1.0），静态库忽略这些属性。
function(cui_add_library _name)
    add_library(${_name} ${CUI_LIBRARY_TYPE} ${ARGN})
    if(CUI_LIBRARY_TYPE STREQUAL "SHARED")
        set_target_properties(${_name} PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
    endif()
endfunction()

set(CUI_PACKAGE_DESCRIPTION
    "Cross-platform C++20 declarative UI framework on Skia")

include(GNUInstallDirs)

# ── Build-time MSVC/SDK wiring (build needs no vcvars environment) ─────
# Configure-time detection set PATH/LIB/INCLUDE only for THIS process; the
# build phase (ninja invoking cl.exe / link.exe directly) inherits whatever
# environment the IDE/CI has. Make the build self-contained: the compiler
# and SDK tools were cached by absolute path during detection, link.exe
# resolves kernel32.lib & co. through /LIBPATH below, and cl.exe resolves
# the standard headers through the SYSTEM includes wired on the cui_core
# target in the Windows block further down.
if(CUI_MSVC_LIB)
    link_directories("${CUI_MSVC_LIB}" "${CUI_SDK_UM_LIB}" "${CUI_SDK_UCRT_LIB}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Triplet fallback for non-preset builds ────────────────────────────────────
# When invoked via CMakePresets, VCPKG_TARGET_TRIPLET is set by the preset.
# For direct cmake invocations (CLion legacy profiles, CI), derive it here.
if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
    if(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows")
    else()
        set(VCPKG_TARGET_TRIPLET "x64-linux")
    endif()
endif()

# ── dist/：编译产物与第三方运行时库统一落地 ───────────────────────────────
# 本仓的全部产物（引擎/控件包库、示例、IDE、测试可执行文件）直接输出到 dist/，
# 再由 cui_dist 把 vcpkg 的第三方动态库（libskia.so、ffmpeg 的 libav*.so 及其
# 依赖）拷进去 —— dist/ 可整体拷走运行，不必设 LD_LIBRARY_PATH。
# 想按构建配置分仓（debug/release、static/shared 不互相覆盖）：
#   cmake -B <dir> -S . -DCUI_DIST_DIR=<dir>/dist
set(CUI_DIST_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH
    "Directory collecting all build artifacts and third-party runtime libraries")
file(MAKE_DIRECTORY "${CUI_DIST_DIR}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CUI_DIST_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CUI_DIST_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CUI_DIST_DIR}")
# 多配置生成器（VS/Xcode）默认会追加 /Debug、/Release 子目录，逐个钉死。
foreach(_cui_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cui_cfg} "${CUI_DIST_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cui_cfg} "${CUI_DIST_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cui_cfg} "${CUI_DIST_DIR}")
endforeach()

# exe 与 .so 同目录：让构建树产物也能靠 $ORIGIN / @loader_path 找到彼此，
# 把 dist/ 拷到别的机器同样生效（CMake 自动生成的 build rpath 仍然保留）。
if(APPLE)
    list(APPEND CMAKE_BUILD_RPATH "@loader_path" "@loader_path/lib")
elseif(UNIX)
    list(APPEND CMAKE_BUILD_RPATH "$ORIGIN" "$ORIGIN/lib")
endif()

# 每次构建刷新 vcpkg 运行时库；静态 triplet 下没有 .so，脚本自然空跑。
# 本仓产物由上面三个 OUTPUT_DIRECTORY 直接写进 dist/，故无需 add_dependencies。
add_custom_target(cui_dist ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DCUI_DIST_DIR=${CUI_DIST_DIR}"
        "-DCUI_VCPKG_ROOT=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        -P "${CMAKE_SOURCE_DIR}/cmake/cui_dist_runtime.cmake"
    COMMENT "Collect vcpkg runtime libraries (skia/ffmpeg/...) into dist/lib/"
    VERBATIM)

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    # /bigobj：gallery / ide 这类「一个 TU 包下整套 header-only 控件」的文件，
    # COFF 默认 65536 个 section 装不下（模板每个 make_shared 参数包都单独成
    # COMDAT），会在链接前直接 fatal error C1128。
    # /utf-8：源文件全是无 BOM 的 UTF-8（Linux 上写的），不给这个开关 cl.exe 就按
    # 系统 ANSI 代码页（本机 936/GBK）解，多字节序列的尾字节会吃掉紧跟其后的 `/`
    # 或 `"` —— 表现是三斜杠注释的续行被打断、注释尾巴掉进代码里（audio_sink.h 的
    # 中文注释块在 tests/test_audio_ring 上直接炸出十几个 C2059/C4430），窄字符串
    # 字面量也一并变乱码。同时它把执行字符集也定成 UTF-8，控制台才拿得到和别的平台
    # 一致的那份字节。
    add_compile_options(/utf-8 /W4 /permissive- /bigobj)
    add_compile_options("$<$<CONFIG:Debug>:/Gd>")
    if(CUI_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    # 保留帧指针：Debug/性能剖析时让 gdb/lldb/perf 能完整展开调用栈。
    add_compile_options("$<$<CONFIG:Debug>:-fno-omit-frame-pointer>")
    if(CUI_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# ── Sanitizers ──────────────────────────────────────────────────────────────
# CUI_SANITIZE replaces the old "grep CMAKE_CXX_FLAGS for -fsanitize=address"
# convention: it instruments every cui_core TU and propagates to link lines, and
# downstream CMakeLists can test CUI_SANITIZE_ADDRESS instead of re-parsing
# flags. A manually configured -fsanitize=address (legacy build-asan/) is
# still honoured so existing build directories keep working.
if(CUI_SANITIZE)
    if(EMSCRIPTEN)
        message(FATAL_ERROR
            "CUI_SANITIZE is not supported for WebAssembly builds "
            "(use the Emscripten Debug assertions instead)")
    endif()
    string(REPLACE "," ";" _cui_sanitize_list "${CUI_SANITIZE}")
    string(REPLACE ";" "," _cui_sanitize_arg "${_cui_sanitize_list}")
    if(MSVC)
        if(NOT _cui_sanitize_arg STREQUAL "address")
            message(FATAL_ERROR "MSVC supports only CUI_SANITIZE=address")
        endif()
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options("-fsanitize=${_cui_sanitize_arg}" -fno-omit-frame-pointer)
        add_link_options("-fsanitize=${_cui_sanitize_arg}")
    endif()
    message(STATUS "Sanitizers: ${_cui_sanitize_arg}")
endif()

if("address" IN_LIST _cui_sanitize_list OR CMAKE_CXX_FLAGS MATCHES "-fsanitize=address")
    set(CUI_SANITIZE_ADDRESS TRUE)
else()
    set(CUI_SANITIZE_ADDRESS FALSE)
endif()

# ── Platform detection ──────────────────────────────────────────────────────
if(ANDROID)
    set(CUI_PLATFORM "android")
elseif(EMSCRIPTEN)
    set(CUI_PLATFORM "web")
elseif(IOS)
    set(CUI_PLATFORM "ios")
elseif(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    # Must precede the UNIX fallback: OHOS is musl-based and would otherwise
    # be classified as linux.
    set(CUI_PLATFORM "ohos")
elseif(WIN32)
    set(CUI_PLATFORM "windows")
elseif(APPLE)
    set(CUI_PLATFORM "macos")
elseif(UNIX AND NOT APPLE)
    set(CUI_PLATFORM "linux")
endif()

message(STATUS "Platform: ${CUI_PLATFORM}")
message(STATUS "Rendering: Skia=${USE_SKIA}")

# ── Dependencies ────────────────────────────────────────────────────────────
if(USE_SKIA)
    find_package(unofficial-skia CONFIG REQUIRED)

    # vcpkg 的 unofficial-skia 把 webp/freetype/fontconfig 等 codec/font 依赖用
    # $<LINK_ONLY:$<$<NOT:$<CONFIG:DEBUG>>:...>> 包起来——也就是说只有「非 Debug」
    # 才把这些库加进链接。本仓的 *-debug 预设（含 x64-linux-debug-shared）是 Debug，
    # 于是它们全程被丢弃：静态 skia 自带全部对象代码不受影响，但动态 libskia.so
    # 运行期会报 undefined symbol: WebPPictureImportRGBX 之类。
    # 这里在 skia 为动态库时显式补上这批库，让 libcui_core.so 把它们列为 NEEDED，
    # 运行期随 libcui_core.so 一起加载，补足 libskia.so 的未定义符号。
    set(_cui_skia_codec_libs "")
    get_target_property(_cui_skia_loc unofficial::skia::skia IMPORTED_LOCATION_RELEASE)
    if(NOT _cui_skia_loc)
        get_target_property(_cui_skia_loc unofficial::skia::skia IMPORTED_LOCATION)
    endif()
    if(_cui_skia_loc MATCHES "\\.so" AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        foreach(_l IN ITEMS png16 freetype fontconfig expat turbojpeg jpeg
                            webpdecoder webpdemux webpmux webp)
            find_library(_cui_skia_${_l} NAMES ${_l}
                PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
                      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
                NO_DEFAULT_PATH)
            if(_cui_skia_${_l})
                list(APPEND _cui_skia_codec_libs "${_cui_skia_${_l}}")
            endif()
        endforeach()
    endif()

    # 动态 skia 不导出 Graphite/Vulkan 工厂符号（如
    # skgpu::graphite::ContextFactory::MakeVulkan），链接可执行文件时这些符号
    # 无处解析。允许未定义，运行期只有真正走 Vulkan 渲染路径才会用到、一般用例
    # 不触发；否则整个 dynamic 构建连可执行文件都链不出来。
    if(_cui_skia_loc MATCHES "\\.so" AND UNIX AND NOT APPLE)
        if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "allow-shlib-undefined")
            string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--allow-shlib-undefined")
        endif()
    endif()

    # Detect Graphite backend support — platform-aware.
    # skia ships ALL backend headers regardless of which features are compiled,
    # so we gate on the actual platform (matching vcpkg.json) rather than just
    # header existence. Dawn additionally requires webgpu_cpp.h from the dawn
    # package, which is only installed on emscripten.
    get_target_property(_skia_includes unofficial::skia::skia INTERFACE_INCLUDE_DIRECTORIES)
    set(_skia_have_graphite FALSE)
    set(_skia_have_dawn FALSE)
    set(_skia_have_vulkan FALSE)
    set(_skia_have_metal FALSE)
    if(_skia_includes)
        foreach(_inc IN LISTS _skia_includes)
            if(EXISTS "${_inc}/gpu/graphite/Context.h")
                set(_skia_have_graphite TRUE)
            endif()
        endforeach()
    endif()
    # Dawn: only for native WebGPU hosts (Windows D3D12). Browser WebGPU
    # adapter/device requests are asynchronous, so the wasm renderer stays on
    # the raster surface and presents through the canvas instead.
    if(CUI_PLATFORM STREQUAL "web" OR CUI_PLATFORM STREQUAL "ohos")
        # ohos: the overlay port builds skia with default-features:false (no
        # vulkan/graphite) — the ArkTS host presents through the raster surface.
        set(_skia_have_graphite FALSE)
    endif()
    # Vulkan: Windows + Linux. Requires the Vulkan SDK (loader + headers).
    # Android is excluded on purpose: the Graphite/Vulkan path in
    # skia_renderer.cpp is built on an XCB swapchain (initWithXcbWindow), so a
    # Vulkan-enabled define there would pull vkCreateInstance without a
    # matching loader — the APK host presents through raster + ANativeWindow
    # instead (cui_core/src/platform/android).
    if(NOT CUI_PLATFORM STREQUAL "osx" AND
       NOT CUI_PLATFORM STREQUAL "ios" AND
       NOT CUI_PLATFORM STREQUAL "web" AND
       NOT CUI_PLATFORM STREQUAL "android" AND
       _skia_have_graphite)
        # Try to find the Vulkan loader.
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            foreach(_inc IN LISTS _skia_includes)
                if(EXISTS "${_inc}/gpu/graphite/vk/VulkanBackendContext.h" OR
                   EXISTS "${_inc}/gpu/graphite/vk/VulkanGraphiteContext.h")
                    set(_skia_have_vulkan TRUE)
                endif()
            endforeach()
        else()
            message(STATUS "Vulkan SDK not found — Graphite/Vulkan disabled")
        endif()
        # 仅靠头文件 + Vulkan SDK 判定不够：vcpkg 的 dynamic skia
        # （如 x64-linux-dynamic）编译期头齐全，但运行期 libskia.so 并不导出
        # skgpu::graphite::ContextFactory::MakeVulkan，于是 libcui_core.so 带着
        # 未定义符号，加载即 symbol lookup error。真实探测 skia 库是否导出该符号，
        # 没导出就关掉 Vulkan 后端，让渲染器回退到 CPU raster。
        if(_skia_have_vulkan AND _cui_skia_loc)
            set(_cui_skia_has_makevulkan FALSE)
            if(_cui_skia_loc MATCHES "\\.so$")
                execute_process(COMMAND "${CMAKE_NM}" -D --defined-only
                    "${_cui_skia_loc}" OUTPUT_VARIABLE _cui_nm_out ERROR_QUIET)
            elseif(_cui_skia_loc MATCHES "\\.a$")
                execute_process(COMMAND "${CMAKE_NM}" --defined-only
                    "${_cui_skia_loc}" OUTPUT_VARIABLE _cui_nm_out ERROR_QUIET)
            endif()
            # 精确匹配 graphite::ContextFactory::MakeVulkan 的 mangled 片段；
            # 不要用 "MakeVulkan" 子串——动态 skia 导出的
            # GrDirectContexts::MakeVulkan（Ganesh 后端）也会误命中。
            if(_cui_nm_out MATCHES "graphite14ContextFactory10MakeVulkan")
                set(_cui_skia_has_makevulkan TRUE)
            endif()
            if(NOT _cui_skia_has_makevulkan)
                message(STATUS
                    "skia library does not export ContextFactory::MakeVulkan "
                    "— Graphite/Vulkan disabled, falling back to CPU raster")
                set(_skia_have_vulkan FALSE)
            endif()
        endif()
    endif()
    # Metal: only on Apple platforms.
    if((CUI_PLATFORM STREQUAL "osx" OR CUI_PLATFORM STREQUAL "ios") AND
       _skia_have_graphite)
        foreach(_inc IN LISTS _skia_includes)
            if(EXISTS "${_inc}/gpu/graphite/mtl/MtlBackendContext.h" OR
               EXISTS "${_inc}/gpu/graphite/mtl/MtlGraphiteTypes.h")
                set(_skia_have_metal TRUE)
            endif()
        endforeach()
    endif()
    
    # Cache detection results for target_compile_definitions after add_library()
    set(SKIA_HAVE_GRAPHITE ${_skia_have_graphite} CACHE INTERNAL "" FORCE)
    set(SKIA_HAVE_DAWN ${_skia_have_dawn} CACHE INTERNAL "" FORCE)
    set(SKIA_HAVE_VULKAN ${_skia_have_vulkan} CACHE INTERNAL "" FORCE)
    set(SKIA_HAVE_METAL ${_skia_have_metal} CACHE INTERNAL "" FORCE)
endif()

if(CUI_PLATFORM STREQUAL "linux")
    find_package(PkgConfig REQUIRED)
    # In Debug builds vcpkg's toolchain points pkg-config at the debug
    # pkgconfig tree, whose .pc files carry the "d"-suffixed names
    # (-lfreetyped / -lbz2d / -lpng16d). FindPkgConfig fails to resolve those
    # to full archive paths here, and the bare -l names reach the linker,
    # which cannot find them. Point pkg-config at the release tree so the
    # dependency closure resolves against the installed release archives.
    set(_vcpkg_pc_release "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig")
    if(EXISTS "${_vcpkg_pc_release}")
        set(ENV{PKG_CONFIG_PATH} "${_vcpkg_pc_release}")
    endif()
    unset(_vcpkg_pc_release)
    pkg_check_modules(XCB REQUIRED xcb)
    # Font resolution for Painter (Skia fontconfig SkFontMgr).
    pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
    # Optional: Xlib for the XIM input-method bridge (CJK text input).
    # Without it x11_window.cpp compiles the IM block out and keys fall
    # back to raw XCB keysym handling (ASCII only). Xlib ships with the
    # system (libxcb transitively needs it), so locate it directly rather
    # than through pkg-config — the vcpkg pkgconfig tree only carries the
    # -dbare names and the x11.pc is not mirrored there.
    find_library(CUI_X11_LIB NAMES X11)
    find_path(CUI_X11_INC NAMES X11/Xlib.h)
    if(CUI_X11_LIB AND CUI_X11_INC)
        set(CUI_HAVE_X11 TRUE)
        message(STATUS "X11/Xlib found: ${CUI_X11_LIB}")
    else()
        set(CUI_HAVE_X11 FALSE)
        message(STATUS "X11/Xlib not found — CJK input-method bridge disabled")
    endif()
endif()

# ── cui_core modular build ──────────────────────────────────────────────────
# The engine is two libraries: cui_core/CMakeLists.txt compiles
# foundation/platform/rendering/framework into `cui_core` and the
# cui_wx/src/widgets sources into `cui_wx` (which links `cui_core`).
# This directory is the pure Flutter three-layer engine, while package/*
# holds the widget families split out as independent packages.  cui_common
# is an INTERFACE target that carries the shared toolchain + third-party
# usage requirements; `cui_core` is an INTERFACE aggregate over both engine
# libraries so examples/tests keep linking one target.
add_library(cui_common INTERFACE)
# The source-tree include must stay BUILD_INTERFACE-only: install(EXPORT)
# refuses to export a target whose interface includes a source directory, and
# consumers get the installed headers through INSTALL_INTERFACE instead.
target_include_directories(cui_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/cui_core/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/cui_wx/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# WebAssembly is single-threaded unless -pthread is added (which requires
# cross-origin isolation on the host): ThreadPool::submit() runs inline there
# and FindThreads' pthread probes cannot succeed.
if(NOT CUI_PLATFORM STREQUAL "web")
    find_package(Threads REQUIRED)
    target_link_libraries(cui_common INTERFACE Threads::Threads)
endif()

# The two engine libraries (`cui_core` / `cui_wx`) are defined in their own
# subdirectories.
add_subdirectory(cui_core)
add_subdirectory(cui_wx)
# `cui_core` is the engine aggregate: the pure cui_core three-layer stack.  The
# widget families split into packages/* (charts / code_editor / dock / video)
# are separate libraries with their own include roots — consumers that want
# them link the concrete packages target (cui_charts, cui_video, …) so that
# the FFmpeg / Media Foundation dependency stays opt-in.
# Static-link order matters: cui_wx depends on cui_core, so wx comes first.
add_library(cui INTERFACE)
target_link_libraries(cui INTERFACE cui_wx cui_core)

# packages/* = widget families carved out of the engine.  Strictly one-way:
# every package links into cui_wx (and transitively cui_core); the engine
# never includes a package header.
add_subdirectory(packages)

# ── 移动平台 app 开关（只声明，不在这里构建）────────────────────────────────
# libcui_gallery.so / libcui.so 都是**示例 App 的产物**，构建定义统一放在
# examples/CMakeLists.txt（应用层），由它把应用根与平台胶水链成 .so。
# cui_core/src/platform/{android,ohos} 只提供平台后端源码（由 cui_core/CMakeLists.txt
# 编进 cui_core）与 java / Manifest / ArkTS 资产；引擎层不定义也不编译任何 app
# 目标，依赖方向严格保持 app → 引擎。
# 桌面/其它平台默认关掉，避免给普通构建增加无人使用的共享库。
if(CUI_PLATFORM STREQUAL "android")
    option(CUI_BUILD_ANDROID_APP "Build the Android APK entry library" ON)
else()
    option(CUI_BUILD_ANDROID_APP "Build the Android APK entry library" OFF)
endif()
if(CUI_PLATFORM STREQUAL "ohos")
    option(CUI_BUILD_OHOS_APP "Build the OpenHarmony HAP entry library" ON)
else()
    option(CUI_BUILD_OHOS_APP "Build the OpenHarmony HAP entry library" OFF)
endif()

# ── Compile definitions ─────────────────────────────────────────────────────
if(USE_SKIA)
    target_compile_definitions(cui_common INTERFACE CUI_SKIA_BACKEND)
    if(SKIA_HAVE_GRAPHITE)
        target_compile_definitions(cui_common INTERFACE CUI_SKIA_GRAPHITE=1)
        message(STATUS "Skia Graphite: enabled")
        if(SKIA_HAVE_DAWN)
            target_compile_definitions(cui_common INTERFACE CUI_SKIA_GRAPHITE_DAWN=1)
            message(STATUS "Skia Graphite backend: Dawn (D3D12/WebGPU)")
        endif()
        if(SKIA_HAVE_VULKAN)
            target_compile_definitions(cui_common INTERFACE CUI_SKIA_GRAPHITE_VULKAN=1)
            message(STATUS "Skia Graphite backend: Vulkan")
        endif()
        if(SKIA_HAVE_METAL)
            target_compile_definitions(cui_common INTERFACE CUI_SKIA_GRAPHITE_METAL=1)
            message(STATUS "Skia Graphite backend: Metal")
        endif()
    else()
        message(STATUS "Skia Graphite: not available (raster fallback)")
    endif()
endif()

# ── Link libraries ──────────────────────────────────────────────────────────
if(USE_SKIA)
    target_link_libraries(cui_common INTERFACE unofficial::skia::skia)
    # 补上 vcpkg 在 Debug 下丢掉的 skia codec/font 运行时依赖（见上方检测）。
    # 这些库的符号是被 libskia.so 引用、而非 libcui_core.so 自身引用，默认
    # --as-needed 会把它们从 NEEDED 里丢弃；用 --no-as-needed 强制保留，
    # 运行期随 libcui_core.so 一起加载，补足 libskia.so 的未定义符号。
    if(_cui_skia_codec_libs)
        target_link_libraries(cui_common INTERFACE
            -Wl,--no-as-needed ${_cui_skia_codec_libs} -Wl,--as-needed)
    endif()
endif()

if(CUI_PLATFORM STREQUAL "linux")
    target_include_directories(cui_common INTERFACE ${XCB_INCLUDE_DIRS} ${FONTCONFIG_INCLUDE_DIRS})
    if(CUI_HAVE_X11)
        target_include_directories(cui_common INTERFACE ${CUI_X11_INC})
        target_link_libraries(cui_common INTERFACE ${CUI_X11_LIB})
        target_compile_definitions(cui_common INTERFACE CUI_XLIB_AVAILABLE=1)
    endif()
    # The pkgconfig closure (fontconfig -> freetype/bz2/png) can come out as
    # bare -l names that FindPkgConfig fails to resolve to archive paths, so
    # hand the linker the vcpkg library trees explicitly (debug first in
    # Debug builds, matching how the rest of the closure is resolved).
    foreach(_tree debug lib)
        if(EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/${_tree}/lib")
            target_link_directories(cui_common INTERFACE
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/${_tree}/lib")
        endif()
    endforeach()
    unset(_tree)
    target_link_libraries(cui_common INTERFACE ${XCB_LIBRARIES} ${FONTCONFIG_LIBRARIES})
    # Vulkan loader — skia built with Vulkan/Graphite needs the loader at
    # link time (vkCreateInstance etc. are resolved from libvulkan.so).
    if(SKIA_HAVE_VULKAN)
        find_package(Vulkan REQUIRED)
        target_link_libraries(cui_common INTERFACE Vulkan::Vulkan)
    endif()
endif()

if(CUI_PLATFORM STREQUAL "ohos")
    # NAPI (napi/native_api.h) ships in libace_napi.z.so with the OHOS NDK.
    # The window/input backend lives in cui_core/src/platform/ohos_window.cpp and
    # links libnative_window.so + libhilog_ndk.z.so through the cui_core target.
    target_link_libraries(cui_common INTERFACE ace_napi.z)
endif()

if(CUI_PLATFORM STREQUAL "windows")
    # Win32 windowing + IME (imm32 for Chinese/Japanese text input).
    # winhttp: 图像核心的内置网络传输层（foundation/image_provider.cpp）。
    target_link_libraries(cui_common INTERFACE user32 imm32 gdi32 ole32 shell32 shlwapi winhttp)
    # Target Windows 10 so GetDpiForWindow / SetProcessDpiAwarenessContext
    # are declared by the SDK headers.
    target_compile_definitions(cui_common INTERFACE
        _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000 WINVER=0x0A00
        NOMINMAX _USE_MATH_DEFINES)
    # Source files are UTF-8; tell MSVC to avoid code-page misparse.
    target_compile_options(cui_common INTERFACE /utf-8)
    # Vulkan headers (vulkan-headers from vcpkg) — skia's internal
    # SkiaVulkan.h includes <vulkan/vulkan_core.h> which lives in the
    # vcpkg include tree, not in skia's own headers.
    if(SKIA_HAVE_VULKAN)
        target_include_directories(cui_common SYSTEM INTERFACE
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
        # Vulkan loader library (from Vulkan SDK).
        find_package(Vulkan REQUIRED)
        target_link_libraries(cui_common INTERFACE Vulkan::Vulkan)
    endif()
    # Ninja does not source vcvarsall.bat: wire the MSVC / Windows SDK
    # standard-library headers into the compile lines explicitly so BUILDING
    # needs no special environment (configure-time auto-detection already set
    # INCLUDE in this process for the compiler checks).
    if(CUI_MSVC_INC)
        target_include_directories(cui_common SYSTEM INTERFACE
            "${CUI_MSVC_INC}"
            "${CUI_SDK_UCRT_INC}"
            "${CUI_SDK_UM_INC}"
            "${CUI_SDK_SHARED_INC}")
    endif()
endif()

if(CUI_PLATFORM STREQUAL "web")
    # Emitted into every executable that links `cui_core` (gallery / ide):
    #   - the raster surface is copied into the canvas each frame, so heap
    #     growth must be allowed for large viewports;
    #   - ccall/UTF8ToString let the page hand IME composition and paste text
    #     to the hooks exported by web_window.cpp;
    #   - cui_core/src/platform/web/shell.html paints the page (canvas + loading state).
    set(CUI_WEB_DIR "${CMAKE_SOURCE_DIR}/cui_core/src/platform/web")
    # Emscripten's CMake toolchain forces CMAKE_EXECUTABLE_SUFFIX to ".js", and
    # emcc then drops --shell-file with "shell-file ignored when not generating
    # html output".  Linking the executables as ".html" emits
    # <name>.html + <name>.js/.wasm/.data and lets web/shell.html take effect.
    # Must come after project() (the toolchain set() shadows the cache) and
    # before the add_subdirectory() calls that own the executables.
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    target_link_options(cui_common INTERFACE
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString"
        "--shell-file=${CUI_WEB_DIR}/shell.html")
    # Debug: keep DWARF (-g3) for the browser debugger, emit a .wasm.map next
    # to the artifact, and turn on Emscripten's runtime assertions (bounds
    # checks, null-pointer traps with readable messages). Release stays lean.
    target_link_options(cui_common INTERFACE
        "$<$<CONFIG:Debug>:-g3>"
        "$<$<CONFIG:Debug>:-gsource-map>"
        "$<$<CONFIG:Debug>:-sASSERTIONS=2>"
        "$<$<CONFIG:Debug>:-sSTACK_OVERFLOW_CHECK=2>")
    # -g3 is a compiler flag as well; without it the DWARF in object files is
    # limited to line tables. Both engine libraries link cui_common, so the
    # INTERFACE option covers them too (they are added before this block).
    target_compile_options(cui_common INTERFACE "$<$<CONFIG:Debug>:-g3>")
    # Optional assets: cui_core/src/platform/web/fonts/<any>.ttf becomes
    # /cui_core/fonts/font.ttf when named font.ttf (see
    # cui_core/src/foundation/painter.cpp), res/ keeps translations.
    if(EXISTS "${CUI_WEB_DIR}/fonts")
        target_link_options(cui_common INTERFACE
            "--preload-file=${CUI_WEB_DIR}/fonts@/cui_core/fonts")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/res")
        target_link_options(cui_common INTERFACE
            "--preload-file=${CMAKE_SOURCE_DIR}/res@/res")
    endif()
endif()

# ── Copy resource files to build directory ────────────────────────────────────
# Android-style res/ directory is copied so executables can find translations
# at runtime via Translator::loadFromDirectory("res").
if(EXISTS "${CMAKE_SOURCE_DIR}/res")
    file(COPY "${CMAKE_SOURCE_DIR}/res" DESTINATION "${CMAKE_BINARY_DIR}")
endif()
# ── Install ──────────────────────────────────────────────────────────────────
# Layout follows GNUInstallDirs (lib/lib64, include, share) so the package can
# be relocated with CMAKE_INSTALL_PREFIX. Consumers use:
#   find_package(cui_core CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE cui_core::cui_core)
if(CUI_ENABLE_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS cui_core cui_wx cui_common cui
                    cui_charts cui_code_editor cui_dock cui_video
            EXPORT cuiTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
                    NAMELINK_COMPONENT Development
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY cui_core/include/cui_core
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            COMPONENT Development)
    # 控件包的头保持自己的子目录（include/cui_core/charts、include/cui_core/video …），
    # 与引擎的 include/cui_core/{widgets,rendering,…} 并列而不混在一起。
    install(DIRECTORY packages/charts/include/cui/charts
                     packages/code_editor/include/cui/code_editor
                     packages/dock/include/cui/dock
                     packages/video/include/cui/video
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cui_core
            COMPONENT Development)
    if(EXISTS "${CMAKE_SOURCE_DIR}/res")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/res"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/cui_core
                COMPONENT Runtime)
    endif()

    install(EXPORT cuiTargets
            FILE cuiTargets.cmake
            NAMESPACE cui_core::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cui_core
            COMPONENT Development)

    configure_package_config_file(
        "${CMAKE_SOURCE_DIR}/cmake/cuiConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cuiConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cui_core)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/cuiConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/cuiConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/cuiConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cui_core
            COMPONENT Development)

    # pkg-config for non-CMake consumers (hand-written build systems).
    set(CUI_PC_REQUIRES "")
    set(CUI_PC_LIBS "")
    if(CUI_PLATFORM STREQUAL "linux")
        set(CUI_PC_REQUIRES "xcb fontconfig")
        set(CUI_PC_LIBS "-pthread")
    elseif(CUI_PLATFORM STREQUAL "windows")
        set(CUI_PC_LIBS "-luser32 -limm32 -lgdi32 -lole32 -lshell32 -lshlwapi")
    elseif(CUI_PLATFORM STREQUAL "macos")
        set(CUI_PC_LIBS "-framework Cocoa")
    elseif(CUI_PLATFORM STREQUAL "ios")
        set(CUI_PC_LIBS "-framework UIKit")
    endif()
    configure_file("${CMAKE_SOURCE_DIR}/cmake/cui.pc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/cui.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cui.pc"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
            COMPONENT Development)
endif()

# ── Applications ─────────────────────────────────────────────────────────────
# 桌面可执行文件受 CUI_BUILD_EXAMPLES 控制；但 android / ohos 的宿主 .so 需要
# examples 层提供的 cui_app_root（cui_core::createAppRoot 的实现），所以打包脚本即使
# 关掉 CUI_BUILD_EXAMPLES（package_apk.sh 就是这么做的），本目录仍要进入以完成
# 装配。examples/CMakeLists.txt 内部再按 CUI_BUILD_EXAMPLES 决定是否建 demo_gallery。
if(CUI_BUILD_EXAMPLES OR CUI_BUILD_ANDROID_APP OR CUI_BUILD_OHOS_APP)
    add_subdirectory("examples")
endif()
if(CUI_BUILD_IDE)
    add_subdirectory("ide")
endif()

# ── CTest integration ─────────────────────────────────────────────────────────
# The browser has no process model / event loop to run the headless native
# harnesses in; only the native trees register tests. Cross-compiled targets
# (iOS, Android) cannot run the executables on the build host either.
if(CUI_BUILD_TESTS AND NOT CUI_PLATFORM STREQUAL "web"
   AND NOT CUI_PLATFORM STREQUAL "ios" AND NOT CUI_PLATFORM STREQUAL "android")
    enable_testing()
    add_subdirectory("tests")
endif()

# ── Debug CRT deployment (Windows / MSVC) ─────────────────────────────────────
# 只装 VS BuildTools（没有完整 VS IDE）的机器上，Debug CRT 不会进 System32：
# vcruntime140d.dll / vcruntime140_1d.dll / msvcp140d.dll 只躺在
#   <VS>/VC/Redist/MSVC/<ver>/debug_nonredist/x64/Microsoft.VC14x.DebugCRT/
# 里，于是 /MDd 编出来的 exe 一启动就弹「找不到 VCRUNTIME140_1D.dll」。
# 这里把整份 DebugCRT 拷到每个放 exe 的构建目录（app-local 部署），效果等同
# VS 安装器注册的调试运行时：不改 PATH、不改 System32、不影响 Release。
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CUI_DEBUG_CRT_DIR "" CACHE PATH "MSVC debug CRT directory (vcruntime140d.dll etc.)")
    if(NOT CUI_DEBUG_CRT_DIR)
        file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" _cui_cl)
        string(REGEX REPLACE "/VC/Tools/MSVC/.*$" "" _cui_vs_root "${_cui_cl}")
        file(GLOB _cui_crt_dirs
            "${_cui_vs_root}/VC/Redist/MSVC/*/debug_nonredist/x64/Microsoft.VC1*DebugCRT")
        if(_cui_crt_dirs)
            list(SORT _cui_crt_dirs)
            list(GET _cui_crt_dirs -1 CUI_DEBUG_CRT_DIR)
        endif()
    endif()
    if(CUI_DEBUG_CRT_DIR)
        foreach(_dst "${CMAKE_BINARY_DIR}" "${CMAKE_BINARY_DIR}/examples"
                     "${CMAKE_BINARY_DIR}/ide" "${CMAKE_BINARY_DIR}/tests")
            file(COPY "${CUI_DEBUG_CRT_DIR}/" DESTINATION "${_dst}")
        endforeach()
        message(STATUS "Debug CRT: ${CUI_DEBUG_CRT_DIR} -> exe dirs")
    else()
        message(STATUS "Debug CRT not found — Debug exe may miss VCRUNTIME140_1D.dll")
    endif()
endif()

# ── Packaging (CPack) ────────────────────────────────────────────────────────
# `cmake --build ... --target package` produces per-component archives
# (Development = headers + static libs + CMake/pkg-config files, Runtime =
# libs/binaries + res/), usable with `cmake --install` into any prefix.
if(CUI_ENABLE_INSTALL)
    set(CPACK_PACKAGE_NAME "cui_core")
    set(CPACK_PACKAGE_VENDOR "cui_core")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${CUI_PACKAGE_DESCRIPTION}")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "cui_core-${PROJECT_VERSION}")
    set(CPACK_GENERATOR "ZIP;TGZ")
    # One full-SDK archive; selective installs still use the components
    # declared on the install() rules:
    #   cmake --install <build> --prefix <p> --component Development
    include(CPack)
endif()
