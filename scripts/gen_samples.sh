#!/usr/bin/env bash
#
# 生成课程统一使用的测试素材，放在仓库根目录的 samples/ 下。
# 全部素材由 ffmpeg 的 lavfi 虚拟输入合成，不需要下载任何文件；
# 已存在的文件会跳过，可以重复运行。
#
# 用法：scripts/gen_samples.sh
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p samples

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "找不到 ffmpeg。学 L01 时可以先用 sudo apt install ffmpeg 安装，L02 之后请使用自己编译的版本。" >&2
    exit 1
fi

# 用法：gen <输出文件> <ffmpeg 参数...>
gen() {
    local out="samples/$1"
    shift
    if [ -s "$out" ]; then
        echo "已存在，跳过：$out"
        return
    fi
    echo "生成：$out"
    ffmpeg -v error -y "$@" "$out"
}

# 1080p30 的 H.264 + AAC 立体声，10 秒。L01 的计算题和 L03、L04 使用。
gen demo_1080p30.mp4 \
    -f lavfi -i testsrc2=size=1920x1080:rate=30 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 10 -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 2

# 360p25 的小文件，5 秒，GOP 为 25 帧并带 B 帧。
# 文件小，适合用 ffprobe 逐包、逐帧观察。
gen small_360p25.mp4 \
    -f lavfi -i testsrc2=size=640x360:rate=25 \
    -f lavfi -i sine=frequency=1000:sample_rate=44100 \
    -t 5 -c:v libx264 -pix_fmt yuv420p -g 25 -bf 2 -c:a aac -ac 2

# 同样的内容换成 MKV 容器，用来对比“容器不同、编码相同”。
gen small_360p25.mkv \
    -i samples/small_360p25.mp4 -c copy

# MPEG-1 视频裸流和 MP2 音频裸流（没有容器）。
# 官方示例 decode_video 和 decode_audio 只接受这两种输入，L05、L06 使用。
gen video_360p25.m1v \
    -i samples/small_360p25.mp4 -an -c:v mpeg1video -b:v 1M -f mpeg1video
gen audio_44k_stereo.mp2 \
    -i samples/small_360p25.mp4 -vn -c:a mp2 -b:a 128k -f mp2

echo
ls -l samples
