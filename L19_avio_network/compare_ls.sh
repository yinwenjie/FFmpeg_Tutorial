#!/usr/bin/env bash
# 把 avio_ls（libavformat 的目录接口）列出来的结果，和系统的 stat、find、ls 比较。
#
# 用法（在仓库根目录）：
#   make -C L19_avio_network
#   L19_avio_network/compare_ls.sh
#
# 在 out/l19_ls/tree 下做一棵目录树：普通文件、空文件、目录、符号链接、命名管道、socket、
# 名字里有空格和中文的文件、权限各不相同的文件、大小和修改时间各不相同的文件。
# 是 root 时还做一个属主是 4294967294(4294967293) 的文件（patch 0085 以前，用户号、组号会被截断）。
#
# 每一项打印一行“相同”“已知”或“不同”，最后汇总。已知是 libavformat 已经确认的限制或没打补丁时的问题，不算失败。

set -u
cd "$(dirname "$0")/.."

LS=L19_avio_network/build/avio_ls
if [ ! -x "$LS" ]; then
    echo "找不到 $LS，请先 make -C L19_avio_network" >&2
    exit 1
fi

D=out/l19_ls
T=$D/tree
rm -rf "$D"
mkdir -p "$T/sub/deeper" "$T/dir with space" "$T/目录"
export TZ=UTC LC_ALL=C

