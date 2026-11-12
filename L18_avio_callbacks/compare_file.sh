#!/usr/bin/env bash
# 把自定义 I/O 的结果，和“libavformat 自己读写文件”的结果比较。
#
# 用法（在仓库根目录）：
#   make -C L18_avio_callbacks
#   L18_avio_callbacks/compare_file.sh
#
# 分三部分：
#   一、输入：avio_mem_in 数出来的包的个数，和 ffprobe 数出来的是否一样
#   二、输出：自己的写回调、avio_open_dyn_buf 写出来的结果，和直接写文件是否逐字节相同
#   三、往返：写回调里“加密”，读回调里“解密”，包的个数是否对得上
#
# 输入文件由 samples/ 里的素材和 ffmpeg 现做，放在 out/l18_cmp/ 下。
# 每一项打印一行“相同”或“不同”，最后汇总。

set -u
cd "$(dirname "$0")/.."

IN=L18_avio_callbacks/build/avio_mem_in
OUT=L18_avio_callbacks/build/avio_mem_out
D=out/l18_cmp
S=samples
mkdir -p "$D"

for b in "$IN" "$OUT"; do
    if [ ! -x "$b" ]; then
        echo "找不到 $b，请先 make -C L18_avio_callbacks" >&2
        exit 1
    fi
done
if [ ! -f "$S/small_360p25.mp4" ] || [ ! -f "$S/audio_44k_stereo.mp2" ]; then
    echo "找不到 $S/ 里的素材，请先运行 scripts/gen_samples.sh" >&2
    exit 1
fi

same=0
diff=0
known=0
diff_list=""

report() {          # report 相同|不同|已知 名字 [说明]
    case $1 in
        相同) same=$((same + 1)) ;;
        已知) known=$((known + 1)) ;;
        不同) diff=$((diff + 1)); diff_list="$diff_list $2" ;;
    esac
    printf '%-4s  %s%s\n' "$1" "$2" "${3:+  $3}"
}

# ---- 一、输入 ----------------------------------------------------------------
# ffprobe 数出来的每路流的包数，写成 125/217
probe_counts() {
    ffprobe -v error -count_packets -show_entries stream=nb_read_packets -of csv=p=0 "$1" |
        paste -sd/
}
# avio_mem_in 数出来的，同样写成 125/217
mine_counts() {
    "$IN" "$@" 2>/dev/null | sed -n 's/^  流 #[0-9]*：\([0-9]*\) 个包.*/\1/p' | paste -sd/
}

echo "== 输入：包的个数（avio_mem_in 和 ffprobe）"
for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv audio_44k_stereo.mp2; do
    want=$(probe_counts "$S/$f")
    got=$(mine_counts "$S/$f")
    if [ "$got" = "$want" ]; then report 相同 "$f 有 seek 回调" "（$want）"
    else report 不同 "$f 有 seek 回调" "（avio_mem_in $got，ffprobe $want）"; fi

    got=$(mine_counts "$S/$f" seek=0)
    if [ "$f" = small_360p25.mp4 ]; then
        # moov 在文件尾，没有 seek 回调读不出包：预期读不到
        if [ -z "$got" ] || [ "$got" = "0/0" ]; then report 已知 "$f 没有 seek 回调" "（读不出包，预期如此）"
        else report 不同 "$f 没有 seek 回调" "（这个文件应该读不出包，却读到 $got）"; fi
    elif [ "$got" = "$want" ]; then report 相同 "$f 没有 seek 回调" "（$want）"
    else report 不同 "$f 没有 seek 回调" "（avio_mem_in $got，ffprobe $want）"; fi
done

# 管道：读不回头
cat "$S/small_360p25.ts" | "$IN" - 2>/dev/null | grep -q '^流参数完整' &&
    report 相同 "ts 从管道读" "（参数完整）" || report 不同 "ts 从管道读"
cat "$S/small_360p25.mp4" | "$IN" - packets=0 >/dev/null 2>&1
if [ $? -eq 2 ]; then report 已知 "mp4 从管道读" "（参数不完整，退出码 2，预期如此）"
else report 不同 "mp4 从管道读" "（应该参数不完整）"; fi
"$IN" - packets=0 < "$S/small_360p25.mp4" >/dev/null 2>&1 &&
    report 相同 "mp4 用 < 重定向读" "（可以 seek，参数完整）" || report 不同 "mp4 用 < 重定向读"

# ---- 二、输出 ----------------------------------------------------------------
echo "== 输出：写回调、dyn_buf 和写文件，逐字节比较（bitexact=1）"
M=$S/small_360p25.mp4
A=$S/audio_44k_stereo.mp2
ff() { ffmpeg -v error -y "$@" || { echo "ffmpeg 命令失败：$*" >&2; exit 1; }; }
ff -i $A -t 1 $D/a.wav
ff -i $A -t 1 $D/a.aiff
ff -i $A -t 1 -c:a pcm_s16le $D/a.caf
ff -i $A -t 1 -c:a wmav2 $D/a.wma
ff -i $A -t 1 -c:a flac $D/a.flac

