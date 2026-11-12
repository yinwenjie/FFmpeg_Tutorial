#!/usr/bin/env bash
# 回归：把 L16 的 transcode_av 和 ffmpeg 命令行转码同一个输入，比较结果。
#
# 用法（在仓库根目录）：
#   make -C L16_transcode
#   L22_engineering/regress_transcode.sh
#
# 每一项有两种“相同”的标准，从严到宽：
#   字节：两个输出文件逐字节相同（cmp）。MP4、MOV、AVI 做得到。
#   逐包：两个文件里每一个包的流序号、pts、dts、时长、大小、校验和都相同（ffmpeg -c copy -f framecrc）。
#         容器里除了包以外的东西不比。MKV、TS、FLV 做不到字节相同，原因都是容器层的小差别，每一项在表后面说明。
# 先比字节，比不出再比逐包，这样“为什么用较宽的标准”是写在脚本里、可以检查的，不是随手放宽。
#
# 退出码：0 全部达到各自的标准；1 有不同；2 缺程序或素材。
# 输入文件由 samples/ 的素材做出来，放在 out/l22_transcode/ 下。

set -u
cd "$(dirname "$0")/.."

B=L16_transcode/build/transcode_av
S=samples
D=out/l22_transcode
mkdir -p "$D"

[ -x "$B" ] || { echo "找不到 $B，请先 make -C L16_transcode" >&2; exit 2; }
for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv; do
    [ -f "$S/$f" ] || { echo "找不到 $S/$f，请先运行 scripts/gen_samples.sh" >&2; exit 2; }
done
command -v ffmpeg >/dev/null || { echo "找不到 ffmpeg，请先设置 PATH（见 L02）" >&2; exit 2; }

ff() { ffmpeg -v error -y "$@" || { echo "ffmpeg 失败：$*" >&2; exit 2; }; }
# 每 25 帧一个关键帧的输入：流拷贝裁剪要有关键帧可以落（L16 第 6 步）
ff -i $S/small_360p25.mp4 -c:v libx264 -g 25 -keyint_min 25 -sc_threshold 0 -threads 1 -c:a copy "$D/gop25.mp4"

same_bytes=0; same_packets=0; bad=0; bad_list=""

crc() { ffmpeg -v error -i "$1" -c copy -f framecrc - 2>/dev/null | grep -v '^#'; }

# check 名字 标准 输出扩展名 输入 本程序的参数... -- 命令行的参数...
#   标准：bytes 要求字节相同；packets 要求逐包相同
#   命令行的参数里写 -i 和输入文件，不写输出文件（脚本补上，并加 -threads 1）
check() {
    local name=$1 level=$2 ext=$3 in=$4
    shift 4
    local mine=() cli=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do mine+=("$1"); shift; done
    shift
    cli=("$@")
    local a="$D/$name.$ext" b="$D/${name}_cli.$ext"
    rm -f "$a" "$b"
    "$B" "$in" "$a" ${mine[@]+"${mine[@]}"} >/dev/null 2>"$D/$name.log"
    local rc=$?
    ffmpeg -v error -y "${cli[@]}" -threads 1 "$b" 2>/dev/null
    if [ $rc -ne 0 ] || [ ! -s "$a" ] || [ ! -s "$b" ]; then
        bad=$((bad + 1)); bad_list="$bad_list $name"; printf '失败  %-22s transcode_av 退出码 %d\n' "$name" $rc
    elif cmp -s "$a" "$b"; then
        same_bytes=$((same_bytes + 1)); printf '字节相同  %s\n' "$name"
    elif [ "$level" = packets ] && [ "$(crc "$a")" = "$(crc "$b")" ] && [ -n "$(crc "$a")" ]; then
        same_packets=$((same_packets + 1)); printf '逐包相同  %-22s（文件字节不同：%d 对 %d）\n' "$name" "$(stat -c %s "$a")" "$(stat -c %s "$b")"
    else
        bad=$((bad + 1)); bad_list="$bad_list $name"; printf '不同  %s\n' "$name"
    fi
}

echo "== 不同的输入，输出 MP4（L16 第 4 步）"
for f in mp4 mkv ts flv; do
    check "in_$f" bytes mp4 "$S/small_360p25.$f" -- -i "$S/small_360p25.$f"
done

echo "== 不同的输出格式"
check out_mov  bytes   mov  "$S/small_360p25.mp4" -- -i "$S/small_360p25.mp4"
check out_avi  bytes   avi  "$S/small_360p25.mp4" -- -i "$S/small_360p25.mp4"
check out_mkv  packets mkv  "$S/small_360p25.mp4" -- -i "$S/small_360p25.mp4"
check out_flv  packets flv  "$S/small_360p25.mp4" -- -i "$S/small_360p25.mp4"
# TS：ffmpeg 命令行默认把复用器的延迟设成 0.7 秒（-muxdelay），所有时间戳加 1.4 秒；本程序不设，所以要写 max_delay=700000
check out_ts   packets ts   "$S/small_360p25.mp4" max_delay=700000 -- -i "$S/small_360p25.mp4"

echo "== 滤镜、裁剪、编码参数"
check scale_copya   bytes mp4 "$S/small_360p25.mp4" vf=scale=320:-2 copy=a -- -i "$S/small_360p25.mp4" -vf scale=320:-2 -c:a copy
check fps10         bytes mp4 "$S/small_360p25.mp4" vf=fps=10 -- -i "$S/small_360p25.mp4" -vf fps=10
check af_volume     bytes mp4 "$S/small_360p25.mp4" af=volume=0.5 -- -i "$S/small_360p25.mp4" -af volume=0.5
check crf18_fast    bytes mp4 "$S/small_360p25.mp4" v:crf=18 v:preset=fast -- -i "$S/small_360p25.mp4" -crf 18 -preset fast
check faststart     bytes mp4 "$S/small_360p25.mp4" movflags=+faststart -- -i "$S/small_360p25.mp4" -movflags +faststart
check cut_exact     bytes mp4 "$S/small_360p25.mp4" ss=2 t=1.5 -- -ss 2 -t 1.5 -i "$S/small_360p25.mp4"
check cut_copy_va   bytes mp4 "$D/gop25.mp4" ss=2.32 t=1.4 copy=va -- -ss 2.32 -t 1.4 -i "$D/gop25.mp4" -c copy

total=$((same_bytes + same_packets + bad))
echo
echo "共 $total 项：字节相同 $same_bytes，逐包相同 $same_packets，不同或失败 $bad"
[ $same_packets -gt 0 ] && echo "逐包相同的几项（out_mkv、out_flv、out_ts）：包的内容和时间戳都一样，文件字节不同，原因见 L22 README 第 6 步。"
if [ $bad -gt 0 ]; then
    echo "不同或失败：$bad_list"
    exit 1
fi
