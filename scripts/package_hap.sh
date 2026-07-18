#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# 构建 OpenHarmony HAP（arm64-v8a）。
#
#   bash scripts/package_hap.sh                  # 配置 + 编 libcui_core.so + 打包 + 签名
#   bash scripts/package_hap.sh --skip-build     # 只重新打包已有的 libcui_core.so
#   bash scripts/package_hap.sh --no-sign        # 出未签名 hap（查 PackingCheck 用）
#   bash scripts/package_hap.sh --out x.hap      # 指定产物
#
# 链路：
#   1. cmake -G Ninja（OHOS NDK 工具链 + vcpkg arm64-ohos）→ libcui_core.so
#      （构建定义在 examples/CMakeLists.txt，napi 模块名 "cui_core"）
#   2. 把工程逐字节拷到**本地路径**的暂存目录，并放好 entry/libs/arm64-v8a/
#      {libcui_core.so, libc++_shared.so}
#   3. hvigorw assembleHap → entry-default-unsigned.hap
#   4. hap-sign-tool 本机自造签名链（Root CA → sub CA → 应用/profile 叶子），
#      再 sign-profile（profile 模板里的 development-certificate 换成自己的应用
#      叶子）→ cui_core-debug-profile.p7b；最后 sign-app → *-signed.hap
#   5. verify-app 复核签名与 profile
#
# 三个必须记住的口径：
#   * **hvigor 拒绝 UNC / 网络映射盘工程路径**（error 00306003 project path
#     validation），所以第 2 步先拷到本地盘再构建；仓里那份只当源码。
#   * **不启用 hvigor 的 externalNativeOptions**（entry/build-profile.json5 里那段
#     注释说明为什么）：那会让 assembleHap 用 DevEco 自带的工具链现编 native 源码，
#     而 cui_core 的依赖（skia / harfbuzz / icu …）必须由仓里的 vcpkg 交叉根提供。
#     这条路是「cmake 先出 .so → 拷进 libs/ → hvigor 只负责打包」。
#   * **libc++_shared.so 必须随包**：NDK 默认 STL 是 shared（NEEDED 里就有它），
#     而它不在系统镜像的 public 命名空间里，装到设备上会 dlopen 失败。
#
# 签名材料是**本机现造的自签 CA**（口令 CUI_HAP_KEY_PWD，默认写在脚本里，只当门闩
# 不当秘密），存在 $BUILD_DIR/hap-sign/ 下、不进仓；SDK 自带的 OpenHarmony.p12 用
# 不了，原因写在第 4 步开头。正式发布要换成自己的 CA 与 profile。profile 里的
# device-ids 决定真机能否安装：换成自己那台的 UDID（hdc shell bm get --udid）。
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJ_SRC="$ROOT/cui_core/src/platform/ohos"
BUILD_DIR="${CUI_OHOS_BUILD_DIR:-$ROOT/build-ohos-arm64}"
STAGING="${CUI_HAP_STAGING:-}"
OUT="${CUI_HAP_OUT:-$ROOT/build-hap-out/cui-core-story-signed.hap}"
ABI="arm64-v8a"
DO_BUILD=1
DO_SIGN=1
BUILD_MODE=debug
PRODUCT=default

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) DO_BUILD=0; shift ;;
        --no-sign) DO_SIGN=0; shift ;;
        --build-mode) BUILD_MODE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --staging) STAGING="$2"; shift 2 ;;
        --abi) ABI="$2"; shift 2 ;;
        -h|--help) sed -n '2,35p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

# ── 工具链定位 ──────────────────────────────────────────────────────────────
NDK="${OHOS_NDK_HOME:-}"
[ -n "$NDK" ] && [ -d "$NDK" ] || die "找不到 OHOS NDK：设置 OHOS_NDK_HOME（指向 SDK 的 native/）"
[ -f "$NDK/build/cmake/ohos.toolchain.cmake" ] || die "$NDK 里没有 ohos.toolchain.cmake，确认它指向 native/ 而不是上一层"
# vcpkg 自己的 scripts/toolchains/ohos.cmake 在 detect_compiler 时只认 OHOS_SDK_ROOT
# （= native/ 的上一级）；cmake/platforms/ohos.cmake 也能推，但这里推一遍让报错更早。
export OHOS_SDK_ROOT="${OHOS_SDK_ROOT:-$(cd "$NDK/.." && pwd)}"

VCPKG="${VCPKG_ROOT:-}"
[ -n "$VCPKG" ] && [ -f "$VCPKG/scripts/buildsystems/vcpkg.cmake" ] || die "找不到 vcpkg：设置 VCPKG_ROOT"

