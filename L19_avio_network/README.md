# L19 协议层：列目录与 HTTP 服务

L18 的 `AVIOContext` 夹在解复用器和数据来源之间。数据来源可以是我们自己写的回调，也可以是 libavformat 自带的**协议层**：
给一个 URL，按前缀选出 `file`、`pipe`、`tcp`、`http` ……，它们背后的读、写、seek 函数都是现成的。
这一讲看协议层本身：它能做哪几件事，怎么用 `avio_open2()` 一视同仁地读写文件、管道、HTTP，
怎么用同一套接口列目录，以及 libavformat 内置的 HTTP 服务端怎么把一个文件发给多个客户端。

官方示例两个：`avio_list_dir.c`（列目录）和 `avio_http_serve_files.c`（HTTP 服务器）。
自己写的程序三个：`avio_ls`（列目录加强版）、`avio_cat`（读一个 URL、写到另一个 URL）、`avio_serve`（HTTP 文件服务器），
外加一个对照用的 `range_server.py`（支持 Range 的服务器，libavformat 内置的做不到）。

这一讲还有一个意外的收获：把示例和 libavformat 一起跑起来以后，找到了 libavformat 本身的 4 个问题
（`avio_close_dir()` 会关掉 0 号文件描述符、关闭 HTTP 监听上下文会卡死、没握手的客户端不关 socket、目录接口不认 `file:` 前缀），
对应 patch 0086、0087、0088、0093，都在第 2、7、9 步复现。

完成标志：说得清楚 URL 怎样变成 `AVIOContext`，`avio_open2()` 对文件、管道、HTTP 有什么相同和不同；
会用 `avio_open_dir()` 一族列目录，知道哪些字段一定有、哪些可能是 -1；
说得清楚 libavformat 内置 HTTP 服务端的三步握手、`listen=1` 和 `listen=2` 的区别、能回复哪几个状态码、有哪些做不到的事（只认 GET、没有 Range）；
知道服务器有没有 Range，对 `moov` 在文件尾的 MP4 意味着什么（回到 L18）；
`compare_ls.sh`、`compare_serve.sh` 全部通过。

## 本讲要点

**协议层：URL 选协议，协议接上 `AVIOContext`**

```text
avformat_open_input("http://…")    avio_open2("file:…", …)    avio_open_dir("…", …)
          │                               │                          │
          └───────────────┬───────────────┴──────────────────────────┘
                          ▼
        URL 的前缀选协议（"://" 或 ":" 之前的部分；没有前缀就是 file）
        ┌────────┬────────┬────────┬────────────────────────────┐
        file     pipe     tcp      http ──建在──► tcp（https 建在 tls 上）
        │        │        │        │
      文件      fd 0/1   socket    请求和回复、分块传输、Range、重定向、cookie ……
                          ▼
         AVIOContext：缓冲区 + 读、写、seek（L18）
```

协议层本身**不是公开 API**：头文件里没有 `URLContext`，也没有 `ffurl_open()` 一类的函数，那些是 libavformat 的内部接口。
我们能碰到它的只有三个入口：

| 入口 | 做什么 |
| --- | --- |
| `avio_open2(&pb, url, flags, int_cb, &options)` | 打开一个 URL 读或者写，得到 `AVIOContext`；选项通过 `AVDictionary` 传给协议 |
| `avio_open_dir()`、`avio_read_dir()`、`avio_close_dir()` | 列目录，要协议实现了这三个操作（file、ftp、smb、sftp） |
| `avio_enum_protocols()`、`avio_find_protocol_name()` | 问有哪些协议、某个 URL 由哪个协议处理 |

`avformat_open_input()` 和 `avio_open()` 底下用的就是它们，所以“URL 能不能用”“选项怎么传”这些规则，对读写文件、管道、HTTP 都一样。
选项是协议自己的，在命令行里就是 `-headers`、`-user_agent`、`-timeout`、`-listen` 这些，`ffmpeg -h protocol=http` 能看到。

**目录接口：三个函数加一个结构**

```c
avio_open_dir(&ctx, url, NULL);          // 打开，得到 AVIODirContext
while (avio_read_dir(ctx, &entry) >= 0 && entry) {   // 一项一项读；返回 0 且 entry 为 NULL 是列完了
    …entry->name、entry->type、entry->size …
    avio_free_directory_entry(&entry);   // 每一项用完都要释放
}
avio_close_dir(&ctx);
```

`AVIODirEntry` 里只有 `name` 和 `type` 是**一定有**的；`size`、三个时间戳（微秒）、`user_id`、`group_id`、`filemode` 取决于协议和平台，拿不到时是 -1。
顺序是协议给的顺序（file 协议是 `readdir()` 的顺序，不排序），要排序自己来。

**HTTP 服务端：`listen`、握手、状态码**

`listen` 选项决定 HTTP 协议是客户端还是服务端：

| `listen` | 含义 |
| --- | --- |
| 0（默认） | 客户端：`avio_open2()` 去连别人 |
| 1 | 服务端，只接受一个客户端：`avio_open2()` 阻塞到有客户端连上，并完成握手；之后这个 `AVIOContext` 就是和那个客户端的连接 |
| 2 | 服务端，接受多个客户端：`avio_open2()` 马上返回一个只负责 accept 的 `AVIOContext`，用 `avio_accept()` 一个一个接，每个客户端一个新的 `AVIOContext` |

`avio_accept()` 得到的客户端要先**握手**，`avio_handshake()` 每调一次往前走一步，返回大于 0 是还没完、0 是完成、负数是出错。
HTTP 服务端的三步：1. 底层协议（tcp）；2. 读请求行和请求头，读完以后选项 `resource`（请求的路径）才有值；3. 把回复头发出去，状态码来自选项 `reply_code`，默认 200。
所以“看一眼请求的路径再决定回复什么”的做法是：握手走完第 2 步，停下来，读 `resource`，设 `reply_code`，再把握手走完（官方示例和 `avio_serve` 都这样做）。

内置服务端是个最小实现：

- 状态码只有 200、400、403、404、429、500（对应 `AVERROR_HTTP_BAD_REQUEST`、`FORBIDDEN`、`NOT_FOUND`、`TOO_MANY_REQUESTS`、`SERVER_ERROR`），别的会让握手返回 `AVERROR(EINVAL)`；
- 200 的回复用分块传输（`Transfer-Encoding: chunked`），没有 `Content-Length`；负数状态码的回复带一段短短的文字正文，有 `Content-Length`；
- 只认 GET，别的方法（HEAD、POST）得到 400；
- **没有 Range**（不会回 206），客户端不能从中间开始取。

**HTTP 上的 seek 靠 Range**

