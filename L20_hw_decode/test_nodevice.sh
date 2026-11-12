#!/usr/bin/env bash
#
# L20：没有显卡也能做的检查。
#
#   1. 软件路径：hw_decode_av device=none 的输出，和 ffmpeg 命令行逐字节一致（素材有 MP4、MKV、TS、FLV、MPEG-1）；
#   2. 出错路径：设备用不了、设备类型不认识、解码器没有这种硬件配置、输入文件不对……退出码都是 1，用 ASan 构建时没有内存错误和泄漏；
#   3. 官方示例 hw_decode 在没有设备的机器上也是这样（patch 0096 到 0098 之前是 255，并且泄漏）；
#   4. hw_info 能跑完，退出码 0。
#
# 用法：test_nodevice.sh            用 build/ 里的程序
#       BUILD=build-asan test_nodevice.sh   用别的构建目录（要先 make SANITIZE=address,undefined BUILD=build-asan）
#
# 这个脚本能不能给出“通过”，不取决于机器上有没有显卡。有显卡的机器上，auto 会真的用上硬件，
# 所以第 1 项显式写 device=none，第 2 项里的“用不了设备”只在没有 VAAPI 设备时才成立，有设备时跳过。
# 硬件那一半见 verify_hw.sh。
#
# 退出码：0 全部通过，1 有失败。

set -u
cd "$(dirname "$0")"
BUILD=${BUILD:-build}
S=../samples
pass=0
fail=0

for p in hw_decode hw_info hw_decode_av; do
    if [ ! -x "$BUILD/$p" ]; then
        echo "找不到 $BUILD/$p，先运行 make（或者 make SANITIZE=address,undefined BUILD=$BUILD）" >&2
        exit 1
    fi
done
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "找不到 ffmpeg，没法做命令行对照" >&2
    exit 1
fi
for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv video_360p25.m1v demo_1080p30.mp4 audio_44k_stereo.mp2; do
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

# ---- 1. 软件路径和命令行逐字节一致 ------------------------------------------------
echo "1. 软件路径和 ffmpeg 命令行比较（输出的全部字节的 MD5）"
# -fps_mode passthrough：命令行默认会按帧率补帧、丢帧（FLV 的时间戳只有毫秒精度，会补 1 帧），要关掉才是解码器本来的输出
cli_md5() { ffmpeg -v error -i "$1" -fps_mode passthrough -f rawvideo "${@:2}" - 2>/dev/null | md5sum | cut -d' ' -f1; }
my_md5()  { "$BUILD/hw_decode_av" "$1" - md5=1 device=none "${@:2}" 2>/dev/null | sed -n 's/^md5=//p'; }

for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv video_360p25.m1v demo_1080p30.mp4; do
    for fmt in "" nv12 gray; do
        if [ -z "$fmt" ]; then
            a=$(cli_md5 "$S/$f"); b=$(my_md5 "$S/$f"); label="$f 原样输出"
        else
            a=$(cli_md5 "$S/$f" -pix_fmt $fmt); b=$(my_md5 "$S/$f" pix_fmt=$fmt); label="$f pix_fmt=$fmt"
        fi
        if [ -n "$a" ] && [ "$a" = "$b" ]; then ok "$label"; else bad "$label：命令行 $a，本程序 $b"; fi
    done
done
a=$(cli_md5 "$S/small_360p25.mp4" -frames:v 10); b=$(my_md5 "$S/small_360p25.mp4" frames=10)
if [ -n "$a" ] && [ "$a" = "$b" ]; then ok "frames=10 和 -frames:v 10"; else bad "frames=10：命令行 $a，本程序 $b"; fi
b1=$(my_md5 "$S/demo_1080p30.mp4" threads=1); b4=$(my_md5 "$S/demo_1080p30.mp4" threads=4)
if [ -n "$b1" ] && [ "$b1" = "$b4" ]; then ok "threads=1 和 threads=4 的输出相同"; else bad "线程数改变了输出：$b1 $b4"; fi
# 写文件和算 MD5 是同一份数据
"$BUILD/hw_decode_av" "$S/small_360p25.mp4" "$tmp/out.nv12" device=none pix_fmt=nv12 2>/dev/null
a=$(md5sum < "$tmp/out.nv12" | cut -d' ' -f1); b=$(my_md5 "$S/small_360p25.mp4" pix_fmt=nv12)
if [ "$a" = "$b" ]; then ok "写出的文件的 md5sum 和 md5=1 一致"; else bad "文件 $a，md5=1 $b"; fi

# ---- 2. 出错路径 -------------------------------------------------------------------
echo "2. 出错路径：退出码 1，没有 sanitizer 报告"
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
"$BUILD/hw_info" try=1 2>/dev/null | grep -q "vaapi *创建成功" && have_vaapi=1