# DevEco Studio 在这里同时是 node / hvigor / java / SDK 的来源；没有它就只能出
# 未签名的中间产物，所以整段打包必须有。
DEVECO="${DEVECO_HOME:-}"
if [ -z "$DEVECO" ]; then
    for cand in "/d/Program Files/devecostudio" "/mnt/d/Program Files/devecostudio" \
                "/c/Program Files/Huawei/DevEco Studio" "$HOME/devtools/devecostudio"; do
        [ -d "$cand/tools/hvigor/bin" ] && DEVECO="$cand" && break
    done
fi
[ -n "$DEVECO" ] && [ -d "$DEVECO" ] || die "找不到 DevEco Studio：设置 DEVECO_HOME（要含 tools/hvigor/bin 与 sdk）"

JAVA="$DEVECO/jbr/bin/java"
[ -x "$JAVA" ] || [ -f "$JAVA.exe" ] || die "$JAVA 不存在"
HVIGOR="$DEVECO/tools/hvigor/bin/hvigorw"
[ -f "$HVIGOR" ] || [ -f "$HVIGOR.bat" ] || die "找不到 hvigorw：$HVIGOR"
[ -d "$DEVECO/tools/node" ] && export NODE_HOME="$DEVECO/tools/node"
export DEVECO_SDK_HOME="${DEVECO_SDK_HOME:-$DEVECO/sdk}"
TOOLCHAINS="$DEVECO/sdk/default/openharmony/toolchains"
[ -d "$TOOLCHAINS/lib" ] || die "$TOOLCHAINS/lib 不存在（DevEco 的 SDK 没装 openharmony 部分？）"
SIGN_LIB="$TOOLCHAINS/lib"
HAP_SIGN_TOOL="$SIGN_LIB/hap-sign-tool.jar"
PROFILE_TEMPLATE="$SIGN_LIB/UnsgnedDebugProfileTemplate.json"
[ -f "$HAP_SIGN_TOOL" ] && [ -f "$PROFILE_TEMPLATE" ] || die "签名工具不全：$HAP_SIGN_TOOL / $PROFILE_TEMPLATE"

# hvigor 的 PackageHap 任务内部 spawn java，PATH 里没有就报 "spawn java ENOENT"。
export PATH="$DEVECO/jbr/bin:$DEVECO/tools/node:$DEVECO/tools/hvigor/bin:${PATH}"

# ── 1. 交叉编译 libcui_core.so ───────────────────────────────────────────────────
LIBCUI=""
if [ "$DO_BUILD" = 1 ]; then
    say "configure（arm64-ohos，build dir=$BUILD_DIR）"
    CMAKE_ARGS=(
        -S "$ROOT" -B "$BUILD_DIR" -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG/scripts/buildsystems/vcpkg.cmake"
        -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$NDK/build/cmake/ohos.toolchain.cmake"
        -DVCPKG_TARGET_TRIPLET=arm64-ohos
        -DVCPKG_INSTALLED_DIR="${VCPKG_INSTALLED_DIR:-$BUILD_DIR/vcpkg_installed}"
        -DOHOS_ARCH=arm64-v8a
        -DOHOS_PLATFORM=OHOS
    )
    # Windows 主机还需要一条把 icu/harfbuzz 的宿主侧问题绕开的 scratch triplet
    # （内容进 ABI 哈希，指回 community 那份就会重编 skia）；非 Windows 主机留空。
    [ -n "${VCPKG_OVERLAY_TRIPLETS:-}" ] && CMAKE_ARGS+=(-DVCPKG_OVERLAY_TRIPLETS="$VCPKG_OVERLAY_TRIPLETS")
    # shellcheck disable=SC2086
    [ -n "${CUI_EXTRA_CMAKE_ARGS:-}" ] && CMAKE_ARGS+=(${CUI_EXTRA_CMAKE_ARGS})
    cmake "${CMAKE_ARGS[@]}"
    say "build libcui_core.so"
    cmake --build "$BUILD_DIR" --target cui_ohos_host --parallel "${CUI_JOBS:-2}"
fi

# 库产物统一落在 CUI_DIST_DIR（默认源码根 dist/），不在构建树子目录里。
DIST_DIR="$(sed -n 's/^CUI_DIST_DIR:PATH=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -1)"
[ -n "$DIST_DIR" ] || DIST_DIR="$ROOT/dist"
LIBCUI="$DIST_DIR/libcui_core.so"
[ -f "$LIBCUI" ] || die "找不到 libcui_core.so（先跑一次不带 --skip-build）：$DIST_DIR/"
say "native 库：$LIBCUI（$(stat -c %s "$LIBCUI") bytes）"