HTTP 是“请求一个资源，回复一段数据”，没有“跳到第 N 个字节”这回事。libavformat 的 HTTP 客户端靠 `Range: bytes=N-` 请求头模拟 seek：
服务器回 `206` 和 `Accept-Ranges: bytes`，`AVIOContext` 就是可以 seek 的，`avio_seek()` 往远处跳时发一个新的带 Range 的请求；
服务器不支持（回 200，没有 `Accept-Ranges`），`AVIOContext` 就是不能 seek 的，只能往后读掉不要的数据。
这正是 L18 讲的“没有 seek 回调会怎样”：`moov` 在文件尾的 MP4 放在不支持 Range 的服务器上，读不全（第 10 步）。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L19_avio_network               # 可执行文件在 L19_avio_network/build/ 下
B=L19_avio_network/build
mkdir -p out out/l19
```

后面要拿官方示例的**原版**做对照。L02 说过，这个版本的 `make examples` 不生成 `avio_http_serve_files`，所以两个都从 `make install` 装好的示例源码自己编：

```bash
O=out/l19/orig; mkdir -p $O
for n in avio_list_dir avio_http_serve_files; do
    cc -std=c11 -D_POSIX_C_SOURCE=200809L -o $O/$n ~/ffmpeg_build/share/ffmpeg/examples/$n.c \
        $(pkg-config --cflags --libs libavformat libavcodec libavutil)
done
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `avio_list_dir` | 官方示例（加了课程注释，打了 patch 0085）：列一个目录，每项一行。原版在 `$O/avio_list_dir` |
| `avio_http_serve_files` | 官方示例（加了课程注释，打了 patch 0089 到 0092）：用内置 HTTP 服务端发一个文件，每个客户端 `fork()` 一个子进程。原版在 `$O/avio_http_serve_files` |
| `avio_ls` | 自己写的列目录：排序、`ls -l` 风格、可读的时间、递归 |
| `avio_cat` | 自己写的“读一个 URL、写到另一个 URL”：文件、管道、HTTP 都行；打印大小、能不能 seek、MIME 类型；`skip=`、`count=` |
| `avio_serve` | 自己写的 HTTP 文件服务器：发一个目录，线程，目录列表，检查路径，Content-Type，429，Ctrl+C 退出 |
| `range_server.py` | 对照用的 Python 服务器，支持 Range（`--no-range` 不支持） |
| `compare_ls.sh`、`compare_serve.sh` | 和 `stat`、`ls`、`find`、`curl` 比较的脚本 |

`avio_ls` 的参数：`avio_ls <目录的URL> [long=0|1] [recurse=0|1] [sort=name|size|time]`，别的 `名字=值` 当协议选项。
`avio_cat` 的参数：`avio_cat <输入URL> [输出URL] [skip=N] [count=N] [bufsize=N] [info=0|1]`，别的 `名字=值` 交给输入的协议；输出不写是 `pipe:1`（标准输出）；`avio_cat -p` 列出协议。
`avio_serve` 的参数：`avio_serve <根目录> <http://地址[:端口]> [max=N]`。

**两份 libavformat。** 本讲发现的 libavformat 问题，在课程默认的 libavformat（提交 `a337e8d`，装在 `~/ffmpeg_build`）里都在。
README 里的命令默认用它，凡是“原版会怎样”的地方输出都是实测的。要看到打了补丁（0084、0086、0087、0088、0093）以后的样子，
用 fork 仓库的那个提交（`a337e8d` 加上课程里到本讲为止的全部 patch，本讲用它验证）编一份放在另一个目录，**只在需要的时候**用 `LD_LIBRARY_PATH` 换上去：

```bash
FFMPEG_REPO=https://github.com/yinwenjie/FFmpeg FFMPEG_REF=38a1e2f2e6ba40ac437476460f392b21d21d0a24 \
SRC_DIR=$HOME/ffmpeg_src_patched PREFIX=$HOME/ffmpeg_patched scripts/build_ffmpeg.sh   # 和 L02 是同一个脚本，要十几分钟
PATCHED=$HOME/ffmpeg_patched/lib
```

之后在命令前面加 `LD_LIBRARY_PATH=$PATCHED`，程序（用 rpath 链接的）就改用这份库。**不要**用它重新生成 `samples/`：素材必须用基线版本生成（见 `scripts/gen_samples.sh`）。
下面标了“（打补丁）”的命令就是这样跑的。

## 第 1 步：官方示例 avio_list_dir.c

```bash
mkdir -p out/l19/lab/sub && L=out/l19/lab
printf 'hello\n' > $L/a.txt; head -c 5000 /dev/zero > $L/b.bin; ln -s a.txt $L/link; mkfifo $L/fifo; printf x > $L/sub/in.txt
chmod 640 $L/b.bin; TZ=UTC touch -h -d '2026-01-02 03:04:05' $L/a.txt $L/b.bin $L/link $L/fifo $L/sub $L/sub/in.txt
$B/avio_list_dir $L
```

```text
TYPE              SIZE                           NAME   UID(GID) UGO         MODIFIED         ACCESSED   STATUS_CHANGED
<FILE>            5000                          b.bin       0(0) 640 1767323045000000 1767323045000000 1794507304000000
<PIPE>               0                           fifo       0(0) 644 1767323045000000 1767323045000000 1794507304000000
<LINK>               5                           link       0(0) 777 1767323045000000 1794507304000000 1794507304000000
<DIR>             4096                            sub       0(0) 755 1767323045000000 1794507311000000 1794507304000000
<FILE>               6                          a.txt       0(0) 644 1767323045000000 1767323045000000 1794507304000000
```

（用户号取决于是谁在跑，这里是 root；`ACCESSED` 和 `STATUS_CHANGED` 是你创建、访问这些文件的时刻，每个人、每次都不一样。）几点：

- 顺序是 `readdir()` 给的，**不是**按名字：`a.txt` 排在最后。想要有序，自己排序（第 3 步）。
- 三个时间是**微秒**数，不是可读的日期：`1767323045000000` 是 2026-01-02 03:04:05 UTC。
- `link` 的类型是 `<LINK>`，大小 5 是它指向的名字 `a.txt` 的长度。`avio_read_dir()` 用的是 `lstat()`，不跟进符号链接。
- 权限 `640` 是八进制，只有 st_mode 的低九位：setuid、setgid、sticky 位看不到（`compare_ls.sh` 里验证了）。
- 没有“`.`”和“`..`”：file 协议把它们跳过了。
- 日志级别被设成了 DEBUG，列表走的是 `av_log(…, AV_LOG_INFO, …)`，所以和 libavformat 的日志一起走 stderr，不能直接接管道。

不存在的目录得到 `Cannot open directory: No such file or directory.`，退出码 1。`http://` 开头的 URL 得到 `Cannot open directory: Function not implemented.`：不是所有协议都能列目录。

## 第 2 步：官方示例 avio_list_dir 的三个问题

**问题一（patch 0085）：大的用户号被截断。** 原版放“用户号(组号)”的数组是 `uid_and_gid[20]`，`snprintf()` 也只写 20 字节，
而两个 `int64_t` 加括号最多要 43 个字符。用户命名空间、网络文件系统里十位的 id 很常见：

