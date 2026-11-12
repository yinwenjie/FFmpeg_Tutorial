#!/usr/bin/env bash
#
# L21：没有显卡也能做的检查。
#
#   1. 软件路径：hw_encode_av 退回 libx264 编码，输出的码流和 ffmpeg 命令行逐字节一致（多种输入格式、尺寸、帧率、编码选项）；
#   2. nv12_linesize：fixed 读法总是对的，naive 读法（官方示例原版）在行有填充的宽度上是错的（patch 0099）；
#   3. 出错路径：设备用不了、编码器不存在、输入不对、选项不对……退出码都是 1，用 ASan 构建时没有内存错误和泄漏；
#   4. 官方示例的课程拷贝 vaapi_encode、vaapi_transcode 在没有设备时也是退出码 1（patch 0100、0102 之前是 255、187）；
#   5. hw_enc_info 能跑完，退出码 0。
#
# 用法：test_nodevice.sh            用 build/ 里的程序
#       BUILD=build-asan test_nodevice.sh   用别的构建目录（要先 make SANITIZE=address,undefined BUILD=build-asan）
#
# 这个脚本能不能给出“通过”，不取决于机器上有没有显卡。有显卡的机器上，auto 会真的用上硬件编码器，
# 所以第 1 项显式指定 enc=libx264；第 3、4 项里“设备用不了”的几项只在没有 VAAPI 设备时才成立，有设备时跳过。
# 硬件那一半见 verify_hw.sh。
#
# 退出码：0 全部通过，1 有失败。

set -u
cd "$(dirname "$0")"
BUILD=${BUILD:-build}
S=../samples
pass=0
fail=0

for p in vaapi_encode vaapi_transcode hw_enc_info hw_encode_av nv12_linesize; do
    if [ ! -x "$BUILD/$p" ]; then
        echo "找不到 $BUILD/$p，先运行 make（或者 make SANITIZE=address,undefined BUILD=$BUILD）" >&2
        exit 1
    fi
done
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "找不到 ffmpeg，没法做命令行对照" >&2
    exit 1
fi
for f in small_360p25.mp4 demo_1080p30.mp4; do
    if [ ! -f "$S/$f" ]; then
        echo "缺素材 $S/$f，先运行 scripts/gen_samples.sh" >&2
        exit 1
    fi
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0
export UBSAN_OPTIONS=print_stacktrace=1

