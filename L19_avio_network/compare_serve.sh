#!/usr/bin/env bash
# 检查 avio_cat、avio_serve 和官方示例 avio_http_serve_files：
# 读到的、发出去的内容和原文件逐字节相同吗？出错时回复的状态码对吗？一个请求结束以后，进程、socket 还干净吗？
#
# 用法（在仓库根目录）：
#   make -C L19_avio_network
#   L19_avio_network/compare_serve.sh
#
# 需要 curl、python3、ffmpeg、ffprobe。全部在 127.0.0.1 上做，端口从 PID 算出来，不占用固定端口。
# 每一项打印一行“相同”“已知”或“不同”，最后汇总。
# 已知 是 libavformat 本身的限制，或者没打补丁时的问题（0087、0088、0093），不算失败。
# 打了补丁的 libavformat 放在 LD_LIBRARY_PATH 的最前面，已知的项会变成相同。

set -u
cd "$(dirname "$0")/.."

B=L19_avio_network/build
for b in avio_cat avio_serve avio_http_serve_files; do
    if [ ! -x "$B/$b" ]; then
        echo "找不到 $B/$b，请先 make -C L19_avio_network" >&2
        exit 1
    fi
done
for f in samples/small_360p25.mp4; do
    if [ ! -f "$f" ]; then
        echo "找不到 $f，请先运行 scripts/gen_samples.sh" >&2
        exit 1
    fi
done

D=out/l19_serve
W=$D/www
rm -rf "$D"
mkdir -p "$W/sub" "$W/目录"
cp samples/small_360p25.mp4 "$W/a.mp4"
ffmpeg -v error -y -i samples/small_360p25.mp4 -c copy -movflags +faststart "$W/fast.mp4"
printf 'hello\n' > "$W/hello world.txt"
: > "$W/empty.txt"
head -c 3000000 /dev/urandom > "$W/big.bin"
printf '中文\n' > "$W/目录/中文.txt"
printf 'inner\n' > "$W/sub/in.txt"
truncate -s 400M "$W/huge.bin"                  # 稀疏文件，读起来很快，占不了多少磁盘
printf 'secret\n' > "$D/secret.txt"              # 在根目录外面
ln -s /etc/hostname "$W/escape"                  # 通过符号链接指到根目录外面

BASE_PORT=$((20000 + $$ % 20000))
next_port=$BASE_PORT
newport() { next_port=$((next_port + 1)); PORT=$next_port; }

same=0; diff=0; known=0; diff_list=""
report() {          # report 相同|不同|已知 名字 [说明]
    case $1 in
        相同) same=$((same + 1)) ;;
        已知) known=$((known + 1)) ;;
        不同) diff=$((diff + 1)); diff_list="$diff_list $2" ;;
    esac
    printf '%-4s  %s%s\n' "$1" "$2" "${3:+  $3}"
}

PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
}
trap cleanup EXIT