STL="$NDK/llvm/lib/aarch64-linux-ohos/libc++_shared.so"
[ -f "$STL" ] || [ ! "$ABI" = "arm64-v8a" ] || die "找不到 $STL（libc++_shared.so 必须随包）"

# ── 2. 暂存工程（hvigor 只认本地路径）───────────────────────────────────────
if [ -z "$STAGING" ]; then
    # 仓在本地盘时直接就地构建；Windows 上的映射盘/UNC 一律挪到本地盘。
    case "$ROOT" in
        //*) STAGING="${CUI_OHOS_SCRATCH:-/c/tmp/cui-core-hap}" ;;
        *)
            if [ "${OS:-}" = "Windows_NT" ]; then
                STAGING="${CUI_OHOS_SCRATCH:-/c/tmp/cui-core-hap}"
            else
                STAGING="$ROOT/build-ohos-hap"
            fi
            ;;
    esac
fi
say "暂存工程 → $STAGING"
mkdir -p "$STAGING"
# 与仓内那份逐字节同构（排除构建产物），构建目录只存在于暂存侧。
(cd "$PROJ_SRC" && tar cf - --exclude=./build --exclude=.hvigor --exclude=oh_modules --exclude=.preview .) |
    (cd "$STAGING" && tar xf -)
[ -f "$STAGING/build-profile.json5" ] || die "暂存目录里没有 build-profile.json5"

LIBS_DIR="$STAGING/entry/libs/$ABI"
mkdir -p "$LIBS_DIR"
STRIP="$NDK/llvm/bin/llvm-strip"
if [ -x "$STRIP" ] || [ -f "$STRIP.exe" ]; then
    cp "$LIBCUI" "$LIBS_DIR/libcui_core.so"
    "$STRIP" --strip-unneeded "$LIBS_DIR/libcui_core.so"
else
    say "没有 llvm-strip，直接拷未 strip 的 .so（体积会大一个量级）"
    cp "$LIBCUI" "$LIBS_DIR/libcui_core.so"
fi
[ -f "$STL" ] && cp "$STL" "$LIBS_DIR/libc++_shared.so"
say "libs：$(ls "$LIBS_DIR" | tr '\n' ' ')"