```bash
touch $L/bigid && sudo chown 4294967294:4294967293 $L/bigid
$O/avio_list_dir $L 2>&1 | grep bigid                      # 原版（提交 a337e8d 的示例）
$B/avio_list_dir $L 2>&1 | grep bigid                      # 课程里的（打了 0085）
```

```text
<FILE>               0                          bigid 4294967294(42949672 644 …      ← 原版，被截断了
<FILE>               0                          bigid 4294967294(4294967293) 644 …   ← 0085
```

这个版本把数组加大到 44 字节，`snprintf()` 用 `sizeof()`。

**问题二（patch 0086，改的是 libavformat）：不认 `file:` 前缀。** `avio_open2("file:/tmp/x")` 和 `ffmpeg -i file:/tmp/x` 都能用，目录接口不行：

```bash
$B/avio_list_dir file:$L
```

```text
Cannot open directory: No such file or directory.
```

`file_open()` 等函数都先去掉前缀，`file_open_dir()` 没有去，`file_read_dir()` 拿带前缀的名字去 `lstat()`。打了补丁（0086）的 libavformat 上同样的命令能列出来（打补丁）：

```bash
LD_LIBRARY_PATH=$PATCHED $B/avio_list_dir file:$L 2>&1 | head -3
```

**问题三（patch 0093，改的是 libavformat）：`avio_close_dir()` 会关掉标准输入。** 看 `strace`：

```bash
strace -e trace=close $B/avio_list_dir $L 2>&1 >/dev/null | tail -3
```

```text
close(3)                                = 0
close(0)                                = 0
+++ exited with 0 +++
```

`close(3)` 是目录；`close(0)` 是不该有的。`avio_open_dir()` 把上下文标成“已连接”，所以 `avio_close_dir()` 最后会调协议的关闭函数，
file 协议的 `file_close()` 去 `close(c->fd)`，而打开目录时从来没有设过 `c->fd`，它是 0。
单线程的小程序丢了标准输入，没人在意；多线程的服务器里它会关掉别人的连接，第 9 步会看到。
打了补丁（0093）的 libavformat 上同样的 `strace` 没有 `close(0)`。

## 第 3 步：avio_ls，加强版的列目录

```bash
$B/avio_ls $L                         # 只有名字，按名字排序，目录后面加 /
$B/avio_ls $L long=1                  # 像 ls -l
```

```text
a.txt
b.bin
fifo
link
sub/
```

```text
-rw-r--r--          0          0            6 2026-01-02 03:04:05 a.txt
-rw-r-----          0          0         5000 2026-01-02 03:04:05 b.bin
prw-r--r--          0          0            0 2026-01-02 03:04:05 fifo
lrwxrwxrwx          0          0            5 2026-01-02 03:04:05 link
drwxr-xr-x          0          0         4096 2026-01-02 03:04:05 sub
```

列：类型加权限、用户号、组号、大小、修改时间（本地时区，`TZ=Asia/Shanghai` 会显示成 11:04:05）、名字。
和 `ls -l -n --time-style=+'%Y-%m-%d %H:%M:%S'` 比，除了少一列“链接数”（目录接口不给）和看不到 setuid 位，一模一样。

```bash
$B/avio_ls $L long=1 sort=size        # 大的在前；sort=time 是新的在前
$B/avio_ls $L recurse=1               # 递归，像 ls -R；符号链接不跟进
```

```text
out/l19/lab:
a.txt
b.bin
fifo
link
sub/

out/l19/lab/sub:
in.txt
```

出错的情况：目录不存在、不是目录、协议不能列目录，都是退出码 2；用法错误是 1；递归列到一半有子目录出错是 3。

```bash
$B/avio_ls out/l19/none;        echo "退出码 $?"   # No such file or directory，2
$B/avio_ls $L/a.txt;            echo "退出码 $?"   # Not a directory，2
$B/avio_ls http://127.0.0.1:1/; echo "退出码 $?"   # Function not implemented，2
```

`avio_ls.c` 比官方示例多出来的部分，值得看三处：先把一个目录的所有项读完、复制一份（`avio_free_directory_entry()` 之后原来的就没了），再 `qsort()`，再打印；
`avio_open_dir()` 会**用掉**它认识的选项，所以每次给它一份 `AVDictionary` 的副本；递归用 `av_append_path_component()` 拼子目录的 URL。

`compare_ls.sh` 在 `out/l19_ls/tree` 下做了一棵目录树（普通文件、空文件、目录、符号链接、命名管道、socket、名字里有空格和中文、各种权限；是 root 时还有属主为 4294967294(4294967293) 的文件），
把 `avio_ls` 给的每一项和 `stat` 比较，把排序和 `ls -S`、`ls -t`、`ls` 比较，把递归和 `find` 比较：

```bash
L19_avio_network/compare_ls.sh
```

```text
相同 11 项；已知的、符合预期的差别 3 项；不同 0 项
```

3 项已知的：setuid 位看不到；`file:` 前缀（0086）；`avio_close_dir()` 关了 0 号描述符（0093）。打了补丁的 libavformat 上是“相同 13 项，已知 1 项”。

## 第 4 步：avio_cat，协议层对文件、管道、HTTP 一视同仁

```bash
$B/avio_cat -p
```

```text
可以读的：async cache concat concatf crypto data fd ffrtmphttp file ftp gopher http httpproxy mmsh mmst pipe rtmp rtmpt rtp shared srtp subfile tcp udp udplite unix
可以写的：crypto fd ffrtmphttp file ftp gopher http httpproxy icecast md5 pipe prompeg rtmp rtmpt rtp srtp tee tcp udp udplite unix
```

这是 `avio_enum_protocols()` 给的名单。课程的构建没有 TLS 库，所以没有 `https`、`tls`。每个协议都能用来读、写或者读写，要看它自己。

同一个程序读本地文件、读管道：

```bash
$B/avio_cat samples/small_360p25.mp4 info=1 > out/l19/c1.mp4
cat samples/small_360p25.mp4 | $B/avio_cat pipe:0 info=1 > out/l19/c2.mp4
cmp out/l19/c1.mp4 samples/small_360p25.mp4 && cmp out/l19/c2.mp4 samples/small_360p25.mp4 && echo 两个都相同
```

```text
协议：file
大小：563196 字节
能 seek：是
复制了 563196 字节，用时 0.000 秒
协议：pipe
大小：未知（Function not implemented）
能 seek：否
复制了 563196 字节，用时 0.000 秒
两个都相同
```

文件知道自己多大、能 seek；管道不知道多大（`avio_size()` 返回 `AVERROR(ENOSYS)`）、不能 seek。这和 L18 的“有没有 seek 回调”是同一件事，只是这一次回调是协议层提供的。
`pipe:0` 是标准输入，`pipe:1` 是标准输出（`avio_cat` 的默认输出）。

`skip=` 和 `count=` 取文件的一段：