# check 名字 输入 格式 扩展名 [参数...]：
#   cb（有 seek 回调）、dyn（avio_open_dyn_buf）两种输出，各和 mode=file 比较
check() {
    local name=$1 in=$2 fmt=$3 ext=$4
    shift 4
    "$OUT" "$in" "$fmt" "$D/$name.file.$ext" mode=file bitexact=1 "$@" >/dev/null 2>&1 ||
        { report 不同 "$name 写文件" "（程序失败）"; return; }
    "$OUT" "$in" "$fmt" "$D/$name.cb.$ext" mode=cb bitexact=1 "$@" >/dev/null 2>&1
    if cmp -s "$D/$name.cb.$ext" "$D/$name.file.$ext"; then report 相同 "$name 写回调"
    else report 不同 "$name 写回调"; fi

    "$OUT" "$in" "$fmt" "$D/$name.dyn.$ext" mode=dyn bitexact=1 "$@" >/dev/null 2>&1
    if cmp -s "$D/$name.dyn.$ext" "$D/$name.file.$ext"; then
        report 相同 "$name dyn_buf"
    else
        # 已知问题：libavformat 的 avio_close_dyn_buf 把补的 64 字节零写在了数据中间（patch 0084）。
        # 关闭之前先把写位置移到末尾（dynend=1）就能绕开，能绕开的算“已知”，绕不开的才是真不同。
        "$OUT" "$in" "$fmt" "$D/$name.dynend.$ext" mode=dyn dynend=1 bitexact=1 "$@" >/dev/null 2>&1
        if cmp -s "$D/$name.dynend.$ext" "$D/$name.file.$ext"; then
            report 已知 "$name dyn_buf" "（比写文件短 $(( $(stat -c %s "$D/$name.file.$ext") - $(stat -c %s "$D/$name.dyn.$ext") )) 字节；加 dynend=1 后相同，见 patch 0084）"
        else
            report 不同 "$name dyn_buf"
        fi
    fi
}

check mp4      $M mp4      mp4
check matroska $M matroska mkv
check mpegts   $M mpegts   ts
check flv      $M flv      flv
check wav      $D/a.wav  wav  wav
check aiff     $D/a.aiff aiff aiff
check caf      $D/a.caf  caf  caf
check asf      $D/a.wma  asf  wma
check flac     $D/a.flac flac flac

# 不能 seek 的输出：能写的格式，写出来的和写文件相同吗？
echo "== 输出：没有 seek 回调（seek=0）"
for pair in mpegts:ts matroska:mkv flv:flv; do
    fmt=${pair%%:*}; ext=${pair##*:}
    "$OUT" $M $fmt $D/ns.file.$ext mode=file bitexact=1 >/dev/null 2>&1
    "$OUT" $M $fmt $D/ns.cb.$ext mode=cb seek=0 bitexact=1 >/dev/null 2>&1
    if cmp -s $D/ns.cb.$ext $D/ns.file.$ext; then
        report 相同 "$fmt 没有 seek 回调" "（和写文件一样）"
    else
        a=$(stat -c %s $D/ns.cb.$ext); b=$(stat -c %s $D/ns.file.$ext)
        report 已知 "$fmt 没有 seek 回调" "（和写文件不同：回头改的字段没改；$([ "$a" = "$b" ] && echo "都是 $a 字节，内容不同" || echo "$a 对 $b 字节")）"
    fi
done
"$OUT" $M mp4 $D/ns.mp4 seek=0 >/dev/null 2>&1
if [ $? -ne 0 ]; then report 已知 "mp4 没有 seek 回调" "（muxer does not support non seekable output，预期失败）"
else report 不同 "mp4 没有 seek 回调" "（应该失败）"; fi
FRAG=movflags=frag_keyframe+empty_moov+default_base_moof
"$OUT" $M mp4 $D/frag.file.mp4 mode=file bitexact=1 $FRAG >/dev/null 2>&1
"$OUT" $M mp4 $D/frag.cb.mp4 seek=0 bitexact=1 $FRAG >/dev/null 2>&1
if cmp -s $D/frag.cb.mp4 $D/frag.file.mp4; then report 相同 "分片 mp4 没有 seek 回调" "（和写文件一样）"
else report 不同 "分片 mp4 没有 seek 回调"; fi

# ---- 三、往返 ----------------------------------------------------------------
echo "== 往返：写回调里加密，读回调里解密"
want=$(probe_counts "$M")
"$OUT" $M mp4 $D/enc.bin xor=secret >/dev/null 2>&1
got=$(mine_counts $D/enc.bin xor=secret)
if [ "$got" = "$want" ]; then report 相同 "xor=secret 往返" "（包的个数 $want）"
else report 不同 "xor=secret 往返" "（$got，应该是 $want）"; fi
"$IN" $D/enc.bin xor=Secret >/dev/null 2>&1
if [ $? -ne 0 ]; then report 已知 "密钥不对" "（打不开，预期如此）"
else report 不同 "密钥不对" "（应该打不开）"; fi
"$IN" $D/enc.bin >/dev/null 2>&1
if [ $? -ne 0 ]; then report 已知 "不解密" "（打不开，预期如此）"
else report 不同 "不解密" "（应该打不开）"; fi

echo
echo "相同 $same 项；已知的、符合预期的差别 $known 项；不同 $diff 项${diff_list:+：}${diff_list}"
[ "$diff" -eq 0 ]
