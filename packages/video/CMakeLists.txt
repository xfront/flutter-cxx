# ── cui_video ──────────────────────────────────────────────────────────────
# 播放器控件族（对齐上游 video_player 包）+ 视频解码核心：
#
#   src/video_decoder.cpp        共享播放引擎 / 动图后端 / 按后端表挑一个
#   src/video_backends.cpp       后端表：系统端（默认优先）+ FFmpeg 端 + 运行期探测
#   src/video_decoder_win32.cpp  Media Foundation（Windows，系统端）
#   src/video_decoder_darwin.mm  AVFoundation（macOS / iOS，系统端）+ audio_sink_darwin.cpp
#   src/video_decoder_gst.cpp    GStreamer（Linux，系统端）+ audio_sink_alsa.cpp
#   src/video_decoder_android.cpp AMediaCodec（Android）+ audio_sink_android.cpp
#   src/video_decoder_web.cpp    HTMLVideoElement（web）
#   src/video_decoder_ohos.cpp   OH_AVSource/OH_AVDemuxer + OH_VideoDecoder（OHOS，系统端）
#                                + audio_sink_ohos.cpp（OH_AudioRenderer）
#   src/video_decoder_ffmpeg.cpp FFmpeg（第二条路线，全平台同一套代码）
#   src/audio_sink.cpp           出声设备兜底（无设备的平台返回空）
#   src/video_player.cpp         VideoPlayerValue 状态机 + VideoPlayer /
#                                VideoProgressIndicator / VideoControls 控件
#
# 解码后端本是 foundation 的一部分，随控件族一起搬出来：这样引擎层不必探测
# FFmpeg，而不看视频的消费方也不付媒体库的编译/链接代价。
# 两条路线（各端系统媒体栈 / FFmpeg）并存，各一个编译开关：
#   -DCUI_VIDEO_SYS=ON|OFF      路线一（默认 ON）
#   -DCUI_VIDEO_FFMPEG=ON|OFF   路线二（默认 ON；库固定来自 vcpkg）
# 谁先用由运行期 `setVideoBackendPreference()` 决定（默认系统端优先）；
# 两个都不可用时退到动图后端。
cui_add_library(cui_video
        src/video_decoder.cpp
        src/video_backends.cpp
        src/video_player.cpp
        src/audio_sink.cpp)

target_include_directories(cui_video PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(cui_video PUBLIC cui_wx)

# ── 编译开关：两条路线各一个 ───────────────────────────────────────────────
# 关掉某一条 = 它连编都不参与，后端表里就没有那一类条目；挑选顺序不用改，
# 表里只剩一条路时自动走那条。两个都关 ⇒ 只剩 Skia 的 GIF/WebP/APNG 动图后端
#（真视频容器打不开，`videoCapabilities()` 会如实上报）。
# `CUI_VIDEO_FFMPEG` / `CUI_VIDEO_FFMPEG_VCPKG` 定在根 CMakeLists：vcpkg 的
# manifest install 在 project() 里就跑完了，「要不要 ffmpeg 这个 feature」必须
# 在那之前定下来，这里只能读。
option(CUI_VIDEO_SYS "路线一：各端系统媒体栈后端（MF / AVFoundation / AMediaCodec / HTMLVideoElement / GStreamer / OH_AVDemuxer）" ON)

# ── 路线一：各端系统媒体栈（零第三方依赖，默认优先） ─────────────────────────
# 每个平台一个源文件 + 一个宏；宏是 `video_backends.cpp` 里那张表的开关。
# 动图后端（GIF/WebP/APNG）恒可用，真视频容器才需要这里的东西。
if(NOT CUI_VIDEO_SYS)
    message(STATUS "Video backend [system]: 已关（-DCUI_VIDEO_SYS=OFF）")
elseif(CUI_PLATFORM STREQUAL "windows")
    # 解码：Media Foundation；出声：WASAPI 共享模式（`audio_sink_win32.cpp`）。
    # MF 后端现在自己交 PCM（`video_decoder_win32.cpp` 的 `readAudio`：先试 IEEE
    # float 交织再试 S16），所以这一端真的有声音，不再是「无声 + 墙钟」。
    # **验证边界**：这一段是「在 Windows 上用真 MSVC + SDK 编、链、跑过测试」的
    # 代码，不再是 mingw-w64 的交叉语法校验（那条路仍然留着，见下面两处 `#if
    # defined(__MINGW32__)` 补的声明与旗标别名 —— 没有 SDK 的机器上还能过语法门）。
    # 设备上 WASAPI 起不来（无默认端点、独占被占、mix format 不是 32/16 位 PCM-float）时引擎退回无声，
    # 不把错误报成「打不开容器」。
    target_sources(cui_video PRIVATE src/video_decoder_win32.cpp src/audio_sink_win32.cpp)
    target_link_libraries(cui_video PUBLIC mfplat mfreadwrite mfuuid shlwapi ole32)
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_MEDIA_FOUNDATION=1)
    # `CUI_HAVE_AUDIO_SINK` 同时把 audio_sink.cpp 里那份兜底实现关掉，否则会重定义。
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_AUDIO_SINK=1 CUI_AUDIO_SINK_NAME="WASAPI")
    message(STATUS "Video backend [system]: Media Foundation（出声 WASAPI）—— "
        "MSVC + Windows SDK 真构建，与 FFmpeg 那条同机可选")