```bash
$B/avio_cat samples/small_360p25.mp4 out/l19/part.bin skip=1000 count=16
od -An -tx1 out/l19/part.bin; tail -c +1001 samples/small_360p25.mp4 | head -c 16 | od -An -tx1
```

两行一样。`skip=` 靠 `avio_seek(SEEK_SET)`：文件直接定位；管道不能 seek，但**往前**跳可以——`AVIOContext` 把要跳过的数据读掉扔掉（`skip=300000` 对管道成功，内容也对）；
跳到比数据还远的位置，管道得到 `seek 到 999999999 失败：End of file`（退出码 3），文件反而成功（像 `lseek()`），读到 0 字节。往回跳，管道才真的不行。

出错：

```bash
$B/avio_cat out/l19/none.mp4; echo "退出码 $?"                       # 打不开输入 … No such file or directory，2
$B/avio_cat samples/small_360p25.mp4 /nonexistent/x; echo "退出码 $?" # 打不开输出，5
```

## 第 5 步：HTTP 读和 seek：服务器要有 Range

先起两个 Python 服务器，一个支持 Range，一个不支持，都发 `samples/`：

```bash
python3 L19_avio_network/range_server.py samples 8101 2> out/l19/rs.log &            # 支持 Range
python3 L19_avio_network/range_server.py samples 8102 --no-range 2> out/l19/rs2.log &  # 不支持
sleep 1
$B/avio_cat http://127.0.0.1:8101/small_360p25.mp4 info=1 > /dev/null
```

```text
协议：http
大小：563196 字节
能 seek：是
MIME 类型：video/mp4
复制了 563196 字节，用时 0.000 秒
```

`avio_cat` 对 HTTP 的 URL 和对文件一样用，多了一个 `MIME 类型`：`http` 协议有只读的选项 `mime_type`，`avio_cat` 用 `av_opt_get()` 取。大小来自回复里的 `Content-Length` 或 `Content-Range`。
`能 seek：是` 是因为服务器回复了 `206` 和 `Accept-Ranges: bytes`。

**seek 在 HTTP 上是怎么做的。** `skip=` 越过缓冲区，`avio_seek()` 才会发新的请求：

```bash
$B/avio_cat http://127.0.0.1:8101/small_360p25.mp4 out/l19/h1.bin skip=1000 count=100       # 不远，在缓冲区里读掉
$B/avio_cat http://127.0.0.1:8101/small_360p25.mp4 out/l19/h2.bin skip=300000 count=100     # 远，新请求
cat out/l19/rs.log
```

```text
GET /small_360p25.mp4 Range=bytes=0- -> 206 563196
GET /small_360p25.mp4 Range=bytes=0- -> 206 458752
GET /small_360p25.mp4 Range=bytes=0- -> 206 563196
GET /small_360p25.mp4 Range=bytes=300000- -> 206 131072
```

四行里前面三个都是 `bytes=0-`：第一个是上面 `info=1` 那一次；第二个是 `skip=1000`，跳 1000 字节是在读回来的数据里往前走的，没有新请求；
第三个是 `skip=300000` 打开时的请求（libavformat 总是先从头问起），第四个才是 seek 发的 `Range: bytes=300000-`，服务器只给了后面的 131072 字节。
（最后一列是服务器写出去的字节数，包括客户端关掉连接时还留在缓冲区里的，所以比 `count` 大；具体数字每次略有不同。）

不支持 Range 的服务器：

```bash
$B/avio_cat http://127.0.0.1:8102/small_360p25.mp4 out/l19/h3.bin skip=300000 count=100 info=1
cmp out/l19/h3.bin out/l19/h2.bin && echo 内容相同; cat out/l19/rs2.log
```

```text
协议：http
大小：563196 字节
能 seek：否
MIME 类型：video/mp4
复制了 100 字节，用时 0.000 秒
内容相同
GET /small_360p25.mp4 Range=- -> 200 563196
```

`能 seek：否`；`skip=300000` 照样成功，内容也对，但是靠“把前面的 300000 字节读掉”做到的：服务器只看到一个请求，没有 Range，回了整个文件。
往前跳能做到，往回跳就不行了：这就是不能 seek 的 `AVIOContext` 的全部。

**请求头和选项。** HTTP 协议的选项通过名字=值传给输入。用 `nc` 当一个“只听不答”的服务器，看 libavformat 发了什么：

```bash
(nc -l 8103 > out/l19/req.txt &); sleep 0.5
timeout 2 $B/avio_cat http://127.0.0.1:8103/x.bin user_agent=abc 'headers=X-Test: 1'$'\r\n' count=1 >/dev/null 2>&1
cat -A out/l19/req.txt
```

```text
GET /x.bin HTTP/1.1^M$
User-Agent: abc^M$
Accept: */*^M$
Range: bytes=0-^M$
Connection: close^M$
Host: 127.0.0.1:8103^M$
Icy-MetaData: 1^M$
X-Test: 1^M$
^M$
```

`user_agent` 换掉了默认的；`headers` 的值要以 `\r\n` 结尾，`avio_cat` 把整个值交给协议，没有结尾的 `\r\n` libavformat 会警告并补上。
默认的请求头里有 `Range: bytes=0-`：libavformat 一开始就假设服务器可能支持 Range。

出错的 HTTP 状态码被翻译成 libavformat 的错误码：

```bash
$B/avio_cat http://127.0.0.1:8101/nope; echo "退出码 $?"
```

```text
[http @ 0x…] HTTP error 404 File not found
打不开输入 http://127.0.0.1:8101/nope：Server returned 404 Not Found
退出码 2
```

`AVERROR_HTTP_NOT_FOUND`、`AVERROR_HTTP_FORBIDDEN` 等错误码是 libavformat 定义的，`av_err2str()` 能打印成“Server returned 404 Not Found”。

```bash
pkill -f range_server.py   # 停掉两个 Python 服务器
```

## 第 6 步：官方示例 avio_http_serve_files.c

```bash
$B/avio_http_serve_files samples/small_360p25.mp4 http://127.0.0.1:8080 2> out/l19/off.log &
curl -s -D - -o /dev/null http://127.0.0.1:8080/samples/small_360p25.mp4
```

```text
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Transfer-Encoding: chunked

```

请求的路径必须是 `/` 加上命令行里的文件名（`/samples/small_360p25.mp4`），别的路径得到 404；回复头里是分块传输、固定的 `application/octet-stream`，没有 `Content-Length`。

```bash
curl -s -D - http://127.0.0.1:8080/other                          # 404，带 Content-Length 和一行文字
curl -s -I http://127.0.0.1:8080/samples/small_360p25.mp4         # HEAD：400
curl -s -o /dev/null -w '%{http_code}\n' 'http://127.0.0.1:8080/samples/small_360p25.mp4?x=1'   # 404：带查询串是另一个资源
```

```text
HTTP/1.1 404 Not Found
Content-Type: text/plain
Content-Length: 15

404 Not Found
```

HEAD 得到 `400 Bad Request`：内置服务端只认 GET。查询串、`%20` 都不处理，`resource` 是原样的路径。

