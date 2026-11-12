#!/usr/bin/env bash
# 一条命令做完 mtool 的全部检查：普通构建和 ASan+UBSan 构建各编译一份（警告当错误），生成测试文件，两份都跑全部自动化测试。
# CI（.github/workflows/mtool.yml）跑的就是这个脚本，本机改完代码也跑它。
#
# 用法（在仓库根目录，环境变量和 L02 一样）：
#   mtool/scripts/check.sh           普通加 ASan，全部测试（约 3 分钟）
#   mtool/scripts/check.sh plain     只有普通构建
#   mtool/scripts/check.sh asan      只有 ASan 构建
# 退出码：0 全部通过；非 0 有一步失败（失败的那一步的输出就在上面）。
set -euo pipefail
cd "$(dirname "$0")/../.."

MODE=${1:-both}
case $MODE in both|plain|asan) ;; *) echo "用法：$0 [both|plain|asan]" >&2; exit 2 ;; esac

for c in ffmpeg ffprobe python3 make cc pkg-config; do
    command -v $c >/dev/null 2>&1 || { echo "找不到 $c" >&2; exit 2; }
done
pkg-config --exists libavformat || { echo "pkg-config 找不到 FFmpeg，先设置 PKG_CONFIG_PATH（见 L02）" >&2; exit 2; }

JOBS=$(nproc 2>/dev/null || echo 2)
if [ "$MODE" != asan ]; then
    echo "== 编译（普通，-Werror）"
    make -C mtool -j"$JOBS" EXTRA_CFLAGS=-Werror
fi
if [ "$MODE" != plain ]; then
    echo "== 编译（ASan + UBSan，-Werror）"
    make -C mtool -j"$JOBS" SANITIZE=address,undefined BUILD=build-asan EXTRA_CFLAGS=-Werror
fi
echo "== 测试文件"
mtool/scripts/gen_testfiles.sh >/dev/null
echo "已生成：$(ls samples/mtool | wc -l) 个文件"
if [ "$MODE" != asan ]; then
    echo "== 测试（普通构建）"
    mtool/tests/run_tests.sh
fi
if [ "$MODE" != plain ]; then
    echo "== 测试（ASan + UBSan 构建）"
    BUILD=mtool/build-asan mtool/tests/run_tests.sh
fi
echo "== mtool 全部检查通过"
