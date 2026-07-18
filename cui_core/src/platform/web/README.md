# cui — Web (WebAssembly) 后端

浏览器里跑的就是同一套 `Application → 三层管线 → Skia`：`WebWindow`
(`../web_window.cpp`) 把 HTML5 事件翻译成 `PlatformEvent`，
`Application` 由 `emscripten_set_main_loop_arg` 驱动（`requestAnimationFrame`），
每帧把 Skia 的 **raster surface** 通过 canvas 2D `putImageData` 呈现。

## 前置

- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
  （`emcc` / `em++` / `emcmake` 在 `PATH`，或导出 `EMSDK`）
- vcpkg（`VCPKG_ROOT` 指向仓库里 `cmake/buildsystems/vcpkg.cmake` 的父目录）
- CMake ≥ 3.20 + Ninja

首次配置会通过 manifest 安装 `wasm32-emscripten` 三元组的 **skia**（含
freetype），编译时间较长。

## 构建

```bash
source "$EMSDK/emsdk_env.sh"          # 或 Windows: emsdk_env.bat
export VCPKG_ROOT=/path/to/vcpkg

cmake --preset wasm32-emscripten-release
cmake --build --preset wasm32-emscripten-release --target story
```

产物：

```
build-wasm-release/examples/story.html
build-wasm-release/examples/story.js
build-wasm-release/examples/story.wasm
```

`demo_ide` 同理；测试目标在 web 下不构建（没有可用的进程/事件循环去跑
headless 断言，`../../../../CMakeLists.txt` 里已跳过 `tests/`）。

## 运行

静态托管 `build-wasm-release/examples/` 即可（`file://` 不行，wasm 需要
HTTP）：

```bash
python3 -m http.server 8000 --directory build-wasm-release/examples
# 打开 http://localhost:8000/story.html
```

没有使用 `-pthread`：不需要 `SharedArrayBuffer`，也就不需要
`Cross-Origin-Opener-Policy` / `Cross-Origin-Embedder-Policy` 响应头。
（代价是 `ThreadPool::submit()` 在浏览器里内联执行，见 `thread_pool.h`。）

## 字体（重要）

浏览器不向 wasm 暴露系统字体：`Painter::resolveTypeface()` 在
`__EMSCRIPTEN__` 下从 `/cui/fonts/font.ttf` 加载一个随包字体。不放字体时
文本不可见（布局仍在跑）。

```bash
mkdir -p src/platform/web/fonts
cp /path/to/NotoSansSC-Regular.otf src/platform/web/fonts/font.ttf   # 文件名固定为 font.ttf
# 重新链接（CMake 检测到 src/platform/web/fonts/ 后会加 --preload-file）
cmake --build --preset wasm32-emscripten-release --target story
```

另一个入口：运行时设置 `CUI_WEB_FONT=/绝对 MEMFS 路径` 指向预加载的字体。

## 已接线

| 能力 | 实现 |
| --- | --- |
| 主循环 | `emscripten_set_main_loop_arg(..., fps=0)` → `Application::runIteration()` |
| 呈现 | `blitToNativeWindow()` 的 `__EMSCRIPTEN__` 分支：脏区 `putImageData` |
| 尺寸 / HiDPI | canvas backing store = CSS 盒尺寸（1 布局 px = 1 CSS px），`resize` 时同步并派发 `Resize`；`dpiScale()` 仍返回 `devicePixelRatio` |
| 鼠标 / 滚轮 | 坐标就是 CSS px（与布局同一坐标系）；`deltaMode` 行/页换算成像素 |
| 键盘 | `code`/`key` → `KeyCode`，可打印字符走 `PlatformEvent::text`；无修饰键的按键阻止默认行为（Tab/Space/方向键留在应用内），Ctrl/Cmd/Alt 组合与 F1–F12 放行给浏览器 |
| 触摸 | `touch*` → `Touch*` + 合成鼠标事件 |
| IME / 粘贴 | 已导出 `cui_web_ime_preedit` / `cui_web_ime_commit` 钩子，并挂了 `composition*` / `paste` 监听；**但 canvas 不是可编辑元素，浏览器不会向它派发 composition 事件**，中文输入还需要一个隐藏 `<input>` 桥（见「已知限制」） |
| 生命周期 | 焦点/可见性 → `../../..`（`resumed` / `inactive` / `hidden`），`beforeunload` → `detached` |
| 剪贴板 | 写入走 `navigator.clipboard.writeText`（失败回退 `execCommand`） |
| 指针锁 | `grabPointer()` → Pointer Lock API |

## 已知限制

- **字体必须随包**（见上）；没有系统字体回退。
- **剪贴板只能写不能同步读**：浏览器禁止同步读取，粘贴通过 canvas 的
  `paste` 监听送进焦点控件。
- **中文/日文输入法不可用**：composition 事件只派发给可编辑 DOM 节点，
  canvas 收不到。下一步是仿 Flutter web 加一个隐藏 `<input>`，文本框获得
  焦点时把 DOM 焦点转过去，再把 `compositionend` / `input` 送进
  `cui_web_ime_commit`。拉丁字母输入不受影响（走 window 的 `keydown`）。
- **系统服务**：`SystemChrome` / `HapticFeedback` / `SystemNavigator`
  没有 web 宿主实现，`invokeMethod` 返回 `nullopt`，gallery 的系统服务卡会
  显示「宿主不可用」。
- **渲染后端**：固定 raster（WebGPU/Dawn 的 adapter 获取是异步的，Skia
  Graphite 无法同步建上下文）；后续可在 `SkiaRenderer::initDawn` 做成
  异步初始化后切换。
- 鼠标离开画布不会补发 `MouseUp`：拖拽中移出窗口后松开需要再点一次。
- **HiDPI 是「浏览器放大」而不是「原生清晰」**：框架按 1 布局 px =
  1 surface px 画图，没有统一 DPR 变换，所以 canvas 按 CSS 尺寸建、
  由浏览器在 2× 屏上放大（文字略软）。要做成 Flutter 那样的逻辑像素 +
  DPR 缩放，需要在 `paintFullFrame` 里加一层呈现变换并把脏区换算到设备像素。
