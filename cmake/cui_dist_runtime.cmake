# ── dist/ 的第三方运行时库收集 ───────────────────────────────────────────────
# 由根 CMakeLists 的 `cui_dist` 自定义目标在每次构建后以 `cmake -P` 执行。
#
# 本仓的库与可执行文件由 CMAKE_{RUNTIME,LIBRARY,ARCHIVE}_OUTPUT_DIRECTORY 直接
# 写进 dist/ 顶层；这里补的是链接时用到的 vcpkg 动态库，统一放进 dist/lib/：
# libskia.so、ffmpeg 的 libav*.so / libsw*.so，以及它们依赖的 libwebp*.so /
# libfreetype.so / libz.so … 本仓目标用 $ORIGIN/lib 的 rpath 找到它们。
# 只有静态库（.a）的 triplet 下这些 glob 全为空，脚本自然空跑。

if(NOT DEFINED CUI_DIST_DIR OR CUI_DIST_DIR STREQUAL "")
    message(FATAL_ERROR "cui_dist_runtime.cmake: -DCUI_DIST_DIR=<dir> is required")
endif()
if(NOT DEFINED CUI_VCPKG_ROOT OR NOT IS_DIRECTORY "${CUI_VCPKG_ROOT}")
    return()
endif()

file(MAKE_DIRECTORY "${CUI_DIST_DIR}")
# 第三方库统一放进 lib/ 子目录；本仓的 libcui-* 仍由构建直接写到 dist/ 顶层。
file(MAKE_DIRECTORY "${CUI_DIST_DIR}/lib")

# release 目录在前；debug/lib、debug/bin 里同名的库不覆盖 release 版本。
set(_cui_vcpkg_dirs
    "${CUI_VCPKG_ROOT}/lib"
    "${CUI_VCPKG_ROOT}/bin"
    "${CUI_VCPKG_ROOT}/debug/lib"
    "${CUI_VCPKG_ROOT}/debug/bin")

foreach(_dir IN LISTS _cui_vcpkg_dirs)
    if(NOT IS_DIRECTORY "${_dir}")
        continue()
    endif()
    if(_dir MATCHES "/debug/")
        set(_cui_debug_tree TRUE)
    else()
        set(_cui_debug_tree FALSE)
    endif()
    # *.so.* 覆盖 libskia.so.0 / libskia.so.0.1.0、libavcodec.so.60 这类版本名。
    file(GLOB _libs
        "${_dir}/*.so" "${_dir}/*.so.*"
        "${_dir}/*.dylib" "${_dir}/*.dll")
    foreach(_lib IN LISTS _libs)
        get_filename_component(_name "${_lib}" NAME)
        set(_dst "${CUI_DIST_DIR}/lib/${_name}")
        set(_present FALSE)
        if(EXISTS "${_dst}" OR IS_SYMLINK "${_dst}")
            set(_present TRUE)
        endif()
        if(IS_SYMLINK "${_lib}")
            # 版本软链（libskia.so -> libskia.so.0 -> libskia.so.0.1.0）原样重建，
            # 避免把同一个大文件 dereference 成三份拷贝。
            if(_present)
                continue()
            endif()
            file(READ_SYMLINK "${_lib}" _link_target)
            file(CREATE_LINK "${_link_target}" "${_dst}"
                 SYMBOLIC COPY_ON_ERROR)
        else()
            # 实体文件每次都走 file(COPY)（CMake 只在内容/时间更新时真拷），
            # 这样 vcpkg 重装库后 dist/ 会跟着刷新；debug 树不抢 release 的名字。
            if(_cui_debug_tree AND _present)
                continue()
            endif()
            file(COPY "${_lib}" DESTINATION "${CUI_DIST_DIR}/lib")
        endif()
        # 清掉上一版平铺在 dist/ 顶层的同名第三方库（本仓 libcui-* 不动），
        # 否则 $ORIGIN 会优先命中顶层旧副本。
        set(_top "${CUI_DIST_DIR}/${_name}")
        if(EXISTS "${_top}" AND NOT _name MATCHES "^libcui")
            file(REMOVE "${_top}")
        endif()
    endforeach()
endforeach()
