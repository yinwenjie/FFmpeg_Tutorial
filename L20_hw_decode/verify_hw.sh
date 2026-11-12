#!/usr/bin/env bash
#
# L20：在有显卡的机器上验证硬件解码，把结果写成 out/l20/results.md。
# README 里标着“【待补 N】”的地方，就是这个文件里“待补 N”那一节，运行以后把结果贴回去（或者发给课程作者）。
#
# 用法：verify_hw.sh [设备类型]          默认 vaapi。机器上没有这种设备时，打印“跳过”，退出码 0
#       DEVICE_PATH=/dev/dri/renderD129 verify_hw.sh     有多块显卡时指定用哪一块
#       STANDIN=1 verify_hw.sh           替身模式：不用显卡，“硬件”这一侧也用软件解码。
#                                         只用来检查这个脚本本身：解析、对比、写 results.md 有没有问题。
#                                         替身模式的结果不是硬件的结果，results.md 开头会写明。
#       BUILD=build-asan verify_hw.sh    用别的构建目录里的程序
#
# 做 5 件事（每件对应 README 里的一个“待补”）：
#   1  hw_info try=1：这台机器上有哪些设备，设备支持的格式和尺寸范围
#   2  官方 hw_decode：能不能跑、写出的文件大小对不对、和软件解码是不是同样的内容
#   3  hw_decode_av：同样的几个素材，硬件解码和软件解码的输出是否逐字节一致（不一致就算 PSNR），硬件帧能传成哪些格式
#   4  速度：1080p 素材，软件（1 线程、自动线程）和硬件的耗时、CPU 时间、搬运耗时
#   5  显卡解不了的视频（10 位 H.264、MPEG-1）：有没有退回软件，退回以后输出对不对
#
# 退出码：0 做完了（包括“没有设备，跳过”）；1 脚本自己出了问题（缺程序、缺素材）。
# 硬件解码的输出和软件不同，不算脚本失败，会写在 results.md 里。

set -u
cd "$(dirname "$0")"
BUILD=${BUILD:-build}
DEV=${1:-vaapi}
STANDIN=${STANDIN:-0}
S=../samples
OUT=../out/l20
RES=$OUT/results.md
PATH_OPT=()
[ -n "${DEVICE_PATH:-}" ] && PATH_OPT=("device_path=$DEVICE_PATH")

for p in hw_decode hw_info hw_decode_av; do
    [ -x "$BUILD/$p" ] || { echo "找不到 $BUILD/$p，先运行 make" >&2; exit 1; }
done
for c in ffmpeg ffprobe; do
    command -v $c >/dev/null 2>&1 || { echo "找不到 $c" >&2; exit 1; }
done
for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv demo_1080p30.mp4 video_360p25.m1v; do
    [ -f "$S/$f" ] || { echo "缺素材 $S/$f，先运行 scripts/gen_samples.sh" >&2; exit 1; }
done
mkdir -p "$OUT"

if [ "$STANDIN" = 1 ]; then
    HW=none         # “硬件”一侧也用软件
    echo "替身模式：不使用显卡，只检查脚本本身"
else
    HW=$DEV
    if ! "$BUILD/hw_info" try=1 "${PATH_OPT[@]}" 2>/dev/null | grep -q "^   $DEV  *创建成功"; then
        echo "跳过：这台机器上创建不了 $DEV 设备（加 DEVICE_PATH=… 指定设备，或者看 $BUILD/hw_info try=1 verbose=1）。"
        echo "没有显卡的机器上，请运行 test_nodevice.sh。"
        exit 0
    fi
fi

