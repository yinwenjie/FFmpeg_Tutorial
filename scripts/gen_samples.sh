#!/usr/bin/env bash
#
# 生成课程统一使用的测试素材，放在仓库根目录的 samples/ 下。
# 全部素材由 ffmpeg 的 lavfi 虚拟输入合成，不需要下载任何文件；
# 已存在的文件会跳过，可以重复运行。
#
# 用法：scripts/gen_samples.sh [--force]
#   --force  删除已有素材，全部重新生成。
#
# 不同版本的 ffmpeg 编码出来的文件略有差别（例如 AAC 的码率）。
# L01 用系统自带的 ffmpeg 生成过素材的话，L02 编译好新版本之后要用 --force 重新生成一次，
# 这样后面各讲 README 里的数字才能逐项对上。
set -euo pipefail

force=0
if [ "${1:-}" = "--force" ]; then
    force=1
elif [ $# -gt 0 ]; then
    echo "用法：$0 [--force]" >&2
    exit 1
fi

cd "$(dirname "$0")/.."
mkdir -p samples

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "找不到 ffmpeg。学 L01 时可以先用 sudo apt install ffmpeg 安装，L02 之后请使用自己编译的版本。" >&2
    exit 1
fi

# 记录素材是用哪个版本生成的，版本变了就提醒重新生成。
version="$(ffmpeg -version | head -n 1)"
stamp="samples/.generated_by"
echo "使用 $(command -v ffmpeg)：$version"
fresh=1
if [ -s samples/small_360p25.mp4 ] || [ -s samples/demo_1080p30.mp4 ]; then
    fresh=0
fi
if [ "$force" -eq 0 ] && [ "$fresh" -eq 0 ]; then
    if [ ! -f "$stamp" ]; then
        echo "注意：不清楚已有素材是用哪个版本生成的。" >&2
        echo "      建议运行 $0 --force 重新生成，否则部分数字会和课程里的不一致。" >&2
    elif [ "$(cat "$stamp")" != "$version" ]; then
        echo "注意：已有素材是用另一个版本生成的（$(cat "$stamp")）。" >&2
        echo "      建议运行 $0 --force 重新生成，否则部分数字会和课程里的不一致。" >&2
    fi
fi

# 用法：gen <输出文件> <ffmpeg 参数...>
gen() {
    local out="samples/$1"
    shift
    if [ "$force" -eq 1 ]; then
        rm -f "$out"
    fi
    if [ -s "$out" ]; then
        echo "已存在，跳过：$out"
        return
    fi
    echo "生成：$out"
    ffmpeg -v error -y "$@" "$out"
}

# 视频编码器的输出与线程数有关，而默认线程数取决于 CPU 核数。
# 这里把线程数固定下来，保证不同机器上生成的文件逐字节相同。
# 具体数值没有特殊含义，只是沿用了编写课程时所用机器上的默认值。
X264_THREADS=6
MPEG1_THREADS=5

# 1080p30 的 H.264 + AAC 立体声，10 秒。L01 的计算题和 L03、L04 使用。
gen demo_1080p30.mp4 \
    -f lavfi -i testsrc2=size=1920x1080:rate=30 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 10 -c:v libx264 -threads "$X264_THREADS" -pix_fmt yuv420p -c:a aac -ac 2

# 360p25 的小文件，5 秒，GOP 为 25 帧并带 B 帧。
# 文件小，适合用 ffprobe 逐包、逐帧观察。
gen small_360p25.mp4 \
    -f lavfi -i testsrc2=size=640x360:rate=25 \
    -f lavfi -i sine=frequency=1000:sample_rate=44100 \
    -t 5 -c:v libx264 -threads "$X264_THREADS" -pix_fmt yuv420p -g 25 -bf 2 -c:a aac -ac 2

# 同样的内容换成 MKV、MPEG-TS、FLV 容器，用来对比“容器不同、编码相同”。L03 使用。
gen small_360p25.mkv \
    -i samples/small_360p25.mp4 -c copy
gen small_360p25.ts \
    -i samples/small_360p25.mp4 -c copy
gen small_360p25.flv \
    -i samples/small_360p25.mp4 -c copy

# MPEG-1 视频裸流和 MP2 音频裸流（没有容器）。
# 官方示例 decode_video 和 decode_audio 只接受这两种输入，L05、L06 使用。
gen video_360p25.m1v \
    -i samples/small_360p25.mp4 -an -c:v mpeg1video -threads "$MPEG1_THREADS" -b:v 1M -f mpeg1video
gen audio_44k_stereo.mp2 \
    -i samples/small_360p25.mp4 -vn -c:a mp2 -b:a 128k -f mp2

# 只有全部素材都出自当前版本时才更新记录。
if [ "$force" -eq 1 ] || [ "$fresh" -eq 1 ]; then
    echo "$version" > "$stamp"
fi

echo
ls -l samples
