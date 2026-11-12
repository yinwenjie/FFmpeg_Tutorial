#!/usr/bin/env bash
# mtool 的自动化测试。
#
# 用法（在仓库根目录，先编译 mtool）：
#   mtool/tests/run_tests.sh                 全部子命令
#   mtool/tests/run_tests.sh info cut        只测这几个
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

SECTIONS=("$@")
[ ${#SECTIONS[@]} -eq 0 ] && SECTIONS=(common info cut)
for s in "${SECTIONS[@]}"; do
    case $s in
        common) test_common ;;
        info) test_info ;;
        cut)  test_cut ;;
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