# ---- 小工具 ------------------------------------------------------------------------
# 命令行的软件解码：-fps_mode passthrough 关掉补帧丢帧，才是解码器本来的输出
cli_md5() { ffmpeg -v error -i "$1" -fps_mode passthrough -f rawvideo "${@:2}" - 2>/dev/null | md5sum | cut -d' ' -f1; }
# hw_decode_av 的 md5 行：用法 av_md5 输入 设备 [选项…]
av_md5() { "$BUILD/hw_decode_av" "$1" - md5=1 "device=$2" "${PATH_OPT[@]}" "${@:3}" 2>/dev/null | sed -n 's/^md5=//p'; }
# 一次运行的统计输出里取一个数。用法 stat 文件 标签
stat_of() { sed -n "s/^$2：\([0-9.]*\) 秒.*/\1/p" "$1" | head -1; }
# 重复 3 次，留下墙钟时间居中的那一次的统计。用法 bench 输出文件 输入 设备 [选项…]
bench() {
    local out=$1 in=$2 dev=$3 i w
    shift 3
    rm -f "$out".run*
    for i in 1 2 3; do
        "$BUILD/hw_decode_av" "$in" - stats=1 "device=$dev" "${PATH_OPT[@]}" "$@" >"$out.run$i" 2>/dev/null
    done
    # 按墙钟排序，取中间那个
    w=$(for i in 1 2 3; do echo "$(stat_of "$out.run$i" '耗时（墙钟）') $i"; done | sort -n | sed -n 2p | cut -d' ' -f2)
    cp "$out.run$w" "$out"
    rm -f "$out".run*
}
psnr() { ffmpeg -v info -f rawvideo -pix_fmt nv12 -s "$3" -i "$1" -f rawvideo -pix_fmt nv12 -s "$3" -i "$2" -lavfi psnr -f null - 2>&1 | sed -n 's/.*average:\([^ ]*\).*/\1/p'; }

{
    echo "# L20 验证结果"
    echo
    echo "- 时间：$(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "- 机器：$(uname -sr)，FFmpeg $("$BUILD/hw_info" 2>/dev/null | sed -n '1s/.*（\(.*\)）.*/\1/p')"
    echo "- 设备类型：$DEV${DEVICE_PATH:+，设备路径 $DEVICE_PATH}"
    if [ "$STANDIN" = 1 ]; then
        echo
        echo "> **替身模式：没有使用显卡。下面所有“硬件”都是软件解码，结果不能当作硬件的结果贴进 README。**"
    fi
} >"$RES"

# ---- 待补 1 ------------------------------------------------------------------------
echo "待补 1：hw_info"
{
    echo
    echo "## 待补 1：hw_info try=1"
    echo
    echo '```console'
    echo "\$ $BUILD/hw_info try=1 ${PATH_OPT[*]:-}"
    "$BUILD/hw_info" try=1 "${PATH_OPT[@]}" 2>&1
    echo '```'
} >>"$RES"

# ---- 待补 2：官方 hw_decode ---------------------------------------------------------
echo "待补 2：官方 hw_decode"
in=$S/small_360p25.mp4
frames=$(ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 "$in")
if [ "$STANDIN" = 1 ]; then
    # 官方示例只会用硬件；替身模式没有设备，这一项没法替身
    rc="（替身模式不运行）"; size="-"; same="-"; log="（替身模式不运行官方示例：它只能用硬件）"
else
    log=$("$BUILD/hw_decode" "$DEV" "$in" "$OUT/official.out" 2>&1); rc=$?
    size=$(stat -c %s "$OUT/official.out" 2>/dev/null || echo 0)
    # 软件解码传成 nv12 的结果，大小和内容都应该一样
    sw=$(av_md5 "$in" none pix_fmt=nv12)
    mine=$(md5sum <"$OUT/official.out" 2>/dev/null | cut -d' ' -f1)
    if [ "$sw" = "$mine" ]; then same="一致"; else same="不一致（如果大小是 nv12 的大小，可能是写出来的格式不是 nv12，看第 3 项里的格式列表）"; fi
    rm -f "$OUT/official.out"
fi
{
    echo
    echo "## 待补 2：官方 hw_decode"
    echo
    echo "\`hw_decode $DEV small_360p25.mp4 out\`"
    echo
    echo "- 退出码：$rc"
    echo "- 输出文件大小：$size 字节（$frames 帧 × 640×360 的 nv12 一帧 345600 字节 = $((frames * 345600))）"
    echo "- 和软件解码传成 nv12 的结果比：$same"
    echo
    echo '```text'
    printf '%s\n' "$log"
    echo '```'
} >>"$RES"