示例用 `AV_LOG_TRACE` 打印一切，把每一步握手都看得见。过滤一下：

```bash
grep -aE 'Entering|Accepted|In child|Lower protocol|Read headers|Reply code|resource=|Set reply|Handshake performed|Opening input|Flushing|Closing' out/l19/off.log
```

```text
Entering main loop.
Accepted client, forking process.
In child.
[http @ 0x…] Lower protocol
[http @ 0x…] Read headers
[AVIOContext @ 0x…] resource=/samples/small_360p25.mp4
Opening input file.
[AVIOContext @ 0x…] Set reply code to 200
[http @ 0x…] Reply code: 200
Handshake performed.
Flushing client
Closing client
Closing input
```

这就是“三步握手”：`Lower protocol`（tcp）、`Read headers`（读请求，之后 `resource` 才有值，示例在这里打印并检查它）、`Reply code`（发回复头）。
示例在第二步和第三步之间停下来，决定状态码（注意 patch 0091 之后，这里先 `Opening input file`，再 `Set reply code`）。
`Accepted client, forking process.` 是父进程，`In child.` 起是子进程：每个客户端一个进程。

```bash
pkill -x avio_http_serve
```

## 第 7 步：原版 libavformat 上，这个服务器不能长期运行

第 6 步的示例看起来一切正常。请求几次，再看看进程和文件描述符：

```bash
$B/avio_http_serve_files samples/small_360p25.mp4 http://127.0.0.1:8080 2>/dev/null &
for i in 1 2 3; do curl -s -o /dev/null http://127.0.0.1:8080/samples/small_360p25.mp4; done
P=$(pgrep -o -x avio_http_serve)
ps -o pid,stat,wchan:14,cmd --ppid $P
echo "父进程里的 socket：$(ls -l /proc/$P/fd | grep -c socket)"
```

```text
  PID STAT WCHAN          CMD
17450 S    poll_schedule_ L19_avio_network/build/avio_http_serve_files samples/small_360p25.mp4 http://127.0.0.1:8080
17452 S    poll_schedule_ L19_avio_network/build/avio_http_serve_files samples/small_360p25.mp4 http://127.0.0.1:8080
17454 S    poll_schedule_ L19_avio_network/build/avio_http_serve_files samples/small_360p25.mp4 http://127.0.0.1:8080
父进程里的 socket：4
```

**三个已经处理完的子进程还在**，停在 `poll()` 上；**父进程里有 4 个 socket**，一个是监听的，三个是泄漏的。curl 看不出任何问题。两个原因，都在 libavformat 里：

**子进程卡在 `avio_close(server)`（patch 0087）。** 子进程 `fork()` 得到一份监听上下文的副本，处理完以后要把它关掉再 `exit(0)`。
`http_close()` 对每个 HTTP 上下文都写分块传输的结束标记 `0\r\n\r\n`；监听上下文的底层是监听用的 socket，永远写不出去，一直等。
修复是监听上下文不写结束标记。

**父进程里的 socket 没关（patch 0088）。** 父进程 `fork()` 以后 `avio_close(client)`，但这个客户端**没有握手过**；`ffurl_closep()` 只对标成“已连接”的上下文调协议的关闭函数，
而“已连接”是握手完成以后才标的。结果 `avio_close(client)` 只是释放了内存，socket 留在了父进程里。
后果不止是泄漏描述符：子进程发完、关了自己的那份以后，**连接并没有关**，父进程手里还有一份。HTTP 客户端靠分块传输的结束标记知道回复完了，curl 没事；读到“连接关闭”才算结束的客户端就一直等：

```bash
printf 'GET /samples/small_360p25.mp4 HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 4 nc 127.0.0.1 8080 | wc -c
```

```text
567138
```

内容全收到了（567138 字节），但 `nc` 一直等到 `timeout` 的 4 秒才退出。换成打了补丁（0087、0088）的 libavformat（打补丁）：同一个命令 0.0 秒退出，父进程里只有监听的 1 个 socket，三个子进程都已经退出。

```bash
pkill -x avio_http_serve; pkill -9 -x avio_http_serve
LD_LIBRARY_PATH=$PATCHED $B/avio_http_serve_files samples/small_360p25.mp4 http://127.0.0.1:8080 2>/dev/null &
for i in 1 2 3; do curl -s -o /dev/null http://127.0.0.1:8080/samples/small_360p25.mp4; done
P=$(pgrep -o -x avio_http_serve); ps --ppid $P --no-headers | wc -l; ls -l /proc/$P/fd | grep -c socket
```

```text
0
1
```

课程里的这份示例已经带着 patch 0092：`signal(SIGCHLD, SIG_IGN)`，系统自己回收子进程。原版的示例在打了 0087 的库上，子进程能退出了，却会留下**僵尸进程**：

```bash
LD_LIBRARY_PATH=$PATCHED $O/avio_http_serve_files samples/small_360p25.mp4 http://127.0.0.1:8081 2>/dev/null &     # 原版示例，打补丁的库
for i in 1 2 3; do curl -s -o /dev/null http://127.0.0.1:8081/samples/small_360p25.mp4; done
ps -o pid,stat,cmd --ppid $(pgrep -o -x avio_http_serve)
```

```text
  PID STAT CMD
 1412 Z    [avio_http_serve] <defunct>
 1414 Z    [avio_http_serve] <defunct>
 1416 Z    [avio_http_serve] <defunct>
```

三个子进程都退出了（`Z`、`<defunct>`），可是父进程从不 `wait()`，没有人来收尸。原来的代码里就写着一句 `XXX: … this produces zombie processes.`。

**patch 0091：文件不存在。** 要发送的文件不存在时（命令行给了一个错的名字），原版先回复 `200 OK`，才去打开文件：

```bash
pkill -x avio_http_serve
$O/avio_http_serve_files out/l19/nofile.mp4 http://127.0.0.1:8080 2>/dev/null &     # 原版
sleep 1; curl -s -i http://127.0.0.1:8080/out/l19/nofile.mp4 | head -4
```

```text
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Transfer-Encoding: chunked

```

正文是 0 字节，客户端以为拿到了一个空文件，服务器的日志里却是 `Failed to open input: … No such file or directory.`。课程里的版本先打开文件，再决定状态码：打不开、`ENOENT` 回 404，别的原因回 500。

## 第 8 步：avio_serve，自己的 HTTP 文件服务器

官方示例每个客户端一个进程，只发一个文件。`avio_serve` 改成每个客户端一个线程，发一个目录：

```bash
W=out/l19/www; mkdir -p $W/sub
cp samples/small_360p25.mp4 $W/a.mp4; printf 'hello\n' > "$W/hello world.txt"; printf 'inner\n' > $W/sub/in.txt
head -c 3000000 /dev/urandom > $W/big.bin; truncate -s 400M $W/huge.bin      # 稀疏文件，后面要用
printf 'secret\n' > out/l19/secret.txt; ln -sf /etc/hostname $W/escape          # 根目录外面的文件、指到外面的符号链接
$B/avio_serve $W http://127.0.0.1:8090 max=2 > out/l19/serve.log 2> out/l19/serve.err &
sleep 1; cat out/l19/serve.err
```

