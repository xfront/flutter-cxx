#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# 构建 Web / WebAssembly 版（wasm32-emscripten）。
#
#   bash scripts/package_web.sh                    # 配置 + 编译 story
#   bash scripts/package_web.sh --skip-build       # 只做产物校验
#   bash scripts/package_web.sh --serve            # 编完顺带起 http.server
#   bash scripts/package_web.sh --verify           # 编完跑 headless 浏览器验证
#   bash scripts/package_web.sh --preset wasm32-emscripten-debug
#   bash scripts/package_web.sh --target demo_ide   # 默认 story
#
# 产物校验之外的两条验证路（都要 HTTP，file:// 下 fetch .wasm/.data 会被拒）：
#   * --verify 走 scripts/web_verify.mjs（CDP）：等真实时间把 16MB .data 下完，
#     读页面里 probe.html 的 window.__probe（画布尺寸/非背景像素/颜色直方图），
#     再按 --click 注入鼠标事件复验交互，最后 Page.captureScreenshot。
#   * 手工：python3 -m http.server + 浏览器打开。
#     别用 `chrome --headless --screenshot --virtual-time-budget`：虚拟时钟只快进
#     定时器，下载仍是真实 I/O，截图会在 main() 开始之前就打完。
#
# 链路：
#   1. source emsdk_env.sh：emcc/em++/emcmake 进 PATH，导出 EMSDK
#   2. cmake --preset wasm32-emscripten-release -B build-wasm-release
#      preset 已把 VCPKG_INSTALLED_DIR 指到 <build>/vcpkg_installed ——
#      manifest 安装会删掉同一个根下不在当前 manifest 里的包，共享根时
#      x64-linux ↔ wasm32-emscripten 会互相掏，桌面构建的 skia 会被清掉。
#      首次配置会交叉编译 skia/abseil/icu/freetype 等 25 个包，耗时较长。
#   3. cmake --build --target story
#      产物：examples/story.{html,js,wasm,data}
#   4. 浏览器只能经 HTTP 打开（file:// 下 fetch .wasm/.data 会被拒）
#
# 字体：wasm 拿不到系统字体。painter.cpp 的 __EMSCRIPTEN__ 分支只认
# cui_core/src/platform/web/fonts/font.ttf（preload 到 MEMFS /cui_core/fonts/font.ttf），
# 缺字体时布局照跑但**文本全部不可见**。脚本会检查并给出抽取命令。
#
# 与桌面构建不同的开关（preset 里已带，无需手传）：
#   * VCPKG_TARGET_TRIPLET=wasm32-emscripten + chainload Emscripten.cmake
#   * Threads 不 find_package（交叉探测不可能成功），ThreadPool::submit() 内联执行
#   * 不用 -pthread ⇒ 不需要 SharedArrayBuffer / COOP·COEP 响应头
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESET="wasm32-emscripten-release"
TARGET="story"
BUILD_DIR="$ROOT/build-wasm-release"
DO_BUILD=1
DO_SERVE=0
DO_VERIFY=0
SERVE_PORT=7000
VERIFY_PORT=7123

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) DO_BUILD=0; shift ;;
        --serve) DO_SERVE=1; shift ;;
        --verify) DO_VERIFY=1; shift ;;
        --preset) PRESET="$2"; shift 2 ;;
        --target) TARGET="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --port) SERVE_PORT="$2"; shift 2 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

# preset 名 → 构建目录（除非用户显式给了 --build-dir）。
# 只有默认值才自动跟随，避免 --preset wasm32-emscripten-debug 仍编到 release 树。
if [ "$BUILD_DIR" = "$ROOT/build-wasm-release" ]; then
    case "$PRESET" in
        *-debug) BUILD_DIR="$ROOT/build-wasm-debug" ;;
        *-release) BUILD_DIR="$ROOT/build-wasm-release" ;;
        *) BUILD_DIR="$ROOT/build-${PRESET}" ;;
    esac
fi

# ── Emscripten SDK ──────────────────────────────────────────────────────────
EMSDK_DIR="${EMSDK:-}"
if [ -z "$EMSDK_DIR" ]; then
    for c in /tools/dev-cache/emsdk "$HOME/emsdk" "$ROOT/../emsdk"; do
        [ -x "$c/upstream/emscripten/emcc" ] && { EMSDK_DIR="$c"; break; }
    done
fi
if [ -z "$EMSDK_DIR" ] && command -v emcc >/dev/null 2>&1; then
    # emcc 在 PATH 里但没导出 EMSDK：从 emcc 路径反推。
    EMSDK_DIR="$(cd "$(dirname "$(command -v emcc)")/../.." && pwd)"