wait_port() {       # wait_port 端口：最多等 3 秒，直到能连上
    local i
    for i in $(seq 1 60); do
        (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null && return 0
        sleep 0.05
    done
    return 1
}

# 启动 avio_serve，PID 放进 SPID，端口放进 PORT，日志在 $D/serve_端口.log
start_serve() {
    newport
    "$B/avio_serve" "$W" "http://127.0.0.1:$PORT" "$@" > "$D/serve_$PORT.log" 2>&1 &
    SPID=$!
    PIDS="$PIDS $SPID"
    wait_port "$PORT" || { echo "avio_serve 没有起来" >&2; cat "$D/serve_$PORT.log" >&2; exit 1; }
}

code() {            # code URL [curl 参数]：只要状态码
    local u=$1; shift
    curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$@" "$u"
}
sockets() {         # sockets PID：这个进程现在有几个 socket
    ls -l /proc/"$1"/fd 2>/dev/null | grep -c socket
}
raw() {             # raw 端口 内容：发一段原始数据，打印回复的第一行（用 bash 的 /dev/tcp，不依赖 nc）
    exec 3<>/dev/tcp/127.0.0.1/"$1"
    printf '%b' "$2" >&3
    timeout 2 head -n 1 <&3
    exec 3>&-
}

# ---- 一、avio_serve ----------------------------------------------------------------------
echo "== avio_serve：内容和原文件逐字节比较"
start_serve max=16
U=http://127.0.0.1:$PORT
for pair in 'a.mp4:a.mp4' 'fast.mp4:fast.mp4' 'hello%20world.txt:hello world.txt' 'empty.txt:empty.txt' \
            'big.bin:big.bin' '%E7%9B%AE%E5%BD%95/%E4%B8%AD%E6%96%87.txt:目录/中文.txt' 'sub/in.txt?x=1:sub/in.txt'; do
    url=${pair%%:*}; file=${pair#*:}
    if curl -s --max-time 10 -o "$D/got.bin" "$U/$url" && cmp -s "$D/got.bin" "$W/$file"; then
        report 相同 "GET /$url" "（$(stat -c %s "$W/$file") 字节）"
    else
        report 不同 "GET /$url"
    fi
done

echo "== avio_serve：目录列表"
want=$(cd "$W" && ls -1A | while read -r f; do if [ -d "$f" ] && [ ! -L "$f" ]; then echo "$f/"; else echo "$f"; fi; done | LC_ALL=C sort)
got=$(curl -s --max-time 5 "$U/" | sed 's/^. *[0-9-]* //' | LC_ALL=C sort)
if [ "$got" = "$want" ]; then report 相同 "GET /" "（$(echo "$want" | wc -l) 项）"; else report 不同 "GET /"; diff <(echo "$want") <(echo "$got") | head -5; fi
got=$(curl -s --max-time 5 "$U/sub" | sed 's/^. *[0-9-]* //')
if [ "$got" = "in.txt" ]; then report 相同 "GET /sub"; else report 不同 "GET /sub"; fi
ct=$(curl -s -o /dev/null -w '%{content_type}' "$U/a.mp4")
if [ "$ct" = video/mp4 ]; then report 相同 "Content-Type：mp4 是 video/mp4"; else report 不同 "Content-Type：mp4（$ct）"; fi

echo "== avio_serve：出错的请求"
check_code() {      # check_code 期望 描述 URL [curl 参数]
    local want=$1 what=$2 got
    shift 2
    got=$(code "$@")
    if [ "$got" = "$want" ]; then report 相同 "$what" "（$got）"; else report 不同 "$what" "（期望 $want，得到 $got）"; fi
}
check_code 404 "不存在的文件"                      "$U/nope"
check_code 403 "路径里有 ..（--path-as-is）"        "$U/../secret.txt" --path-as-is
check_code 403 "路径里有 ../ 在中间"                "$U/sub/../../secret.txt" --path-as-is
check_code 403 "百分号编码的 ..（%2e%2e）"           "$U/%2e%2e/secret.txt" --path-as-is
check_code 403 "符号链接指到根目录外"                "$U/escape"
check_code 400 "解码出 NUL（%00）"                  "$U/a.mp4%00.txt"
check_code 400 "百分号编码不完整（%4）"              "$U/a%4"
reply=$(raw "$PORT" 'GARBAGE\r\n\r\n')
case $reply in *"400 Bad Request"*) report 相同 "不是 HTTP 的请求" "（$reply）" ;; *) report 不同 "不是 HTTP 的请求" "（$reply）" ;; esac
got=$(code "$U/a.mp4" -I)
if [ "$got" = 400 ]; then report 已知 "HEAD 回 400" "（libavformat 内置的 HTTP 服务器只认 GET）"; else report 不同 "HEAD" "（得到 $got）"; fi

echo "== avio_serve：并发"
rm -f "$D"/par*.bin
cpids=()
for i in 1 2 3 4 5 6 7 8; do curl -s --max-time 20 -o "$D/par$i.bin" "$U/big.bin" & cpids+=($!); done
wait "${cpids[@]}" 2>/dev/null
ok=0
for i in 1 2 3 4 5 6 7 8; do cmp -s "$D/par$i.bin" "$W/big.bin" && ok=$((ok + 1)); done
if [ $ok -eq 8 ]; then report 相同 "8 个客户端同时下载 3 MB"; else report 不同 "8 个客户端同时下载" "（只有 $ok 个对）"; fi
kill "$SPID"; wait "$SPID" 2>/dev/null

echo "== avio_serve：同时处理的客户端上限（max=1）、中途断开、退出"
start_serve max=1
U=http://127.0.0.1:$PORT
curl -s --limit-rate 3M --max-time 1.5 -o /dev/null "$U/huge.bin" &      # 占住唯一的名额
slow=$!
sleep 0.6
got=$(code "$U/a.mp4")
if [ "$got" = 429 ]; then report 相同 "名额用完以后回 429"; else report 不同 "名额用完以后回 429" "（得到 $got）"; fi
wait "$slow" 2>/dev/null
sleep 0.5
got=$(code "$U/a.mp4")
if [ "$got" = 200 ]; then report 相同 "客户端断开以后名额放回来了" "（200）"; else report 不同 "客户端断开以后名额放回来了" "（$got）"; fi
if grep -q '^/huge.bin 200 [0-9]* aborted' "$D/serve_$PORT.log"; then report 相同 "中途断开的客户端被发现，日志里有 aborted"
else report 不同 "中途断开的客户端被发现"; fi