```text
在 http://127.0.0.1:8090 上发送 /…/out/l19/www（Ctrl+C 退出）
```

stderr 里是启动信息；每处理完一个请求，stdout 里（这里是 `serve.log`）多一行：路径、状态码、发送的字节数。

```bash
U=http://127.0.0.1:8090
curl -s $U/                 # 目录列表
curl -s $U/sub
curl -s "$U/hello%20world.txt?x=1"
```

```text
-       563196 a.mp4
-      3000000 big.bin
l           13 escape
-            6 hello world.txt
-    419430400 huge.bin
d         4096 sub/
-            6 in.txt
hello
```

目录列表是纯文本：类型（`d` 目录、`l` 符号链接、`-` 别的）、大小、名字，排好序。`%20` 解码成空格、查询串去掉了。这两件事官方示例都不做。

**检查路径。** 服务器把请求的路径接在根目录后面去打开文件，如果不检查，`/../secret.txt` 就能读到根目录外面的文件。`avio_serve` 做三道检查：

```bash
curl -s --path-as-is -D - -o /dev/null $U/../secret.txt | head -1       # 路径里有 ..     → 403
curl -s --path-as-is -D - -o /dev/null $U/%2e%2e/secret.txt | head -1   # 编码过的 ..     → 403（先解码，再检查）
curl -s -D - -o /dev/null $U/escape | head -1                           # 符号链接指到外面 → 403（realpath() 解开链接，看是不是还在根目录里面）
curl -s -o /dev/null -w '%{http_code}\n' "$U/a.mp4%00.txt"               # 解码出 NUL → 400（NUL 会让 C 字符串提前结束，绕过检查）
curl -s -D - $U/nope                                                     # 404
```

```text
HTTP/1.1 403 Forbidden
HTTP/1.1 403 Forbidden
HTTP/1.1 403 Forbidden
400
HTTP/1.1 404 Not Found
Content-Type: text/plain
Content-Length: 15

404 Not Found
```

`curl` 默认会把 `../` 整理掉，要加 `--path-as-is` 才会把它原样发出去。

**同时处理的客户端上限。** `max=2`：已经有两个客户端在下载，第三个得到 429。两个慢的下载用稀疏文件和 `--limit-rate`：

```bash
curl -s --limit-rate 20M --max-time 2 -o /dev/null $U/huge.bin &
curl -s --limit-rate 20M --max-time 2 -o /dev/null $U/huge.bin &
sleep 0.6; curl -s -D - $U/a.mp4
wait %2 %3        # 只等两个 curl；不带参数的 wait 会连后台的服务器一起等，永远不返回
```

```text
HTTP/1.1 429 Too Many Requests
Content-Type: text/plain
Content-Length: 23

429 Too Many Requests
```

两个慢客户端 2 秒后被 curl 自己掐断，`tail -3 out/l19/serve.log` 看到：

```text
/a.mp4 429 0
/huge.bin 200 46202880 aborted
/huge.bin 200 52723712 aborted
```

（字节数每次不同。）

`aborted`：客户端中途断开了。`avio_write()` 没有返回值，写失败要从 `AVIOContext` 的 `error` 字段里看；官方示例没有检查，`avio_serve` 每写一块检查一次，发现断开就不发了、名额放回来。

**Ctrl+C 退出。** `SIGINT`、`SIGTERM` 把一个标志置 1；主循环用 `listen_timeout`（300 毫秒）让 `avio_accept()` 定期返回 `AVERROR(ETIMEDOUT)`，借此检查标志；
退出前等正在发送的客户端发完（最多 10 秒），再关监听。原版 libavformat 上最后的 `avio_close(server)` 会卡住（第 7 步的 0087），
`avio_serve` 在它前面设了 `alarm(2)`：2 秒内没返回，闹钟结束进程（退出码 142）；打了补丁的 libavformat 上马上返回。

```bash
kill -INT %1; wait %1; echo "退出码 $?"
```

```text
[1]+  Alarm clock             …/avio_serve …
退出码 142
```

142 是 128 加上 `SIGALRM` 的 14，用时 2.3 秒（原版 libavformat）。打了补丁以后退出码是 0，用时 0.3 秒。

**握手没完成的连接。** 连上来就断开、发一段不是 HTTP 的东西、发 HEAD：服务器的日志里是 `- 400 0 (握手失败：…)`，没有路径可记。
原版 libavformat 里这些连接的 socket 都会泄漏（0088）：`compare_serve.sh` 用“server 进程里还有几个 socket”检查。

## 第 9 步：目录列表和 0 号描述符

`avio_serve` 的目录列表默认用 POSIX 的 `opendir()`，不用本讲前半部分的 `avio_open_dir()`。原因是 patch 0093 的那个 `close(0)`：

```bash
make -C L19_avio_network clean; make -C L19_avio_network EXTRA_CFLAGS=-DUSE_AVIO_DIR   # 改用 avio_open_dir()
$B/avio_serve $W http://127.0.0.1:8091 >/dev/null 2>&1 &
for i in 1 2 3; do curl -sS -o /dev/null -w '/ %{http_code}  ' http://127.0.0.1:8091/; curl -sS -o /dev/null -w '/sub %{http_code}  ' http://127.0.0.1:8091/sub; curl -sS -o /dev/null -w '/nope %{http_code}\n' http://127.0.0.1:8091/nope; done
```

```text
/ 200  curl: (52) Empty reply from server
/sub 000  /nope 200
curl: (52) Empty reply from server
/ 000  /sub 200  /nope 404
curl: (52) Empty reply from server
/ 000  /sub 200  /nope 404
```

乱了：有的请求得到“Empty reply”，有的得到**别人的回复**（第一轮的 `/nope` 本来是 404，得到了 200）。具体哪几个请求出错每次都不一样，这正是这类问题难查的地方。
发生了什么：第一次列目录以后，`avio_close_dir()` 关掉了 0 号描述符（标准输入）。0 号成了“最小的空号”，下一次 `accept()` 返回的 socket 就是 0 号；
这个连接的线程再列一次目录，`avio_close_dir()` 又 `close(0)`，把自己的连接关了；线程接着往“0 号”写回复，而这时下一个客户端的连接已经拿到了 0 号，回复就写进了别人的连接。

先把刚才那个服务器停掉（`pkill -INT -x avio_serve`），换成打了补丁（0093）的 libavformat 就正常（打补丁）：

