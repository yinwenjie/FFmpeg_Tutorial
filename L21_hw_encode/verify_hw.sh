#!/usr/bin/env bash
#
# L21：在有显卡的机器上验证硬件编码和“全 GPU 转码”，把结果写成 out/l21/results.md。
# README 里标着“【待补 N】”的地方，就是这个文件里“待补 N”那一节，运行以后把结果贴回去（或者发给课程作者）。
#
# 用法：verify_hw.sh                        默认 VAAPI。机器上没有 VAAPI 设备时，打印“跳过”，退出码 0
#       DEVICE_PATH=/dev/dri/renderD129 verify_hw.sh     有多块显卡时指定用哪一块
#       STANDIN=1 verify_hw.sh              替身模式：不用显卡，“硬件”这一侧也用 libx264 软件编码。
#                                            只用来检查这个脚本本身：解析、对比、写 results.md 有没有问题。
#                                            替身模式的结果不是硬件的结果，results.md 开头会写明。
#       BUILD=build-asan verify_hw.sh       用别的构建目录里的程序
#
# 做 5 件事（每件对应 README 里的一个“待补”）：
#   1  hw_enc_info try=1：这台机器上有哪些设备，设备能接受什么格式和尺寸的帧
#   2  官方 vaapi_encode：能不能跑、输出的帧数、和输入比 PSNR；行有填充的宽度（352x288）有没有读歪（patch 0099）
#   3  hw_encode_av：硬件编码几种尺寸和输入格式，帧数、PSNR、码流大小；帧池取多大才能跑通；和 ffmpeg 命令行的硬件编码比
#   4  全 GPU 转码：官方 vaapi_transcode、命令行的“全留在显存里”“下载再上传”“只硬件解码”“纯软件”各自的耗时和 CPU 时间；
#      输出的时间信息对不对（patch 0101：平均帧率 25/1，时长 5.0 秒）
#   5  设备能做哪些编码器：h264、hevc、vp9、av1、mpeg2、mjpeg 各编 10 帧，成功还是失败，失败的话是什么原因
#
# 退出码：0 做完了（包括“没有设备，跳过”）；1 脚本自己出了问题（缺程序、缺素材）。
# 硬件编码的输出质量低于预期、或者某个编码器用不了，都不算脚本失败，会写在 results.md 里。

set -u
cd "$(dirname "$0")"
BUILD=${BUILD:-build}
STANDIN=${STANDIN:-0}
S=../samples
OUT=../out/l21
RES=$OUT/results.md
DEVPATH=${DEVICE_PATH:-/dev/dri/renderD128}
PATH_OPT=()
[ -n "${DEVICE_PATH:-}" ] && PATH_OPT=("device_path=$DEVICE_PATH")

for p in vaapi_encode vaapi_transcode hw_enc_info hw_encode_av; do
    [ -x "$BUILD/$p" ] || { echo "找不到 $BUILD/$p，先运行 make" >&2; exit 1; }
done
for c in ffmpeg ffprobe; do
    command -v $c >/dev/null 2>&1 || { echo "找不到 $c" >&2; exit 1; }
done
for f in small_360p25.mp4 demo_1080p30.mp4; do
    [ -f "$S/$f" ] || { echo "缺素材 $S/$f，先运行 scripts/gen_samples.sh" >&2; exit 1; }
done
mkdir -p "$OUT"

if [ "$STANDIN" = 1 ]; then
    ENC=libx264     # “硬件”一侧也用软件
    echo "替身模式：不使用显卡，只检查脚本本身"
else
    ENC=h264_vaapi
    if ! "$BUILD/hw_enc_info" try=1 "${PATH_OPT[@]/#device_path=/device=}" 2>/dev/null | grep -q "^   vaapi  *创建成功"; then
        echo "跳过：这台机器上创建不了 vaapi 设备（加 DEVICE_PATH=… 指定设备，或者看 $BUILD/hw_enc_info try=1 verbose=1）。"
        echo "没有显卡的机器上，请运行 test_nodevice.sh。"
        exit 0
    fi