fi
[ -n "$EMSDK_DIR" ] || die "找不到 Emscripten SDK：git clone https://github.com/emscripten-core/emsdk /tools/dev-cache/emsdk && cd \$_ && ./emsdk install latest && ./emsdk activate latest"
# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1 || true
export EMSDK="$EMSDK_DIR"
command -v emcc >/dev/null 2>&1 || die "source $EMSDK_DIR/emsdk_env.sh 之后 emcc 仍不在 PATH"

# ── vcpkg ───────────────────────────────────────────────────────────────────
if [ -z "${VCPKG_ROOT:-}" ]; then
    for c in /tools/dev-cache/vcpkg "$HOME/vcpkg"; do
        [ -f "$c/cmake/buildsystems/vcpkg.cmake" ] && { export VCPKG_ROOT="$c"; break; }
    done
fi
[ -n "${VCPKG_ROOT:-}" ] || die "找不到 vcpkg：导出 VCPKG_ROOT（需含 cmake/buildsystems/vcpkg.cmake）"
[ -f "$VCPKG_ROOT/triplets/community/wasm32-emscripten.cmake" ] \
    || die "$VCPKG_ROOT 里没有 wasm32-emscripten triplet（需较新的 vcpkg）"

# ── 字体（缺失只警告，不阻断：管线/交互仍可验证）────────────────────────────
FONT_DIR="$ROOT/cui_core/src/platform/web/fonts"
if [ ! -f "$FONT_DIR/font.ttf" ]; then
    say "warn: 缺 $FONT_DIR/font.ttf —— wasm 里文本将不可见"
    say "      从本机 Noto CJK 抽简体中文 face（需要 python3-fonttools）："
    say "        mkdir -p $FONT_DIR && python3 -c \""
    say "        from fontTools.ttLib import TTCollection"
    say "        c = TTCollection('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')"
    say "        c.fonts[2].save('$FONT_DIR/font.ttf')\"   # face 2 = Noto Sans CJK SC"
fi

say "EMSDK:      $EMSDK_DIR  ($(emcc --version | head -1))"
say "VCPKG_ROOT: $VCPKG_ROOT"
say "preset:     $PRESET   build dir: $BUILD_DIR"

# ── 1. configure + 2. build ─────────────────────────────────────────────────
if [ "$DO_BUILD" = 1 ]; then
    # /tmp 在本机只有 10MB，交叉编译的临时文件必须换地方。
    export TMPDIR="$BUILD_DIR/tmp"
    mkdir -p "$TMPDIR"
    say "configure"
    cmake --preset "$PRESET" -B "$BUILD_DIR"
    say "build $TARGET"
    cmake --build "$BUILD_DIR" --target "$TARGET" -j"$(nproc 2>/dev/null || echo 4)"
fi

# ── 3. 产物校验 ─────────────────────────────────────────────────────────────
OUT_DIR="$BUILD_DIR/examples"
[ -f "$OUT_DIR/$TARGET.html" ] || die "缺少 $OUT_DIR/$TARGET.html（先不带 --skip-build 跑一次）"
[ -f "$OUT_DIR/$TARGET.wasm" ] || die "缺少 $OUT_DIR/$TARGET.wasm"
say "artifacts:"
for f in "$OUT_DIR/$TARGET".{html,js,wasm,data}; do
    [ -f "$f" ] && say "  $(printf '%-24s' "$(basename "$f")") $(du -h "$f" | cut -f1)"
done
[ -f "$FONT_DIR/font.ttf" ] || say "  (无 font.ttf：文本不可见)"

# ── 4. headless 浏览器验证 ───────────────────────────────────────────────────
# probe.html（与 shell.html 同目录）会被 web_verify.mjs 拷到产物目录旁。
if [ "$DO_VERIFY" = 1 ]; then
    command -v google-chrome >/dev/null 2>&1 || command -v /usr/bin/google-chrome >/dev/null 2>&1 \
        || die "验证需要 google-chrome"
    say "verify $TARGET（CDP，等待 .data 下载 + 首帧）"
    node "$ROOT/scripts/web_verify.mjs" --dir "$OUT_DIR" --port "$VERIFY_PORT" \
        --timeout 150 --click 60,567 --shot "$BUILD_DIR/web_verify.png" \
        || die "浏览器验证未通过（见上面的 JSON 报告）"
    say "screenshot: $BUILD_DIR/web_verify.png"
fi

# ── 5. 托管 ─────────────────────────────────────────────────────────────────
URL="http://localhost:$SERVE_PORT/$TARGET.html"
if [ "$DO_SERVE" = 1 ]; then
    say "serving $OUT_DIR → $URL"
    exec python3 -m http.server "$SERVE_PORT" --directory "$OUT_DIR"
fi
say "done. 运行：python3 -m http.server $SERVE_PORT --directory $OUT_DIR"
say "      打开 $URL"
