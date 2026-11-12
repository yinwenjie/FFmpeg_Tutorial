#!/usr/bin/env bash
# 用 ASan/UBSan（或 valgrind）检查课程里 L02 到 L19 的全部程序：
#   1. 每个程序用正常的输入跑一遍，退出码必须是 0；
#   2. 再用“坏输入”跑一遍（空文件、乱码、截断的文件、只有视频或只有音频的文件、一张图片、
#      17x15 的奇数尺寸），不要求成功，但不能崩溃、不能超时、不能有任何 sanitizer 报告；
#   3. 最后用不存在的输入、不存在的输出目录各跑几个。
#
# 用法（在仓库根目录）：
#   L22_engineering/check_all.sh                 # ASan + UBSan，全部课次
#   L22_engineering/check_all.sh asan L16 L17    # 只检查某几讲
#   L22_engineering/check_all.sh valgrind L05    # valgrind（慢，只跑正常输入）
#   L22_engineering/check_all.sh plain           # 不带检查，只看退出码和超时（对照用）
#
# 每个检查的程序是把课次目录复制到 out/l22/<模式>/ 以后重新编译出来的，不影响各讲自己的 build/。
# 做法：Makefile 里写的是 "CFLAGS += …"，所以用**环境变量** CFLAGS 加上检查选项就行，
# 不要在命令行上写 make CFLAGS=…（会盖掉 Makefile 里的 -std 和 pkg-config 的头文件路径）。
#
# 退出码：0 全部通过；1 有失败。结果表在 out/l22/<模式>/results.tsv。

set -u
cd "$(dirname "$0")/.."

MODE=${1:-asan}
[ $# -gt 0 ] && shift
LESSONS=("$@")
[ ${#LESSONS[@]} -eq 0 ] && LESSONS=(L02 L03 L04 L05 L06 L07 L08 L09 L10 L11 L12 L13 L14 L15 L16 L17 L18 L19)

case $MODE in
    asan)
        export CFLAGS='-O1 -fsanitize=address,undefined -fno-omit-frame-pointer'
        export ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:exitcode=99
        export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:exitcode=98
        WRAP=() ;;
    valgrind)
        export CFLAGS='-O0'
        WRAP=(valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,indirect) ;;
    plain)
        export CFLAGS='-O1'
        WRAP=() ;;
    *) echo "用法：$0 [asan|valgrind|plain] [LNN ...]" >&2; exit 2 ;;
esac
TIMEOUT=${TIMEOUT:-120}
[ "$MODE" = valgrind ] && TIMEOUT=${TIMEOUT_VALGRIND:-900}

W=out/l22/$MODE          # 工作目录
IN=out/l22/in            # 输入文件
mkdir -p "$W/run" "$IN"
S=samples
for f in small_360p25.mp4 audio_44k_stereo.mp2 video_360p25.m1v; do
    [ -f "$S/$f" ] || { echo "找不到 $S/$f，请先运行 scripts/gen_samples.sh" >&2; exit 1; }
done
command -v ffmpeg >/dev/null || { echo "找不到 ffmpeg，请先设置 PATH（见 L02）" >&2; exit 1; }
if [ "$MODE" = valgrind ] && ! command -v valgrind >/dev/null; then
    echo "没有安装 valgrind" >&2; exit 1
fi

# ---- 准备输入 ----------------------------------------------------------------
ff() { ffmpeg -v error -y "$@" || { echo "ffmpeg 失败：$*" >&2; exit 1; }; }
MP4=$S/small_360p25.mp4
MP2=$S/audio_44k_stereo.mp2
if [ ! -f "$IN/.ready" ]; then
    : > "$IN/empty.bin"
    head -c 100000 /dev/urandom > "$IN/garbage.bin"
    head -c 120000 $MP4 > "$IN/trunc.mp4"                                    # moov 在文件尾，截断后没有 moov
    ff -i $MP4 -c copy -movflags +faststart "$IN/fast.mp4"
    head -c 200000 "$IN/fast.mp4" > "$IN/fast_trunc.mp4"                     # moov 在，数据被截断
    ff -i $MP4 -an -c:v copy "$IN/video_only.mp4"
    ff -i $MP4 -vn -c:a copy "$IN/audio_only.m4a"
    ff -f lavfi -i color=c=red:s=64x64 -frames:v 1 "$IN/one.png"
    ff -f lavfi -i color=c=blue:s=17x15:d=1 -c:v mpeg4 -threads 1 "$IN/odd.mkv"
    ff -f lavfi -i sine=d=0.01 "$IN/short.wav"
    ff -i $MP2 "$IN/tone.wav"
    head -c 20000 "$IN/tone.wav" > "$IN/trunc.wav"
    head -c 30000 $S/video_360p25.m1v > "$IN/trunc.m1v"
    head -c 20000 $MP2 > "$IN/trunc.mp2"
    ff -i $MP4 -c:v copy -an -bsf:v h264_mp4toannexb -f h264 "$IN/clip.h264"
    head -c 30000 "$IN/clip.h264" > "$IN/trunc.h264"
    ff -i $MP4 -t 1 -an -vf scale=160:90 -pix_fmt yuv420p "$IN/clip.y4m"
    head -c 30000 "$IN/clip.y4m" > "$IN/trunc.y4m"
    touch "$IN/.ready"
