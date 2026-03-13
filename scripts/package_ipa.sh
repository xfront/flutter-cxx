#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# package_ipa.sh — 把 cui_core 引擎 + gallery 应用层打包成 iOS IPA。
#
# 流程（对齐 package_apk.sh / package_hap.sh 的结构）：
#   1. CMake（ios preset）把引擎编成 libcui-core.a，把应用层（cui_app_root）
#      编成 libcui_app.a；
#   2. XcodeGen 用 flutter/src/platform/ios/project.yml 生成 ios.xcodeproj；
#   3. xcodebuild archive → exportArchive 出 IPA。
#
# 用法：
#   ./scripts/package_ipa.sh [--simulator] [--configuration Debug|Release]
#                            [--team TEAMID] [--clean] [--help]
#
#   --simulator   免签名打到 iOS Simulator（不需要开发者证书）；
#   --team TEAMID 真机/分发用的开发团队 ID（CODE_SIGN_STYLE=Manual 时必填）；
#   --configuration  默认 Release（也可 Debug，对应 ios-arm64-debug preset）；
#   --clean       重新 configure 构建目录。
#
# ⚠️ 本脚本只能在 macOS + 已装 Xcode + 已装 XcodeGen（brew install xcodegen）
#    的环境运行；它的存在是为了让 iOS 打包流程与 apk/hap 一致，CI 在 Mac 上跑。
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SIMULATOR=0
CONFIG=Release
TEAM=""
CLEAN=0

say()  { printf '\033[1;34m[ipa]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[ipa]\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --simulator)     SIMULATOR=1 ;;
        --configuration) CONFIG="${2:-Release}"; shift ;;
        --team)          TEAM="${2:-}"; shift ;;
        --clean)         CLEAN=1 ;;
        -h|--help)       sed -n '3,28p' "$0"; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
    shift
done

case "$CONFIG" in
    Debug|Release) ;;
    *) die "configuration 只能是 Debug 或 Release（得到: $CONFIG）" ;;
esac

command -v xcodebuild >/dev/null 2>&1 || die "需要 macOS + Xcode（xcodebuild 未找到）"
command -v xcodegen  >/dev/null 2>&1 || die "需要 XcodeGen：brew install xcodegen"
[ -n "$TEAM" ] || [ "$SIMULATOR" = 1 ] || \
    die "真机打包需 --team TEAMID，或用 --simulator 免签名"

PRESET="ios-arm64-$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
BUILD_DIR="$ROOT/build-ios-$CONFIG"

# ── 1. 构建引擎 + 应用层静态库 ───────────────────────────────────────────────
say "配置/构建引擎静态库（preset=$PRESET → $BUILD_DIR）"
if [ "$CLEAN" = 1 ]; then
    rm -rf "$BUILD_DIR"
fi
cmake --preset "$PRESET" >/dev/null
cmake --build "$BUILD_DIR" --target cui_core cui_app_root --parallel

[ -f "$BUILD_DIR/libcui-core.a" ] || die "libcui-core.a 未生成：$BUILD_DIR"
[ -f "$BUILD_DIR/libcui_app.a" ]  || die "libcui_app.a 未生成：$BUILD_DIR"

# ── 2. XcodeGen 生成工程 ─────────────────────────────────────────────────────
say "XcodeGen 生成 ios.xcodeproj（链接 $BUILD_DIR 的静态库）"
GEN_YML="/tmp/cui-ios-project.yml"
sed "s|__CUI_BUILD_DIR__|$BUILD_DIR|g" \
    "$ROOT/flutter/src/platform/ios/project.yml" > "$GEN_YML"
xcodegen generate --spec "$GEN_YML" \
    --project "$ROOT/flutter/src/platform/ios/"

# ── 3. xcodebuild archive ───────────────────────────────────────────────────
SCHEME=cui_core
ARCHIVE="$BUILD_DIR/cui.xcarchive"
if [ "$SIMULATOR" = 1 ]; then
    DEST="generic/platform=iOS Simulator"
    # 模拟器免签名：用 null 身份 + Automatic 风格，跳过证书校验。
    SIGN="CODE_SIGN_STYLE=Automatic CODE_SIGN_IDENTITY=- DEVELOPMENT_TEAM= -allowProvisioningUpdates"
else
    DEST="generic/platform=iOS"
    SIGN="CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM=$TEAM"
fi

say "xcodebuild archive（$DEST）"
xcodebuild -scheme "$SCHEME" \
    -configuration "$CONFIG" \
    -destination "$DEST" \
    -archivePath "$ARCHIVE" \
    $SIGN \
    archive | tail -20

# ── 4. export IPA ────────────────────────────────────────────────────────────
EXPORT_OPTS="/tmp/cui-ipa-export.plist"
if [ "$SIMULATOR" = 1 ]; then
    cat > "$EXPORT_OPTS" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>method</key><string>development</string>
  <key>signingStyle</key><string>automatic</string>
  <key>compileBitcode</key><false/>
</dict></plist>
PLIST
else
    cat > "$EXPORT_OPTS" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>method</key><string>development</string>
  <key>signingStyle</key><string>manual</string>
  <key>teamID</key><string>$TEAM</string>
  <key>compileBitcode</key><false/>
</dict></plist>
PLIST
fi

DIST_DIR="$ROOT/dist"
mkdir -p "$DIST_DIR"
say "exportArchive → $DIST_DIR/cui.ipa"
xcodebuild -archivePath "$ARCHIVE" \
    -exportOptionsPlist "$EXPORT_OPTS" \
    -exportPath "$DIST_DIR" \
    $SIGN \
    | tail -20

IPA="$DIST_DIR/cui.ipa"
[ -f "$IPA" ] || die "IPA 未生成：$IPA"
say "完成：$IPA"
