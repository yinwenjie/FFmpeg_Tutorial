#!/usr/bin/env bash
# 课程的总回归：一条命令，把前面各讲里的检查全部跑一遍，最后给出一张表。
#
# 用法（在仓库根目录）：
#   L22_engineering/regress.sh            全部：编译、版本检查、ASan 扫描、各种和命令行的对比、没有显卡的硬件检查、单元测试
#   L22_engineering/regress.sh quick      跳过最慢的两项（ASan 扫描和 CMake 构建），75 秒左右
#   L22_engineering/regress.sh valgrind   把 ASan 扫描换成 valgrind（慢，6 分钟以上）
#
# 每一步的完整输出放在 out/l22_regress/<步骤名>.log，失败时从日志里看原因。
# 退出码：0 全部通过；1 有失败；2 缺工具或素材。
#
# 这个脚本不检查“什么”，它只是把前面各讲的检查脚本串起来，并且规定了每一项算通过的标准。
# 其中有两处是“已知的不同”，写死在这里：改了程序以后，已知的不同变了（多了或者少了）就算失败。

set -u
cd "$(dirname "$0")/.."

MODE=${1:-full}
case $MODE in full|quick|valgrind) ;; *) echo "用法：$0 [full|quick|valgrind]" >&2; exit 2 ;; esac

LOG=out/l22_regress
mkdir -p "$LOG"

# ---- 环境 ---------------------------------------------------------------------------
for c in ffmpeg ffprobe pkg-config make cc curl python3; do
    command -v $c >/dev/null 2>&1 || { echo "找不到 $c" >&2; exit 2; }
done
pkg-config --exists libavformat || { echo "pkg-config 找不到 FFmpeg，先设置 PKG_CONFIG_PATH（见 L02）" >&2; exit 2; }
for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv demo_1080p30.mp4 audio_44k_stereo.mp2 video_360p25.m1v; do
    [ -f "samples/$f" ] || { echo "找不到 samples/$f，请先运行 scripts/gen_samples.sh（见 L02）" >&2; exit 2; }
done

# ---- 步骤的登记和汇总 ----------------------------------------------------------------
declare -a NAMES=() RESULTS=() SECS=() NOTES=()
fail=0

# step 名字 命令...   命令的退出码 0 算通过
step() {
    local name=$1; shift
    local t0=$SECONDS rc
    printf '%s  ' "$name"
    "$@" >"$LOG/$name.log" 2>&1
    rc=$?
    # 备注：优先取日志里的汇总行，没有就取最后一行
    local note
    note=$(grep -E '^(共 [0-9]+ 项|相同 [0-9]+ 项|逐字节相同|全部编译|[0-9]+ 个线程|已知的|Total Test)' "$LOG/$name.log" | tail -n 1)
    [ -n "$note" ] || note=$(tail -n 1 "$LOG/$name.log")
    record "$name" $rc "$(( SECONDS - t0 ))" "$note"
}

record() {
    local name=$1 rc=$2 secs=$3 note=$4 res=通过
    [ "$rc" -ne 0 ] && { res=失败; fail=$((fail + 1)); }
    NAMES+=("$name"); RESULTS+=("$res"); SECS+=("$secs"); NOTES+=("$note")
    printf '%s（%s 秒）\n' "$res" "$secs"
}

# ---- 各步骤 ----------------------------------------------------------------------------
build_all() {
    local d rc=0
    for d in L[0-9][0-9]_*/; do
        d=${d%/}
        [ -f "$d/Makefile" ] || continue
        make -s -j4 -C "$d" >"$LOG/build_$d.log" 2>&1 || { echo "编译失败：$d（见 $LOG/build_$d.log）"; rc=1; }
    done
    [ $rc -eq 0 ] && echo "全部编译通过"
    return $rc
}

# L17 的 compare_cli.sh 已知有 5 个输出和命令行不同（原因都在 L17 README 里）；多了或者少了都算失败
l17_compare() {
    local out line got want="ac3 enc_flac out_mkv video_flv wma"
    out=$(L17_transcode_aac/compare_cli.sh 2>&1) || { printf '%s\n' "$out"; return 1; }
    printf '%s\n' "$out"
    line=$(printf '%s\n' "$out" | tail -n 1)
    got=$(printf '%s\n' "$line" | sed -n 's/.*不同 [0-9]* 个： *//p' | tr ' ' '\n' | sort | tr '\n' ' ' | sed 's/ $//')
    if [ "$got" = "$want" ]; then
        echo "已知的 5 项不同没有变：$want"
    else
        echo "已知的不同变了。应该是：$want；现在是：${got:-（没有）}"
        return 1
    fi
}


# CMake 构建加 ctest：用 CMake 把整个仓库编译一遍（和各讲的 Makefile 是两条独立的构建路径），再跑 L22 的 ctest
cmake_build() {
    command -v cmake >/dev/null 2>&1 || { echo "没有 cmake，跳过"; return 0; }
    cmake -S . -B out/l22_cmake -DCMAKE_BUILD_TYPE=Debug >/dev/null || return 1
    cmake --build out/l22_cmake -j4 >/dev/null || return 1
    (cd out/l22_cmake/L22_engineering && ctest --output-on-failure) || return 1
}

# 多线程日志：8 个线程各写 2000 行，行数必须一个不少
log_threads() {
    local out
    out=$(L22_engineering/build/log_demo threads 8 2000 2>&1) || { printf '%s\n' "$out"; return 1; }
    printf '%s\n' "$out"
    printf '%s\n' "$out" | grep -q "应该有 16000 行警告，实际 16000 行"
}

echo "== 回归（模式：$MODE）"
step 编译全部              build_all
step 版本检查              L22_engineering/build/version_info -q
step 错误码演示            L22_engineering/build/err_codes demo
step 日志多线程            log_threads
case $MODE in
    full)     step 扫描_ASan        L22_engineering/check_all.sh asan ;;
    valgrind) step 扫描_valgrind    L22_engineering/check_all.sh valgrind ;;
esac
step L16_转码对比命令行    L22_engineering/regress_transcode.sh
step L17_转码对比命令行    l17_compare
step L18_自定义IO对比      L18_avio_callbacks/compare_file.sh
step L19_目录对比          L19_avio_network/compare_ls.sh
step L19_服务和读取对比    L19_avio_network/compare_serve.sh
step L20_没有显卡的检查    L20_hw_decode/test_nodevice.sh
step L21_没有显卡的检查    L21_hw_encode/test_nodevice.sh
[ "$MODE" = full ] && step CMake_构建和ctest   cmake_build

# ---- 汇总 -------------------------------------------------------------------------------
echo
echo "== 汇总"
for i in "${!NAMES[@]}"; do
    printf '%s：%s（%s 秒）%s\n' "${NAMES[$i]}" "${RESULTS[$i]}" "${SECS[$i]}" "${NOTES[$i]:+  ${NOTES[$i]}}"
done
echo
if [ $fail -eq 0 ]; then
    echo "全部通过（${#NAMES[@]} 步）。"
    exit 0
fi
echo "有 $fail 步失败，日志在 $LOG/ 下。"
exit 1