elseif(CUI_PLATFORM STREQUAL "android")
    # AMediaExtractor（解复用）+ AMediaCodec（解码），libmediandk 自 API 21 起在系统里。
    target_sources(cui_video PRIVATE src/video_decoder_android.cpp)
    target_link_libraries(cui_video PUBLIC mediandk)
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_MEDIACODEC=1)
    # 出声：AAudio。`aaudio` 仍然只 dlopen、不链接 —— 下限抬到 android-28 之后
    # 直接链已经能过（API 26 起才有那些符号，旧的 android-24 下限会被 ABI 检查拦下），
    # 保留 dlopen 是为了「ROM 不带 libaaudio.so」时退回无声，而不是让 .so 加载失败。
    # 抬下限本身的原因在 `scripts/package_apk.sh` 注释里（ffmpeg 的 HAVE_GLOB 要 bionic API 28）。
    # `CUI_HAVE_AUDIO_SINK` 同时把 audio_sink.cpp 里那份兜底实现关掉，否则会重定义。
    target_sources(cui_video PRIVATE src/audio_sink_android.cpp)
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_AUDIO_SINK=1 CUI_AUDIO_SINK_NAME="AAudio")
    message(STATUS "Video backend [system]: AMediaCodec + AMediaExtractor（出声 AAudio）")
elseif(CUI_PLATFORM STREQUAL "web")
    # 浏览器：解复用/解码/出声全交给 `<video>`，比往 wasm 里塞 FFmpeg 划算得多。
    # `src/video_decoder_web.cpp` 是泵驱动后端（单线程 wasm 起不了工作线程），
    # 出声不经本仓的 AudioSink ⇒ `CUI_AUDIO_SINK_NAME` 不给，后端表里那一项
    # `selfAudio = true`、`audioName` 清空。能不能真用只有真实浏览器知道。
    target_sources(cui_video PRIVATE src/video_decoder_web.cpp)
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_BROWSER_VIDEO=1)
    # EM_ASM 的体是 JS 文本，但预处理器仍按 C++ 词法给它分词 ⇒ `$0`/`""`/对象
    # 字面量 `{a: 1}` 各会报一条 C++ 方言告警（web_window.cpp 里同样有，只是数量少
    # 到没人注意）。这个文件几乎整篇都是 JS，所以按 TU 关掉这几条，而不是去
    # 改写 JS（`$0` 是 EM_ASM 唯一的取参写法，没有绕过它的办法）。
    if(NOT MSVC)
        target_compile_options(cui_video PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-dollar-in-identifier-extension
                -Wno-invalid-pp-token -Wno-multichar -Wno-language-extension-token
                -Wno-gnu-designator>)
    endif()
    message(STATUS "Video backend [system]: HTMLVideoElement（自带出声，不经本仓 AudioSink）")
