#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# 构建 Android APK（arm64-v8a）。
#
#   bash scripts/package_apk.sh                 # 配置 + 编译 + 打包 + 签名
#   bash scripts/package_apk.sh --skip-build    # 只重新打包已有 .so
#   bash scripts/package_apk.sh --out a.apk     # 指定产物
#
# 链路：
#   1. cmake --preset android-arm64-release -B build-android-apk
#      （NDK 交叉编译 cui_core-core / cui_core-wx + packages 静态库，再链成 libcui_gallery.so）
#   2. aapt2 link    → base.apk（manifest，无 res）
#   3. javac + d8    → classes.dex（GalleryActivity + framework 的 4 个 Java 类）
#   4. zip           → 塞入 classes.dex 与 lib/arm64-v8a/libcui_gallery.so
#   5. zipalign + apksigner（$BUILD_DIR/debug.keystore，首次运行自动生成）
#
# 两个与桌面构建不同的开关：
#   * CMAKE_BUILD_TYPE=Release：vcpkg 的 arm64-android 只安装 release 版静态库
#     （installed/arm64-android/lib），Debug 消费方的 IMPORTED_LOCATION_DEBUG 指向
#     不存在的 debug/lib，链接必然报 "no such file or directory: libskia.a"。
#   * VCPKG_INSTALLED_DIR=<build>/vcpkg_installed：manifest 安装会删掉同根下不在
#     当前 manifest 里的包（x64-linux ↔ arm64-android 会互相掏），单独一个根才
#     不会把桌面构建的依赖清掉。
#
# 宿主代码分两处，打包时分别收集：
#   cui_core/src/platform/android/  平台实现：cpp/main.cpp + java/dev/cui_core/framework 平台桥
#   examples/platform/android/     app 资产：AndroidManifest.xml + GalleryActivity.java
# app 目标（libcui_gallery.so）的构建定义在 examples/CMakeLists.txt（应用层）；
# 引擎目录不定义也不编译 app 目标。
#
# 不用 Gradle：整个 APK 只需要 aapt2/javac/d8/zipalign/apksigner，全部在 SDK
# 的 build-tools 里，构建结果可复现且不依赖网络。
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$ROOT/cui_core/src/platform/android"
APP_ASSETS_DIR="$ROOT/examples/platform/android"
BUILD_DIR="$ROOT/build-android-apk"
OUT="$ROOT/build-android-apk/cui-gallery.apk"
DO_BUILD=1
ABI="arm64-v8a"

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) DO_BUILD=0; shift ;;
        --out) OUT="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --abi) ABI="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

# ── SDK / build-tools / NDK ─────────────────────────────────────────────────
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -n "$SDK" ] && [ -d "$SDK" ] || die "找不到 Android SDK：设置 ANDROID_HOME 或 ANDROID_SDK_ROOT"
[ -d "$SDK/build-tools" ] || die "$SDK/build-tools 不存在（sdkmanager --install 'build-tools;36.0.0'）"
[ -d "$SDK/platforms" ] || die "$SDK/platforms 不存在（sdkmanager --install 'platforms;android-34'）"

BT_VERSION="$(ls "$SDK/build-tools" | sort -V | tail -1)"
BT="$SDK/build-tools/$BT_VERSION"
PLATFORM_VERSION="$(ls "$SDK/platforms" | sort -V | tail -1)"
ANDROID_JAR="$SDK/platforms/$PLATFORM_VERSION/android.jar"
[ -f "$ANDROID_JAR" ] || die "缺少 $ANDROID_JAR"

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    NDK_VERSION="$(ls "$SDK/ndk" 2>/dev/null | sort -V | tail -1 || true)"
    [ -n "$NDK_VERSION" ] || die "找不到 NDK：sdkmanager --install 'ndk;<版本>'（需 r28+，原因见下方 javac 段的 glob 说明）"
    export ANDROID_NDK_HOME="$SDK/ndk/$NDK_VERSION"
fi

say "SDK:        $SDK"
say "build-tools:$BT_VERSION   platform: $PLATFORM_VERSION   NDK: $ANDROID_NDK_HOME"

# ── 1. native 侧 ────────────────────────────────────────────────────────────
if [ "$DO_BUILD" = 1 ]; then
    export TMPDIR="$BUILD_DIR/tmp"
    mkdir -p "$TMPDIR"
    say "configure (android-arm64-release → $BUILD_DIR)"
    cmake --preset android-arm64-release -B "$BUILD_DIR" \
        -DCUI_BUILD_EXAMPLES=OFF -DCUI_BUILD_IDE=OFF -DCUI_BUILD_TESTS=OFF \
        -DCUI_BUILD_ANDROID_APP=ON \
        -DVCPKG_INSTALLED_DIR="$BUILD_DIR/vcpkg_installed"
    say "build cui_gallery"
    cmake --build "$BUILD_DIR" --target cui_gallery -j"$(nproc 2>/dev/null || echo 4)"
fi

# 根 CMakeLists 把库产物统一落到 CUI_DIST_DIR（默认源码根 dist/），不在构建树
# 的子目录里；app .so 现在也由 examples/CMakeLists.txt 定义。
DIST_DIR="$(sed -n 's/^CUI_DIST_DIR:PATH=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -1)"
[ -n "$DIST_DIR" ] || DIST_DIR="$ROOT/dist"
LIB="$DIST_DIR/libcui_gallery.so"
[ -f "$LIB" ] || die "缺少 $LIB（先不带 --skip-build 跑一次）"
say "native lib: $LIB"

