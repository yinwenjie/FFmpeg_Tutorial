#!/usr/bin/env bash
# mtool 的自动化测试。
#
# 用法（在仓库根目录，先编译 mtool）：
#   mtool/tests/run_tests.sh                 全部子命令
#   mtool/tests/run_tests.sh info cut        只测这几个（common info cut thumb audio）
#   BUILD=mtool/build-asan mtool/tests/run_tests.sh      测 ASan 构建（验收标准：无泄漏、无越界）
#
# 测什么、怎么判断（每一项的标准写死在这里）：
#   - 测试文件由 mtool/scripts/gen_testfiles.sh 生成，清单在 mtool/tests/testfiles.tsv（好的、打不开的、能打开但损坏的）
#   - 每一次运行都检查有没有 sanitizer 报告（ASan、UBSan、LeakSanitizer）
#   - 能处理的文件退出码 0；打不开的文件退出码 1，stderr 有“mtool：错误：”，不留下输出文件；用法错误退出码 2
#   - 数字用 ffprobe 和 ffmpeg 命令行对：包数、关键帧数、帧类型、截取出来的每个包的 CRC 是不是输入里连续的一段
# 退出码：0 全部通过；1 有失败；2 缺工具或测试文件。
set -u
cd "$(dirname "$0")/../.."

BUILD=${BUILD:-mtool/build}
MT=$BUILD/mtool
[ -x "$MT" ] || MT=mtool/$BUILD/mtool
[ -x "$MT" ] || { echo "找不到 $BUILD/mtool，先编译：make -C mtool" >&2; exit 2; }
MT=$(realpath "$MT")
for c in ffmpeg ffprobe python3; do
    command -v $c >/dev/null 2>&1 || { echo "找不到 $c" >&2; exit 2; }
done
T=mtool/tests
H="python3 -I $T/helpers.py"
S=samples/mtool
[ -f "$S/h264_aac.mp4" ] || mtool/scripts/gen_testfiles.sh >/dev/null || exit 2
mapfile -t GOOD < <(awk -F'\t' '!/^#/ && $2=="good" {print $1}' $T/testfiles.tsv)
mapfile -t BAD < <(awk -F'\t' '!/^#/ && $2=="bad" {print $1}' $T/testfiles.tsv)
mapfile -t DAMAGED < <(awk -F'\t' '!/^#/ && $2=="damaged" {print $1}' $T/testfiles.tsv)

W=$(mktemp -d out/mtool_test.XXXXXX 2>/dev/null || { mkdir -p out && mktemp -d out/mtool_test.XXXXXX; })
trap 'rm -rf "$W"' EXIT
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1

pass=0; fail=0; failed=()
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); failed+=("$1"); printf '  ✗ %s\n' "$1"; [ -n "${2:-}" ] && printf '      %s\n' "$2"; }
# check 名字 期望 实际：相等算通过
eq()  { if [ "$2" = "$3" ]; then ok; else bad "$1" "期望「$2」，实际「$3」"; fi; }
# yes 名字 命令…：命令退出码 0 算通过
yes() { local n=$1; shift; if "$@" >/dev/null 2>&1; then ok; else bad "$n"; fi; }

# run 名字 参数…：运行 mtool，结果放在 RC、OUT（stdout 文件）、ERR（stderr 文件），并检查有没有 sanitizer 报告
run() {
    local n=$1; shift
    OUT=$W/stdout.txt; ERR=$W/stderr.txt
    "$MT" "$@" >"$OUT" 2>"$ERR"
    RC=$?
    if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error:|ThreadSanitizer' "$ERR"; then
        bad "$n：有 sanitizer 报告" "$(grep -m1 -E 'Sanitizer|runtime error' "$ERR")"
    else
        ok
    fi
}
# 输出文件能被 ffmpeg 完整解码，没有任何错误
decodes_clean() { [ -z "$(ffmpeg -v error -i "$1" -f null - 2>&1)" ]; }
# 逐包 CRC 清单（视频或音频流，包数据的 CRC，不含时间戳）
# framecrc 每行：流, dts, pts, 时长, 大小, CRC[, 附加数据…]；只取大小和包数据的 CRC，不含时间戳和附加数据
crcs() { ffmpeg -v error -i "$1" -map "0:$2:0" -c copy -f framecrc - 2>/dev/null | awk -F',' '!/^#/ {gsub(/ /, ""); print $6 "," $5}'; }
# 两个 WAV：后者是前者从第 start 秒开始的一段，PCM 数据逐字节相同（PCM 的包是按位置重新切的，不能按包比）
wav_is_range() {   # 输入 输出 起点秒 采样率 每个采样字节数
    local a=$W/a.raw b=$W/b.raw
    ffmpeg -v error -y -i "$1" -f s16le "$a" && ffmpeg -v error -y -i "$2" -f s16le "$b" &&
    python3 -I -c '
import sys
a = open(sys.argv[1], "rb").read(); b = open(sys.argv[2], "rb").read()
off = int(float(sys.argv[3]) * int(sys.argv[4])) * int(sys.argv[5])
sys.exit(0 if len(b) > 0 and a[off:off + len(b)] == b else 1)' "$a" "$b" "$3" "$4" "$5"
}

echo "== mtool 测试（$MT）"

# ================================================================== 通用
test_common() {
    echo "-- 通用：用法、退出码、版本"
    run common --version;           eq "--version 退出码" 0 $RC
    yes "--version 带许可证" grep -q '许可证' "$OUT"
    run common;                     eq "没有子命令：退出码 2" 2 $RC
    run common foo;                 eq "不认识的子命令：退出码 2" 2 $RC
    yes "不认识的子命令：stderr 有说明" grep -q '不认识的子命令' "$ERR"
    run common --bogus info;        eq "不认识的全局选项：退出码 2" 2 $RC
    run common --help;              eq "--help 退出码" 0 $RC
}

