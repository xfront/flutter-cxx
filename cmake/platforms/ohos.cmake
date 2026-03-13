# ohos.cmake — HarmonyOS Next（arm64）静态库。
#
# CMakePresets.json 里没有 OHOS 预设（OHOS 的 CMAKE_SYSTEM_NAME 由 NDK 工具链
# 注入，无法在预设里按主机条件声明），所以这里手工配置：
#   cmake -S . -B build-ohos-arm64-<profile> -G Ninja
#     -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=<ohos-ndk>/build/cmake/ohos.toolchain.cmake
#     -DVCPKG_TARGET_TRIPLET=arm64-ohos
# 前置：VCPKG_ROOT 指向 vcpkg 根（toolchain 路径由根 CMakeLists 解析）；
#       OHOS_NDK_HOME 指向 native/ 目录（本机 E:/dev-cache/ohos-sdk/26.0.0/native）；
#       OHOS_SDK_ROOT 指向 native/ 的**上一级**（vcpkg 的 scripts/toolchains/ohos.cmake
#       要的是 SDK 根，不是 native 目录；本机 E:/dev-cache/ohos-sdk/26.0.0）——
#       未设置时会由 OHOS_NDK_HOME 自动推导；
#       arm64-ohos 的 Skia 依赖走仓库自带的 overlay port（cmake/vcpkg-overlay-ports/skia
#       的 016-ohos.patch + target_os="ohos" 分支），下面已自动接上。
# Windows 主机额外注意：skia 之前的 icu 依赖在 community/arm64-ohos triplet 下会挂在
# 两处，都是「宿主 pkgdata.exe 按 MSVC 编」造成的，与 OHOS 无关 ——
#   ① extra/uconv：pkgdata 把 "uconvmsg\uconvmsg_dat.S" 塞进 bash -c，反斜杠被吃掉
#      ⇒ configure 加 --disable-extras（vcpkg 的 icu port 对 emscripten/UWP 同款）；
#   ② data 安装：pkgdata.cpp 的 CYGWINMSVC 分支无条件再装一个 libicudata.lib，
#      Unix 目标根本不产它 ⇒ make 命令行覆盖 INSTALL 成一个「只跳过缺失 *.lib」的
#      包装脚本（data/pkgdataMakefile.in 写的是 INSTALL_CMD=$(INSTALL)）。
# 两者都通过 triplet 的 VCPKG_MAKE_CONFIGURE_OPTIONS / VCPKG_MAKE_OPTIONS 注入，
# 属于主机环境补丁，不放进仓库；Linux/macOS 主机交叉编译不需要。
# 产物：源码根 dist/ 下的 libcui-core.a + libcui-wx.a
#
# 注意：cui_core 目前只接了 OHOS 的平台服务桥（napi），窗口/输入后端还没实现，
# 所以产物要由 ArkTS 宿主（src/platform/ohos/）自行驱动：platform_factory.h 在
# 这一端返回 nullptr ⇒ AppWindow::createNative() 必然失败，宿主走 createOffscreen()
# 并自己把 surface 交给 ArkTS 侧。