# ── 2. 资源 / manifest ──────────────────────────────────────────────────────
WORK="$BUILD_DIR/apk-work"
rm -rf "$WORK"
mkdir -p "$WORK/gen" "$WORK/classes" "$WORK/dex" "$WORK/lib/$ABI"

say "aapt2 link"
"$BT/aapt2" link \
    -o "$WORK/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$APP_ASSETS_DIR/AndroidManifest.xml" \
    --java "$WORK/gen" \
    --min-sdk-version 28 \
    --target-sdk-version 34 \
    --version-code 1 \
    --version-name 1.0 \
    --auto-add-overlay

# ── 3. Java → dex ───────────────────────────────────────────────────────────
say "javac"
# 平台桥（引擎层）与 GalleryActivity（应用层）分处两个源根，一起交给 javac。
mapfile -t JAVA_FILES < <(find "$WORK/gen" "$APP_DIR/java" "$APP_ASSETS_DIR/java" -name '*.java')
[ "${#JAVA_FILES[@]}" -gt 0 ] || die "没有 Java 源文件"
# JDK 9+ 不再接受 "-source 11 -bootclasspath"（bootclasspath 只留给 -source 8），
# 而 android.jar 本来就能当普通 classpath 用：android.* 从这里解析，java.* 仍走
# 当前 JDK（minSdk 28 的 API 面由 d8 --min-api 28 兜）。
# minSdk 为什么是 28 而不是 24：vcpkg 那份 arm64-android 的 ffmpeg 探到了
# HAVE_GLOB=1（libavformat 的 img2dec 引用 glob/globfree），而 bionic 只在
# **API 28 起**的 libc.so 里提供这两个符号 —— 按 android-24 链会在链接期就报
# undefined symbol。NDK r27 时代探不出 glob，升 r28+ 后探测口径变了。
# 抬下限比给 ffmpeg 打补丁（overlay port 钉住 API 再重编一份）便宜得多；
# 真需要兼容 Android 7/8 时，走那条：port 里把 --target=aarch64-linux-android24
# 喂进 ffmpeg 的 configure，让 HAVE_GLOB 探成 0（img2dec 自己有「不支持」分支）。
javac -source 11 -target 11 -encoding UTF-8 -Xlint:-options \
    -classpath "$ANDROID_JAR" \
    -d "$WORK/classes" \
    "${JAVA_FILES[@]}" >"$WORK/javac.log" 2>&1 || {
    cat "$WORK/javac.log" >&2
    die "javac 失败"
}
[ -n "$(find "$WORK/classes" -name '*.class' -print -quit)" ] || die "javac 没有产出 class"

say "d8"
jar cf "$WORK/classes.jar" -C "$WORK/classes" .
"$BT/d8" --lib "$ANDROID_JAR" --min-api 28 --release \
    --output "$WORK/dex" "$WORK/classes.jar"
[ -f "$WORK/dex/classes.dex" ] || die "d8 没产出 classes.dex"

# ── 4. 组装 APK ─────────────────────────────────────────────────────────────
say "assemble"
cp "$WORK/base.apk" "$WORK/unsigned.apk"
cp "$LIB" "$WORK/lib/$ABI/libcui_gallery.so"

# 静态链接进来的 skia/freetype 带 debug 段，未 strip 的 .so 有 180MB+；
# 只 strip 装进 APK 的副本，构建目录里的产物保持可符号化。
STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
if [ -x "$STRIP" ]; then
    "$STRIP" --strip-unneeded "$WORK/lib/$ABI/libcui_gallery.so"
else
    say "warn: 找不到 llvm-strip，APK 会因未剥离符号而偏大"
fi

# native 库用 -0（不压缩）：APK 里 store 状态可以直接 mmap，
# Android 12+ 对压缩的 .so 会先解到数据目录，启动更慢也更占空间。
(cd "$WORK" && zip -q -X -0 unsigned.apk "lib/$ABI/libcui_gallery.so")
(cd "$WORK/dex" && zip -q -X ../unsigned.apk classes.dex)

say "zipalign"
"$BT/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"

# ── 5. 签名 ─────────────────────────────────────────────────────────────────
KS="$BUILD_DIR/debug.keystore"
if [ ! -f "$KS" ]; then
    say "生成 debug keystore: $KS"
    KEYTOOL="$(command -v keytool || true)"
    [ -n "$KEYTOOL" ] || KEYTOOL="${JAVA_HOME:-/usr}/bin/keytool"
    "$KEYTOOL" -genkeypair -v \
        -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi

mkdir -p "$(dirname "$OUT")"
say "apksigner sign → $OUT"
"$BT/apksigner" sign \
    --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey --min-sdk-version 28 \
    --out "$OUT" "$WORK/aligned.apk"

"$BT/apksigner" verify --verbose "$OUT" | sed -n '1,6p'
"$BT/aapt2" dump badging "$OUT" | sed -n '1,4p'
say "done: $OUT  ($(du -h "$OUT" | cut -f1))"