# 没有握手完的连接：连上来什么都不发就断开、发垃圾、发 HEAD。完整的连接关掉以后 socket 要全部关闭
for i in 1 2 3; do raw "$PORT" 'GARBAGE\r\n\r\n' >/dev/null; done
(exec 3<>/dev/tcp/127.0.0.1/"$PORT"; exec 3>&-)
sleep 0.5
n=$(sockets "$SPID")
if [ "$n" -le 1 ]; then
    report 相同 "握手失败的连接关闭以后 socket 都关了" "（server 进程里 $n 个 socket，只剩监听的那个）"
else
    report 已知 "握手失败的连接的 socket 泄漏" "（server 进程里还有 $n 个 socket：libavformat 没打 patch 0088）"
fi

start=$(date +%s.%N)
kill -INT "$SPID"
wait "$SPID" 2>/dev/null; st=$?
elapsed=$(echo "$(date +%s.%N) - $start" | bc)
if [ $st -eq 0 ] && [ "$(echo "$elapsed < 1.5" | bc)" = 1 ]; then
    report 相同 "SIGINT 以后正常退出" "（退出码 0，$(printf '%.1f' "$elapsed") 秒）"
elif [ $st -eq 142 ]; then
    report 已知 "SIGINT 以后 avio_close(server) 卡住，2 秒后被闹钟结束" "（退出码 142：libavformat 没打 patch 0087）"
else
    report 不同 "SIGINT 以后退出" "（退出码 $st，$(printf '%.1f' "$elapsed") 秒）"
fi

# ---- 二、官方示例 avio_http_serve_files --------------------------------------------------
echo "== 官方示例 avio_http_serve_files"
newport
"$B/avio_http_serve_files" samples/small_360p25.mp4 "http://127.0.0.1:$PORT" > "$D/official.log" 2>&1 &
OPID=$!; PIDS="$PIDS $OPID"
wait_port "$PORT"
U=http://127.0.0.1:$PORT
ok=0
for i in 1 2 3; do curl -s --max-time 10 -o "$D/off$i.bin" "$U/samples/small_360p25.mp4" && cmp -s "$D/off$i.bin" samples/small_360p25.mp4 && ok=$((ok + 1)); done
if [ $ok -eq 3 ]; then report 相同 "3 次下载都和原文件逐字节相同"; else report 不同 "官方示例的下载" "（$ok/3）"; fi
check_code 404 "路径不对" "$U/other"
check_code 404 "带查询串的路径（resource 是原样的）" "$U/samples/small_360p25.mp4?x=1"
sleep 1
kids=$(pgrep -P "$OPID" | wc -l)
n=$(sockets "$OPID")
if [ "$kids" -eq 0 ]; then report 相同 "处理完的子进程都退出了、没有僵尸" "（patch 0087、0092）"
else report 已知 "还有 $kids 个子进程没结束" "（libavformat 没打 patch 0087：子进程里 avio_close(server) 卡住）"; fi
if [ "$n" -le 1 ]; then report 相同 "父进程没有泄漏 socket"
else report 已知 "父进程里有 $n 个 socket" "（libavformat 没打 patch 0088：父进程里 avio_close(client) 没关 socket，比监听的 1 个多出来的就是泄漏的）"; fi
kill "$OPID" 2>/dev/null; pkill -P "$OPID" 2>/dev/null; wait "$OPID" 2>/dev/null

# 文件不存在：patch 0091 以前回的是“200 OK”加空正文
newport
"$B/avio_http_serve_files" "$D/nofile.mp4" "http://127.0.0.1:$PORT" > "$D/official2.log" 2>&1 &
OPID=$!; PIDS="$PIDS $OPID"
wait_port "$PORT"
got=$(code "http://127.0.0.1:$PORT/$D/nofile.mp4")
if [ "$got" = 404 ]; then report 相同 "要发送的文件不存在：回 404" "（patch 0091）"; else report 不同 "要发送的文件不存在" "（得到 $got）"; fi
kill "$OPID" 2>/dev/null; pkill -P "$OPID" 2>/dev/null; wait "$OPID" 2>/dev/null

# ---- 三、avio_cat ------------------------------------------------------------------------
echo "== avio_cat"
C=$B/avio_cat
M=samples/small_360p25.mp4
$C "$M" "$D/cat1.bin" && cmp -s "$D/cat1.bin" "$M" && report 相同 "文件到文件" || report 不同 "文件到文件"
$C "file:$M" | cmp -s - "$M" && report 相同 "file: 前缀、写到 stdout" || report 不同 "file: 前缀、写到 stdout"
cat "$M" | $C pipe:0 | cmp -s - "$M" && report 相同 "从管道读" || report 不同 "从管道读"
$C "$M" "$D/cat2.bin" skip=1000 count=500 && cmp -s "$D/cat2.bin" <(tail -c +1001 "$M" | head -c 500) &&
    report 相同 "skip=1000 count=500" || report 不同 "skip=1000 count=500"
