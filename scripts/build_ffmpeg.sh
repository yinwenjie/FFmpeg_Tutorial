#!/usr/bin/env bash
#
# 从源码编译课程使用的 FFmpeg，安装到 $HOME/ffmpeg_build。
# 不会改动系统自带的 ffmpeg，也不需要 root 权限（安装依赖除外）。
#
# 用法：
#   scripts/build_ffmpeg.sh            # 使用默认设置
#   JOBS=2 scripts/build_ffmpeg.sh     # 限制并行编译的任务数
#
# 可用环境变量覆盖的设置：
#   FFMPEG_REPO  源码地址
#   FFMPEG_REF   要编译的提交或标签（课程录制所用的版本）
#   SRC_DIR      源码目录
#   PREFIX       安装目录
#   JOBS         并行任务数
set -euo pipefail

FFMPEG_REPO="${FFMPEG_REPO:-https://github.com/FFmpeg/FFmpeg.git}"
FFMPEG_REF="${FFMPEG_REF:-a337e8ddaa095a3cf82564272ec518cced09641e}"
SRC_DIR="${SRC_DIR:-$HOME/ffmpeg_src}"
PREFIX="${PREFIX:-$HOME/ffmpeg_build}"
JOBS="${JOBS:-$(nproc)}"

APT_PACKAGES="build-essential git pkg-config nasm cmake \
libx264-dev libsdl2-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev \
libva-dev libdrm-dev"

# 1. 检查依赖。缺什么就提示安装命令，而不是在这里偷偷调用 sudo。
missing=0
for tool in gcc make git pkg-config nasm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "缺少工具：$tool" >&2
        missing=1
    fi
done
if command -v pkg-config >/dev/null 2>&1; then
    for module in x264 sdl2 freetype2 harfbuzz fontconfig libva; do
        if ! pkg-config --exists "$module"; then
            echo "缺少开发库：$module" >&2
            missing=1
        fi
    done
fi
if [ "$missing" -ne 0 ]; then
    echo >&2
    echo "请先安装依赖，然后重新运行本脚本：" >&2
    echo "  sudo apt update && sudo apt install -y $APT_PACKAGES" >&2
    exit 1
fi

# 2. 获取源码。只下载指定的那一个提交，比完整克隆快得多。
if [ ! -d "$SRC_DIR/.git" ]; then
    mkdir -p "$SRC_DIR"
    git -C "$SRC_DIR" init -q
    git -C "$SRC_DIR" remote add origin "$FFMPEG_REPO"
fi
if [ "$(git -C "$SRC_DIR" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$FFMPEG_REF" ]; then
    git -C "$SRC_DIR" fetch --depth 1 origin "$FFMPEG_REF"
    git -C "$SRC_DIR" checkout -q --detach FETCH_HEAD
fi
echo "源码版本：$(git -C "$SRC_DIR" log -1 --format='%h %cs %s')"

# 3. 配置。每个选项的含义见 L02_environment/README.md。
cd "$SRC_DIR"
./configure \
    --prefix="$PREFIX" \
    --enable-gpl \
    --enable-libx264 \
    --enable-libfreetype \
    --enable-libharfbuzz \
    --enable-libfontconfig \
    --enable-shared \
    --disable-static \
    --enable-rpath \
    --disable-stripping

# 4. 编译、安装，再编译 doc/examples 下的官方示例。
make -j"$JOBS"
make install
make -j"$JOBS" examples

cat <<MSG

编译完成。
  源码目录：$SRC_DIR（官方示例的可执行文件在 doc/examples/ 下）
  安装目录：$PREFIX

请把下面两行加入 ~/.bashrc，然后重新打开终端：
  export PATH="$PREFIX/bin:\$PATH"
  export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:\${PKG_CONFIG_PATH:-}"
MSG