# ── 3. hvigor assembleHap ───────────────────────────────────────────────────
say "hvigorw assembleHap（product=$PRODUCT buildMode=$BUILD_MODE）"
HVIGOR_ARGS=(--mode module -p product="$PRODUCT" -p buildMode="$BUILD_MODE" assembleHap --no-daemon)
# hvigor 的 PackageHap 任务用 node 的 spawn 起 java 打包，而 **node.exe 只认 Windows
# 形式的 PATH 条目**：从 Git Bash 直接跑时 MSYS 给的 PATH 是 /d/Program Files/... 这种
# POSIX 串，node 眼里全是无效目录 ⇒ "spawn java ENOENT"（Code 00308018）。
# 所以这里把整条 PATH 转成 Windows 形式再交给 cmd 起 hvigorw.bat。
if [ "${OS:-}" = "Windows_NT" ] && command -v cygpath >/dev/null 2>&1; then
    CMD_EXE="$(command -v cmd.exe 2>/dev/null || command -v cmd || true)"
    [ -n "$CMD_EXE" ] || die "找不到 cmd.exe（Git Bash 里应在 /c/Windows/System32）"
    # 整条 PATH 逐条目转 Windows 形式。**不能用 cygpath -wp**：它会先按 ':' 切分，
    # 于是 DEVECO_HOME 这种 "D:/Program Files/…" 写法（脚本前面 export 进 PATH 的就是
    # 这个形态）被切成 "D" + "/Program Files/…"，实测产出
    # `D;D:\Program Files\Git\Program Files\devecostudio\tools\node` 这种串——node.exe
    # 就找不到了（hvigorw.bat 的 execute 段只 call 裸 "node.exe"，靠 PATH 解析）。
    WIN_NODE="$(cygpath -w "$DEVECO/tools/node")"
    WIN_PATH="$(cygpath -w "$DEVECO/jbr/bin");$WIN_NODE;$(cygpath -w "$DEVECO/tools/hvigor/bin")"
    _old_ifs="$IFS"; IFS=:
    for _p in $PATH; do
        [ -n "$_p" ] || continue
        case "$_p" in
            [A-Za-z]:*) _w="$(printf '%s' "$_p" | tr / \\)" ;;
            /*)         _w="$(cygpath -w "$_p" 2>/dev/null || true)" ;;
            *)          _w="" ;;
        esac
        [ -n "$_w" ] && WIN_PATH="$WIN_PATH;$_w"
    done
    IFS="$_old_ifs"
    unset _old_ifs _p _w
    case "$WIN_PATH$WIN_NODE" in *%*) die "PATH/NODE_HOME 里有 % 号，cmd 批处理会把它当变量展开" ;; esac
    # 内联 cmd 也不行：MSYS 会给整条参数再包一层引号并把内层引号转义成 \"，而 cmd
    # 不认 \"，于是 set \"PATH=…\" 变成给名为 "PATH 的变量赋值（实测 PATH 原封不动，
    # 报 "'hvigorw.bat' 不是内部或外部命令"）。落到 .bat 文件里就没有这层引号可啃。
    WRAPPER="$STAGING/.cui-hvigor.bat"
    {
        printf '@echo off\r\n'
        printf 'set "PATH=%s"\r\n' "$WIN_PATH"
        printf 'set "NODE_HOME=%s"\r\n' "$WIN_NODE"
        printf 'set "DEVECO_SDK_HOME=%s"\r\n' "$(cygpath -w "${DEVECO_SDK_HOME:-$DEVECO/sdk}")"
        printf 'cd /d "%s"\r\n' "$(cygpath -w "$STAGING")"
        printf 'call "%s" --mode module -p product=%s -p buildMode=%s assembleHap --no-daemon\r\n' \
            "$(cygpath -w "$HVIGOR.bat")" "$PRODUCT" "$BUILD_MODE"
        printf 'exit /b %%ERRORLEVEL%%\r\n'
    } > "$WRAPPER"
    "$CMD_EXE" //c "$(cygpath -w "$WRAPPER")"
    RC=$?
    rm -f "$WRAPPER"
    [ "$RC" = 0 ] || die "hvigorw 失败（rc=$RC）"
else
    [ -x "$HVIGOR" ] || die "$HVIGOR 不可执行（chmod +x 或改用 hvigorw 的发行版包装）"
    (cd "$STAGING" && "$HVIGOR" "${HVIGOR_ARGS[@]}")
fi

UNSIGNED="$(find "$STAGING/entry/build" -name '*-unsigned.hap' -type f 2>/dev/null | head -1)"
[ -n "$UNSIGNED" ] || die "assembleHap 没产出 *-unsigned.hap（看 hvigor 输出里的 PackingCheck）"
say "未签名 hap：$UNSIGNED（$(stat -c %s "$UNSIGNED") bytes）"

[ "$DO_SIGN" = 1 ] || { say "--no-sign：产物留在 $UNSIGNED"; exit 0; }

# ── 4. profile + hap 签名 ───────────────────────────────────────────────────
SIGN_ALG="SHA256withECDSA"
# .hap 签名要写「最低兼容 api」，与 build-profile.json5 的 compatibleSdkVersion 同口径。
COMPAT_API="${CUI_HAP_COMPAT_API:-12}"
# 口令只是本机开发材料的门闩，不是秘密：工具要求 ≥8 位且字母+数字。
STORE_PWD="${CUI_HAP_KEY_PWD:-1234567890a}"

BUNDLE="$(sed -n 's/.*"bundleName"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$STAGING/AppScope/app.json5" | head -1)"
[ -n "$BUNDLE" ] || die "AppScope/app.json5 里读不到 bundleName"

# 材料放在构建目录旁边（不在仓里），跨次运行复用；CUI_HAP_REGENERATE_KEYS=1 换密钥。
SIGN_DIR="${CUI_HAP_SIGN_DIR:-$BUILD_DIR/hap-sign}"
mkdir -p "$SIGN_DIR"
KEYSTORE="$SIGN_DIR/cui-debug.p12"
ROOT_CA="$SIGN_DIR/root-ca.cer"
APP_CA="$SIGN_DIR/app-ca.cer"
PROFILE_CA="$SIGN_DIR/profile-ca.cer"
APP_CERT="$SIGN_DIR/app-cert.cer"
PROFILE_CERT="$SIGN_DIR/profile-cert.cer"
PROFILE_JSON="$SIGN_DIR/cui-debug-profile.json"
PROFILE_P7B="$SIGN_DIR/cui-debug-profile.p7b"
SIGNED="$SIGN_DIR/cui-gallery-signed.hap"

SUBJ_ROOT="C=CN,O=cui,OU=cui Dev,CN=cui Root CA"
SUBJ_APP_CA="C=CN,O=cui,OU=cui Dev,CN=cui Application Debug CA"
SUBJ_APP="C=CN,O=cui,OU=cui Dev,CN=cui Gallery Debug"
SUBJ_PROFILE_CA="C=CN,O=cui,OU=cui Dev,CN=cui Profile Debug CA"
SUBJ_PROFILE="C=CN,O=cui,OU=cui Dev,CN=cui Profile Debug"

# 为什么不用 SDK 自带的 OpenHarmony.p12（试过的两条路都堵死）：
#   * 它 "openharmony application release" 条目里的证书是**自签**的
#     （subject==issuer，序列号 2b584686），而 profile 模板内嵌的那条
#     development-certificate 是「Application CA 签发的 Release」（序列号 68e0bfcc），
#     公钥根本不是同一个；
#   * 单证书签 ⇒ 11013004 "Profile cert must a cert chain"；把
#     [Release, App CA, Root CA] 拼上 ⇒ 11013002 "verify certificate chain failed!
#     Signature does not match"（Release 不是 App CA 签的）。
# 所以这里走 DevEco 离线自动签名同一套：用 hap-sign-tool 现造
# Root CA → 两个 sub CA → 应用/profile 叶子 的三级链，并把 profile 模板里的
# development-certificate 换成我们自己的应用叶子。密钥一旦生成就复用
# （generate-keypair 明确拒绝覆盖已存在的 alias），证书每次重签。
sign_tool() {
    "$JAVA" -jar "$HAP_SIGN_TOOL" "$@" \
        -keystoreFile "$KEYSTORE" -keystorePwd "$STORE_PWD" -pwdInputMode 0 \
        2>&1 | grep -aiE "error|Error Message|success" | tail -4 || true
}

if [ ! -s "$KEYSTORE" ] || [ "${CUI_HAP_REGENERATE_KEYS:-0}" = 1 ]; then
    say "生成签名密钥（$KEYSTORE）"
    rm -f "$KEYSTORE"
    for alias in cui-core-root-ca-key-v1 cui-core-app-ca-key-v1 cui-core-app-key-v1 \
                 cui-core-profile-ca-key-v1 cui-core-profile-key-v1; do
        sign_tool generate-keypair -keyAlias "$alias" -keyPwd "$STORE_PWD" \
            -keyAlg ECC -keySize NIST-P-256
        [ -s "$KEYSTORE" ] || die "generate-keypair $alias 失败"
    done
fi

say "签发 CA 与叶子证书"
sign_tool generate-ca -keyAlias cui-core-root-ca-key-v1 -keyPwd "$STORE_PWD" \
    -keyAlg ECC -keySize NIST-P-256 -subject "$SUBJ_ROOT" -validity 3650 \
    -signAlg "$SIGN_ALG" -outFile "$ROOT_CA"
sign_tool generate-ca -keyAlias cui-core-app-ca-key-v1 -keyPwd "$STORE_PWD" \
    -keyAlg ECC -keySize NIST-P-256 -issuer "$SUBJ_ROOT" \
    -issuerKeyAlias cui-core-root-ca-key-v1 -issuerKeyPwd "$STORE_PWD" \
    -subject "$SUBJ_APP_CA" -validity 3650 -signAlg "$SIGN_ALG" -outFile "$APP_CA"
sign_tool generate-ca -keyAlias cui-core-profile-ca-key-v1 -keyPwd "$STORE_PWD" \
    -keyAlg ECC -keySize NIST-P-256 -issuer "$SUBJ_ROOT" \
    -issuerKeyAlias cui-core-root-ca-key-v1 -issuerKeyPwd "$STORE_PWD" \
    -subject "$SUBJ_PROFILE_CA" -validity 3650 -signAlg "$SIGN_ALG" -outFile "$PROFILE_CA"
# -outForm certChain 出 [叶子, sub CA, Root CA] 三块——sign-app 要的就是这个顺序。
sign_tool generate-app-cert -keyAlias cui-core-app-key-v1 -keyPwd "$STORE_PWD" \
    -issuer "$SUBJ_APP_CA" -issuerKeyAlias cui-core-app-ca-key-v1 -issuerKeyPwd "$STORE_PWD" \
    -subject "$SUBJ_APP" -validity 3650 -signAlg "$SIGN_ALG" \
    -outForm certChain -rootCaCertFile "$ROOT_CA" -subCaCertFile "$APP_CA" \
    -outFile "$APP_CERT"
sign_tool generate-profile-cert -keyAlias cui-core-profile-key-v1 -keyPwd "$STORE_PWD" \
    -issuer "$SUBJ_PROFILE_CA" -issuerKeyAlias cui-core-profile-ca-key-v1 -issuerKeyPwd "$STORE_PWD" \
    -subject "$SUBJ_PROFILE" -validity 3650 -signAlg "$SIGN_ALG" \
    -outForm certChain -rootCaCertFile "$ROOT_CA" -subCaCertFile "$PROFILE_CA" \
    -outFile "$PROFILE_CERT"
for f in "$ROOT_CA" "$APP_CA" "$PROFILE_CA" "$APP_CERT" "$PROFILE_CERT"; do
    [ -s "$f" ] || die "证书没出齐：$f 缺失（看上面的 java 输出）"
done

# profile 里三处必须跟着改：bundle-name（设备侧直接比对被签 hap）、
# development-certificate（= 上面那条应用叶子）、validity（模板里那份 2023-12 就
# 过期了）。base64 字母表没有 | 也没有 &，所以 sed 用 | 当分隔符、无需再转义。
LEAF_PEM="$(awk '/BEGIN CERT/{p=1} p{printf "%s\\n", $0} /END CERT/{exit}' "$APP_CERT")"
NOW="$(date +%s)"
sed -e "s|\"bundle-name\": \"[^\"]*\"|\"bundle-name\": \"$BUNDLE\"|" \
    -e "s|\"development-certificate\": \"[^\"]*\"|\"development-certificate\": \"$LEAF_PEM\"|" \
    -e "s|\"not-before\": [0-9]*|\"not-before\": $NOW|" \
    -e "s|\"not-after\": [0-9]*|\"not-after\": $((NOW + 3600 * 24 * 3650))|" \
    "$PROFILE_TEMPLATE" > "$PROFILE_JSON"

# 模板里的 debug-info.device-ids 是 OpenHarmony 社区的两条测试 UDID：模拟器/开发者
# 镜像通常已收录，**真机要换成自己的 UDID**（hdc shell bm get --udid）。这里不做
# 自动替换：多行 JSON 数组用 sed 改容易出错文件，宁可让脚本失败得明确。
[ -s "$PROFILE_JSON" ] || die "profile JSON 生成失败"

say "sign-profile → $(basename "$PROFILE_P7B")"
sign_tool sign-profile -mode localSign -keyAlias cui-core-profile-key-v1 -keyPwd "$STORE_PWD" \
    -profileCertFile "$PROFILE_CERT" -inFile "$PROFILE_JSON" -signAlg "$SIGN_ALG" \
    -outFile "$PROFILE_P7B"
[ -s "$PROFILE_P7B" ] || die "sign-profile 没产出 $PROFILE_P7B（看上面的 java 输出）"

say "sign-app → $(basename "$SIGNED")"
sign_tool sign-app -mode localSign -keyAlias cui-core-app-key-v1 -keyPwd "$STORE_PWD" \
    -appCertFile "$APP_CERT" -profileFile "$PROFILE_P7B" -profileSigned 1 \
    -inFile "$UNSIGNED" -signAlg "$SIGN_ALG" \
    -compatibleVersion "$COMPAT_API" -signCode 1 -outFile "$SIGNED"
[ -s "$SIGNED" ] || die "sign-app 没产出 $SIGNED"

mkdir -p "$(dirname "$OUT")"
cp "$SIGNED" "$OUT"

# verify-app 的 -outProfile 只认 .p7b 结尾（.json/.pem/.cer 一律 11012005 Not support file）。
say "verify-app"
"$JAVA" -jar "$HAP_SIGN_TOOL" verify-app -inFile "$OUT" \
    -outCertChain "$SIGN_DIR/verify-certchain.cer" -outProfile "$SIGN_DIR/verify-profile.p7b" \
    2>&1 | grep -aiE "error|fail|success|result|INFO" | tail -5 || true

say "产物：$OUT（$(stat -c %s "$OUT") bytes）"
say "装机：\"$TOOLCHAINS/bin/hdc\" install -r \"$OUT\" && \"$TOOLCHAINS/bin/hdc\" shell aa start -b $BUNDLE -a EntryAbility"
say "注意：根 CA（$ROOT_CA）是本机自签的，不在系统镜像的信任列表里——真机放行与否未实测。"