elseif(CUI_PLATFORM STREQUAL "linux")
    # 出声：ALSA 是这端唯一「必然存在」的音频层（Pulse / PipeWire 都在它下面接），
    # 装不上开发包就退回无声 + 墙钟，不拦整条路线。
    # 两条解码路线现在都交 PCM（GStreamer 与 FFmpeg 的 `readAudio` 都覆写了），
    # 所以「编进 ALSA」= 「这条路上真会用得到」，不再需要区分是哪一路接走解码。
    find_path(CUI_ALSA_INCLUDE_DIR alsa/asoundlib.h)
    find_library(CUI_ALSA_LIBRARY NAMES asound)
    set(CUI_ALSA_OK FALSE)
    if(CUI_ALSA_INCLUDE_DIR AND CUI_ALSA_LIBRARY)
        target_sources(cui_video PRIVATE src/audio_sink_alsa.cpp)
        target_include_directories(cui_video PRIVATE ${CUI_ALSA_INCLUDE_DIR})
        target_link_libraries(cui_video PUBLIC ${CUI_ALSA_LIBRARY})
        target_compile_definitions(cui_video PUBLIC CUI_HAVE_AUDIO_SINK=1 CUI_AUDIO_SINK_NAME="ALSA")
        set(CUI_ALSA_OK TRUE)
    endif()
    # 解码：GStreamer（本端真正的「平台 video 能力」）。vcpkg 不管这个，只能问 pkg-config；
    # 没装 libgstreamer1.0-dev 就是没这条路，老实写出来，不拿宿主机头文件假装编过。
    find_package(PkgConfig QUIET)
    set(CUI_GST_FOUND FALSE)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(CUI_GST gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
        if(CUI_GST_FOUND)
            target_sources(cui_video PRIVATE src/video_decoder_gst.cpp)
            target_include_directories(cui_video PRIVATE ${CUI_GST_INCLUDE_DIRS})
            target_link_directories(cui_video PRIVATE ${CUI_GST_LIBRARY_DIRS})
            target_link_libraries(cui_video PUBLIC ${CUI_GST_LIBRARIES})
            target_compile_definitions(cui_video PUBLIC CUI_HAVE_GSTREAMER=1)
        endif()
    endif()
    if(CUI_GST_FOUND)
        set(CUI_VIDEO_SYS_VIDEO "GStreamer")
    else()
        set(CUI_VIDEO_SYS_VIDEO "无（缺 gstreamer 开发包 ⇒ 真容器只用 FFmpeg）")
    endif()
    # 出声那一栏只说「编进来了吗」—— 两条路线都交 PCM，编进来就会用上。
    if(CUI_ALSA_OK)
        set(CUI_VIDEO_SYS_AUDIO "ALSA")
    else()
        set(CUI_VIDEO_SYS_AUDIO "无（缺 alsa 开发包 ⇒ 无声 + 墙钟）")
    endif()
    unset(CUI_ALSA_OK)
    message(STATUS "Video backend [system]: 解码=${CUI_VIDEO_SYS_VIDEO}，出声=${CUI_VIDEO_SYS_AUDIO}")
elseif(CUI_PLATFORM STREQUAL "macos" OR CUI_PLATFORM STREQUAL "ios")
    # AVFoundation（`AVAssetReader` 解复用 + 解码）+ AudioQueue 出声。不用 `AVPlayer`：
    # 主时钟必须留在本仓 `AudioRing` 那一边（见 video_decoder_darwin.mm 文件头）。
    # `.mm` 要 -fobjc-arc（整个文件按 ARC 写），`audio_sink_darwin.cpp` 是纯 C API
    # ⇒ 不跟它沾 ARC，按普通 .cpp 编。
    target_sources(cui_video PRIVATE src/video_decoder_darwin.mm src/audio_sink_darwin.cpp)
    # `-fobjc-arc` 只能给那个 `.mm`（文件里所有 ObjC 对象都按 ARC 写），不能给
    # `audio_sink_darwin.cpp` —— 后者是纯 C API。GCC 没有 ARC，非 Clang 时直接拦下来
    # 而不是静默地编出一个引用计数错乱的二进制。
    if(NOT CMAKE_OBJCXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "video 的 AVFoundation 后端需要带 ARC 的 Objective-C++ 编译器（当前 "
            "${CMAKE_OBJCXX_COMPILER_ID}），请用 AppleClang/Clang 或 -DCUI_VIDEO_SYS=OFF")
    endif()
    set_source_files_properties(src/video_decoder_darwin.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    # 静态库不携带自己的系统库引用，最终链接方（examples/tests/APK）也要看到 ⇒ PUBLIC
    # （与 flutter/src/platform 那份 `-framework Cocoa` 同一条写法）。
    target_link_libraries(cui_video PUBLIC
        "-framework AVFoundation" "-framework CoreMedia"
        "-framework CoreVideo" "-framework AudioToolbox")
    # ObjC++ 不吃 CMAKE_CXX_STANDARD：不显式给就落回编译器默认（gnu++17），
    # 而这个 TU 包的是本仓 C++20 头文件。
    set_target_properties(cui_video PROPERTIES OBJCXX_STANDARD 20)
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_AVFOUNDATION=1)
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_AUDIO_SINK=1 CUI_AUDIO_SINK_NAME="AudioQueue")
    message(STATUS "Video backend [system]: AVFoundation（出声 AudioQueue）—— 本机无 macOS/iOS SDK，"
        "这两个文件只过了自写 stub 头的 ObjC++ 语法/ARC 校验，没链过真 SDK")