$C "$D/none.bin" >/dev/null 2>&1; [ $? -eq 2 ] && report 相同 "输入打不开：退出码 2" || report 不同 "输入打不开"
$C "$M" /nonexistent_dir/x.bin >/dev/null 2>&1; [ $? -eq 5 ] && report 相同 "输出打不开：退出码 5" || report 不同 "输出打不开"
$C -p | grep -q '可以读的：.*\bhttp\b' && $C -p | grep -q '可以写的：.*\bfile\b' && report 相同 "-p 列出协议" || report 不同 "-p 列出协议"

start_serve max=16
SPORT=$PORT                                                  # avio_serve 的端口
newport; python3 L19_avio_network/range_server.py "$W" "$PORT" > "$D/rs.log" 2>&1 &
RPID=$!; PIDS="$PIDS $RPID"; wait_port "$PORT"; RPORT=$PORT
newport; python3 L19_avio_network/range_server.py "$W" "$PORT" --no-range > "$D/rs2.log" 2>&1 &
NPID=$!; PIDS="$PIDS $NPID"; wait_port "$PORT"; NPORT=$PORT

$C "http://127.0.0.1:$SPORT/a.mp4" | cmp -s - "$W/a.mp4" && report 相同 "http，对着 avio_serve" || report 不同 "http，对着 avio_serve"
out=$($C "http://127.0.0.1:$RPORT/a.mp4" "$D/cat3.bin" skip=300000 count=100 info=1 2>&1)
if cmp -s "$D/cat3.bin" <(tail -c +300001 "$W/a.mp4" | head -c 100) && echo "$out" | grep -q '能 seek：是' &&
   grep -q 'Range=bytes=300000-' "$D/rs.log"; then
    report 相同 "支持 Range 的服务器：seek 到 300000 发了 Range: bytes=300000-"
else report 不同 "支持 Range 的服务器上 seek"; fi
out=$($C "http://127.0.0.1:$NPORT/a.mp4" "$D/cat4.bin" skip=300000 count=100 info=1 2>&1)
if cmp -s "$D/cat4.bin" <(tail -c +300001 "$W/a.mp4" | head -c 100) && echo "$out" | grep -q '能 seek：否'; then
    report 相同 "不支持 Range 的服务器：不能 seek，但 skip 靠读掉前面的数据做到了"
else report 不同 "不支持 Range 的服务器上 skip"; fi
out=$($C "http://127.0.0.1:$SPORT/nope" 2>&1); st=$?
if [ $st -eq 2 ] && echo "$out" | grep -q '404'; then report 相同 "404：退出码 2，消息里有 404"; else report 不同 "404" "（$st：$out）"; fi
out=$($C "http://127.0.0.1:$SPORT/a.mp4" info=1 user_agent=abc 2>&1 >/dev/null)
echo "$out" | grep -q 'MIME 类型：video/mp4' && report 相同 "info=1 显示 MIME 类型 video/mp4" || report 不同 "info=1 的 MIME 类型"

# ---- 四、HTTP 上的 MP4：moov 在文件尾和在文件头 ------------------------------------------------
echo "== ffprobe 读 HTTP 上的 MP4（回忆 L18：没有 seek，moov 在文件尾的 MP4 读不全）"
probe() { timeout 30 ffprobe -v error -show_entries stream=codec_name,pix_fmt -of csv=p=0 "$1" 2>&1 | tr '\n' ' '; }
r=$(probe "http://127.0.0.1:$SPORT/a.mp4")
case $r in *yuv420p*aac*) report 不同 "moov 在文件尾，服务器没有 Range" "（居然读全了：$r）" ;;
           *)             report 已知 "moov 在文件尾，服务器没有 Range：读不全" "（$r）" ;; esac
r=$(probe "http://127.0.0.1:$SPORT/fast.mp4")
case $r in *yuv420p*aac*) report 相同 "moov 在文件头（faststart），服务器没有 Range：读全了" ;; *) report 不同 "faststart 的 mp4" "（$r）" ;; esac
r=$(probe "http://127.0.0.1:$RPORT/a.mp4")
case $r in *yuv420p*aac*) report 相同 "moov 在文件尾，服务器有 Range：读全了" ;; *) report 不同 "有 Range 的服务器" "（$r）" ;; esac

echo
echo "相同 $same 项；已知的、符合预期的差别 $known 项；不同 $diff 项${diff_list:+：}${diff_list}"
[ "$diff" -eq 0 ]