# ================================================================== info
test_info() {
    echo "-- info"
    local f j n v a
    for f in "${GOOD[@]}" "${DAMAGED[@]}"; do
        j=$W/$f.json
        run "info $f" info --json "$S/$f"; cp "$OUT" "$j"
        eq "info $f：退出码" 0 $RC
        yes "info $f：JSON 合法" python3 -m json.tool "$j"
        n=$(awk -F'\t' -v f="$f" '$1==f {print $3, $4}' $T/testfiles.tsv)
        eq "info $f：视频流数和音频流数" "$n" "$($H streams "$j")"
        run "info $f 文本" info "$S/$f"; eq "info $f 文本：退出码" 0 $RC
    done
    for f in "${BAD[@]}"; do
        run "info $f" info "$S/$f"
        eq "info $f（打不开）：退出码" 1 $RC
        yes "info $f（打不开）：stderr 有“mtool：错误：打不开”" grep -q 'mtool：错误：打不开' "$ERR"
        yes "info $f（打不开）：stdout 是空的" test ! -s "$OUT"
    done
    run info info "$S/nonexistent.mp4"; eq "info 文件不存在：退出码" 1 $RC
    yes "info 文件不存在：说明原因" grep -q '不存在' "$ERR"
    run info info;                      eq "info 缺文件名：退出码 2" 2 $RC
    run info info --bogus "$S/h264_aac.mp4"; eq "info 不认识的选项：退出码 2" 2 $RC

    # 数字和 ffprobe、ffmpeg 对：包数、关键帧、帧类型
    for f in h264_aac.mp4 h264_aac.mkv h264_aac.ts h264_aac.flv mpeg4_mp2.avi vfr.mp4 long_gop.mp4 truncated.ts rotate90.mp4 corrupt_mid.mp4; do
        j=$W/$f.json
        "$MT" info --json "$S/$f" >"$j" 2>/dev/null
        v=$(ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 "$S/$f" | awk '{n++; if (index($1,"K")) k++} END {print n, k}')
        eq "info $f：视频包数和关键帧数与 ffprobe 相同" "$v" "$($H vstat "$j" packets) $($H vstat "$j" keyframes)"
        v=$(ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 "$S/$f" 2>/dev/null | sort | uniq -c | awk '{c[$2]=$1} END {print c["I"]+0, c["P"]+0, c["B"]+0}')
        eq "info $f：I/P/B 帧数与 ffprobe 相同" "$v" "$($H vstat "$j" I) $($H vstat "$j" P) $($H vstat "$j" B)"
        a=$(ffprobe -v error -select_streams a:0 -show_entries packet=size -of csv=p=0 "$S/$f" 2>/dev/null | wc -l)
        [ "$a" -gt 0 ] && eq "info $f：音频包数与 ffprobe 相同" "$a" "$($H astat "$j" packets)"
    done
    # 具体的特征
    "$MT" info --json "$S/rotate90.mp4" >"$W/r.json" 2>/dev/null;  eq "info rotate90：旋转 90 度" 90 "$($H vstat "$W/r.json" rotation)"
    "$MT" info --json "$S/h264_aac.mp4" >"$W/g.json" 2>/dev/null;  eq "info 基准文件：旋转 0" 0 "$($H vstat "$W/g.json" rotation)"
    eq "info 基准文件：GOP 有 5 个" 5 "$($H vstat "$W/g.json" gop_count)"
    eq "info 基准文件：不是可变帧率" false "$($H vstat "$W/g.json" vfr)"
    "$MT" info --json "$S/vfr.mp4" >"$W/v.json" 2>/dev/null;       eq "info vfr：是可变帧率" true "$($H vstat "$W/v.json" vfr)"
    "$MT" info --json "$S/long_gop.mp4" >"$W/l.json" 2>/dev/null;  eq "info long_gop：只有 1 个关键帧" 1 "$($H vstat "$W/l.json" keyframes)"
    "$MT" info --json "$S/corrupt_mid.mp4" >"$W/c.json" 2>/dev/null
    [ "$($H vstat "$W/c.json" corrupt_frames)" -ge 1 ] && ok || bad "info corrupt_mid：应该报告至少 1 个损坏的帧"
    for f in h264_aac.mkv h264_aac.ts h264_aac.flv; do
        "$MT" info --json "$S/$f" >"$W/x.json" 2>/dev/null
        eq "info $f：宽高 640x360" "640 360" "$($H vstat "$W/x.json" width) $($H vstat "$W/x.json" height)"
    done
    run info info --no-decode --json "$S/h264_aac.mp4"
    yes "info --no-decode：没有 frame_types" test -z "$(grep frame_types "$OUT")"
    eq "info 文本输出有“streams”" 1 "$("$MT" info "$S/h264_aac.mp4" 2>/dev/null | grep -c '^  streams:')"
}

# ================================================================== cut
# 截取出来的视频包，是输入里连续的一段，并且第一个包是关键帧
cut_is_slice() {   # 输入 输出 流类型(v|a)
    crcs "$1" "$3" >"$W/in.crc"; crcs "$2" "$3" >"$W/out.crc"
    $H slice "$W/in.crc" "$W/out.crc" >/dev/null
}
first_is_key() { [ "$(ffprobe -v error -select_streams v:0 -show_entries packet=flags -of csv=p=0 "$1" | head -n 1 | cut -c1)" = "K" ]; }

