#!/usr/bin/env bash
# 在有 VAAPI 设备的机器上验证 mtool transcode --hw vaapi（L21 的做法：软件解码 → hwupload → VAAPI 编码）。
# 没有设备时说明原因，退出码 77（跳过，不算失败）：自动化测试里没有显卡的那部分（--hw vaapi 报错、--hw auto 退回软件）
# 已经在 tests/run_tests.sh 的 transcode 节里测了，这个脚本测的是“真有设备”时的另一半。
#
# 用法（在仓库根目录，先编译 mtool，环境变量和 L02 一样）：
#   mtool/scripts/verify_hw.sh
#   VAAPI_DEVICE=/dev/dri/renderD129 mtool/scripts/verify_hw.sh      设备不在默认位置时（同时传给 mtool 的 --hw-device）
# 退出码：0 全部通过；1 有失败；77 没有设备，跳过；2 缺工具。
#
# 作者的开发机没有显卡，这个脚本没有在真实的设备上跑过（README 里对应的数字标成了【待补 1】）；
# 有设备的人跑一遍，把下面打印的表填进去。
set -u
cd "$(dirname "$0")/../.."

MT=${MT:-mtool/build/mtool}
DEV=${VAAPI_DEVICE:-/dev/dri/renderD128}
S=samples/mtool
W=out/mtool_hw

[ -x "$MT" ] || { echo "找不到 $MT，先编译：make -C mtool" >&2; exit 2; }
for c in ffmpeg ffprobe; do command -v $c >/dev/null 2>&1 || { echo "找不到 $c" >&2; exit 2; }; done
if [ ! -e "$DEV" ]; then
    echo "没有 $DEV：这台机器没有可用的 VAAPI 设备（没有显卡，或者驱动没装、没有读写权限），跳过。"
    exit 77
fi
ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_vaapi || { echo "这份 FFmpeg 没有 h264_vaapi 编码器（编译时要有 libva）。" >&2; exit 2; }
[ -f "$S/h264_aac.mp4" ] || mtool/scripts/gen_testfiles.sh >/dev/null || exit 2

rm -rf "$W"; mkdir -p "$W"
fail=0
printf '%-34s %-6s %s\n' 项目 结果 数字
row() { printf '%-34s %-6s %s\n' "$1" "$2" "$3"; [ "$2" = 通过 ] || fail=$((fail + 1)); }
dur() { ffprobe -v error -show_entries format=duration -of csv=p=0 "$1" 2>/dev/null | head -n 1; }
frames() { ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$1" 2>/dev/null; }
close() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN {d = a - b; if (d < 0) d = -d; exit (d <= t) ? 0 : 1}'; }

# 1. 基本：--hw vaapi 转码，用的是 h264_vaapi，时长误差 <0.1 秒，帧数不变，能完整解码
if "$MT" transcode -y --hw vaapi --hw-device "$DEV" "$S/h264_aac.mp4" "$W/a.mp4" >"$W/a.out" 2>"$W/a.err"; then
    grep -q h264_vaapi "$W/a.out" && enc=是 || enc=否
    d=$(dur "$W/a.mp4"); n=$(frames "$W/a.mp4")
    if [ $enc = 是 ] && close "$d" 5.0 0.1 && [ "$n" = 125 ] && [ -z "$(ffmpeg -v error -i "$W/a.mp4" -f null - 2>&1)" ]; then
        row "--hw vaapi" 通过 "h264_vaapi，时长 $d 秒，$n 帧，$(stat -c %s "$W/a.mp4") 字节"
    else
        row "--hw vaapi" 失败 "用 h264_vaapi：$enc，时长 $d，帧数 $n"
    fi
else
    row "--hw vaapi" 失败 "退出码非 0：$(tail -n 1 "$W/a.err")"
fi
# 2. --crf 在 VAAPI 里换成恒定质量 qp：值越大文件越小
"$MT" transcode -y --hw vaapi --hw-device "$DEV" --crf 20 "$S/h264_aac.mp4" "$W/q20.mp4" >/dev/null 2>&1
"$MT" transcode -y --hw vaapi --hw-device "$DEV" --crf 35 "$S/h264_aac.mp4" "$W/q35.mp4" >/dev/null 2>&1
a=$(stat -c %s "$W/q20.mp4" 2>/dev/null || echo 0); b=$(stat -c %s "$W/q35.mp4" 2>/dev/null || echo 0)
[ "$a" -gt "$b" ] && [ "$b" -gt 0 ] && row "--crf 20 对 35" 通过 "$a 字节 > $b 字节" || row "--crf 20 对 35" 失败 "$a 字节，$b 字节"
# 3. 缩放在软件里做，再上传
"$MT" transcode -y --hw vaapi --hw-device "$DEV" -w 320 "$S/h264_aac.mp4" "$W/w.mp4" >/dev/null 2>&1
s=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$W/w.mp4" 2>/dev/null)
[ "$s" = 320x180 ] && row "-w 320" 通过 "$s" || row "-w 320" 失败 "${s:-没有输出}"
# 4. 裁剪
"$MT" transcode -y --hw vaapi --hw-device "$DEV" -s 1.5 -d 2 "$S/h264_aac.mp4" "$W/t.mp4" >/dev/null 2>&1
d=$(dur "$W/t.mp4")
[ -n "$d" ] && close "$d" 2.0 0.1 && row "-s 1.5 -d 2" 通过 "时长 $d 秒" || row "-s 1.5 -d 2" 失败 "时长 ${d:-没有输出}"
# 5. 旋转的输入：转正以后上传
"$MT" transcode -y --hw vaapi --hw-device "$DEV" "$S/rotate90.mp4" "$W/r.mp4" >/dev/null 2>&1
s=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$W/r.mp4" 2>/dev/null)
[ "$s" = 360x640 ] && row "旋转 90 度的输入" 通过 "$s" || row "旋转 90 度的输入" 失败 "${s:-没有输出}"
# 6. --hw auto 有设备时用 VAAPI
"$MT" transcode -y --hw auto --hw-device "$DEV" "$S/h264_aac.mp4" "$W/auto.mp4" >"$W/auto.out" 2>/dev/null
grep -q h264_vaapi "$W/auto.out" && row "--hw auto" 通过 "用的是 h264_vaapi" || row "--hw auto" 失败 "没有用 h264_vaapi"
# 7. 和 ffmpeg 命令行用同样的设备同样的链路，帧数相同
ffmpeg -v error -y -vaapi_device "$DEV" -i "$S/h264_aac.mp4" -vf format=nv12,hwupload -c:v h264_vaapi -c:a aac "$W/cli.mp4" 2>/dev/null
a=$(frames "$W/a.mp4"); b=$(frames "$W/cli.mp4")
[ -n "$a" ] && [ "$a" = "$b" ] && row "帧数和 ffmpeg 命令行相同" 通过 "$a 帧" || row "帧数和 ffmpeg 命令行相同" 失败 "mtool $a 帧，命令行 ${b:-没有输出}"

echo
if [ $fail -eq 0 ]; then echo "全部通过。把上面的表填进 mtool/README.md 的【待补 1】。"; exit 0; fi
echo "有 $fail 项失败。"; exit 1