fi

# 每一类程序的“正常输入”和“坏输入”
good_of() { case $1 in
    av) echo $MP4 ;; audio) echo $MP2 ;; m1v) echo $S/video_360p25.m1v ;; mp2raw) echo $MP2 ;;
    h264) echo $IN/clip.h264 ;; y4m) echo $IN/clip.y4m ;; wav) echo $IN/tone.wav ;; *) echo "" ;; esac; }
bad_of() { case $1 in
    av|audio) echo "empty.bin garbage.bin trunc.mp4 fast_trunc.mp4 video_only.mp4 audio_only.m4a one.png odd.mkv short.wav" ;;
    m1v) echo "empty.bin garbage.bin trunc.m1v" ;;
    mp2raw) echo "empty.bin garbage.bin trunc.mp2" ;;
    h264) echo "empty.bin garbage.bin trunc.h264" ;;
    y4m) echo "empty.bin garbage.bin trunc.y4m" ;;
    wav) echo "empty.bin garbage.bin trunc.wav short.wav" ;;
    *) echo "" ;; esac; }

# ---- 编译 ------------------------------------------------------------------
declare -A BIN      # 课次 -> 复制出来的 build 目录
echo "== 编译（$MODE）：${LESSONS[*]}"
for l in "${LESSONS[@]}"; do
    d=$(ls -d ${l}_* 2>/dev/null | head -1)
    [ -n "$d" ] || { echo "没有 $l 这一讲" >&2; exit 1; }
    rm -rf "${W:?}/${d:?}"
    mkdir -p "$W/$d"
    cp "$d"/*.c "$d/Makefile" "$W/$d/"
    cp "$d"/*.h "$W/$d/" 2>/dev/null
    if ! make -s -j4 -C "$W/$d" >"$W/$d.build.log" 2>&1; then
        echo "编译失败：$d（见 $W/$d.build.log）" >&2; exit 1
    fi
    BIN[$l]="$W/$d/build"
done

# ---- 运行 ------------------------------------------------------------------
total=0; pass=0; fail=0; fail_list=""; warn=0; warn_list=""; known=0; known_list=""

# 已经查清楚的问题，不算失败，单独列出来。键是 "课次 程序|标签"；值是原因：
#   纯数字：官方示例的问题，已经有 upstream_patches 里的 patch，课程拷贝保留原样并在代码里标注；
#   “外部”开头：不在示例也不在课程代码里，是库自己的问题。
declare -A KNOWN=(
    ["L06 decode_audio|坏输入 empty.bin"]="0019：输入是空文件时 decoded_frame 还没分配就送去冲刷，段错误"
    ["L15 mux|出错路径"]="0094：输出文件打不开时直接 return 1，资源全部泄漏"
    ["L16 transcode_av|坏输入 odd.mkv"]="外部：libx264 在 17x15 打不开编码器时自己泄漏 166 字节（ffmpeg 命令行同样，valgrind 确认）"
)
# 退出码不规范（不是 0 到 10）的程序，同样按 patch 归类；键是 "课次 程序"
declare -A KNOWN_RC=(
    ["L03 show_metadata"]="0009：直接返回负的 AVERROR"
    ["L17 transcode_aac"]="0095：出错一律返回 AVERROR_EXIT，退出码 187"
)
: > "$W/results.tsv"
N=0
# one 课次 程序 标签 期望 参数…    期望 0：退出码必须是 0；any：随便，但不能崩溃、超时、被检查工具报告
one() {
    local l=$1 prog=$2 label=$3 want=$4; shift 4
    local args=() a
    N=$((N + 1))
    for a in "$@"; do args+=("${a//@OUT@/$W/run/$N}"); done
    local log="$W/run/$N.log"
    timeout "$TIMEOUT" "${WRAP[@]}" "${BIN[$l]}/$prog" "${args[@]}" </dev/null >"$W/run/$N.out" 2>"$log"
    local rc=$?
    local why=""
    if [ $rc -eq 124 ]; then why="超时"
    elif grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error|==[0-9]+== (Invalid|Conditional|Use of uninit|[0-9,]+ bytes in [0-9]+ blocks are (definitely|indirectly))' "$log"; then
        why="检查工具有报告"
    elif [ $rc -eq 132 ] || [ $rc -eq 134 ] || [ $rc -eq 135 ] || [ $rc -eq 136 ] || [ $rc -eq 139 ]; then
        why="被信号终止（$((rc - 128))，崩溃）"      # SIGILL、SIGABRT、SIGBUS、SIGFPE、SIGSEGV
    elif [ "$want" = 0 ] && [ $rc -ne 0 ]; then why="退出码 $rc，应该是 0"
    fi
    total=$((total + 1))
    # 退出码大于 10：多半是 main() 直接返回了 AVERROR（负数，退出码是它的低 8 位）。不算失败，但要记下来
    local rcnote=""
    if [ -z "$why" ] && [ $rc -gt 10 ]; then
        rcnote="${KNOWN_RC[$l $prog]:-}"
        if [ -n "$rcnote" ]; then
            known=$((known + 1)); known_list="$known_list
  $l $prog [$label]：退出码 $rc  ←  $rcnote"
        else
            warn=$((warn + 1)); warn_list="$warn_list
  $l $prog [$label]：退出码 $rc"
        fi
    fi
    if [ -z "$why" ]; then
        pass=$((pass + 1))
        printf '%s\t%s\t%s\t%s\tok\n' "$l" "$prog" "$label" "$rc" >> "$W/results.tsv"
    elif [ -n "${KNOWN[$l $prog|$label]:-}" ]; then
        known=$((known + 1))
        known_list="$known_list
  $l $prog [$label]：$why  ←  ${KNOWN[$l $prog|$label]}"
        printf '%s\t%s\t%s\t%s\tKNOWN %s\n' "$l" "$prog" "$label" "$rc" "${KNOWN[$l $prog|$label]%%：*}" >> "$W/results.tsv"
    else
        fail=$((fail + 1))
        fail_list="$fail_list
  $l $prog [$label] ${args[*]}  →  $why（日志 $log）"
        printf '%s\t%s\t%s\t%s\tFAIL %s\n' "$l" "$prog" "$label" "$rc" "$why" >> "$W/results.tsv"
        printf 'FAIL  %s %s [%s]：%s\n' "$l" "$prog" "$label" "$why"
    fi
}

# spec 课次 程序 类别 参数…   类别决定 @IN@ 用哪个好文件、坏输入有哪些（类别 none：没有输入文件）
spec() {
    local l=$1 prog=$2 kind=$3; shift 3
    [[ " ${LESSONS[*]} " == *" $l "* ]] || return 0
    local good a b args=()
    good=$(good_of "$kind")
    for a in "$@"; do args+=("${a//@IN@/$good}"); done
    one "$l" "$prog" 正常 0 "${args[@]}"
    [ "$MODE" = valgrind ] && return 0
    for b in $(bad_of "$kind"); do
        args=()
        for a in "$@"; do args+=("${a//@IN@/$IN/$b}"); done
        one "$l" "$prog" "坏输入 $b" any "${args[@]}"
    done
}

echo "== 运行"
spec L02 hello_ffmpeg none
spec L03 show_metadata av @IN@
spec L03 media_info av @IN@
spec L03 probe_stages av @IN@
spec L03 open_with_options av @IN@ -
spec L04 remux av @IN@ @OUT@.mkv
spec L04 packet_info av @IN@ 5
spec L04 timebase_math none
spec L04 remux_cut av @IN@ @OUT@.mkv -ss 1 -to 2
spec L05 decode_video m1v @IN@ @OUT@
spec L05 decode_trace m1v mpeg1video @IN@ 1 5
spec L05 decode_yuv h264 h264 @IN@ @OUT@.yuv
spec L06 decode_audio mp2raw @IN@ @OUT@.pcm
spec L06 sample_formats none
spec L06 decode_wav mp2raw mp2 @IN@ @OUT@.wav
spec L07 codec_params av @IN@
spec L07 decode_av av @IN@ @OUT@.yuv @OUT@.pcm
spec L07 demux_decode av @IN@ @OUT@.yuv @OUT@.pcm
spec L08 extract_mvs av @IN@
spec L08 frame_info av @IN@
spec L08 grab_frame av @IN@ 1 @OUT@.pgm
spec L09 scale_video none @OUT@.rgb 160x120
spec L09 pix_fmt_info none yuv420p
spec L09 grab_color av @IN@ 1 @OUT@.ppm
spec L10 resample_audio none @OUT@.pcm
spec L10 resample_file audio @IN@ @OUT@.wav 48000 mono
spec L11 filter_audio none 1
spec L11 filter_info none volume
spec L11 volume_file audio @IN@ @OUT@.wav 0.5
spec L12 decode_filter_audio av @IN@
spec L12 decode_filter_video av @IN@
spec L12 filter_video av @IN@ @OUT@.y4m scale=64:48
spec L13 encode_video none @OUT@.mpg mpeg1video
spec L13 encode_yuv y4m @IN@ @OUT@.mp4 libx264 threads=1
spec L13 encoder_info none libx264
spec L14 encode_audio none @OUT@.mp2
spec L14 encode_wav wav @IN@ @OUT@.mp2 mp2
spec L15 mux none @OUT@.mp4
mux_av_cases() {     # mux_av 要两个输入（y4m 和 wav），所以不用 spec
    [[ " ${LESSONS[*]} " == *" L15 "* ]] || return 0
    local y=$IN/clip.y4m w=$IN/tone.wav b
    one L15 mux_av 正常 0 "$y" "$w" @OUT@.mp4
    one L15 mux_av "只有视频" 0 "$y" none @OUT@.mp4
    one L15 mux_av "只有音频" 0 none "$w" @OUT@.mp4
    one L15 mux_av "两路都是 none" any none none @OUT@.mp4
    [ "$MODE" = valgrind ] && return 0
    for b in empty.bin garbage.bin trunc.y4m; do one L15 mux_av "坏视频 $b" any "$IN/$b" "$w" @OUT@.mp4; done
    for b in empty.bin garbage.bin trunc.wav short.wav; do one L15 mux_av "坏音频 $b" any "$y" "$IN/$b" @OUT@.mp4; done
}
mux_av_cases
spec L15 extract_h264 av @IN@ @OUT@.h264
spec L16 transcode av @IN@ @OUT@.mp4
spec L16 transcode_av av @IN@ @OUT@.mp4
spec L17 transcode_aac audio @IN@ @OUT@.m4a
spec L17 transcode_aac_av audio @IN@ @OUT@.m4a
spec L18 avio_read_callback av @IN@
spec L18 avio_mem_in av @IN@
spec L18 avio_mem_out av @IN@ mp4 @OUT@.mp4 mode=cb
spec L19 avio_list_dir none samples
spec L19 avio_ls none samples long=1
spec L19 avio_cat av @IN@ @OUT@

# 出错路径：输入文件不存在、输出目录不存在、编码器名字不对（必须不崩溃、不泄漏）
echo "== 不存在的输入和输出"
ghost() {
    local l=$1 prog=$2; shift 2
    [[ " ${LESSONS[*]} " == *" $l "* ]] || return 0
    one "$l" "$prog" "出错路径" any "$@"
}
D=/nonexistent/dir
ghost L03 show_metadata $D/in.mp4
ghost L04 remux $D/in.mp4 @OUT@.mkv
ghost L04 remux $MP4 $D/out.mkv
ghost L05 decode_video $D/in.m1v @OUT@
ghost L05 decode_video $S/video_360p25.m1v $D/frame
ghost L06 decode_audio $MP2 $D/out.pcm
ghost L07 demux_decode $MP4 $D/v.yuv $D/a.pcm
ghost L10 resample_audio $D/out.pcm
ghost L13 encode_video $D/out.mpg mpeg1video
ghost L13 encode_video @OUT@.x nonexistent_codec
ghost L14 encode_audio $D/out.mp2
ghost L15 mux $D/out.mp4
ghost L15 mux @OUT@.xyz
ghost L16 transcode $MP4 $D/out.mp4
ghost L16 transcode_av $MP4 $D/out.mp4
ghost L17 transcode_aac $MP2 $D/out.m4a
ghost L17 transcode_aac_av $MP2 $D/out.m4a
ghost L18 avio_mem_out $MP4 mp4 $D/out.mp4 mode=file
ghost L19 avio_cat $MP4 $D/out.bin

echo
echo "共 $total 项：通过 $pass，已知问题 $known，失败 $fail；退出码大于 10 又没有记录的有 $warn 项（模式 $MODE，结果表 $W/results.tsv）"
[ $known -gt 0 ] && echo "已知问题（已经有 patch 或者确认是库的问题，不算失败）：$known_list"
[ $warn -gt 0 ] && echo "退出码大于 10 的项（不算失败，请查原因）：$warn_list"
[ $fail -gt 0 ] && echo "失败的项：$fail_list"
[ $fail -eq 0 ]
