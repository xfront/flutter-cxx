#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# cui_core 一键构建入口（纯 CMake 脚本，Linux / macOS 与 build.cmd 等价）。
#
#   bash build.sh                       当前主机的调试全量构建 + ctest
#   bash build.sh release
#   bash build.sh linux asan
#   bash build.sh web --serve
#   bash build.sh --help                查看平台 / profile / 全部选项
#
# 实现在 cmake/build.cmake：参数最终转成 CMakePresets 里的预设调用，
# 所以脚本构建与 IDE / CI 直接跑预设的结果完全一致。
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail
exec cmake -P "$(cd "$(dirname "$0")" && pwd)/cmake/build.cmake" -- "$@"