ok()  { pass=$((pass + 1)); echo "  通过  $1"; }
bad() { fail=$((fail + 1)); echo "  失败  $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | head -8 | sed 's/^/        /'; }

# ---- 准备输入（原始像素文件） -----------------------------------------------------
raw() { # raw 输出 素材 像素格式 宽 高 [帧数]
    local out=$1 src=$2 fmt=$3 w=$4 h=$5 n=${6:-0}
    local extra=()
    [ "$n" != 0 ] && extra=(-frames:v "$n")
    ffmpeg -v error -y -i "$src" "${extra[@]}" -vf "scale=$w:$h" -pix_fmt "$fmt" -f rawvideo "$out"
}
raw "$tmp/360.nv12"   "$S/small_360p25.mp4" nv12     640 360
raw "$tmp/360.yuv"    "$S/small_360p25.mp4" yuv420p  640 360
raw "$tmp/352.nv12"   "$S/small_360p25.mp4" nv12     352 288 10
raw "$tmp/854.yuv"    "$S/small_360p25.mp4" yuv420p  854 480 10
raw "$tmp/1000.nv12"  "$S/demo_1080p30.mp4" nv12    1000 562 10
raw "$tmp/1080.nv12"  "$S/demo_1080p30.mp4" nv12    1920 1080 20

# ---- 1. 软件路径和命令行逐字节一致 ------------------------------------------------
echo "1. 退回 libx264 的码流和 ffmpeg 命令行比较（输出码流的 MD5）"
# 命令行：裸像素输入，libx264，单线程，裸 H.264 输出
cli_md5() { # cli_md5 文件 像素格式 宽 高 帧率 [ffmpeg 选项...]
    local f=$1 fmt=$2 w=$3 h=$4 r=$5
    shift 5
    ffmpeg -v error -f rawvideo -pix_fmt "$fmt" -s "${w}x${h}" -r "$r" -i "$f" "$@" -c:v libx264 -threads 1 -f h264 - 2>/dev/null | md5sum | cut -d' ' -f1
}
my_md5() { # my_md5 文件 宽 高 [选项...]
    local f=$1 w=$2 h=$3
    shift 3
    "$BUILD/hw_encode_av" "$w" "$h" "$f" - md5=1 enc=libx264 "$@" 2>/dev/null | sed -n 's/^md5=//p'
}
cmp_md5() { # cmp_md5 说明 命令行值 本程序值
    if [ -n "$2" ] && [ "$2" = "$3" ]; then ok "$1"; else bad "$1：命令行 $2，本程序 $3"; fi
}
cmp_md5 "640x360 nv12，125 帧，默认选项" "$(cli_md5 "$tmp/360.nv12" nv12 640 360 25)" "$(my_md5 "$tmp/360.nv12" 640 360)"
cmp_md5 "640x360 yuv420p 输入（pix_fmt=yuv420p）" "$(cli_md5 "$tmp/360.yuv" yuv420p 640 360 25)" "$(my_md5 "$tmp/360.yuv" 640 360 pix_fmt=yuv420p)"
cmp_md5 "352x288（行有填充），10 帧" "$(cli_md5 "$tmp/352.nv12" nv12 352 288 25)" "$(my_md5 "$tmp/352.nv12" 352 288)"
cmp_md5 "854x480 yuv420p（行有填充），10 帧" "$(cli_md5 "$tmp/854.yuv" yuv420p 854 480 25)" "$(my_md5 "$tmp/854.yuv" 854 480 pix_fmt=yuv420p)"
cmp_md5 "1000x562（行有填充），10 帧" "$(cli_md5 "$tmp/1000.nv12" nv12 1000 562 25)" "$(my_md5 "$tmp/1000.nv12" 1000 562)"
cmp_md5 "1920x1080，20 帧" "$(cli_md5 "$tmp/1080.nv12" nv12 1920 1080 25)" "$(my_md5 "$tmp/1080.nv12" 1920 1080)"
cmp_md5 "fps=30" "$(cli_md5 "$tmp/360.nv12" nv12 640 360 30)" "$(my_md5 "$tmp/360.nv12" 640 360 fps=30)"
cmp_md5 "frames=10 和 -frames:v 10" "$(cli_md5 "$tmp/360.nv12" nv12 640 360 25 -frames:v 10)" "$(my_md5 "$tmp/360.nv12" 640 360 frames=10)"
cmp_md5 "opts=preset=veryfast:crf=28 和 -preset veryfast -crf 28" \
    "$(cli_md5 "$tmp/360.nv12" nv12 640 360 25 -preset veryfast -crf 28)" "$(my_md5 "$tmp/360.nv12" 640 360 opts=preset=veryfast:crf=28)"
a=$(my_md5 "$tmp/360.nv12" 640 360 threads=1); b=$("$BUILD/hw_encode_av" 640 360 - - md5=1 enc=libx264 2>/dev/null < "$tmp/360.nv12" | sed -n 's/^md5=//p')
cmp_md5 "从标准输入读（输入文件写 -）和读文件一样" "$a" "$b"
"$BUILD/hw_encode_av" 640 360 "$tmp/360.nv12" "$tmp/out.h264" md5=1 enc=libx264 >"$tmp/o.txt" 2>/dev/null
a=$(md5sum < "$tmp/out.h264" | cut -d' ' -f1); b=$(sed -n 's/^md5=//p' "$tmp/o.txt")
cmp_md5 "写出的文件的 md5sum 和 md5=1 一致" "$a" "$b"
# 解码出来的帧数等于输入帧数
n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$tmp/out.h264" 2>/dev/null)
if [ "$n" = 125 ]; then ok "输出码流解码后是 125 帧"; else bad "输出码流解码后的帧数：$n，应该是 125"; fi

# ---- 2. nv12_linesize ---------------------------------------------------------------
echo "2. nv12_linesize：fixed 读法总是对的，naive 读法在行有填充时是错的"
"$BUILD/nv12_linesize" table > "$tmp/table.txt" 2>&1 && ok "table 能运行" || bad "table 失败" "$(cat "$tmp/table.txt")"
for spec in "352 288 $tmp/352.nv12" "1000 562 $tmp/1000.nv12" "640 360 $tmp/360.nv12"; do
    set -- $spec
    w=$1; h=$2; f=$3
    "$BUILD/nv12_linesize" fixed "$w" "$h" "$f" "$tmp/fixed.nv12" >/dev/null 2>&1
    if cmp -s "$f" "$tmp/fixed.nv12"; then ok "fixed ${w}x${h}：输出和输入逐字节一样"; else bad "fixed ${w}x${h}：输出和输入不一样"; fi
    "$BUILD/nv12_linesize" naive "$w" "$h" "$f" "$tmp/naive.nv12" >/dev/null 2>&1
    ls=$("$BUILD/nv12_linesize" table "$w" | awk -v w="$w" '$1 == w { print $2 }')
    if [ "$ls" = "$w" ]; then
        if cmp -s "$f" "$tmp/naive.nv12"; then ok "naive ${w}x${h}：linesize 等于宽，碰巧一样"; else bad "naive ${w}x${h}：linesize 等于宽，却不一样"; fi
    else
        if cmp -s "$f" "$tmp/naive.nv12"; then bad "naive ${w}x${h}：linesize 是 $ls，应该读歪"; else ok "naive ${w}x${h}：linesize 是 $ls，读歪了（和 fixed 不一样）"; fi
    fi
done
head -c 100000 "$tmp/360.nv12" > "$tmp/part.nv12"
"$BUILD/nv12_linesize" fixed 640 360 "$tmp/part.nv12" "$tmp/x.nv12" >/dev/null 2>&1; [ $? -eq 1 ] && ok "输入不是整数帧：退出码 1" || bad "输入不是整数帧"
"$BUILD/nv12_linesize" fixed 640 360 /nonexistent "$tmp/x.nv12" >/dev/null 2>&1; [ $? -eq 1 ] && ok "输入不存在：退出码 1" || bad "nv12_linesize 输入不存在"
"$BUILD/nv12_linesize" >/dev/null 2>&1; [ $? -eq 1 ] && ok "缺参数：退出码 1" || bad "nv12_linesize 缺参数"

# ---- 3. 出错路径 -------------------------------------------------------------------
echo "3. 出错路径：退出码 1，没有 sanitizer 报告"
# expect_fail 说明 命令...   ：命令必须以 1 退出，stderr 里不能有 sanitizer 的报告
expect_fail() {
    local label=$1 out rc
    shift
    out=$("$@" 2>&1 >/dev/null)
    rc=$?
    if [ $rc -ne 1 ]; then
        bad "$label：退出码 $rc，应该是 1" "$out"
    elif printf '%s' "$out" | grep -q "Sanitizer\|runtime error"; then
        bad "$label：sanitizer 报告" "$out"
    else
        ok "$label"
    fi
}
have_vaapi=0
"$BUILD/hw_enc_info" try=1 2>/dev/null | grep -q "vaapi *创建成功" && have_vaapi=1
E="$BUILD/hw_encode_av"
N="$tmp/360.nv12"

if [ $have_vaapi -eq 0 ]; then
    expect_fail "enc=h264_vaapi，机器上没有设备"            "$E" 640 360 "$N" - enc=h264_vaapi
    expect_fail "enc=hevc_vaapi，机器上没有设备"            "$E" 640 360 "$N" - enc=hevc_vaapi
else
    echo "  跳过  机器上有 VAAPI 设备，“设备用不了”的几项不成立"
fi
expect_fail "enc=bogus，没有这个编码器"                    "$E" 640 360 "$N" - enc=bogus
expect_fail "enc=h264_v4l2m2m，没有 /dev/video 设备"       "$E" 640 360 "$N" - enc=h264_v4l2m2m
expect_fail "enc=mpeg4，不支持 nv12 输入"                  "$E" 640 360 "$N" - enc=mpeg4
expect_fail "opts 里有编码器不认识的选项"                  "$E" 640 360 "$N" - enc=libx264 opts=nosuch=1
expect_fail "opts 的写法不对"                              "$E" 640 360 "$N" - enc=libx264 opts=crf
expect_fail "输入文件不存在"                               "$E" 640 360 /nonexistent - enc=libx264
expect_fail "输出文件写不了"                               "$E" 640 360 "$N" /nonexistent/x.h264 enc=libx264
expect_fail "输入文件的大小不是整数帧"                     "$E" 640 360 "$tmp/part.nv12" - enc=libx264
expect_fail "输入宽高对不上（640x360 的文件当 642x360）"   "$E" 642 360 "$N" - enc=libx264
expect_fail "宽不是数字"                                   "$E" abc 360 "$N" -
expect_fail "不认识的像素格式"                             "$E" 640 360 "$N" - pix_fmt=bogus
expect_fail "不认识的日志级别"                             "$E" 640 360 "$N" - loglevel=bogus
expect_fail "不认识的选项"                                 "$E" 640 360 "$N" - nosuch=1
expect_fail "选项的值不对（pool=0）"                       "$E" 640 360 "$N" - pool=0
expect_fail "缺参数"                                       "$E" 640 360

# 退回软件：成功，输出和 enc=libx264 一样
if [ $have_vaapi -eq 0 ]; then
    a=$(my_md5 "$N" 640 360)
    b=$("$E" 640 360 "$N" - md5=1 enc=h264_vaapi fallback=1 2>/dev/null | sed -n 's/^md5=//p')
    c=$("$E" 640 360 "$N" - md5=1 2>/dev/null | sed -n 's/^md5=//p')
    if [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" = "$c" ]; then
        ok "没有设备时，fallback=1 和 enc=auto 都退回 libx264，输出和 enc=libx264 一样"
    else
        bad "退回软件的输出不一样：libx264 $a，fallback=1 $b，auto $c"
    fi
fi

# ---- 4. 官方示例的课程拷贝 ---------------------------------------------------------
echo "4. 官方示例 vaapi_encode、vaapi_transcode 的课程拷贝：没有设备时退出码 1"
if [ $have_vaapi -eq 0 ]; then
    expect_fail "vaapi_encode，没有设备"                   "$BUILD/vaapi_encode" 640 360 "$N" "$tmp/x.h264"
    expect_fail "vaapi_transcode，没有设备"                "$BUILD/vaapi_transcode" "$S/small_360p25.mp4" h264_vaapi "$tmp/x.mp4"
fi
expect_fail "vaapi_encode，缺参数"                         "$BUILD/vaapi_encode" 640 360
expect_fail "vaapi_encode，宽高不对"                       "$BUILD/vaapi_encode" 0 0 "$N" "$tmp/x.h264"
expect_fail "vaapi_encode，输入文件不存在"                 "$BUILD/vaapi_encode" 640 360 /nonexistent "$tmp/x.h264"
expect_fail "vaapi_encode，输出文件写不了"                 "$BUILD/vaapi_encode" 640 360 "$N" /nonexistent/x.h264
expect_fail "vaapi_transcode，缺参数"                      "$BUILD/vaapi_transcode" "$S/small_360p25.mp4"

# ---- 5. hw_enc_info ----------------------------------------------------------------
echo "5. hw_enc_info"
if "$BUILD/hw_enc_info" try=1 >"$tmp/info.txt" 2>&1; then ok "hw_enc_info try=1 退出码 0"; else bad "hw_enc_info try=1 失败" "$(cat "$tmp/info.txt")"; fi
if grep -q "^1\. " "$tmp/info.txt" && grep -q "^2\. " "$tmp/info.txt" && grep -q "^3\. " "$tmp/info.txt" && grep -q "^4\. " "$tmp/info.txt"; then
    ok "四层都打印了"
else
    bad "输出里缺了某一层" "$(cat "$tmp/info.txt")"
fi
"$BUILD/hw_enc_info" codec=libx264 >"$tmp/info2.txt" 2>&1 && grep -q "^   libx264 " "$tmp/info2.txt" && ok "codec=libx264 只看一个编码器" || bad "codec=libx264" "$(cat "$tmp/info2.txt")"
"$BUILD/hw_enc_info" codec=bogus >/dev/null 2>&1; [ $? -eq 1 ] && ok "codec=bogus：退出码 1" || bad "hw_enc_info codec=bogus"
"$BUILD/hw_enc_info" nosuch=1 >/dev/null 2>&1; [ $? -eq 1 ] && ok "不认识的选项退出码 1" || bad "hw_enc_info 不认识的选项"

echo
echo "共 $((pass + fail)) 项：通过 $pass，失败 $fail"
[ $fail -eq 0 ]