printf 'one' > "$T/a.txt"
: > "$T/empty"
head -c 12345 /dev/zero > "$T/mid.bin"
head -c 100000 /dev/zero > "$T/big.bin"
printf 'x' > "$T/名字 有 空格.txt"
printf 'in sub' > "$T/sub/in.txt"
printf 'deep' > "$T/sub/deeper/deep.txt"
chmod 600 "$T/a.txt"; chmod 755 "$T/mid.bin"; chmod 640 "$T/big.bin"; chmod 700 "$T/目录"
ln -s a.txt "$T/link"
ln -s sub "$T/dirlink"
ln -s /nonexistent "$T/broken"
mkfifo "$T/fifo"
python3 - "$T/sock" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])
EOF
# 修改时间各不相同，排序才有确定的结果
n=0
for f in "$T"/* "$T"/sub/*; do
    n=$((n + 1))
    touch -h -d "2026-01-01 00:00:00 UTC + $((n * 1000)) seconds" "$f"
done
if [ "$(id -u)" = 0 ]; then
    : > "$T/bigid"
    chown 4294967294:4294967293 "$T/bigid" 2>/dev/null
fi

same=0; diff=0; known=0; diff_list=""
report() {          # report 相同|不同|已知 名字 [说明]
    case $1 in
        相同) same=$((same + 1)) ;;
        已知) known=$((known + 1)) ;;
        不同) diff=$((diff + 1)); diff_list="$diff_list $2" ;;
    esac
    printf '%-4s  %s%s\n' "$1" "$2" "${3:+  $3}"
}

# 系统给出的“标准答案”：每项一行 “类型 权限 uid gid 大小 修改时间(秒) 名字”，按名字排序
# stat 的 %F 换成和 avio_ls 一样的一个字符；%a 是八进制权限，只取最后三位（avio_ls 只给低九位）
expected() {
    local dir=$1 f
    for f in "$dir"/* "$dir"/.[!.]*; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        stat -c '%F|%a|%u|%g|%s|%Y|%n' "$f"
    done | awk -F'|' '{
        t = "?"
        if ($1 == "directory") t = "d"
        else if ($1 ~ /symbolic link/) t = "l"
        else if ($1 ~ /fifo/) t = "p"
        else if ($1 ~ /socket/) t = "s"
        else if ($1 ~ /regular/) t = "-"
        perm = sprintf("%03d", $2 % 1000)          # 4755 → 755
        n = split($7, parts, "/"); name = parts[n]
        printf "%s %s %s %s %s %s %s\n", t, perm, $3, $4, $5, $6, name
    }' | sort -k7
}

# avio_ls long=1 的每一行：-rw------- 用户号 组号 大小 日期 时间 名字 → 同样的格式
mine() {
    "$LS" "$1" long=1 "${@:2}" | awk '{
        mode = substr($1, 2)
        perm = 0
        for (i = 0; i < 3; i++) {
            v = 0
            if (substr(mode, i * 3 + 1, 1) == "r") v += 4
            if (substr(mode, i * 3 + 2, 1) == "w") v += 2
            if (substr(mode, i * 3 + 3, 1) == "x") v += 1
            perm = perm * 10 + v
        }
        cmd = "date -u -d \"" $5 " " $6 "\" +%s"; cmd | getline secs; close(cmd)
        name = $7; for (i = 8; i <= NF; i++) name = name " " $i
        printf "%s %03d %s %s %s %s %s\n", substr($1, 1, 1), perm, $2, $3, $4, secs, name
    }' | sort -k7
}

echo "== 每一项的类型、权限、用户号、组号、大小、修改时间、名字（和 stat 比较）"
for d in "$T" "$T/sub"; do
    want=$(expected "$d")
    got=$(mine "$d")
    if [ "$got" = "$want" ]; then
        report 相同 "$d" "（$(echo "$want" | wc -l) 项）"
    else
        report 不同 "$d"
        diff <(echo "$want") <(echo "$got") | head -8
    fi
done

echo "== 名字，不加 long（目录后面有 /）"
want=$(cd "$T" && for f in *; do if [ -d "$f" ] && [ ! -L "$f" ]; then echo "$f/"; else echo "$f"; fi; done | sort)
got=$("$LS" "$T" | sort)
if [ "$got" = "$want" ]; then report 相同 "名字"; else report 不同 "名字"; fi

echo "== 排序"
ls_order=$(cd "$T" && ls -1 -S)
got=$("$LS" "$T" sort=size | sed 's|/$||')
if [ "$got" = "$ls_order" ]; then report 相同 "sort=size 和 ls -S"; else report 不同 "sort=size 和 ls -S"; fi
ls_order=$(cd "$T" && ls -1 -t)
got=$("$LS" "$T" sort=time | sed 's|/$||')
if [ "$got" = "$ls_order" ]; then report 相同 "sort=time 和 ls -t"; else report 不同 "sort=time 和 ls -t"; fi
ls_order=$(cd "$T" && ls -1)
got=$("$LS" "$T" | sed 's|/$||')
if [ "$got" = "$ls_order" ]; then report 相同 "sort=name 和 ls"; else report 不同 "sort=name 和 ls"; fi

echo "== 递归"
want=$(cd "$T" && find . -mindepth 1 \( -type d -o -type f -o -type l -o -type p -o -type s \) -not -path './dirlink/*' | sed 's|^\./||' | sort)
# avio_ls recurse=1：每个目录一块，“路径:” 开头；把它们拼回完整路径
got=$("$LS" "$T" recurse=1 | awk -v root="$T" '
    /:$/ { dir = substr($0, 1, length($0) - 1); next }
    NF   { name = $0; sub(/\/$/, "", name); p = dir "/" name; sub("^" root "/?", "", p); print p }' | sort)
if [ "$got" = "$want" ]; then report 相同 "recurse=1 和 find（$(echo "$want" | wc -l) 项，符号链接不跟进）"
else report 不同 "recurse=1 和 find"; diff <(echo "$want") <(echo "$got") | head -6; fi

echo "== 出错的情况"
"$LS" "$D/nonexistent" >/dev/null 2>&1
if [ $? -eq 2 ]; then report 相同 "目录不存在：退出码 2"; else report 不同 "目录不存在"; fi
"$LS" "$T/a.txt" >/dev/null 2>&1
if [ $? -eq 2 ]; then report 相同 "不是目录：退出码 2"; else report 不同 "不是目录"; fi
"$LS" http://127.0.0.1:1/ >/dev/null 2>&1
if [ $? -eq 2 ]; then report 相同 "http 不能列目录：退出码 2"; else report 不同 "http 不能列目录"; fi
if [ "$(id -u)" != 0 ]; then
    mkdir "$D/noperm"; chmod 000 "$D/noperm"
    "$LS" "$D/noperm" >/dev/null 2>&1
    if [ $? -eq 2 ]; then report 相同 "没有权限：退出码 2"; else report 不同 "没有权限"; fi
    chmod 755 "$D/noperm"
fi

echo "== 已知的限制和 libavformat 的问题"
# setuid、setgid、sticky 位：file 协议只给 st_mode 的低九位
chmod u+s "$T/mid.bin"
if "$LS" "$T" long=1 | grep -q ' mid.bin$' && "$LS" "$T" long=1 | grep 'mid.bin' | grep -q '^-rwxr-xr-x'; then
    report 已知 "setuid 位看不到" "（file 协议只给 st_mode & 0777）"
else report 不同 "setuid 位"; fi
chmod u-s "$T/mid.bin"

# "file:" 前缀：打了 patch 0086 的 libavformat 才认
if "$LS" "file:$T" >/dev/null 2>&1; then
    report 相同 "file: 前缀"
else
    report 已知 "file: 前缀" "（libavformat 没打 patch 0086：avio_open_dir() 不认 file:）"
fi

# 十位以上的用户号、组号：用的是 int64_t，avio_ls 不截断。官方示例 patch 0085 以前会截断
if [ -e "$T/bigid" ] && [ "$(stat -c %u "$T/bigid")" = 4294967294 ]; then
    if "$LS" "$T" long=1 | grep ' bigid$' | grep -q ' 4294967294 *4294967293 '; then
        report 相同 "用户号 4294967294、组号 4294967293 完整"
    else
        report 不同 "大的用户号、组号"
    fi
fi

# avio_close_dir() 有没有关掉 0 号文件描述符（patch 0093）
if command -v strace >/dev/null 2>&1; then
    if strace -e trace=close "$LS" "$T" 2>&1 >/dev/null | grep -q '^close(0)'; then
        report 已知 "avio_close_dir 关掉了 0 号描述符" "（libavformat 没打 patch 0093）"
    else
        report 相同 "avio_close_dir 没有动 0 号描述符"
    fi
fi

echo
echo "相同 $same 项；已知的、符合预期的差别 $known 项；不同 $diff 项${diff_list:+：}${diff_list}"
[ "$diff" -eq 0 ]