if [ $have_vaapi -eq 0 ]; then
    expect_fail "device=vaapi，机器上没有设备" "$BUILD/hw_decode_av" "$S/small_360p25.mp4" - device=vaapi
    expect_fail "官方 hw_decode vaapi，机器上没有设备" "$BUILD/hw_decode" vaapi "$S/small_360p25.mp4" "$tmp/x.nv12"
else
    echo "  跳过  机器上有 VAAPI 设备，“设备用不了”的几项不成立"
fi
expect_fail "device=drm，h264 解码器没有 drm 配置"  "$BUILD/hw_decode_av" "$S/small_360p25.mp4" - device=drm
expect_fail "device=bogus，不认识的设备类型"         "$BUILD/hw_decode_av" "$S/small_360p25.mp4" - device=bogus
expect_fail "device=vaapi，MPEG-1 没有硬件配置"      "$BUILD/hw_decode_av" "$S/video_360p25.m1v" - device=vaapi
expect_fail "输入文件不存在"                         "$BUILD/hw_decode_av" /nonexistent -
expect_fail "输入里没有视频流"                       "$BUILD/hw_decode_av" "$S/audio_44k_stereo.mp2" -
expect_fail "输出文件写不了"                         "$BUILD/hw_decode_av" "$S/small_360p25.mp4" /nonexistent/x.nv12 device=none
expect_fail "不认识的像素格式"                       "$BUILD/hw_decode_av" "$S/small_360p25.mp4" - pix_fmt=bogus
expect_fail "不认识的选项"                           "$BUILD/hw_decode_av" "$S/small_360p25.mp4" - nosuch=1
expect_fail "缺参数"                                 "$BUILD/hw_decode_av"
head -c 30000 "$S/small_360p25.mp4" > "$tmp/trunc.mp4"
expect_fail "截断的 MP4（moov 在文件尾，没了）"        "$BUILD/hw_decode_av" "$tmp/trunc.mp4" - device=none
head -c 50000 /dev/urandom > "$tmp/rand.bin"
expect_fail "随机数据"                               "$BUILD/hw_decode_av" "$tmp/rand.bin" - device=none

# fallback=1 / auto：退回软件，成功，输出和纯软件一样
if [ $have_vaapi -eq 0 ]; then
    a=$(my_md5 "$S/small_360p25.mp4")
    b=$("$BUILD/hw_decode_av" "$S/small_360p25.mp4" - md5=1 device=vaapi fallback=1 2>/dev/null | sed -n 's/^md5=//p')
    c=$("$BUILD/hw_decode_av" "$S/small_360p25.mp4" - md5=1 2>/dev/null | sed -n 's/^md5=//p')
    if [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" = "$c" ]; then
        ok "没有设备时，fallback=1 和 device=auto 都退回软件，输出和 device=none 一样"
    else
        bad "退回软件的输出不一样：none $a，fallback=1 $b，auto $c"
    fi
fi
# 解码器不认识的输入：给了硬件设备也得失败，不是崩溃
expect_fail "官方 hw_decode，设备类型不认识"         "$BUILD/hw_decode" bogus "$S/small_360p25.mp4" "$tmp/x.nv12"
expect_fail "官方 hw_decode，MPEG-1 没有 vaapi 配置" "$BUILD/hw_decode" vaapi "$S/video_360p25.m1v" "$tmp/x.nv12"
expect_fail "官方 hw_decode，输入文件不存在"          "$BUILD/hw_decode" vaapi /nonexistent "$tmp/x.nv12"
expect_fail "官方 hw_decode，缺参数"                  "$BUILD/hw_decode" vaapi

# ---- 3. hw_info --------------------------------------------------------------------
echo "3. hw_info"
if "$BUILD/hw_info" try=1 >"$tmp/info.txt" 2>&1; then ok "hw_info try=1 退出码 0"; else bad "hw_info try=1 失败" "$(cat "$tmp/info.txt")"; fi
if grep -q "^1\. " "$tmp/info.txt" && grep -q "^2\. " "$tmp/info.txt" && grep -q "^3\. " "$tmp/info.txt" && grep -q "^4\. " "$tmp/info.txt"; then
    ok "四层都打印了"
else
    bad "输出里缺了某一层" "$(cat "$tmp/info.txt")"
fi
"$BUILD/hw_info" codec=h264 >"$tmp/info2.txt" 2>&1 && grep -q "^  h264 " "$tmp/info2.txt" && ok "codec=h264 只看一个解码器" || bad "codec=h264" "$(cat "$tmp/info2.txt")"
"$BUILD/hw_info" nosuch=1 >/dev/null 2>&1; [ $? -eq 1 ] && ok "不认识的选项退出码 1" || bad "hw_info 不认识的选项"

echo
echo "共 $((pass + fail)) 项：通过 $pass，失败 $fail"
[ $fail -eq 0 ]