fi

# ---- 小工具 ------------------------------------------------------------------------
# 原始像素输入：raw 输出 素材 像素格式 宽 高 [帧数]
raw() {
    local out=$1 src=$2 fmt=$3 w=$4 h=$5 n=${6:-0}
    local extra=()
    [ "$n" != 0 ] && extra=(-frames:v "$n")
    ffmpeg -v error -y -i "$src" "${extra[@]}" -vf "scale=$w:$h" -pix_fmt "$fmt" -f rawvideo "$out"
}
nframes() { ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$1" 2>/dev/null; }
# 把编码结果解码成 nv12，和输入比 PSNR（取平均）：psnr 输入.nv12 编码结果 宽x高
psnr() {
    ffmpeg -v error -y -i "$2" -pix_fmt nv12 -f rawvideo "$OUT/dec.nv12" 2>/dev/null
    ffmpeg -v info -f rawvideo -pix_fmt nv12 -s "$3" -i "$1" -f rawvideo -pix_fmt nv12 -s "$3" -i "$OUT/dec.nv12" -lavfi psnr -f null - 2>&1 \
        | sed -n 's/.*average:\([^ ]*\).*/\1/p'
    rm -f "$OUT/dec.nv12"
}
# 计时：timeit 命令...，输出 “墙钟 CPU时间”。CPU 时间是用户加系统，所有线程加起来
timeit() {
    local t
    t=$( { TIMEFORMAT='%R %U %S'; time "$@" >/dev/null 2>&1; } 2>&1 )
    echo "$t" | awk '{ printf "%.3f %.3f\n", $1, $2 + $3 }'
}
# 重复 3 次，取墙钟居中的那一次：median 命令...
median() {
    local i
    for i in 1 2 3; do timeit "$@"; done | sort -n | sed -n 2p
}
ffprobe_val() { ffprobe -v error -select_streams v:0 -show_entries "stream=$2" -of csv=p=0 "$1" 2>/dev/null | head -1; }

# 输入
raw "$OUT/in_640x360.nv12"  "$S/small_360p25.mp4" nv12     640 360
raw "$OUT/in_352x288.nv12"  "$S/small_360p25.mp4" nv12     352 288 10
raw "$OUT/in_1000x562.nv12" "$S/demo_1080p30.mp4" nv12    1000 562 10
raw "$OUT/in_854x480.yuv"   "$S/small_360p25.mp4" yuv420p  854 480 10

{
    echo "# L21 验证结果"
    echo
    echo "- 时间：$(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "- 机器：$(uname -sr)，FFmpeg $("$BUILD/hw_enc_info" 2>/dev/null | sed -n '1s/.*（\(.*\)）.*/\1/p')"
    echo "- 设备：vaapi，设备路径 $DEVPATH"
    if [ "$STANDIN" = 1 ]; then
        echo
        echo "> **替身模式：没有使用显卡。下面所有“硬件”都是 libx264 软件编码，结果不能当作硬件的结果贴进 README。**"
    fi
} >"$RES"

# ---- 待补 1 ------------------------------------------------------------------------
echo "待补 1：hw_enc_info"
{
    echo
    echo "## 待补 1：hw_enc_info try=1"
    echo
    echo '```console'
    echo "\$ $BUILD/hw_enc_info try=1 ${PATH_OPT[*]:+device=$DEVICE_PATH}"
    "$BUILD/hw_enc_info" try=1 ${DEVICE_PATH:+device=$DEVICE_PATH} 2>&1
    echo '```'
} >>"$RES"

# ---- 待补 2：官方 vaapi_encode ------------------------------------------------------
echo "待补 2：官方 vaapi_encode"
{
    echo
    echo "## 待补 2：官方 vaapi_encode"
    echo
    echo "| 输入 | 退出码 | 输出码流大小（字节） | 解码出的帧数 / 输入帧数 | PSNR（dB，和输入比） |"
    echo "| --- | --- | --- | --- | --- |"
} >>"$RES"
for spec in "640 360 125" "352 288 10"; do
    set -- $spec
    w=$1; h=$2; n=$3
    in=$OUT/in_${w}x${h}.nv12
    if [ "$STANDIN" = 1 ]; then
        # 官方示例只会用硬件；替身模式没有设备，这一项没法替身，用 hw_encode_av 的软件编码代替，只是为了让后面的列有值
        "$BUILD/hw_encode_av" "$w" "$h" "$in" "$OUT/official.h264" enc=libx264 >/dev/null 2>&1; rc="替身"
    else
        "$BUILD/vaapi_encode" "$w" "$h" "$in" "$OUT/official.h264" >"$OUT/official.log" 2>&1; rc=$?
    fi
    size=$(stat -c %s "$OUT/official.h264" 2>/dev/null || echo 0)
    got=$(nframes "$OUT/official.h264")
    p=$(psnr "$in" "$OUT/official.h264" "${w}x${h}")
    echo "| ${w}x${h}，$n 帧 | $rc | $size | ${got:-0} / $n | ${p:-?} |" >>"$RES"
    rm -f "$OUT/official.h264"
done
{
    echo
    echo "352x288 的行有填充（行距 384，宽 352）：patch 0099 之前的读法会把画面读歪，PSNR 只有 8 dB 左右。这一行应当是 30 dB 以上。"
    [ "$STANDIN" = 1 ] && echo "（替身模式不运行官方示例，上面用的是 hw_encode_av enc=libx264。）"
    [ -s "$OUT/official.log" ] && { echo; echo '```text'; head -10 "$OUT/official.log"; echo '```'; }
} >>"$RES"
rm -f "$OUT/official.log"

# ---- 待补 3：hw_encode_av 的硬件编码 -------------------------------------------------
echo "待补 3：hw_encode_av"
{
    echo
    echo "## 待补 3：hw_encode_av，硬件编码"
    echo
    echo "| 输入 | 编码器 | 退出码 | 码流大小（字节） | 解码出的帧数 / 输入帧数 | PSNR（dB） | 上传耗时（秒） |"
    echo "| --- | --- | --- | --- | --- | --- | --- |"
} >>"$RES"
run_enc() { # run_enc 说明 输入 宽 高 帧数 像素格式
    local label=$1 in=$2 w=$3 h=$4 n=$5 fmt=$6 rc size got p up
    "$BUILD/hw_encode_av" "$w" "$h" "$in" "$OUT/mine.h264" "enc=$ENC" "${PATH_OPT[@]}" pix_fmt="$fmt" stats=1 >"$OUT/mine.stdout" 2>"$OUT/mine.stderr"
    rc=$?
    size=$(stat -c %s "$OUT/mine.h264" 2>/dev/null || echo 0)
    got=$(nframes "$OUT/mine.h264")
    if [ "$fmt" = nv12 ]; then p=$(psnr "$in" "$OUT/mine.h264" "${w}x${h}"); else p="（输入不是 nv12，没比）"; fi
    up=$(sed -n 's/^上传耗时（内存拷到显存）：\([0-9.]*\) 秒/\1/p' "$OUT/mine.stdout")
    echo "| $label | $ENC | $rc | $size | ${got:-0} / $n | ${p:-?} | ${up:-?} |" >>"$RES"
    [ $rc -ne 0 ] && { echo; echo "  （$label 的错误信息：$(head -2 "$OUT/mine.stderr" | tr '\n' ' ')）"; } >>"$RES"
    rm -f "$OUT/mine.h264" "$OUT/mine.stdout" "$OUT/mine.stderr"
}
run_enc "640x360 nv12，125 帧"            "$OUT/in_640x360.nv12"  640 360 125 nv12
run_enc "352x288 nv12（行有填充），10 帧"  "$OUT/in_352x288.nv12"  352 288 10 nv12
run_enc "1000x562 nv12（行有填充），10 帧" "$OUT/in_1000x562.nv12" 1000 562 10 nv12
run_enc "854x480 yuv420p（先转 nv12 再上传），10 帧" "$OUT/in_854x480.yuv" 854 480 10 yuv420p

{
    echo
    echo "帧池的大小（pool=N，640x360，125 帧）：池子小了会怎样？"
    echo
    echo "| pool | 退出码 | 解码出的帧数 | 错误信息 |"
    echo "| --- | --- | --- | --- |"
} >>"$RES"
for pool in 1 2 4 8 20; do
    "$BUILD/hw_encode_av" 640 360 "$OUT/in_640x360.nv12" "$OUT/pool.h264" "enc=$ENC" "${PATH_OPT[@]}" "pool=$pool" >/dev/null 2>"$OUT/pool.err"
    rc=$?
    got=$(nframes "$OUT/pool.h264")
    msg=$(head -2 "$OUT/pool.err" | tr '\n|' '  ')
    echo "| $pool | $rc | ${got:-0} | ${msg:--} |" >>"$RES"
    rm -f "$OUT/pool.h264" "$OUT/pool.err"
done

{
    echo
    echo "和 ffmpeg 命令行的硬件编码比（同样的输入，640x360，125 帧）："
    echo
    echo '```console'
    if [ "$STANDIN" = 1 ]; then
        echo "（替身模式不运行）"
    else
        echo "\$ ffmpeg -vaapi_device $DEVPATH -f rawvideo -pix_fmt nv12 -s 640x360 -r 25 -i in.nv12 -vf format=nv12,hwupload -c:v h264_vaapi out.h264"
        ffmpeg -v error -y -vaapi_device "$DEVPATH" -f rawvideo -pix_fmt nv12 -s 640x360 -r 25 -i "$OUT/in_640x360.nv12" \
               -vf format=nv12,hwupload -c:v h264_vaapi "$OUT/cli.h264" 2>"$OUT/cli.err"
        echo "退出码 $?，码流 $(stat -c %s "$OUT/cli.h264" 2>/dev/null || echo 0) 字节，解码出 $(nframes "$OUT/cli.h264") 帧，PSNR $(psnr "$OUT/in_640x360.nv12" "$OUT/cli.h264" 640x360) dB"
        [ -s "$OUT/cli.err" ] && head -5 "$OUT/cli.err"
        echo "\$ hw_encode_av 640 360 in.nv12 out.h264 enc=h264_vaapi"
        "$BUILD/hw_encode_av" 640 360 "$OUT/in_640x360.nv12" "$OUT/mine.h264" "enc=$ENC" "${PATH_OPT[@]}" >/dev/null 2>&1
        echo "退出码 $?，码流 $(stat -c %s "$OUT/mine.h264" 2>/dev/null || echo 0) 字节，解码出 $(nframes "$OUT/mine.h264") 帧，PSNR $(psnr "$OUT/in_640x360.nv12" "$OUT/mine.h264" 640x360) dB"
        echo "（两者的码流大小不会逐字节一样：两边用的编码参数不同，比的是帧数和 PSNR 在同一个范围里。）"
    fi
    echo '```'
} >>"$RES"
rm -f "$OUT/cli.h264" "$OUT/cli.err" "$OUT/mine.h264"

# ---- 待补 4：全 GPU 转码 ------------------------------------------------------------
echo "待补 4：全 GPU 转码"
{
    echo
    echo "## 待补 4：全 GPU 转码"
    echo
    echo "输入 small_360p25.mp4（640x360，125 帧）和 demo_1080p30.mp4（1080p，300 帧），每种方式跑 3 次取墙钟居中的一次。"
    echo "CPU 时间是用户加系统，所有线程加起来。输出的 PSNR 不算，这里比的是用了哪条路、花了多少时间和 CPU。"
    echo
    echo "| 输入 | 方式 | 墙钟（秒） | CPU 时间（秒） | 输出帧数 | 平均帧率 | 时长（秒） |"
    echo "| --- | --- | --- | --- | --- | --- | --- |"
} >>"$RES"
row() { # row 输入名 方式 墙钟CPU 输出文件
    local name=$1 how=$2 t=$3 f=$4
    echo "| $name | $how | ${t% *} | ${t#* } | $(nframes "$f") | $(ffprobe_val "$f" avg_frame_rate) | $(ffprobe_val "$f" duration) |" >>"$RES"
}
for in in small_360p25.mp4 demo_1080p30.mp4; do
    f=$S/$in
    o=$OUT/t.mp4
    if [ "$STANDIN" = 1 ]; then
        t=$(median ffmpeg -v error -y -i "$f" -c:v libx264 -threads 1 "$o"); row "$in" "纯软件（libx264，1 线程）" "$t" "$o"
        echo "| $in | （替身模式不运行硬件的几种方式） | - | - | - | - | - |" >>"$RES"
        continue
    fi
    t=$(median "$BUILD/vaapi_transcode" "$f" h264_vaapi "$o");                                                        row "$in" "官方 vaapi_transcode：解码编码都在显卡上，帧不离开显存" "$t" "$o"
    t=$(median ffmpeg -v error -y -hwaccel vaapi -hwaccel_device "$DEVPATH" -hwaccel_output_format vaapi -i "$f" -c:v h264_vaapi "$o")
    row "$in" "命令行：-hwaccel_output_format vaapi（同上）" "$t" "$o"
    t=$(median ffmpeg -v error -y -init_hw_device vaapi=va:"$DEVPATH" -filter_hw_device va -hwaccel vaapi -hwaccel_device va -i "$f" \
                      -vf "format=nv12,hwupload" -c:v h264_vaapi "$o")
    row "$in" "命令行：硬件解码，下载到内存，再上传去硬件编码" "$t" "$o"
    t=$(median ffmpeg -v error -y -hwaccel vaapi -hwaccel_device "$DEVPATH" -i "$f" -c:v libx264 -threads 1 "$o")
    row "$in" "命令行：硬件解码，下载到内存，libx264 软件编码" "$t" "$o"
    t=$(median ffmpeg -v error -y -i "$f" -c:v libx264 -threads 1 "$o");                                              row "$in" "命令行：纯软件（libx264，1 线程）" "$t" "$o"
    rm -f "$o"
done
{
    echo
    echo "patch 0101 检查：官方 vaapi_transcode 的输出，平均帧率应当是 25/1（1080p 是 30/1），时长应当是 5.0（1080p 是 10.0）。"
    echo "本机线程数：$(nproc)。"
} >>"$RES"

# ---- 待补 5：设备能做哪些编码器 -------------------------------------------------------
echo "待补 5：设备能做哪些编码器"
{
    echo
    echo "## 待补 5：设备能做哪些编码器"
    echo
    echo "每种编码器编 10 帧（640x360 nv12）。硬件编码器能不能用，取决于显卡和驱动：失败不奇怪，要看错误信息说的是哪一层。"
    echo
    echo "| 编码器 | 退出码 | 解码出的帧数 | 错误信息（第一行） |"
    echo "| --- | --- | --- | --- |"
} >>"$RES"
for enc in h264_vaapi hevc_vaapi vp9_vaapi av1_vaapi mpeg2_vaapi mjpeg_vaapi; do
    e=$enc
    [ "$STANDIN" = 1 ] && e=libx264
    "$BUILD/hw_encode_av" 640 360 "$OUT/in_640x360.nv12" "$OUT/enc.out" "enc=$e" "${PATH_OPT[@]}" frames=10 >/dev/null 2>"$OUT/enc.err"
    rc=$?
    got=$(nframes "$OUT/enc.out")
    msg=$(head -1 "$OUT/enc.err" | tr '|' ' ')
    echo "| $enc | $rc | ${got:-0} | ${msg:--} |" >>"$RES"
    rm -f "$OUT/enc.out" "$OUT/enc.err"
done
rm -f "$OUT"/in_*.nv12 "$OUT"/in_*.yuv "$OUT/t.mp4"

echo
echo "结果写在 $RES"