elseif(CUI_PLATFORM STREQUAL "ohos")
    # HarmonyOS Next：解复用 OH_AVSource/OH_AVDemuxer + 解码 OH_VideoDecoder/OH_AudioCodec，
    # 出声 OH_AudioRenderer（回调模式）。角色与 android 那一份完全对齐：系统只解码，
    # PCM 仍经本仓 AudioRing ⇒ 主时钟留在这边（见 video_decoder_ohos.cpp 文件头）。
    # 这些是 NDK 里的普通 so（`ohos.toolchain.cmake` 已把 sysroot/lib 放进搜索路径），
    # 写法与上面 `ace_napi.z` 同一条：只给库名，不带 lib/.so。
    #   core       OH_AVFormat / OH_AVBuffer / OH_AVCodec 基类
    #   avsource   OH_AVSource（含 OH_AVDataSource 内存来源）
    #   avdemuxer  OH_AVDemuxer
    #   vdec       OH_VideoDecoder      acodec  OH_AudioCodec
    #   codecbase  OH_AVCodec* 的公共部分（capability / 错误码）
    #   ohaudio    OH_AudioStreamBuilder + OH_AudioRenderer
    target_sources(cui_video PRIVATE src/video_decoder_ohos.cpp src/audio_sink_ohos.cpp)
    foreach(_cui_ohos_lib IN ITEMS
            native_media_core native_media_avsource native_media_avdemuxer
            native_media_codecbase native_media_vdec native_media_acodec
            ohaudio)
        target_link_libraries(cui_video PUBLIC ${_cui_ohos_lib})
    endforeach()
    unset(_cui_ohos_lib)
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_OH_AV_CODEC=1)
    target_compile_definitions(cui_video PUBLIC
        CUI_HAVE_AUDIO_SINK=1 CUI_AUDIO_SINK_NAME="OH_AudioRenderer")
    message(STATUS "Video backend [system]: OH_AVDemuxer + OH_VideoDecoder/OH_AudioCodec"
        "（出声 OH_AudioRenderer）—— API 12 起；本机有 NDK 26.0.0，这两个文件已用它"
        "真头编过（-Wall -Wextra -Wpedantic 干净）并对过 sysroot 里 native_media_*/"
        "libohaudio 的导出符号，但仍没链过真依赖、没在设备上跑过")
else()
    # 开关开了但这一端还没实现：不能假报「已启用」—— 表里因此只有 FFmpeg 那一条。
    message(STATUS "Video backend [system]: ${CUI_PLATFORM} —— 尚未实现（已写好的是 Windows 的 "
        "Media Foundation、Android 的 AMediaCodec、Apple 的 AVFoundation、OHOS 的 "
        "OH_AVDemuxer、web 的 HTMLVideoElement 与 Linux 的 GStreamer），这一路不进后端表 "
        "⇒ 真容器只用 FFmpeg")
endif()

# ── 路线二：FFmpeg（全平台同一套代码，库固定来自 vcpkg） ────────────────────
# 不碰发行版的 libav*：版本随环境飘（编进来一份、运行期找到另一份就得降级），
# 而且交叉编译时 pkg-config 解析的是宿主机的树 —— 它会给 arm64 的链接乐呵呵地
# 报 /usr/lib/x86_64-linux-gnu/libavformat.so。库从哪来统一交给根 CMakeLists：
# `CUI_VIDEO_FFMPEG_VCPKG=ON`（默认）时它已经把 feature `ffmpeg` 追加进
# VCPKG_MANIFEST_FEATURES，vcpkg 按当前 triplet（= VCPKG_ROOT 那棵树里的
# `installed/${VCPKG_TARGET_TRIPLET}`）下载现编，`find_package(FFMPEG)` 命中的
# 就是那一份（port 自带 FindFFMPEG + wrapper）。
set(CUI_FFMPEG_FOUND FALSE)
set(CUI_FFMPEG_WHERE "")