test_cut() {
    echo "-- cut"
    local f ext o n ref
    for f in "${GOOD[@]}"; do
        ext=${f##*.}; o=$W/cut_$f
        run "cut $f" cut -y -s 1.5 -d 2 "$S/$f" "$o"
        eq "cut $f：退出码" 0 $RC
        yes "cut $f：输出能完整解码" decodes_clean "$o"
        n=$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$o" | sort | tr '\n' ' ')
        eq "cut $f：流的种类不变" "$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$S/$f" | sort | tr '\n' ' ')" "$n"
        if ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 "$S/$f" | grep -q video; then
            yes "cut $f：视频包是输入里连续的一段（CRC 相同）" cut_is_slice "$S/$f" "$o" v
            yes "cut $f：第一个视频包是关键帧" first_is_key "$o"
        fi
        if [ "$ext" = wav ]; then
            yes "cut $f：PCM 数据是输入从 1.5 秒起的一段（逐字节相同）" wav_is_range "$S/$f" "$o" 1.5 8000 2
        elif ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 "$S/$f" | grep -q audio; then
            yes "cut $f：音频包是输入里连续的一段（CRC 相同）" cut_is_slice "$S/$f" "$o" a
        fi
    done
    # 起点终点的含义：基准文件每秒一个关键帧
    run cut cut -y -s 2 -d 2 "$S/h264_aac.mp4" "$W/a.mp4"
    eq "cut 2~4 秒：视频 52 个包（终点按 dts，多出重排延迟的 2 个）" 52 "$(ffprobe -v error -select_streams v -count_packets -show_entries stream=nb_read_packets -of csv=p=0 "$W/a.mp4")"
    run cut cut -y -s 2.5 -e 4 "$S/h264_aac.mp4" "$W/b.mp4"
    yes "cut 2.5~4：起点退到 2 秒的关键帧，输出和 2~4 秒一样" cmp -s "$W/a.mp4" "$W/b.mp4"
    run cut cut -y "$S/h264_aac.mp4" "$W/all.mp4"
    eq "cut 不给起点终点：包数不变" 125 "$(ffprobe -v error -select_streams v -count_packets -show_entries stream=nb_read_packets -of csv=p=0 "$W/all.mp4")"
    # 换容器
    for ext in mkv ts flv mp4; do
        run "cut →$ext" cut -y -s 1 -d 2 "$S/h264_aac.mp4" "$W/to.$ext"
        eq "cut mp4→$ext：退出码" 0 $RC
        yes "cut mp4→$ext：输出能完整解码" decodes_clean "$W/to.$ext"
        # TS 里的 H.264 是 Annex B 格式（复用器加的），数据和 MP4 里的不同；和“同样是 mp4 拷贝成 ts”的文件比
        ref=$S/h264_aac.mp4; [ $ext = ts ] && ref=$S/h264_aac.ts
        yes "cut mp4→$ext：视频包是输入里连续的一段" cut_is_slice "$ref" "$W/to.$ext" v
    done
    run cut cut -y -s 1 -d 2 --faststart "$S/h264_aac.mp4" "$W/fs.mp4"
    eq "cut --faststart：moov 在 mdat 前面" moov "$(python3 -I - "$W/fs.mp4" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(); i = 0
while i < len(d):
    sz, t = struct.unpack('>I4s', d[i:i + 8]); 
    if t in (b'moov', b'mdat'): print(t.decode()); break
    i += sz
PY
)"
    run cut cut -y --video-only "$S/h264_aac.mp4" "$W/vo.mp4"
    eq "cut --video-only：只有视频" "video" "$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$W/vo.mp4" | tr '\n' ' ' | sed 's/ $//')"
    run cut cut -y --audio-only "$S/h264_aac.mp4" "$W/ao.m4a"
    eq "cut --audio-only：只有音频" "audio" "$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$W/ao.m4a" | tr '\n' ' ' | sed 's/ $//')"
    run cut cut -y -s 1 -d 2 "$S/rotate90.mp4" "$W/rot.mp4"
    eq "cut 旋转的视频：旋转角度带到输出里" 90 "$(ffprobe -v error -show_entries stream_side_data=rotation -of csv=p=0 "$W/rot.mp4")"
    run cut cut -y -s 1 -d 2 "$S/h264_aac.mp4" "$W/meta.mp4"
    eq "cut：元数据 encoder 之外的流元数据（language）带过去" und "$(ffprobe -v error -select_streams a -show_entries stream_tags=language -of csv=p=0 "$W/meta.mp4")"

    # 该拒绝的
    run cut cut -y "$S/mpeg4_mp2.avi" "$W/no.flv"
    eq "cut avi(mpeg4+mp2)→flv：退出码 1" 1 $RC
    yes "cut avi→flv：说明装不下" grep -q '装不下' "$ERR"
    yes "cut avi→flv：不留下输出文件" test ! -e "$W/no.flv"
    for f in "${BAD[@]}"; do
        run "cut $f" cut -y "$S/$f" "$W/bad_$f"
        eq "cut $f（打不开）：退出码" 1 $RC
        yes "cut $f（打不开）：stderr 有“mtool：错误：”" grep -q '^mtool：错误：' "$ERR"
        yes "cut $f（打不开）：不留下输出文件" test ! -e "$W/bad_$f"
    done
    for f in "${DAMAGED[@]}"; do
        run "cut $f" cut -y "$S/$f" "$W/dmg_$f"
        eq "cut $f（损坏但能打开）：退出码 0，不崩溃" 0 $RC
    done
    run cut cut "$S/h264_aac.mp4" "$W/a.mp4";   eq "cut 输出已存在不加 -y：退出码 1" 1 $RC
    yes "cut 输出已存在：说明加 -y" grep -q '加 -y' "$ERR"
    cp "$S/h264_aac.mp4" "$W/same.mp4"
    run cut cut -y "$W/same.mp4" "$W/same.mp4"; eq "cut 输入输出同一个文件：退出码 1" 1 $RC
    yes "cut 输入输出同一个文件：输入没有被写坏" cmp -s "$S/h264_aac.mp4" "$W/same.mp4"
    run cut cut -y -s 99 "$S/h264_aac.mp4" "$W/late.mp4"; eq "cut 起点超过文件长度：退出码 1" 1 $RC
    yes "cut 起点超过文件长度：不留下输出文件" test ! -e "$W/late.mp4"
    run cut cut -y "$S/h264_aac.mp4" "$W/x.xyz";   eq "cut 不认识的扩展名：退出码 1" 1 $RC
    run cut cut -y "$S/h264_aac.mp4" /nonexistent_dir/x.mp4; eq "cut 输出目录不存在：退出码 1" 1 $RC
    run cut cut -y --audio-only "$S/video_only.mp4" "$W/n.m4a"; eq "cut 没有音频却要音频：退出码 1" 1 $RC
    run cut cut -y -s abc "$S/h264_aac.mp4" "$W/u.mp4";          eq "cut 起点不是时间：退出码 2" 2 $RC
    run cut cut -y -s 3 -e 2 "$S/h264_aac.mp4" "$W/u.mp4";       eq "cut 终点在起点之前：退出码 2" 2 $RC
    run cut cut -y -e 2 -d 1 "$S/h264_aac.mp4" "$W/u.mp4";       eq "cut -e 和 -d 同时给：退出码 2" 2 $RC
    run cut cut -y --video-only --audio-only "$S/h264_aac.mp4" "$W/u.mp4"; eq "cut --video-only 和 --audio-only 同时给：退出码 2" 2 $RC
    run cut cut "$S/h264_aac.mp4";                                eq "cut 缺输出：退出码 2" 2 $RC
    run cut cut -y -s 1:30 "$S/h264_aac.mp4" "$W/u.mp4";         eq "cut 时间写成 1:30（90 秒，超过文件长度）：退出码 1" 1 $RC
}

