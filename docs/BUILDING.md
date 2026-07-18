# 构建、调试与安装

本文覆盖 Windows / Linux / macOS / iOS / Android / OHOS / Web(WASM) 目标的
配置、构建、测试、调试与安装。所有命令都基于仓库根目录的
`CMakePresets.json`；没有预设覆盖的场合（OHOS）由一键脚本给出等价的
`cmake` 调用，下文也附手工命令。

> 依赖管理统一走 [vcpkg](https://github.com/microsoft/vcpkg)：`vcpkg.json`
> 声明 Skia（按平台带 graphite / vulkan / metal feature）、libxcb、
> fontconfig。预设通过 `$env{VCPKG_ROOT}` 定位 vcpkg 与
> `$env{VCPKG_ROOT}/installed`。

## 1. 通用前置

| 依赖 | 版本 | 说明 |
|------|------|------|
| CMake | ≥ 3.20（建议 3.25+） | 预设文件为 version 6，较老的 CMake 读不了新版本字段 |
| Ninja | 任意 | 预设统一使用 Ninja 生成器 |
| C++20 编译器 | GCC 13+ / Clang 16+ / MSVC 19.35+ | Apple 平台额外需要 Objective-C++ |
| vcpkg | 最新 | 设置环境变量 `VCPKG_ROOT` |

```bash
cmake --list-presets=all          # 查看本机可用的全部预设
cmake --preset <preset>
cmake --build --preset <preset>
ctest --preset <preset>           # 仅 x64-windows / x64-linux / macOS 注册了测试
```

### 1.1 产物目录 `dist/`

编译完成后，所有产物都直接落在源码根的 `dist/`，不再散落在
`build-*/cui_core/`、`build-*/packages/*/`、`build-*/examples/`：

* 本仓的库（`libcui-core.*` / `libcui-wx.*` / `libcui_charts.*` 等）、
  全部可执行文件（`story`、`demo_ide`、`vs2022`、各测试）；
* SHARED 构建时 vcpkg 的运行时动态库也一并收集：`libskia.so*`、ffmpeg 的
  `libav*.so*` / `libsw*.so*`，以及它们依赖的 `libwebp*.so` / `libfreetype.so` 等
  （版本软链原样重建）。静态 triplet 下没有 `.so`，这一步自动空跑。

因此 `dist/` 可整体拷走运行，不需要 `LD_LIBRARY_PATH`：

```bash
./dist/story
```

默认所有构建配置共用一个 `dist/`；若同时保留 debug/release 或
static/shared，用 `CUI_DIST_DIR` 按构建目录分仓，避免同名产物互相覆盖：

```bash
cmake -B build-x64-linux-debug -S . -DCUI_DIST_DIR=$PWD/build-x64-linux-debug/dist
```

收集第三方库的逻辑在 `cmake/cui_dist_runtime.cmake`，由 `cui_dist` 自定义目标
在每次构建末尾执行。

## 2. 平台矩阵

| 平台 | configure 预设 | 构建产物（均在源码根 `dist/`） | 测试 | 备注 |
|------|----------------|----------|------|------|
| Windows x64 | `x64-windows-{debug,release}` | `story.exe` / `demo_ide.exe` / `vs2022.exe` / `tests/*.exe` | ✅ | MSVC；`build.cmd` 会自动加载下面的 `vcvars64.bat` 环境 |
| Linux x64 | `x64-linux-{debug,release}` | 同上（无后缀） | ✅ | XCB + fontconfig + 可选 Xlib(XIM) |
| Linux + Sanitizer | `x64-linux-asan` / `x64-linux-ubsan` / `x64-linux-asan-ubsan` | 同上 | ✅ | `CUI_SANITIZE=...`，LSAN 抑制表自动挂上 |
| macOS arm64 / x64 | `macos-arm64-{debug,release}` / `macos-x64-{debug,release}` | 同上 | ✅ | AppKit 后端（Objective-C++），链接 `-framework Cocoa` |
| iOS arm64 | `ios-arm64-{debug,release}` | `libcui-core.a` / `libcui-wx.a` | — | 只编库；测试/示例默认关闭（交叉编译产物不可在宿主机运行） |
| Android arm64 | `android-arm64-{debug,release}` | `libcui-core.a` / `libcui-wx.a` | — | NDK 工具链（`ANDROID_NDK_HOME`），triplet `arm64-android` |
| OHOS arm64 | 无预设（`build.sh ohos` / 手工） | `libcui-core.a` / `libcui-wx.a` | — | NDK 26 + triplet `arm64-ohos`（release-only）；skia 的 ohos 分支由 overlay port 提供，见下 |
| Web / WASM | `wasm32-emscripten-{debug,release}` | `story.html/.js/.wasm` | — | 需要 emsdk（`EMSDK`，或 `--install-emsdk`）；浏览器内无进程模型，不注册测试 |

### 一键脚本（build.cmd / build.sh）

不想记预设名时用仓库根的两个入口：它们把参数翻译成预设调用后交给
`cmake -P`，构建结果与直接跑预设完全一致。实现是纯 CMake
（`../cmake/build.cmake` + `../cmake/platforms`），所以 **Windows 上
不需要 Git Bash**，`build.cmd` 会自动加载 `vcvars64.bat` 的环境
（cl / ninja / INCLUDE / LIB），从普通 cmd 窗口即可用。

```bat
build.cmd                        :: 当前主机（Windows）debug 全量构建 + ctest
build.cmd release                :: Release
build.cmd --target demo_ide      :: 只建一个目标
build.cmd --install C:\cui-out   :: 构建后安装到指定前缀
build.cmd --configure-only       :: 只验证工具链
build.cmd --help                 :: 平台 / profile / 全部选项
```

```bash
bash build.sh                       # Linux / macOS，同上
bash build.sh linux asan            # Sanitizer
bash build.sh macos release --arch arm64
bash build.sh android --configure-only
bash build.sh web --serve           # 构建 wasm 并起本地 HTTP 服务（8080）
bash build.sh web --install-emsdk   # 缺 Emscripten 时自动安装到 ~/emsdk
bash build.sh ios release
bash build.sh ohos                  # 需要 OHOS_NDK_HOME；无预设，手工配置
```

参数形式 `[平台] [profile] [选项]`：平台省略取当前主机，profile 省略取
`debug`；`-D<变量>=<值>` 原样透传给 `cmake --preset`。

Windows 上优先用 `build.cmd`；习惯 Git Bash 时 `bash build.sh` 也能跑：
脚本会把 `//host/Share/...` 形式的 UNC 源码路径换成映射盘（如 `Z:\...`），
否则 MSVC 的 cmd 规则会因 UNC 工作目录在链接阶段报 `LNK1104`。

### Windows

* `build.cmd` 会自动找 VS（vswhere → 常见安装路径）并加载 `vcvars64.bat`
  的环境，所以普通 cmd 窗口直接 `build.cmd` 即可，**不需要**先开
  Developer Command Prompt。直接跑预设（或 IDE/CI）时，若机器上只装了
  VS Build Tools 且没有注册到 vswhere，configure 阶段 vcpkg 找不到 VS
  实例，先手动执行一次：

  ```bat
  "D:\Program Files\VisualStudio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  cmake --preset x64-windows-debug
  ```
* Debug 构建会把 MSVC 的 Debug CRT（`vcruntime140d.dll` 等）复制到各个
  可执行文件目录（app-local 部署），所以只装 Build Tools 的机器也能直接跑。
  也可以用 `-DCUI_DEBUG_CRT_DIR=<目录>` 指定。
* MSVC 的 ASan 只支持 `-DCUI_SANITIZE=address`；Skia 预编译库未插桩，
  `skia_renderer.cpp` 会自动关掉插桩以保持容器布局一致。

### Linux

* 依赖 `libxcb`、`fontconfig`；`libX11` 可选（缺省时 XIM 中文输入被编译掉，
  只保留 keysym 的 ASCII 输入）。
* 当前只接线了 **X11** 后端。`../cui_core` 已有实现，
  但仍需 `wayland-scanner` 生成 `xdg-shell-client-protocol.h` 才能编译，
  CMake 尚未接线（见第 8 节）。

### macOS / iOS

* 两个 Apple 后端都是 Objective-C++（`cocoa_window.mm` / `ios_window.mm`），
  根 CMake 只在 `APPLE` 主机会 `enable_language(OBJCXX)`。
* macOS 链接 `-framework Cocoa`，iOS 链接 `-framework UIKit`（PUBLIC，
  静态库自己不带框架引用）。
* iOS 预设使用 vcpkg community triplet `arm64-ios`，并关闭示例/测试/IDE：
  `cmake --build --preset ios-arm64-debug` 只产出可嵌进 Xcode 工程的静态库。
* iOS 宿主工程参考 `../cui_core`（Swift 桥 + `cui_ios_bridge.h`）。
* iOS 打包：`scripts/package_ipa.sh [--simulator|--team ID] [--configuration Debug|Release]`，先 `cmake --preset ios-arm64-<cfg>` 出 `libcui-core.a`/`libcui_app.a`，再用 XcodeGen（`brew install xcodegen`）据 `../cui_core` 生成 `ios.xcodeproj`，最后 `xcodebuild` 出 `dist/cui.ipa`。

### Android

* 需要 `ANDROID_NDK_HOME` 指向 NDK（r25+）。预设通过
  `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` 把 NDK 的 `android.toolchain.cmake`
  交给 vcpkg 串联，ABI 固定 `arm64-v8a`、`android-28`。
  （下限从 `android-24` 抬到 28 的原因：vcpkg 那份 `ffmpeg:arm64-android` 探到
  `HAVE_GLOB=1`，而 bionic 自 API 28 才提供 `glob`/`globfree` ⇒ 按 24 链
  `libcui_gallery.so` 会在链接期报 undefined symbol。细节与退化路径写在
  `scripts/package_apk.sh` 的注释里。）
* 与 iOS 一样是「只编库」：Java 宿主参考 `../cui_core`。
* 想换 ABI 时覆盖 `-DANDROID_ABI=x86_64` 并把 `VCPKG_TARGET_TRIPLET` 改成
  对应的 `x64-android`。

### OHOS（HarmonyOS，arm64-ohos）

* 需要 OHOS Native SDK（如 `E:\dev-cache\ohos-sdk\26.0.0\native`）与 vcpkg
  community triplet `arm64-ohos`，工具链走 NDK 自带的
  `native/build/cmake/ohos.toolchain.cmake`（同样是
  `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`）。
* **`OHOS_SDK_ROOT` 必须设成 `native/` 的父目录**（即 `…/26.0.0`）：
  vcpkg 的 `detect_compiler` 会跑自己的 `scripts/toolchains/ohos.cmake`，
  不设就报 `Could not find the OHOS SDK`，与是否显式给了 chainload 无关。
  `build.sh ohos` 会在未设置时按 `OHOS_NDK_HOME` 自动推导，手工配置则要自己导出。
* skia 的 ohos 分支由仓库内 overlay port 提供
  （`cmake/vcpkg-overlay-ports/skia/016-ohos.patch`：`gn/BUILDCONFIG.gn` 的
  `is_ohos`、`SkGetExecutablePath_linux.cpp`、`SkLog_stdio.cpp`，以及
  Windows 宿主上 `gn/toolchain/BUILD.gn` 的 alink/copy 规则引号修复），所以
  配置时带上 `-DVCPKG_OVERLAY_PORTS=<repo>/cmake/vcpkg-overlay-ports`。
  `vcpkg.json` 里 ohos 已关掉 `dng`/`gl`（port 的 dng 不认 ohos，GL 走系统 EGL）。

  ```bash
  export OHOS_SDK_ROOT=/path/to/ohos-sdk/26.0.0
  export OHOS_NDK_HOME=$OHOS_SDK_ROOT/native
  cmake -S . -B build-ohos-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$OHOS_NDK_HOME/build/cmake/ohos.toolchain.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-ohos \
    -DVCPKG_OVERLAY_PORTS=$PWD/cmake/vcpkg-overlay-ports \
    -DOHOS_ARCH=arm64-v8a -DOHOS_PLATFORM=OHOS
  cmake --build build-ohos-arm64 --parallel
  ```
* 产物是静态库（`libcui-core.a` / `libcui-wx.a`，可嵌进 DevEco 工程）；示例与测试虽然也会被编出来，
  但都是 AArch64 产物，**不能在构建机上运行**。另外 **`arm64-ohos` 只装了 release**
  （`VCPKG_BUILD_TYPE=release`），所以本平台宿主依赖只有 Release 一份。
  窗口后端仍是占位实现（`createPlatformWindow()` 在 OHOS 返回 `nullptr`），
  真正上屏由 ArkTS 宿主接管 surface，见
  `../cui_core`。

### Web / Emscripten

* 需要 [emsdk](https://emscripten.org/docs/getting_started/downloads.html)，
  `EMSDK` 指向 emsdk 根目录；vcpkg 用 `wasm32-emscripten` triplet。
* 一键脚本会自动探测 `$EMSDK`、`~/emsdk`、`~/<任意>/emsdk`、`/opt/emsdk`
  等位置；装了发行版包（`apt install emscripten`）时用
  `emcc --print-resource-dir` 反推资源目录，并在
  `build-wasm-<profile>/.emsdk` 里补 `upstream/emscripten` 符号链接。
* 都没有时加 `--install-emsdk`：脚本 `git clone --depth 1` 到 `~/emsdk`
  后执行 `emsdk install latest` / `activate latest`（需要 git、Python 3
  和网络，首次下载工具链要几分钟），并直接把环境接到本次构建。
* Debug 构建自动带 `-g3 -gsource-map -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2`：
  浏览器 DevTools 里能看到 C++ 栈与源码映射，越界/空指针会给出可读报错。
* 字体：把 TTF 放到 `../cui_core` 会被预加载成
  `/cui/fonts/font.ttf`；也可以用环境变量 `CUI_WEB_FONT` 指定其它虚拟路径。
* 服务方式与浏览器限制见 `../cui_core`。

## 3. 构建选项

| 选项 | 默认 | 作用 |
|------|------|------|
| `USE_SKIA` | `ON` | 使用 Skia 渲染后端（关闭后只剩基础库，需自行提供 Painter） |
| `CUI_BUILD_EXAMPLES` | `ON` | 构建 `examples/`（`story`、`demo_ide`） |
| `CUI_BUILD_IDE` | `ON` | 构建 `ide/`（`vs2022` 可视化设计器） |
| `CUI_BUILD_TESTS` | `ON` | 构建并注册单元测试（Web/iOS/Android 永远不注册） |
| `CUI_ENABLE_INSTALL` | `ON` | 生成 install / export / CMake 包 / pkg-config / CPack 规则 |
| `CUI_ENABLE_PIC` | `ON` | 静态库加 `-fPIC`，便于被共享库/插件链接 |
| `CUI_LIBRARY_TYPE` | `STATIC` | `STATIC` ⇒ `libcui-core.a` + `libcui-wx.a`，`SHARED` ⇒ 对应 `.so`（控件包同开关） |
| `CUI_SANITIZE` | 空 | `address` / `undefined` / `thread` / `address,undefined` |
| `CUI_WARNINGS_AS_ERRORS` | `OFF` | 警告即错误（CI 用） |

只想要库（CI 交叉编译常用）：

```bash
cmake --preset android-arm64-release \
  -DCUI_BUILD_EXAMPLES=OFF -DCUI_BUILD_IDE=OFF -DCUI_BUILD_TESTS=OFF
```

## 4. 测试

```bash
ctest --preset x64-linux-debug          # 或 x64-windows-debug / macOS 预设
cd build-x64-linux-debug && ctest --output-on-failure -j 4
```

* 测试目标名与源文件同名（`test_pipeline` → `tests/test_pipeline.cpp`）；
  新增测试必须在 `tests/CMakeLists.txt` 里 `add_executable` + `add_test`。
* Sanitizer 构建会自动给全部测试设置
  `LSAN_OPTIONS=suppressions=tests/lsan_fontconfig.supp`（fontconfig/expat
  的进程级缓存不属于 cui 泄漏）。
* Web 构建没有进程模型，测试不会注册；iOS/Android 交叉编译产物也无法在
  宿主机运行，所以同样不注册。

## 5. 调试

### VS Code

`tools/vscode/` 下有两份模板（`.vscode/` 被 git 忽略，所以模板放在仓库内）：

```bash
mkdir -p .vscode && cp tools/vscode/*.json .vscode/
```

* `launch.json`：Gallery / IDE 设计器 / 单个测试，Windows 用 `cppvsdbg`，
  Linux 用 `cppdbg + gdb`，另附 ASan 配置（自动挂 LSAN 抑制表）。
* `tasks.json`：全部基于 `cmake --preset` / `ctest --preset`，与预设保持同步。
* Windows 上 configure 需要在 vcvars 环境里执行（`build.cmd` 会自动处理；
  手动跑预设时见第 2 节的 Windows 说明）。

### Sanitizer

```bash
cmake --preset x64-linux-asan
cmake --build --preset x64-linux-asan
ctest --preset x64-linux-asan
```

* `CUI_SANITIZE=address` 会打开插桩并传播到链接命令；旧的「在
  `CMAKE_CXX_FLAGS` 里手写 `-fsanitize=address`」方式仍然兼容
  （`tests/`、`../cui_core` 检测的是 `CUI_SANITIZE_ADDRESS`，由它统一推导）。
* Skia 预编译库没有插桩，`skia_renderer.cpp` 会被单独排除以保持
  `skia_private::TArray` / `Recording` 的布局一致。
* `CUI_FORCE_RASTER=1` 可整体绕开 GPU 后端，排查 Graphite 相关崩溃时先试它。

### 运行时开关（Gallery / 示例）

| 变量 | 作用 |
|------|------|
| `CUI_PAGE=<n>` | 启动直达第 n 页 |
| `CUI_SHOT=<png>` | 动作序列结束后保存一帧截图 |
| `CUI_SEQ="scroll:200;click:100,200;tick:3"` | 脚本化输入序列 |
| `CUI_EXIT_AFTER_START=1` | 渲染一帧后退出（冒烟 / 验析构路径） |
| `CUI_FORCE_RASTER=1` | 强制 CPU 光栅，禁用 Graphite |
| `CUI_FPS_INTERVAL_MS=<n>` | 覆盖帧间隔（默认值见 `application.cpp`） |
| `CUI_LANG=<code>` | 覆盖界面语言 |

## 6. 安装

### 安装到前缀

```bash
cmake --preset x64-windows-release       # 或 x64-linux-release / macos-arm64-release
cmake --build --preset x64-windows-release
cmake --install build-x64-windows-release --prefix D:/sdk/cui_core
# 只装运行/开发某一组件：
cmake --install build-x64-windows-release --prefix D:/sdk/cui_core --component Development
```

安装布局（GNUInstallDirs）：

```
<prefix>/
  include/cui/...                      # Development
  lib/libcui-core.a                    # Development（SHARED 时为 libcui-core.so*）
  lib/libcui-wx.a                      # Development（SHARED 时为 libcui-wx.so*）
  lib/cmake/cui/cuiConfig.cmake        # find_package(cui CONFIG)
  lib/cmake/cui/cuiConfigVersion.cmake
  lib/cmake/cui/cuiTargets.cmake
  lib/pkgconfig/cui.pc
  share/cui/res/...                    # Runtime（存在时）
```

### 在自己项目里使用

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(cui_core CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE cui::cui)
```

* 引擎本体导出两个目标：`cui::cui-core`（foundation/platform/rendering/
  framework）与 `cui::cui-wx`（只含 `../cui_core`，并链接 core）；
  `cui::cui` 是同时链接两者的 INTERFACE 聚合目标。可选控件包导出
  `cui::cui_charts` / `cui::cui_code_editor` / `cui::cui_dock` / `cui::cui_video`。
* `cuiConfig.cmake` 会递归 `find_dependency`：Skia（`unofficial-skia`）、
  `Threads`，以及构建时启用 Vulkan 的情况下的 `Vulkan`。消费者仍需
  `CMAKE_TOOLCHAIN_FILE` 指向 vcpkg 或自行提供这些包。
* 因为引擎库是静态库且直接链接了 vcpkg 的库树，安装包与构建时的依赖树
  绑定（同一台机器/同一份 vcpkg 安装可随意迁移前缀；跨机器需重新配置依赖）。
* 非 CMake 工程可用 `pkg-config --cflags --libs cui`，但第三方依赖
  （Skia/Vulkan）不在 `.pc` 里，推荐 CMake 消费。

### 打包

```bash
cmake --build build-x64-linux-release --target package
# 产物在 build-x64-linux-release/cui_core-<version>-<platform>.zip / .tar.gz
# （整包，含头文件 + 静态库 + CMake/pkg-config 元数据；只想装一部分时用
#  上面的 cmake --install --component Development / Runtime）
```

## 7. 故障排除

| 现象 | 处理 |
|------|------|
| configure 报 vcpkg `Unable to find a valid Visual Studio instance` | 用 `build.cmd`（自动加载 vcvars），或手动跑 `vcvars64.bat`（见第 2 节 Windows） |
| Debug exe 缺 `VCRUNTIME140_1D.dll` | 确认 `CUI_DEBUG_CRT_DIR` 被设置；只装 Build Tools 的机器必看 |
| 链接找不到 `xcb` / `fontconfig` | Debug 构建下 vcpkg 的 `pkgconfig` 目录带 `d` 后缀，根 CMake 已把 `PKG_CONFIG_PATH` 指到 release 树；自定义环境时照做 |
| Skia 在 Debug 下崩溃但 Release 正常 | 先 `CUI_FORCE_RASTER=1` 排除 Graphite，再查 ASan（Skia 未插桩的已知限制） |
| `cmake --preset` 提示 preset 不存在 | 该预设带主机条件（如 macOS/iOS 只在 Darwin 列出）；`--list-presets=all` 看全部 |

## 8. 已知缺口（与本轮构建/调试/安装改动相关）

* **Wayland 后端未接线**：`../cui_core` 需要
  `wayland-scanner` 生成 xdg-shell 协议头，还要补 text-input-v3 的完整实现；
  Linux 目前固定 X11。
* **iOS / Android 没有示例宿主工程文件**：只有 `../cui_core` 与
  `../cui_core` 的参考宿主，构建产物是静态库，需自行嵌进
  Xcode / Gradle 工程。
* **安装包不是完全可重定位的**：Linux/Windows 的第三方依赖以 vcpkg 树里的
  绝对路径导出（见 6.2 的说明）。要做到「拷到别的机器就能用」，还需要把
  依赖改成 imported target 或随包分发。