if(NOT CUI_VIDEO_FFMPEG)
    message(STATUS "Video backend [ffmpeg]: 已关（-DCUI_VIDEO_FFMPEG=OFF）")
elseif(CUI_PLATFORM STREQUAL "web")
    # 浏览器：解复用/解码/出声交给 `<video>`；vcpkg 的 ffmpeg port 也不支持 emscripten
    message(STATUS "Video backend [ffmpeg]: web 端不编（用 HTMLVideoElement）")
else()
    # 旧版走 pkg-config 时，`pkg_check_modules(FFMPEG ...)` 在 cache 里写下一整套
    # `FFMPEG_*`（INTERNAL，且库名是 avformat;avcodec;swscale;avutil —— **没有
    # swresample**）。而 vcpkg 的 FindFFMPEG 整个包在 `if(NOT FFMPEG_FOUND)` 里，
    # `find_package(FFMPEG)` 看到 cache 里那个 FFMPEG_FOUND=1 就直接短路返回，
    # 模块根本不跑 —— 报表会说「命中了」，拿到的还是宿主机那四个 bare name，
    # 链接静态 ffmpeg 时定会在 libavcodec.a 的 opus dec.o 上报
    # `undefined symbol: swr_alloc`。所以每次配置先把这些残留抹干净再探
    # （自备路径请用 CMAKE_PREFIX_PATH，别用 -DFFMPEG_DIR）。
    get_cmake_property(_cui_ffmpeg_cache_vars CACHE_VARIABLES)
    foreach(_v IN LISTS _cui_ffmpeg_cache_vars)
        if(_v MATCHES "^FFMPEG(_|$)")
            unset(${_v} CACHE)
        endif()
    endforeach()
    unset(_cui_ffmpeg_cache_vars)

    find_package(FFMPEG QUIET)
    if(FFMPEG_FOUND)
        set(CUI_FFMPEG_FOUND TRUE)
        set(CUI_FFMPEG_WHERE "vcpkg ${VCPKG_TARGET_TRIPLET}")
        # vcpkg 那份 FindFFMPEG 把链接期的 cfg 关键词一并塞进了
        # `FFMPEG_libavformat_LIBRARY`：`optimized;<路径>;debug;<路径>`。拿它做
        # `^${VCPKG_INSTALLED_DIR}` 的锚定匹配永远不命中 ⇒ 报表会说
        # 「外部 optimized;/tools/.../libavformat.a」，看着像找到了宿主机库，
        # 其实就是 vcpkg 自己。先取 `_RELEASE` 那一个，没有就抽中间那个路径。
        set(_cui_ffmpeg_probe "${FFMPEG_libavformat_LIBRARY_RELEASE}")
        if(NOT _cui_ffmpeg_probe)
            set(_cui_ffmpeg_probe "${FFMPEG_libavformat_LIBRARY}")
            # 只剩多配置形态：拿中间那个路径（optimized 后面第一个）
            if(_cui_ffmpeg_probe MATCHES "optimized;([^;]+)")
                set(_cui_ffmpeg_probe "${CMAKE_MATCH_1}")
            endif()
        endif()
        # 报表要说实话：命中那份不在 vcpkg 树里（自交叉编 + CMAKE_PREFIX_PATH）就换说法，
        # 免得再出现「写着 vcpkg、链的却是宿主机库」这类假消息。
        if(_cui_ffmpeg_probe AND VCPKG_INSTALLED_DIR AND
           NOT _cui_ffmpeg_probe MATCHES "^${VCPKG_INSTALLED_DIR}")
            set(CUI_FFMPEG_WHERE "外部 ${_cui_ffmpeg_probe}")
        endif()
        unset(_cui_ffmpeg_probe)
    elseif(CUI_VIDEO_FFMPEG_VCPKG AND NOT VCPKG_MANIFEST_MODE)
        # vcpkg 的工具链根本没接上（-DUSE_SKIA=OFF 且没走 preset）⇒ 谈不上自动装
        message(STATUS
            "Video backend [ffmpeg]: vcpkg 未接入（preset 会带 toolchain；直接调 cmake 需要 "
            "VCPKG_ROOT 且 -DUSE_SKIA=ON）⇒ 这一路没编进来")
    elseif(CUI_VIDEO_FFMPEG_VCPKG)
        message(STATUS
            "Video backend [ffmpeg]: ${VCPKG_TARGET_TRIPLET} 里没有 FFmpeg，且 feature 没生效 —— "
            "上面应当有一条 vcpkg install 的记录；没有就是开关被 cache 住了（重新配置）")
    else()
        # OHOS 这类 vcpkg 没有 triplet 的端：自己交叉编一份，用 CMAKE_PREFIX_PATH 喂进来
        message(STATUS
            "Video backend [ffmpeg]: 没找到 —— 已关掉自动安装（-DCUI_VIDEO_FFMPEG_VCPKG=OFF），"
            "这一路只认 CMAKE_PREFIX_PATH 里自备的那份 FFmpeg")
    endif()