# ================================================================== thumb
# 图片的像素（rgb24）的 md5。读 PNG 要加 -noautorotate：ffmpeg 会按 PNG 里的 eXIf 方向再转一次
rgb_md5() { ffmpeg -v error -noautorotate -i "$1" -f rawvideo -pix_fmt rgb24 - 2>/dev/null | md5sum | cut -d' ' -f1; }
img_wh() { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$1"; }
# mtool thumb 的 stdout 里每行 “路径  时间 秒”，取出时间
printed_times() { awk '/ 秒$/ {print $(NF-1)}' "$OUT"; }

test_thumb() {
    echo "-- thumb"
    local f nv d n i ok_all t want got
    # 所有测试文件
    for f in "${GOOD[@]}"; do
        nv=$(awk -F'\t' -v f="$f" '$1==f {print $3}' $T/testfiles.tsv); d=$W/th_$f
        run "thumb $f" thumb -y -m 3 "$S/$f" "$d"
        if [ "$nv" -gt 0 ]; then
            eq "thumb $f：退出码" 0 $RC
            n=$(find "$d" -name 'thumb_*.jpg' | wc -l)
            [ "$n" -ge 1 ] && [ "$n" -le 3 ] && ok || bad "thumb $f：图片张数应在 1 到 3 之间，实际 $n"
            ok_all=1
            for i in "$d"/thumb_*.jpg; do
                decodes_clean "$i" && [ "$(img_wh "$i")" = "320x$(img_wh "$i" | cut -dx -f2)" ] || ok_all=0
            done
            eq "thumb $f：每张图都能解码，宽都是 320" 1 $ok_all
        else
            eq "thumb $f（没有视频）：退出码 1" 1 $RC
            yes "thumb $f（没有视频）：说明没有视频流" grep -q '没有视频流' "$ERR"
            yes "thumb $f（没有视频）：不留下输出" test ! -e "$d"
        fi
    done
    for f in "${DAMAGED[@]}"; do
        run "thumb $f" thumb -y "$S/$f" "$W/thd_$f"
        eq "thumb $f（损坏但能打开）：退出码 0，不崩溃" 0 $RC
        [ "$(find "$W/thd_$f" -name 'thumb_*.jpg' | wc -l)" -ge 1 ] && ok || bad "thumb $f：至少要有 1 张图"
    done
    for f in "${BAD[@]}"; do
        run "thumb $f" thumb -y "$S/$f" "$W/thb_$f"
        eq "thumb $f（打不开）：退出码 1" 1 $RC
        yes "thumb $f（打不开）：stderr 有“mtool：错误：”" grep -q '^mtool：错误：' "$ERR"
        yes "thumb $f（打不开）：不留下输出" test ! -e "$W/thb_$f"
    done

    # 张数和时间点：基准文件 5 秒、25 帧每秒、每秒一个关键帧
    run thumb thumb -y "$S/h264_aac.mp4" "$W/t1"
    eq "thumb 默认每秒一张：5 张" 5 "$(find "$W/t1" -name '*.jpg' | wc -l)"
    eq "thumb 默认：时间是 0 到 4 秒" "0.000 1.000 2.000 3.000 4.000" "$(printed_times | tr '\n' ' ' | sed 's/ $//')"
    run thumb thumb -y -i 0.5 "$S/h264_aac.mp4" "$W/t2"
    eq "thumb -i 0.5：每次取到点之后的第一帧（25 帧每秒，0.5 秒不是帧的时间点，所以是 0.52）" "0.000 0.520 1.000 1.520 2.000 2.520 3.000 3.520 4.000 4.520" "$(printed_times | tr '\n' ' ' | sed 's/ $//')"
    run thumb thumb -y -m 3 "$S/h264_aac.mp4" "$W/t3"
    eq "thumb -m 3：只取 3 张" 3 "$(find "$W/t3" -name '*.jpg' | wc -l)"
    run thumb thumb -y -k "$S/h264_aac.mp4" "$W/t4"
    eq "thumb -k：关键帧时间和 ffprobe 相同" \
        "$(ffprobe -v error -select_streams v:0 -show_entries packet=pts_time,flags -of csv=p=0 "$S/h264_aac.mp4" | awk -F, 'index($2,"K") {printf "%.3f\n", $1}' | sort -n | tr '\n' ' ' | sed 's/ $//')" \
        "$(printed_times | tr '\n' ' ' | sed 's/ $//')"
    run thumb thumb -y -k "$S/vfr.mp4" "$W/t5"
    eq "thumb -k 可变帧率：关键帧在 0、1.2、2.84、4.0、4.8 秒" "0.000 1.200 2.840 4.000 4.800" "$(printed_times | tr '\n' ' ' | sed 's/ $//')"
    # 可变帧率按时间取：用 ffprobe 的帧时间按同样的规则算一遍
    ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 "$S/vfr.mp4" >"$W/vfr.pts"
    run thumb thumb -y -i 0.437 "$S/vfr.mp4" "$W/t6"
    want=$($H picktimes "$W/vfr.pts" 0.437 | tr '\n' ' ')
    eq "thumb -i 0.437 可变帧率：取的帧和按规则算的一样" "$want" "$(printed_times | tr '\n' ' ')"
    # 图片的内容：-k 取的关键帧和 ffmpeg 命令行 select=key 取的逐像素相同（PNG 无损）
    run thumb thumb -y -k --format png "$S/h264_aac.mp4" "$W/p1"
    ffmpeg -v error -y -i "$S/h264_aac.mp4" -vf "select=key,scale=320:-2,setsar=1" -fps_mode passthrough "$W/cli_%04d.png" 2>/dev/null
    ok_all=1
    for i in 1 2 3 4 5; do
        n=$(printf '%04d' $i)
        [ "$(rgb_md5 "$W/p1/thumb_$n.png")" = "$(rgb_md5 "$W/cli_$n.png")" ] || ok_all=0
    done
    eq "thumb -k PNG：5 张图的像素和 ffmpeg 命令行逐张相同" 1 $ok_all

    # 旋转
    run thumb thumb -y -m 1 --format png "$S/rotate90.mp4" "$W/r1"
    eq "thumb rotate90：转正后 320x568" 320x568 "$(img_wh "$W/r1/thumb_0001.png")"
    ffmpeg -v error -y -i "$S/rotate90.mp4" -frames:v 1 -vf "scale=320:-2,setsar=1" "$W/r_cli.png" 2>/dev/null
    eq "thumb rotate90：像素和 ffmpeg 命令行自动旋转的结果相同" "$(rgb_md5 "$W/r_cli.png")" "$(rgb_md5 "$W/r1/thumb_0001.png")"
    yes "thumb rotate90：PNG 里没有 eXIf 方向（不会被看图软件转第二次）" test -z "$(grep -ac eXIf "$W/r1/thumb_0001.png" | grep -v '^0$')"
    run thumb thumb -y -m 1 --no-rotate "$S/rotate90.mp4" "$W/r2"
    eq "thumb rotate90 --no-rotate：保持 320x180" 320x180 "$(img_wh "$W/r2/thumb_0001.jpg")"
    run thumb thumb -y -m 1 "$S/rotate90.mp4" "$W/r3"
    eq "thumb rotate90 默认 JPEG：320x568" 320x568 "$(img_wh "$W/r3/thumb_0001.jpg")"

    # 尺寸
    run thumb thumb -y -m 1 -w 100 "$S/h264_aac.mp4" "$W/w1";   eq "thumb -w 100：100x56" 100x56 "$(img_wh "$W/w1/thumb_0001.jpg")"
    run thumb thumb -y -m 1 -s 200x100 "$S/h264_aac.mp4" "$W/w2"; eq "thumb -s 200x100：缩进框里，178x100" 178x100 "$(img_wh "$W/w2/thumb_0001.jpg")"
    run thumb thumb -y -m 1 -w 320 "$S/tiny_16x14.mp4" "$W/w3";  eq "thumb 很小的视频放大到 -w：320x280" 320x280 "$(img_wh "$W/w3/thumb_0001.jpg")"
    run thumb thumb -y -m 1 -q 31 "$S/h264_aac.mp4" "$W/q31"; run thumb thumb -y -m 1 -q 2 "$S/h264_aac.mp4" "$W/q2"
    [ "$(stat -c %s "$W/q2/thumb_0001.jpg")" -gt "$(stat -c %s "$W/q31/thumb_0001.jpg")" ] && ok || bad "thumb -q 2 的文件应该比 -q 31 的大"
    run thumb thumb -y -m 1 "$S/h264_aac.mp4" "$W/q3"
    ffmpeg -v error -y -i "$S/h264_aac.mp4" -frames:v 1 -vf scale=320:-2 -q:v 3 "$W/cli_q3.jpg" 2>/dev/null
    yes "thumb 默认质量：第一张 JPEG 和 ffmpeg 命令行 -q:v 3 的结果逐字节相同" cmp -s "$W/q3/thumb_0001.jpg" "$W/cli_q3.jpg"
    ffmpeg -v error -y -i "$S/h264_aac.mp4" -frames:v 1 -vf scale=320:-2 -q:v 10 "$W/cli_q10.jpg" 2>/dev/null
    run thumb thumb -y -m 1 -q 10 "$S/h264_aac.mp4" "$W/q10"
    yes "thumb -q 10：和 ffmpeg 命令行 -q:v 10 逐字节相同" cmp -s "$W/q10/thumb_0001.jpg" "$W/cli_q10.jpg"

    # 拼图
    run thumb thumb -y --sheet 4x3 "$S/h264_aac.mp4" "$W/s1.jpg"
    eq "thumb --sheet 4x3：1300x556（每格 320x180，边距 4，间隔 4）" 1300x556 "$($H sheet "$W/s1.jpg" | tr ' ' x)"
    yes "thumb --sheet：JPEG 能解码" decodes_clean "$W/s1.jpg"
    run thumb thumb -y --sheet 4x3 -w 200 "$S/rotate90.mp4" "$W/s2.png"
    eq "thumb --sheet 旋转的视频 PNG：820x1084" 820x1084 "$($H sheet "$W/s2.png" | tr ' ' x)"
    run thumb thumb -y --sheet 1x1 "$S/h264_aac.mp4" "$W/s3.jpg"; eq "thumb --sheet 1x1：328x188" 328x188 "$($H sheet "$W/s3.jpg" | tr ' ' x)"
    run thumb thumb -y --sheet 3x2 -k "$S/h264_aac.mp4" "$W/s4.jpg"
    eq "thumb --sheet 3x2 -k：只有 5 个关键帧，图片大小仍然是 3x2 的" 976x372 "$($H sheet "$W/s4.jpg" | tr ' ' x)"
    run thumb thumb -y --sheet 2x2 "$S/tiny_16x14.mp4" "$W/s5.png"; eq "thumb --sheet 很小的视频：652x572" 652x572 "$($H sheet "$W/s5.png" | tr ' ' x)"
    for f in truncated.ts corrupt_mid.mp4 long_gop.mp4 vfr.mp4 h264_aac.flv; do
        run thumb thumb -y --sheet 4x3 "$S/$f" "$W/sf_$f.jpg"
        eq "thumb --sheet $f：退出码" 0 $RC
        yes "thumb --sheet $f：拼图能解码" decodes_clean "$W/sf_$f.jpg"
    done
    # 拼图里 12 格取的是不同的帧（基准文件的画面一直在动），并且没填满的格子是黑的
    n=$(for i in $(seq 0 11); do
            ffmpeg -v error -i "$W/s1.jpg" -vf "crop=320:180:$((4 + (i % 4) * 324)):$((4 + (i / 4) * 184))" -f rawvideo -pix_fmt gray - 2>/dev/null | md5sum
        done | sort -u | wc -l)
    eq "thumb --sheet 4x3：12 格是 12 个不同的画面" 12 "$n"
    eq "thumb --sheet 3x2 -k：只有 5 个关键帧，第 6 格全黑（最大亮度小于 8）" 1 \
        "$(ffmpeg -v error -i "$W/s4.jpg" -vf "crop=320:180:$((4 + 2 * 324)):$((4 + 184))" -f rawvideo -pix_fmt gray - 2>/dev/null | python3 -I -c 'import sys; d = sys.stdin.buffer.read(); print(1 if len(d) == 320 * 180 and max(d) < 8 else 0)')"

    # 该拒绝的
    run thumb thumb "$S/h264_aac.mp4" "$W/t1";                   eq "thumb 目录里已有图片不加 -y：退出码 1" 1 $RC
    yes "thumb 目录里已有图片：说明加 -y" grep -q '加 -y' "$ERR"
    run thumb thumb --sheet 2x2 "$S/h264_aac.mp4" "$W/s1.jpg";   eq "thumb 拼图文件已存在不加 -y：退出码 1" 1 $RC
    : > "$W/afile"
    run thumb thumb -y "$S/h264_aac.mp4" "$W/afile";             eq "thumb 输出是个已有的文件：退出码 1" 1 $RC
    run thumb thumb -y "$S/h264_aac.mp4" /proc/nodir/x;          eq "thumb 输出目录建不出来：退出码 1" 1 $RC
    run thumb thumb -y "$S/nonexistent.mp4" "$W/u";              eq "thumb 文件不存在：退出码 1" 1 $RC
    run thumb thumb -i 0 "$S/h264_aac.mp4" "$W/u";               eq "thumb -i 0：退出码 2" 2 $RC
    run thumb thumb -i abc "$S/h264_aac.mp4" "$W/u";             eq "thumb -i abc：退出码 2" 2 $RC
    run thumb thumb -k -i 1 "$S/h264_aac.mp4" "$W/u";            eq "thumb -k 和 -i 同时给：退出码 2" 2 $RC
    run thumb thumb -w 100 -s 100x100 "$S/h264_aac.mp4" "$W/u";  eq "thumb -w 和 -s 同时给：退出码 2" 2 $RC
    run thumb thumb -w 10 "$S/h264_aac.mp4" "$W/u";              eq "thumb -w 10（太小）：退出码 2" 2 $RC
    run thumb thumb --format gif "$S/h264_aac.mp4" "$W/u";       eq "thumb --format gif：退出码 2" 2 $RC
    run thumb thumb -q 99 "$S/h264_aac.mp4" "$W/u";              eq "thumb -q 99：退出码 2" 2 $RC
    run thumb thumb --sheet 0x3 "$S/h264_aac.mp4" "$W/u.jpg";    eq "thumb --sheet 0x3：退出码 2" 2 $RC
    run thumb thumb --sheet 2x2 "$S/h264_aac.mp4" "$W/u.bmp";    eq "thumb 拼图输出不是 jpg/png：退出码 2" 2 $RC
    run thumb thumb "$S/h264_aac.mp4";                           eq "thumb 缺输出：退出码 2" 2 $RC
    yes "thumb 用法错误：没有留下输出" test ! -e "$W/u"
}

# ================================================================== audio
# 一个文件解码成 s16le PCM 的 md5（额外的 ffmpeg 参数放在后面）
pcm_md5() { local f=$1; shift; ffmpeg -v error -i "$f" -vn "$@" -f s16le - 2>/dev/null | md5sum | cut -d' ' -f1; }
astream() { ffprobe -v error -select_streams a:0 -show_entries "stream=$2" -of csv=p=0 "$1" | head -n 1; }
# 解码出来的时长（秒，3 位小数）：PCM 字节数 / (采样率 × 声道 × 2)
pcm_seconds() { ffmpeg -v error -i "$1" -vn -f s16le - 2>/dev/null | wc -c | awk -v r="$(astream "$1" sample_rate)" -v c="$(astream "$1" channels)" '{printf "%.3f", $1 / (r * c * 2)}'; }
close_to() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN {d = a - b; if (d < 0) d = -d; exit (d <= t) ? 0 : 1}'; }

test_audio() {
    echo "-- audio"
    local f nv na d o src dur
    # 所有测试文件：转成 WAV，PCM 和 ffmpeg 命令行解码的结果逐字节相同
    for f in "${GOOD[@]}"; do
        na=$(awk -F'\t' -v f="$f" '$1==f {print $4}' $T/testfiles.tsv); o=$W/au_$f.wav
        run "audio $f" audio -y "$S/$f" "$o"
        if [ "$na" -gt 0 ]; then
            eq "audio $f → wav：退出码" 0 $RC
            eq "audio $f → wav：PCM 和 ffmpeg 命令行解码的结果逐字节相同" "$(pcm_md5 "$S/$f")" "$(pcm_md5 "$o")"
            eq "audio $f → wav：编码、采样率、声道和输入相同" "pcm_s16le,$(astream "$S/$f" sample_rate),$(astream "$S/$f" channels)" \
                "$(astream "$o" codec_name),$(astream "$o" sample_rate),$(astream "$o" channels)"
        else
            eq "audio $f（没有音频）：退出码 1" 1 $RC
            yes "audio $f（没有音频）：说明没有音频流" grep -q '没有音频流' "$ERR"
            yes "audio $f（没有音频）：不留下输出" test ! -e "$o"
        fi
    done
    # 转成有损编码和无损编码：时长和输入相差不到 0.1 秒（验收标准），FLAC 解码回来和输入的 PCM 逐字节相同
    for f in h264_aac.mp4 h264_aac.mkv h264_aac.ts h264_aac.flv mpeg4_mp2.avi audio_only.m4a mono_8k.wav; do
        src=$(pcm_seconds "$S/$f")
        for o in m4a flac mp2 aac; do
            run "audio $f → $o" audio -y "$S/$f" "$W/au_$f.$o"
            eq "audio $f → $o：退出码" 0 $RC
            yes "audio $f → $o：输出能完整解码" decodes_clean "$W/au_$f.$o"
            [ $o != aac ] && yes "audio $f → $o：时长和输入的音频只差不到 0.1 秒（输入 $src 秒）" close_to "$(pcm_seconds "$W/au_$f.$o")" "$src" 0.1
        done
        eq "audio $f → flac：无损，解码回来和输入的 PCM 逐字节相同" "$(pcm_md5 "$S/$f")" "$(pcm_md5 "$W/au_$f.flac")"
        eq "audio $f → m4a：编码是 aac，采样率和声道不变" "aac,$(astream "$S/$f" sample_rate),$(astream "$S/$f" channels)" \
            "$(astream "$W/au_$f.m4a" codec_name),$(astream "$W/au_$f.m4a" sample_rate),$(astream "$W/au_$f.m4a" channels)"
    done
    for f in truncated.ts corrupt_mid.mp4; do
        run "audio $f" audio -y "$S/$f" "$W/aud_$f.m4a"
        eq "audio $f（损坏但能打开）：退出码 0，不崩溃" 0 $RC
        yes "audio $f（损坏）：输出能完整解码" decodes_clean "$W/aud_$f.m4a"
    done
    for f in "${BAD[@]}"; do
        run "audio $f" audio -y "$S/$f" "$W/aub_$f.wav"
        eq "audio $f（打不开）：退出码 1" 1 $RC
        yes "audio $f（打不开）：stderr 有“mtool：错误：”" grep -q '^mtool：错误：' "$ERR"
        yes "audio $f（打不开）：不留下输出" test ! -e "$W/aub_$f.wav"
    done

    # 采样率和声道：和 ffmpeg 命令行 -ar -ac 的结果逐字节相同（同样的 aresample）
    for o in "16000 1" "8000 1" "48000 2" "22050 2"; do
        set -- $o
        run audio audio -y -r $1 -c $2 "$S/h264_aac.mp4" "$W/rc.wav"
        eq "audio -r $1 -c $2：采样率和声道" "$1,$2" "$(astream "$W/rc.wav" sample_rate),$(astream "$W/rc.wav" channels)"
        eq "audio -r $1 -c $2：PCM 和 ffmpeg -ar $1 -ac $2 逐字节相同" "$(pcm_md5 "$S/h264_aac.mp4" -ar $1 -ac $2)" "$(pcm_md5 "$W/rc.wav")"
    done
    run audio audio -y -c 2 -r 44100 "$S/mono_8k.wav" "$W/up.wav"
    eq "audio 单声道 8 kHz → 立体声 44.1 kHz：PCM 和 ffmpeg 逐字节相同" "$(pcm_md5 "$S/mono_8k.wav" -ar 44100 -ac 2)" "$(pcm_md5 "$W/up.wav")"
    run audio audio -y -r 11025 "$S/mono_8k.wav" "$W/au_up2.m4a"
    eq "audio wav 8 kHz → m4a 11.025 kHz" "11025,1" "$(astream "$W/au_up2.m4a" sample_rate),$(astream "$W/au_up2.m4a" channels)"
    run audio audio -y -c 6 "$S/h264_aac.mp4" "$W/c6.m4a";      eq "audio -c 6 → m4a：6 声道" 6 "$(astream "$W/c6.m4a" channels)"
    run audio audio -y -r 22050 "$S/h264_aac.mp4" "$W/r22.mp2";  eq "audio mp2 -r 22050：采样率" 22050 "$(astream "$W/r22.mp2" sample_rate)"
    run audio audio -y --codec pcm_s24le "$S/h264_aac.mp4" "$W/s24.wav"
    eq "audio --codec pcm_s24le：24 位" "pcm_s24le,24" "$(astream "$W/s24.wav" codec_name),$(astream "$W/s24.wav" bits_per_sample)"
    run audio audio -y --codec pcm_f32le -c 1 "$S/h264_aac.mp4" "$W/f32.wav"
    eq "audio --codec pcm_f32le：浮点，单声道" "pcm_f32le,1" "$(astream "$W/f32.wav" codec_name),$(astream "$W/f32.wav" channels)"
    run audio audio -y -f flac "$S/h264_aac.mp4" "$W/force.dat";  eq "audio -f flac 输出 .dat：退出码 0，容器是 flac" "0 flac" "$RC $(ffprobe -v error -show_entries format=format_name -of csv=p=0 "$W/force.dat")"

    # 码率和音质
    for o in 64 96 128; do
        run audio audio -y -b ${o}k "$S/h264_aac.mp4" "$W/br$o.m4a"
        d=$(ffprobe -v error -show_entries stream=bit_rate -of csv=p=0 "$W/br$o.m4a")
        awk -v b=$d -v t=$((o * 1000)) 'BEGIN {exit (b >= t * 0.8 && b <= t * 1.05) ? 0 : 1}' && ok || bad "audio -b ${o}k：实际码率 $d 应该在请求的 80% 到 105% 之间"
    done
    ffmpeg -v error -y -i "$S/h264_aac.mp4" -vn -f s16le "$W/src.raw"; ffmpeg -v error -y -i "$W/br96.m4a" -f s16le "$W/br96.raw"
    awk -v s="$($H snr "$W/src.raw" "$W/br96.raw")" 'BEGIN {exit (s >= 25) ? 0 : 1}' && ok || bad "audio -b 96k：解码回来和输入的信噪比应该不小于 25 dB"
    eq "audio m4a 解码回来的采样数和输入相同（AAC 的预热帧由复用器记在编辑列表里）" "$(wc -c < "$W/src.raw")" "$(wc -c < "$W/br96.raw")"

    # 截取：解码以后按采样裁，和完整解码的结果比
    for f in h264_aac.mp4 h264_aac.mkv h264_aac.flv h264_aac.ts mpeg4_mp2.avi audio_only.m4a mono_8k.wav; do
        o=$W/full_$f.wav; "$MT" audio -y "$S/$f" "$o" >/dev/null 2>&1
        ffmpeg -v error -y -i "$o" -f s16le "$W/full.raw"
        local rate ch
        rate=$(astream "$o" sample_rate); ch=$(astream "$o" channels)
        for a in "1 -d 2" "2.5 -e 2.9"; do
            set -- $a
            run "audio $f -s $a" audio -y -s $a "$S/$f" "$W/cut.wav"
            eq "audio $f -s $a：退出码" 0 $RC
            ffmpeg -v error -y -i "$W/cut.wav" -f s16le "$W/cut.raw"
            yes "audio $f -s $a：PCM 是完整解码结果里从 $1 秒起的一段（起点差不超过 1 个采样）" $H pcmslice "$W/full.raw" "$W/cut.raw" $1 $rate $ch
            want=$([ "$2" = -d ] && echo "$3" || awk -v e="$3" -v s="$1" 'BEGIN {print e - s}')
            eq "audio $f -s $a：采样数" "$(awk -v d="$want" -v r=$rate 'BEGIN {printf "%d", d * r + 0.5}')" "$(( $(wc -c < "$W/cut.raw") / (ch * 2) ))"
        done
    done
    run audio audio -y -s 4.9999 "$S/h264_aac.mp4" "$W/tail.wav"
    eq "audio -s 4.9999：只剩最后 4 个采样" 4 "$(( $(ffmpeg -v error -i "$W/tail.wav" -f s16le - | wc -c) / 4 ))"
    run audio audio -y -d 0.5 "$S/h264_aac.mp4" "$W/d05.m4a"
    yes "audio -d 0.5 → m4a：时长 0.5 秒（差不到 0.1 秒）" close_to "$(pcm_seconds "$W/d05.m4a")" 0.5 0.1

    # 该拒绝的
    run audio audio -y -r 7000 "$S/h264_aac.mp4" "$W/u.m4a";      eq "audio AAC 不支持的采样率 7000：退出码 1" 1 $RC
    yes "audio 不支持的采样率：列出编码器支持的" grep -q '44100' "$ERR"
    yes "audio 不支持的采样率：不留下输出" test ! -e "$W/u.m4a"
    run audio audio -y "$S/h264_aac.mp4" "$W/u.xyz";              eq "audio 不认识的扩展名：退出码 1" 1 $RC
    run audio audio -y --codec bogus "$S/h264_aac.mp4" "$W/u.wav"; eq "audio 没有这个编码器：退出码 1" 1 $RC
    run audio audio -y --codec aac "$S/h264_aac.mp4" "$W/u.wav";  eq "audio WAV 里放 AAC：退出码 1" 1 $RC
    yes "audio WAV 里放 AAC：不留下输出" test ! -e "$W/u.wav"
    run audio audio -y --codec libx264 "$S/h264_aac.mp4" "$W/u.mp4"; eq "audio 视频编码器当音频编码：退出码 1" 1 $RC
    run audio audio -y --track 3 "$S/h264_aac.mp4" "$W/u.wav";    eq "audio 没有第 3 路音频：退出码 1" 1 $RC
    run audio audio -y -s 99 "$S/h264_aac.mp4" "$W/u.wav";        eq "audio 起点超过文件长度：退出码 1" 1 $RC
    yes "audio 起点超过文件长度：不留下输出" test ! -e "$W/u.wav"
    run audio audio -y "$S/h264_aac.mp4" /nonexistent_dir/u.wav;  eq "audio 输出目录不存在：退出码 1" 1 $RC
    run audio audio -y "$S/nonexistent.mp4" "$W/u.wav";           eq "audio 文件不存在：退出码 1" 1 $RC
    run audio audio "$S/h264_aac.mp4" "$W/rc.wav";                eq "audio 输出已存在不加 -y：退出码 1" 1 $RC
    yes "audio 输出已存在：说明加 -y" grep -q '加 -y' "$ERR"
    cp "$S/mono_8k.wav" "$W/same.wav"
    run audio audio -y "$W/same.wav" "$W/same.wav";               eq "audio 输入输出同一个文件：退出码 1" 1 $RC
    yes "audio 输入输出同一个文件：输入没有被写坏" cmp -s "$S/mono_8k.wav" "$W/same.wav"
    run audio audio -r abc "$S/h264_aac.mp4" "$W/u.wav";          eq "audio -r abc：退出码 2" 2 $RC
    run audio audio -r 100 "$S/h264_aac.mp4" "$W/u.wav";          eq "audio -r 100（太小）：退出码 2" 2 $RC
    run audio audio -c 0 "$S/h264_aac.mp4" "$W/u.wav";            eq "audio -c 0：退出码 2" 2 $RC
    run audio audio -c 9 "$S/h264_aac.mp4" "$W/u.wav";            eq "audio -c 9：退出码 2" 2 $RC
    run audio audio -b xyz "$S/h264_aac.mp4" "$W/u.m4a";          eq "audio -b xyz：退出码 2" 2 $RC
    run audio audio -b 10 "$S/h264_aac.mp4" "$W/u.m4a";           eq "audio -b 10（太小）：退出码 2" 2 $RC
    run audio audio -s abc "$S/h264_aac.mp4" "$W/u.wav";          eq "audio -s abc：退出码 2" 2 $RC
    run audio audio -s 3 -e 2 "$S/h264_aac.mp4" "$W/u.wav";       eq "audio 终点在起点之前：退出码 2" 2 $RC
    run audio audio -e 2 -d 1 "$S/h264_aac.mp4" "$W/u.wav";       eq "audio -e 和 -d 同时给：退出码 2" 2 $RC
    run audio audio --track x "$S/h264_aac.mp4" "$W/u.wav";       eq "audio --track x：退出码 2" 2 $RC
    run audio audio -y "$S/h264_aac.mp4";                         eq "audio 缺输出：退出码 2" 2 $RC
    run audio audio -y --bogus "$S/h264_aac.mp4" "$W/u.wav";      eq "audio 不认识的选项：退出码 2" 2 $RC
}

SECTIONS=("$@")
[ ${#SECTIONS[@]} -eq 0 ] && SECTIONS=(common info cut thumb audio)
for s in "${SECTIONS[@]}"; do
    case $s in
        common) test_common ;;
        info) test_info ;;
        cut)  test_cut ;;
        thumb) test_thumb ;;
        audio) test_audio ;;
        *) echo "不认识的测试：$s" >&2; exit 2 ;;
    esac
done

echo
echo "共 $((pass + fail)) 项：通过 $pass，失败 $fail"
if [ $fail -gt 0 ]; then
    printf '失败的：\n'; printf '  %s\n' "${failed[@]}"
    exit 1
fi
exit 0