# ---- 待补 3：hw_decode_av 的输出和软件解码比 --------------------------------------------
echo "待补 3：hw_decode_av 硬件和软件比较"
{
    echo
    echo "## 待补 3：hw_decode_av，硬件和软件的输出"
    echo
    echo "| 素材 | 输出格式 | 软件 MD5 | 硬件 MD5 | 结果 | 硬件帧数 / 内存帧数 |"
    echo "| --- | --- | --- | --- | --- | --- |"
} >>"$RES"
fmt_line=""
for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv; do
    for fmt in nv12 yuv420p; do
        a=$(av_md5 "$S/$f" none pix_fmt=$fmt)
        "$BUILD/hw_decode_av" "$S/$f" - md5=1 stats=1 "device=$HW" "${PATH_OPT[@]}" pix_fmt=$fmt >"$OUT/hw.stdout" 2>"$OUT/hw.stderr"
        b=$(sed -n 's/^md5=//p' "$OUT/hw.stdout")
        cnt=$(sed -n 's/^帧数：[0-9]*（硬件帧 \([0-9]*\)，内存帧 \([0-9]*\)）/\1 \/ \2/p' "$OUT/hw.stdout")
        [ -z "$fmt_line" ] && fmt_line=$(grep "^硬件帧" "$OUT/hw.stderr" | head -1)
        if [ -n "$a" ] && [ "$a" = "$b" ]; then
            res="逐字节一致"
        else
            "$BUILD/hw_decode_av" "$S/$f" "$OUT/sw.nv12" device=none pix_fmt=nv12 2>/dev/null
            "$BUILD/hw_decode_av" "$S/$f" "$OUT/hw.nv12" "device=$HW" "${PATH_OPT[@]}" pix_fmt=nv12 2>/dev/null
            p=$(psnr "$OUT/hw.nv12" "$OUT/sw.nv12" 640x360)
            res="**不一致**，PSNR ${p:-?} dB"
            rm -f "$OUT/sw.nv12" "$OUT/hw.nv12"
        fi
        echo "| $f | $fmt | \`${a:0:8}…\` | \`${b:0:8}…\` | $res | ${cnt:-?} |" >>"$RES"
    done
done
{
    echo
    echo "第一次拿到硬件帧时 hw_decode_av 打印的格式列表（硬件帧能直接传成什么内存格式）："
    echo
    echo '```text'
    echo "${fmt_line:-（没有硬件帧：硬件没有被用上）}"
    echo '```'
    echo
    echo "不指定 pix_fmt 时传成哪种格式（“不指定格式时传成第一种”后面括号里的就是）："
    echo
    echo '```text'
} >>"$RES"
x=$("$BUILD/hw_decode_av" "$S/small_360p25.mp4" - "device=$HW" "${PATH_OPT[@]}" frames=1 2>&1 | grep "^硬件帧")
echo "${x:-（没有硬件帧：硬件没有被用上）}" >>"$RES"
echo '```' >>"$RES"