```bash
LD_LIBRARY_PATH=$PATCHED $B/avio_serve $W http://127.0.0.1:8092 >/dev/null 2>&1 &     # 这个程序还是 -DUSE_AVIO_DIR 编的
for i in 1 2 3; do curl -sS -o /dev/null -w '/ %{http_code}  ' http://127.0.0.1:8092/; curl -sS -o /dev/null -w '/sub %{http_code}  ' http://127.0.0.1:8092/sub; curl -sS -o /dev/null -w '/nope %{http_code}\n' http://127.0.0.1:8092/nope; done
```

```text
/ 200  /sub 200  /nope 404
/ 200  /sub 200  /nope 404
/ 200  /sub 200  /nope 404
```

这个例子的教训比 bug 本身大：**文件描述符 0、1、2 是进程里“最小的空号”**，任何一次多余的 `close()` 都可能关掉别人的东西，而且症状离原因很远。
排查这类问题的工具是 `strace -f -e trace=close,accept,accept4`。

```bash
pkill -INT -x avio_serve
make -C L19_avio_network clean; make -C L19_avio_network      # 换回默认
```

用 CMake 的话，对应的开关是 `cmake -DUSE_AVIO_DIR=ON`。

## 第 10 步：HTTP 服务器的限制，和 MP4（回到 L18）

L18 的结论是：`moov` 在文件尾的 MP4，没有 seek 就读不全。HTTP 服务器没有 Range，客户端就不能 seek。

```bash
ffmpeg -v error -y -i samples/small_360p25.mp4 -c copy -movflags +faststart $W/fast.mp4     # moov 搬到文件头（L15）
(setsid $B/avio_serve $W http://127.0.0.1:8093 >/dev/null 2>&1 &)
python3 L19_avio_network/range_server.py $W 8094 2>/dev/null &
sleep 1
probe() { ffprobe -v error -show_entries stream=codec_name,pix_fmt -of csv=p=0 "$1" 2>&1 | tr '\n' ' '; echo; }
probe http://127.0.0.1:8093/a.mp4       # moov 在文件尾，服务器没有 Range
probe http://127.0.0.1:8093/fast.mp4    # moov 在文件头，服务器没有 Range
probe http://127.0.0.1:8094/a.mp4       # moov 在文件尾，服务器有 Range
pkill -INT -x avio_serve; pkill -f range_server.py
```

```text
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x…] stream 0, offset 0x30: partial file
h264,unknown aac
h264,yuv420p aac
h264,yuv420p aac
```

（第一行是 `ffprobe` 在 stderr 里的警告，`probe` 函数把它和第一次的结果合在了一行，这里分开写。）

三种组合：

| 文件 | 服务器 | 结果 |
| --- | --- | --- |
| `a.mp4`（moov 在文件尾） | 没有 Range（`avio_serve`） | **读不全**：`partial file`，视频流的像素格式未知（和 L18 没有 seek 回调时一模一样） |
| `fast.mp4`（moov 在文件头） | 没有 Range | 读全了：`moov` 在最前面，不用 seek |
| `a.mp4` | 有 Range（`range_server.py`） | 读全了：seek 变成了带 Range 的新请求 |

所以：**用来“点播”的 MP4 要加 `-movflags +faststart`**（L15 讲过为什么），或者服务器要支持 Range。libavformat 内置的服务端两样都没有，
它适合“把一个文件原样倒给一个能顺序读的客户端”（比如 `ffplay`、`ffmpeg -i http://… `读 MPEG-TS），不适合做点播服务器。
要做点播，换 nginx 这类服务器，或者像 `range_server.py` 那样自己实现 Range。

内置服务端的限制清单（都在 `compare_serve.sh` 里验证过）：只认 GET（HEAD 得到 400）；200 的回复没有 `Content-Length`（分块传输），
客户端不知道要下载多大；没有 Range；状态码只有六个；`Content-Type` 可以用选项 `content_type` 设（`avio_serve` 按扩展名设了），别的头可以用选项 `headers` 加。

## 和系统工具比一比

```bash
L19_avio_network/compare_ls.sh
L19_avio_network/compare_serve.sh      # 约 12 秒，用到 curl、python3、ffmpeg、ffprobe，端口从进程号算出来
```

`compare_serve.sh` 比较的内容：

- `avio_serve` 发出去的内容（MP4、带空格的名字、空文件、3 MB、中文名字、带查询串）和原文件逐字节相同；目录列表和 `ls` 一致；`Content-Type`；
- 出错的请求的状态码：404、`..`、编码过的 `..`、符号链接、`%00`、残缺的百分号编码、不是 HTTP 的请求、HEAD；
- 8 个客户端同时下载；`max=1` 时第二个客户端得到 429；客户端中途断开能发现、名额放回来；握手失败的连接关闭以后 socket 的个数；SIGINT 退出；
- 官方示例：下载、404、带查询串的路径、处理完的子进程、父进程里的 socket、文件不存在回 404；
- `avio_cat`：文件、`file:`、管道、`skip`/`count`、出错的退出码、对着 `avio_serve` 和两种 `range_server.py` 读、404、`info=1` 的 MIME 类型；
- `ffprobe` 读 HTTP 上的三种 MP4 组合。

用默认的（原版）libavformat 跑：

```text
相同 40 项；已知的、符合预期的差别 6 项；不同 0 项
```

6 项已知：HEAD 回 400（内置服务端的限制）；握手失败的连接泄漏 socket（0088）；SIGINT 以后 `avio_close(server)` 卡住（0087）；官方示例有子进程没结束（0087）；官方示例父进程里有泄漏的 socket（0088）；
`moov` 在文件尾而服务器没有 Range 时读不全（限制）。换成打了补丁的 libavformat（打补丁），是“相同 44 项，已知 2 项”：只剩 HEAD 和最后那条 MP4（都是内置服务端的限制），
其余 4 项变成“相同”。`valgrind` 和 ASan/UBSan 下，三个自己的程序和两个官方示例的正常路径与出错路径都没有报告。

## 常见错误

