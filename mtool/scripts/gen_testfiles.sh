#!/usr/bin/env bash
#
# 综合项目 mtool 的测试文件：18 个，放在 samples/mtool/ 下。
# 全部由 ffmpeg 的 lavfi 虚拟输入合成，不需要下载；已存在的文件跳过，--force 全部重新生成。
#
# 其中 12 个是能正常打开的（五种容器、旋转的、可变帧率的、没有音频的、没有视频的、很小的、声道和采样率不同的……），
# 6 个是有问题的（空文件、不是媒体文件、截断的、中间被改坏的……）。
# mtool 的验收标准要求：好的文件全部能处理，坏的文件不崩溃、给出能看懂的提示。
# 每个文件是“好”还是“坏”、哪个子命令适用，写在 mtool/tests/testfiles.tsv 里，测试脚本读那张表。
#
# 用法：mtool/scripts/gen_testfiles.sh [--force]
#
# 和 scripts/gen_samples.sh 一样，视频编码的线程数固定，保证不同机器上生成的文件逐字节相同。
set -euo pipefail

force=0
if [ "${1:-}" = "--force" ]; then
    force=1
elif [ $# -gt 0 ]; then
    echo "用法：$0 [--force]" >&2
    exit 1
fi

cd "$(dirname "$0")/../.."
for c in ffmpeg ffprobe python3; do
    command -v $c >/dev/null 2>&1 || { echo "找不到 $c" >&2; exit 2; }
done
D=samples/mtool
mkdir -p "$D"

X264_THREADS=6

# 用法：gen 文件名 ffmpeg参数...（输出文件是最后一个参数，由这里补上）
gen() {
    local out="$D/$1"
    shift
    [ "$force" -eq 1 ] && rm -f "$out"
    if [ -s "$out" ]; then
        echo "已存在，跳过：$out"
        return
    fi
    echo "生成：$out"
    ffmpeg -v error -y "$@" "$out"
}

V360="testsrc2=size=640x360:rate=25"
A44="sine=frequency=1000:sample_rate=44100"

# ---- 能正常打开的 ----------------------------------------------------------------------
# 1 基准：H.264 + AAC，5 秒，GOP 25 带 B 帧
gen h264_aac.mp4 -f lavfi -i "$V360" -f lavfi -i "$A44" -t 5 \
    -c:v libx264 -threads $X264_THREADS -pix_fmt yuv420p -g 25 -bf 2 -c:a aac -ac 2
# 2 到 4 同样的内容换容器
gen h264_aac.mkv -i "$D/h264_aac.mp4" -c copy
gen h264_aac.ts  -i "$D/h264_aac.mp4" -c copy
gen h264_aac.flv -i "$D/h264_aac.mp4" -c copy
# 5 AVI：MPEG-4 视频 + MP2 音频（老格式，没有 B 帧的时间戳）
gen mpeg4_mp2.avi -f lavfi -i "$V360" -f lavfi -i "$A44" -t 5 -c:v mpeg4 -q:v 5 -c:a mp2 -b:a 128k
# 6 旋转：显示矩阵里写了 90 度，编码的像素没动（手机竖拍的文件就是这样）
gen rotate90.mp4 -display_rotation 90 -i "$D/h264_aac.mp4" -c copy
# 7 可变帧率：每 10 帧丢 4 帧，时间戳留着空洞（平均帧率约 15 帧/秒，r_frame_rate 还是 25）
gen vfr.mp4 -f lavfi -i "$V360" -f lavfi -i "$A44" -t 5 -vf "select='lt(mod(n,10),6)'" -fps_mode vfr \
    -c:v libx264 -threads $X264_THREADS -pix_fmt yuv420p -g 25 -c:a aac -ac 2
# 8 没有音频
gen video_only.mp4 -f lavfi -i "$V360" -t 5 -c:v libx264 -threads $X264_THREADS -pix_fmt yuv420p -g 25 -bf 2
# 9 没有视频：AAC 放在 MP4（m4a）里
gen audio_only.m4a -f lavfi -i "$A44" -t 5 -c:a aac -ac 2
# 10 没有视频：16 位 PCM 的 WAV，单声道 8 kHz（采样率和声道数都和别的不一样）
gen mono_8k.wav -f lavfi -i "sine=frequency=440:sample_rate=8000" -t 3 -ac 1 -c:a pcm_s16le
# 11 很小的画面：要求 17x15（奇数），编码出来是 16x14（libx264 只收偶数尺寸，ffmpeg 自动取成偶数），2 秒
gen tiny_16x14.mp4 -f lavfi -i "testsrc2=size=17x15:rate=25" -t 2 -c:v libx264 -threads $X264_THREADS -pix_fmt yuv420p
# 12 只有一个关键帧的长 GOP（整段 5 秒只有开头一个 I 帧），cut、thumb 取关键帧时要处理
gen long_gop.mp4 -f lavfi -i "$V360" -f lavfi -i "$A44" -t 5 \
    -c:v libx264 -threads $X264_THREADS -pix_fmt yuv420p -g 250 -keyint_min 250 -sc_threshold 0 -c:a aac -ac 2

# ---- 有问题的 ----------------------------------------------------------------------------
# 13 空文件
[ "$force" -eq 1 ] && rm -f "$D/empty.mp4"
[ -e "$D/empty.mp4" ] || { echo "生成：$D/empty.mp4"; : > "$D/empty.mp4"; }
# 14 不是媒体文件（一段文字，扩展名却是 .mp4）
[ "$force" -eq 1 ] && rm -f "$D/not_media.mp4"
[ -s "$D/not_media.mp4" ] || { echo "生成：$D/not_media.mp4"; printf 'this is not a media file\n' > "$D/not_media.mp4"; }
# 15 截断：MP4 只剩前 40% 的字节，moov 在文件尾，所以连“有几个流”都读不出来
[ "$force" -eq 1 ] && rm -f "$D/truncated_nomoov.mp4"
if [ ! -s "$D/truncated_nomoov.mp4" ]; then
    echo "生成：$D/truncated_nomoov.mp4"
    size=$(stat -c %s "$D/h264_aac.mp4")
    head -c $((size * 4 / 10)) "$D/h264_aac.mp4" > "$D/truncated_nomoov.mp4"
fi
# 16 截断：TS 只剩前 40%（TS 没有“文件尾的索引”，前面的包照样能解码，只是短了）
[ "$force" -eq 1 ] && rm -f "$D/truncated.ts"
if [ ! -s "$D/truncated.ts" ]; then
    echo "生成：$D/truncated.ts"
    size=$(stat -c %s "$D/h264_aac.ts")
    head -c $(( (size * 4 / 10) / 188 * 188 )) "$D/h264_aac.ts" > "$D/truncated.ts"
fi
# 17 中间被改坏：把 MP4 的 mdat 中间 300 个字节换成固定的伪随机数（种子固定，每次生成的内容相同）。
#    moov 完好，所以能打开、能读出所有的包，解码时会报错、有的帧花屏。
[ "$force" -eq 1 ] && rm -f "$D/corrupt_mid.mp4"
if [ ! -s "$D/corrupt_mid.mp4" ]; then
    echo "生成：$D/corrupt_mid.mp4"
    cp "$D/h264_aac.mp4" "$D/corrupt_mid.mp4"
    python3 -I - "$D/corrupt_mid.mp4" <<'PY'
import random, sys
path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
i = data.find(b'mdat')
assert i > 0
mid = i + (len(data) - i) // 3          # mdat 的三分之一处，避开文件头部的几个关键帧
rng = random.Random(12345)
for k in range(300):
    data[mid + k] = rng.randrange(256)
open(path, 'wb').write(data)
PY
fi
# 18 文件头被改坏：MP4 开头的 ftyp 盒子没了（前 32 个字节换成 0）
[ "$force" -eq 1 ] && rm -f "$D/bad_header.mp4"
if [ ! -s "$D/bad_header.mp4" ]; then
    echo "生成：$D/bad_header.mp4"
    cp "$D/h264_aac.mp4" "$D/bad_header.mp4"
    dd if=/dev/zero of="$D/bad_header.mp4" bs=1 count=32 conv=notrunc status=none
fi

echo
echo "完成。文件在 $D/，每个文件的说明在 mtool/tests/testfiles.tsv。"
ls -l "$D"