# ---- 待补 4：速度 -------------------------------------------------------------------
echo "待补 4：速度（demo_1080p30.mp4，300 帧，每种跑 3 次取居中的）"
big=$S/demo_1080p30.mp4
bench "$OUT/b_sw1"  "$big" none threads=1
bench "$OUT/b_swN"  "$big" none
bench "$OUT/b_hw"   "$big" "$HW" pix_fmt=nv12
{
    echo
    echo "## 待补 4：1080p 300 帧的速度"
    echo
    echo "每种跑 3 次，取墙钟居中的一次。CPU 时间是进程的用户时间加系统时间，所有线程加起来。"
    echo
    echo "| 方式 | 墙钟（秒） | 帧/秒 | CPU 时间（秒） | 搬运耗时（秒） |"
    echo "| --- | --- | --- | --- | --- |"
    for row in "软件，1 线程:b_sw1" "软件，自动线程:b_swN" "硬件（$HW），传成 nv12:b_hw"; do
        label=${row%%:*}; file=$OUT/${row##*:}
        w=$(stat_of "$file" '耗时（墙钟）')
        fps=$(sed -n 's/^耗时（墙钟）：[0-9.]* 秒，\([0-9.]*\) 帧\/秒/\1/p' "$file")
        c=$(stat_of "$file" 'CPU 时间（用户+系统，所有线程）')
        t=$(stat_of "$file" '搬运耗时（显存拷回内存）')
        echo "| $label | ${w:-?} | ${fps:-?} | ${c:-?} | ${t:-?} |"
    done
    echo
    echo "本机线程数：$(nproc)。"
} >>"$RES"
rm -f "$OUT"/b_sw1 "$OUT"/b_swN "$OUT"/b_hw

# ---- 待补 5：显卡解不了的视频 ---------------------------------------------------------
echo "待补 5：显卡解不了的视频"
# 10 位 H.264（High 10 profile）：很多显卡不支持。-threads 1 让编码结果不随机器变
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x240:rate=25:duration=2 -pix_fmt yuv420p10le \
       -c:v libx264 -profile:v high10 -threads 1 "$OUT/h264_10bit.mkv" 2>/dev/null
{
    echo
    echo "## 待补 5：显卡解不了的视频"
    echo
    echo "| 素材 | device=$DEV fallback=1 的结果 | 和软件解码比 |"
    echo "| --- | --- | --- |"
} >>"$RES"
for in in "$OUT/h264_10bit.mkv" "$S/video_360p25.m1v"; do
    [ -f "$in" ] || continue
    name=$(basename "$in")
    "$BUILD/hw_decode_av" "$in" - md5=1 stats=1 "device=$HW" fallback=1 "${PATH_OPT[@]}" >"$OUT/fb.stdout" 2>"$OUT/fb.stderr"
    b=$(sed -n 's/^md5=//p' "$OUT/fb.stdout")
    a=$(av_md5 "$in" none)
    nhw=$(sed -n 's/^帧数：[0-9]*（硬件帧 \([0-9]*\)，.*/\1/p' "$OUT/fb.stdout")
    cnt=$(sed -n 's/^帧数：[0-9]*（\(.*\)）/\1/p' "$OUT/fb.stdout")
    how=$(grep "^警告\|^没有可用" "$OUT/fb.stderr" | head -1)
    if [ "${nhw:-0}" -gt 0 ]; then
        how="用上了硬件"
    elif [ -z "$how" ]; then
        how="软件解码"
    fi
    if [ -n "$a" ] && [ "$a" = "$b" ]; then res="逐字节一致"; else res="不一致（软件 ${a:0:8}…，本次 ${b:0:8}…）"; fi
    echo "| $name | $how（$cnt） | $res |" >>"$RES"
done
rm -f "$OUT/h264_10bit.mkv" "$OUT/fb.stdout" "$OUT/fb.stderr" "$OUT/hw.stdout" "$OUT/hw.stderr"

# ---- 命令行对照 ---------------------------------------------------------------------
echo "附：ffmpeg 命令行的硬件解码"
{
    echo
    echo "## 附：ffmpeg 命令行的硬件解码，和 hw_decode_av 比"
    echo
    echo '```console'
    if [ "$STANDIN" = 1 ]; then
        echo "（替身模式不运行）"
    else
        hwdev=()
        [ -n "${DEVICE_PATH:-}" ] && hwdev=(-hwaccel_device "$DEVICE_PATH")
        echo "\$ ffmpeg -hwaccel $DEV ${hwdev[*]:-} -i small_360p25.mp4 -fps_mode passthrough -f rawvideo -pix_fmt nv12 - | md5sum"
        c=$(ffmpeg -v error -hwaccel "$DEV" "${hwdev[@]}" -i "$S/small_360p25.mp4" -fps_mode passthrough -f rawvideo -pix_fmt nv12 - 2>"$OUT/cli.err" | md5sum | cut -d' ' -f1)
        echo "$c"
        echo "\$ hw_decode_av small_360p25.mp4 - device=$HW pix_fmt=nv12 md5=1"
        echo "md5=$(av_md5 "$S/small_360p25.mp4" "$HW" pix_fmt=nv12)"
        [ -s "$OUT/cli.err" ] && head -5 "$OUT/cli.err"
    fi
    echo '```'
} >>"$RES"
rm -f "$OUT/cli.err"

echo
echo "结果写在 $RES"