| 现象 | 原因和办法 |
| --- | --- |
| `avio_open_dir()` 对 `http://…` 返回 `Function not implemented` | 只有 file、ftp、smb、sftp 实现了列目录；HTTP 没有“目录”的概念 |
| `avio_open_dir("file:/…")` 说没有这个文件 | 原版 libavformat 的问题（patch 0086）；先不带 `file:` |
| 列目录以后程序读标准输入失败、多线程服务器里连接莫名其妙被关 | `avio_close_dir()` 关掉了 0 号描述符（patch 0093）；用 `strace -e trace=close` 看。在没有补丁的库上，列目录可以用 `opendir()` |
| `avio_read_dir()` 的结果每次顺序不同 | 顺序是协议给的，file 协议是 `readdir()` 的顺序；要有序就自己排序 |
| 打印 `AVIODirEntry` 的时间看不懂 | 是**微秒**数（从 1970 年算起），`/ 1000000` 得到秒；没有就是 -1 |
| 用 `entry->size`、`entry->filemode` 时拿到 -1 | 协议没给：只有 `name` 和 `type` 是一定有的，别的要先判断 -1 |
| `avio_free_directory_entry()` 之后还用 `entry->name` | 释放之后名字也没了；要排序、要保存，先 `av_strdup()` |
| `avio_open_dir()` 之后原来的选项字典变空了 | 它会用掉认识的选项；要重复使用，每次传一个副本（`av_dict_copy()`） |
| `avio_open2()` 读 HTTP，选项 `headers` 不起作用 | 值要以 `\r\n` 结尾，不然 libavformat 警告并补上；`-headers` 要在 `-i` 之前（命令行里） |
| HTTP 服务端 `avio_handshake()` 之后 `resource` 是空的 | 握手要先走到“读请求头”那一步，`resource` 才有值；循环调 `avio_handshake()` 直到它非空 |
| `reply_code` 设了 201、302、206，握手返回 `Invalid argument` | 内置服务端只认 200、400、403、404、429、500 |
| 请求带查询串或者 `%20`，服务端 404 | `resource` 是原样的路径，没有解码；自己处理（`avio_serve` 的 `decode_path()`） |
| 服务端发的 MP4，`ffprobe` 读不全 | 服务器没有 Range，`moov` 又在文件尾；文件加 `-movflags +faststart`，或者换支持 Range 的服务器 |
| `avio_close(server)` 卡住 | 原版 libavformat 的问题（patch 0087）；用 `alarm()` 兜底，或者不关监听、让进程退出时由系统关闭 |
| 服务器每接一个客户端多一个文件描述符 | 没握手过的客户端 `avio_close()` 不关 socket（patch 0088）；先把握手走完，再关 |
| `fork()` 的服务器，进程列表里一堆 `<defunct>` | 父进程不回收子进程：`signal(SIGCHLD, SIG_IGN)`（patch 0092），或者 `waitpid()` |
| 客户端中途断开，服务端还在白白读文件 | `avio_write()` 没有返回值；要查 `AVIOContext` 的 `error` 字段（`avio_serve` 的做法） |
| 把用户给的路径接在根目录后面就去打开 | 路径穿越：解码、拒绝 `..`、`realpath()` 以后检查前缀；解码出来的 NUL 也要拒绝 |
| `curl` 测 `..` 的时候测不出来 | `curl` 默认把 `../` 整理掉了，要加 `--path-as-is` |

## 练习

1. 给 `avio_ls` 加一个 `sort=name` 之外的倒序（`reverse=1`）。再想一想：为什么 `qsort()` 的比较函数里要把“名字”作为最后的决胜条件？（提示：`sort=size` 时有很多大小相同的项。）
2. 给 `avio_cat` 加 `append=1`：输出是文件时往后追加，而不是清空重写（提示：`ffmpeg -h protocol=file` 看 file 协议有哪些选项；打开之后还要把位置移到文件尾）。
3. 用 `avio_cat` 的 `seekable=0` 选项读 `range_server.py` 的 `a.mp4`，`info=1` 显示什么？再用 `skip=300000` 试，服务器的日志里有几个请求？和第 5 步里支持 Range 时的比较。
4. 官方示例 `avio_http_serve_files.c` 的复制循环里，客户端中途断开，进程怎么处理？用 `curl --limit-rate 100k --max-time 1` 请求一个大文件，看子进程是不是还在读文件（`strace -p` 子进程）。按 `avio_serve` 的做法改它：检查 `client->error`。
5. 为什么内置 HTTP 服务端对 HEAD 回 400？在 `libavformat/http.c` 里搜 `autodetected`，读 `http_read_header()` 开头的那段，回答：它期望的方法是什么、由什么决定？一个监听上下文能不能同时接受 GET 和 HEAD？（选项 `method` 起什么作用？）
6. `avio_serve` 的线程是“来一个创建一个”，没有上限（`max` 只是让上限之外的客户端回 429，线程还是创建了）。一个连上来什么都不发的客户端会占住一个线程多久？它占不占 `max` 的名额？怎么给它加超时？（提示：`avio_open2()` 的中断回调参数；`http_accept()` 给客户端上下文传了什么、没传什么？）
7. 把 `avio_serve` 的目录列表改成 HTML（`Content-Type: text/html`，每项一个 `<a href>`）。要处理哪两件事才不出安全问题？（提示：名字里的 `<`、`&`，和链接里的百分号编码。）
8. 用 `strace -f -e trace=close,accept,accept4 $B/avio_serve …`，请求两次目录列表，在原版 libavformat 上找到 `close(0)`，再用 `-DUSE_AVIO_DIR` 编的版本找到谁拿到了 0 号。

练习的参考答案和思路：

1. 去掉决胜条件后，相同大小的项的顺序由 `qsort()` 的实现决定，同一个目录两次排出来可能不同；加上名字，结果是确定的，测试才能逐字节比较（`compare_ls.sh` 里 `sort=size` 和 `ls -S` 一致靠的就是这点）。
2. file 协议有选项 `truncate`（默认 1，写的时候清空原文件）。`truncate=0` 打开以后位置还在文件头，写会盖掉开头的内容，所以还要 `avio_seek(out, 0, SEEK_END)`。
3. `seekable=0` 让 http 协议不发 Range、不当成能 seek；`info=1` 显示 `能 seek：否`。`skip=300000` 靠把前面的数据读掉，服务器的日志里还是一个请求（`Range=-`，回整个文件）。
   支持 Range 时是两个请求（第 5 步）：代价是前者读了 300000 字节，后者几乎没读。
4. 客户端断开后，`avio_write()` 往一个已关闭的连接写，失败了但没有返回值，示例继续读完整个文件才结束。改法：`avio_write()` 之后 `if (client->error) break;`。
5. 服务端期望的方法：设了选项 `method` 就是它，没设就自动判断——这个上下文是写（`AVIO_FLAG_WRITE`）的，期望的就是 `GET`（读的上下文期望 `POST`）。收到别的方法一律 `AVERROR_HTTP_BAD_REQUEST`，也就是 400。
   一个上下文只期望一种方法，所以不能同时接受 GET 和 HEAD；要支持 HEAD，得自己在握手前后另做处理，或者改 libavformat。
6. 连上来什么都不发，线程停在第一次 `avio_handshake()` 里，**永远**，并且占着 `max` 的一个名额：`max=2` 时开两个这样的连接（`exec 7<>/dev/tcp/127.0.0.1/端口`），正常请求就得到 429（实测，关掉那两个连接又恢复）。
   `http_accept()` 调 `ffurl_alloc()` 创建客户端上下文，只把监听上下文的中断回调传给了它，选项字典（比如 `timeout`）没有传。
   所以能用的办法是 `avio_open2()` 的中断回调：在回调里判断“这个客户端开始多久了”，超时返回 1。

## 下一讲

L20 硬件解码：官方示例 `hw_decode.c`。画面不再是内存里的 `AVFrame`，而在显卡的显存里，`AVHWDeviceContext`、`AVHWFramesContext`、`hwdownload` 要登场。
回忆 L9 说过的 `nv12`：硬件解码出来的就是它。