function(cui_ohos_main)
    if(NOT CUI_PROFILE MATCHES "^(debug|release)$")
        cui_die("ohos 的 profile 只支持 debug / release（收到 '${CUI_PROFILE}'）。")
    endif()
    if("$ENV{OHOS_NDK_HOME}" STREQUAL "")
        cui_die(
            "OHOS_NDK_HOME 未设置。\n"
            "  指向 HarmonyOS SDK 的 native 目录，例如：\n"
            "  set OHOS_NDK_HOME=<sdk>/default/openharmony/native")
    endif()

    set(_ohos_toolchain "$ENV{OHOS_NDK_HOME}/build/cmake/ohos.toolchain.cmake")
    if(NOT EXISTS "${_ohos_toolchain}")
        set(_ohos_toolchain "$ENV{OHOS_NDK_HOME}/native/build/cmake/ohos.toolchain.cmake")
    endif()
    if(NOT EXISTS "${_ohos_toolchain}")
        cui_die("在 OHOS_NDK_HOME=$ENV{OHOS_NDK_HOME} 下找不到 build/cmake/ohos.toolchain.cmake。")
    endif()
    cui_info("OHOS NDK: $ENV{OHOS_NDK_HOME}")

    # vcpkg 的 scripts/toolchains/ohos.cmake 在 detect_compiler 时只认 OHOS_SDK_ROOT
    # （= native/ 的上一级），即使 configure 已显式给了 chainload toolchain 也照跑；
    # 没设时就按 NDK 目录推出来，省得每次手工 export。set(ENV) 会被
    # cui_configure_dir 的 execute_process 子进程继承。
    if("$ENV{OHOS_SDK_ROOT}" STREQUAL "")
        get_filename_component(_ohos_sdk_root "$ENV{OHOS_NDK_HOME}" DIRECTORY)
        set(ENV{OHOS_SDK_ROOT} "${_ohos_sdk_root}")
        cui_info("OHOS_SDK_ROOT 未设置，按 NDK 目录推导: ${_ohos_sdk_root}")
    endif()

    cui_require_vcpkg()
    if(CUI_PROFILE STREQUAL "debug")
        set(_build_type Debug)
    else()
        set(_build_type Release)
    endif()

    set(_dir "${CUI_ROOT}/build-ohos-arm64-${CUI_PROFILE}")
    # 仓库的 skia overlay 排最前（arm64-ohos 分支只在这儿），主机已有的
    # VCPKG_OVERLAY_PORTS 追加在后而不是被顶掉：命令行 --overlay-ports 会整体
    # 覆盖环境变量，本机就靠环境里那份 vcpkg-tool-gn 绕开连不通的 CIPD 下载。
    set(_overlay_ports "${CUI_ROOT}/cmake/vcpkg-overlay-ports")
    if(NOT "$ENV{VCPKG_OVERLAY_PORTS}" STREQUAL "")
        list(APPEND _overlay_ports "$ENV{VCPKG_OVERLAY_PORTS}")
    endif()
    # VCPKG_INSTALLED_DIR 必须是**每个 triplet 自己的根**（和 android/web 预设同一条
    # 铁律）：manifest install 会把当前 manifest 里没有的包从共享根里裁掉，指到
    # $VCPKG_ROOT/installed 的话，配置一次鸿蒙就能把 x64-linux / x64-windows 正在用
    # 的那份 skia 删掉。
    set(_args
        -DCMAKE_BUILD_TYPE=${_build_type}
        -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${_ohos_toolchain}
        -DVCPKG_TARGET_TRIPLET=arm64-ohos
        -DVCPKG_INSTALLED_DIR=${_dir}/vcpkg_installed
        -DVCPKG_OVERLAY_PORTS=${_overlay_ports}
        -DOHOS_ARCH=arm64-v8a
        -DOHOS_PLATFORM=OHOS)
    find_program(_ninja ninja)
    if(_ninja)
        list(APPEND _args -G Ninja)
    endif()

    cui_warn("OHOS 没有预设：手工配置（arm64-ohos triplet，首次会先编依赖）。")
    cui_warn("Skia 走仓库 overlay port 的 ohos 分支；OHOS_SDK_ROOT 要指向 native/ 的上一级。")
    cui_configure_dir("${_dir}" ${_args})

    if(CUI_CONFIGURE_ONLY)
        cui_info("--configure-only：停在配置阶段 ${_dir}")
        return()
    endif()

    cui_build_dir_only("${_dir}")
    if(NOT CUI_INSTALL_PREFIX STREQUAL "")
        cui_step("cmake --install → ${CUI_INSTALL_PREFIX}")
        cui_run_checked("安装" cmake --install "${_dir}" --prefix "${CUI_INSTALL_PREFIX}")
    endif()

    cui_report_libs("${_dir}")
    cui_info("")
    cui_info("接入宿主：把 .a 加进 DevEco 工程的 CMake 子工程（ABI 为 arm64-v8a），")
    cui_info("ArkTS 侧参考 src/platform/ohos/entry/src/main/ets/cui_core/。")
    cui_info("构建目录: ${_dir}")
endfunction()