endif()

if(CUI_FFMPEG_FOUND)
    target_sources(cui_video PRIVATE src/video_decoder_ffmpeg.cpp)
    target_include_directories(cui_video PRIVATE ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(cui_video PRIVATE ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(cui_video PUBLIC ${FFMPEG_LIBRARIES})
    target_compile_definitions(cui_video PUBLIC CUI_HAVE_FFMPEG=1)
    # 仅当 FFmpeg 是【静态 .a】且本项目编成 .so 时才需要下面的兜底：静态
    # libavcodec.a 里的 x86 汇编（cavsidct.asm 等）用 R_X86_64_PC32 引用
    # ff_pw_4 / ff_pb_80 这类全局常量，属于非 PIC 代码；链进 .so 时符号可被
    # 外部抢占，lld 会拒绝 "relocation R_X86_64_PC32 cannot be used against
    # symbol ...; recompile with -fPIC"。把这几个静态归档的符号设为本地即可
    # 让 PC32 合法，顺带不污染 cui_video.so 的导出表。
    # 动态 triplet（x64-linux-dynamic 等）下 FFmpeg 本身是 .so，不存在这个
    # 问题，选项自动跳过；STATIC 构建也不需要（可执行文件里符号不可抢占）。
    set(_cui_ffmpeg_has_static FALSE)
    foreach(_cui_fflib IN LISTS FFMPEG_LIBRARIES)
        if(_cui_fflib MATCHES "\\.a$")
            set(_cui_ffmpeg_has_static TRUE)
        endif()
    endforeach()
    if(_cui_ffmpeg_has_static AND CUI_LIBRARY_TYPE STREQUAL "SHARED"
       AND UNIX AND NOT APPLE)
        target_link_options(cui_video PRIVATE
            "LINKER:--exclude-libs=libavformat.a:libavcodec.a:libswresample.a:libswscale.a:libavutil.a")
    endif()
    unset(_cui_ffmpeg_has_static)
    # 「废弃声明」告警只关那一个 TU：需要遮的是 libav* 头里的旧名字
    # （`AV_CODEC_CAP_*`、`AVCodecContext.coded_frame`、旧的 channel_layout 那一族），
    # 不是本包的全部代码 —— 按整个 target 关会把新写的系统端后端里的误用一起遮掉。
    # 本机（vcpkg 那份 FFmpeg）实测：带不带这个开关都是 0 条 deprecation 告警，
    # 所以它现在是**保险**而不是必需项，留着是因为换一份更老的 libav* 就会重新冒出来。
    # （写法同上面 `.mm` 那条 `-fobjc-arc`：按源文件给 COMPILE_OPTIONS。）
    if(NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(src/video_decoder_ffmpeg.cpp PROPERTIES
            COMPILE_OPTIONS "-Wno-deprecated-declarations")
    endif()
    message(STATUS "Video backend [ffmpeg]: FFmpeg（${CUI_FFMPEG_WHERE}）")
endif()

if(NOT CUI_VIDEO_SYS AND NOT CUI_VIDEO_FFMPEG)
    message(STATUS "Video backend: 两条路线全关 —— 只剩 GIF/WebP/APNG 动图后端")
endif()
